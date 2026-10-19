// glvk: GLSL programs on Vulkan.
//
// The renderer's GLSL 3.30 is compiled by glslang with its "Vulkan rules
// relaxed" mode (the GL-to-Vulkan path made for this): loose uniforms go into
// one uniform block, gl_DefaultUniformBlock, laid out std140, and bindings and
// locations are assigned automatically. The SPIR-V's bindings are then
// patched to the one descriptor set layout every program shares (glvk_impl.h)
// and its vertex inputs to the glBindAttribLocation locations, so programs
// differ only in their modules.
//
// Two things GL does without being asked are added to the last stage before
// rasterisation: the point size (glPointSize; Vulkan reads gl_PointSize) and,
// without VK_EXT_depth_clip_control, GL's [-w, w] clip depth mapped to
// Vulkan's [0, w].
#include "glvk_impl.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace glvk {

namespace {

bool s_glslangReady = false;
uint32_t s_nextProgramId = 1;
const uint32_t kBinaryVersion = 4;
const char kBinaryMagic[8] = {'G', 'L', 'V', 'K', 'P', 'B', '0', '1'};

bool depthFixInShader() { return !D.depthClipControl; }

// Inserts `text` after the #version line (or at the start).
std::string afterVersion(const std::string& src, const std::string& text) {
    size_t at = 0;
    size_t v = src.find("#version");
    if (v != std::string::npos) {
        at = src.find('\n', v);
        at = at == std::string::npos ? src.size() : at + 1;
    }
    std::string s = src;
    s.insert(at, text);
    return s;
}

// Without sampler LOD bias (Metal), each texture(u_tex[N], coord) call gets
// the bias as its third argument, from push constants the draw sets.
std::string biasTextureCalls(const std::string& src) {
    std::string s = src;
    const std::string call = "texture(u_tex[";
    for (size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
        const size_t idx = at + call.size();
        const size_t close = s.find(']', idx);
        if (close == std::string::npos) break;
        const std::string n = s.substr(idx, close - idx);
        int depth = 0;
        size_t end = at + call.size() - 1;  // the call's opening parenthesis
        for (size_t i = at + 7; i < s.size(); i++) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (depth != 0) break;
        const std::string arg = ", glvk_bias[(" + n + ") >> 2][(" + n + ") & 3]";
        s.insert(end, arg);
    }
    return s;
}

std::string prepare(const std::string& src, EShLanguage stage, bool hasGeometry, bool lineFallback) {
    static const char* kPush = "layout(push_constant) uniform GlvkVertexPC { vec4 glvk_pc; };\n";
    if (stage == EShLangVertex) {
        std::string body = src;
        if (lineFallback) {
            // a line program without its geometry shader: the vertex shader's
            // outputs keep their names (genLineVS renames v_ to vl_ for it)
            size_t at = 0;
            while ((at = body.find("#define v_", at)) != std::string::npos) {
                size_t eol = body.find('\n', at);
                if (eol == std::string::npos) eol = body.size();
                if (body.substr(at, eol - at).find(" vl_") != std::string::npos)
                    body.erase(at, eol - at + (eol < body.size() ? 1 : 0));
                else
                    at = eol;
            }
        }
        std::string s = afterVersion(body, "#define main glvk_user_main\n");
        s += "\n#undef main\n";
        s += kPush;
        s += "void main() {\n  glvk_user_main();\n  gl_PointSize = glvk_pc.x;\n";
        if (!hasGeometry && depthFixInShader()) s += "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
        s += "}\n";
        return s;
    }
    if (stage == EShLangFragment && D.noLodBias && src.find("texture(u_tex[") != std::string::npos)
        return afterVersion(biasTextureCalls(src),
                            "layout(push_constant) uniform GlvkFragmentPC { layout(offset = 16) vec4 glvk_bias[2]; };\n");
    if (stage == EShLangGeometry && depthFixInShader()) {
        return afterVersion(src, std::string(kPush) +
                                     "void glvk_emit() { gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; EmitVertex(); }\n"
                                     "#define EmitVertex() glvk_emit()\n");
    }
    return src;
}

bool isSamplerType(int t) {
    return t == GL_SAMPLER_2D || t == GL_SAMPLER_2D_SHADOW || t == GL_SAMPLER_3D || t == GL_SAMPLER_CUBE ||
           t == GL_INT_SAMPLER_2D || t == GL_UNSIGNED_INT_SAMPLER_2D || t == GL_SAMPLER_2D_ARRAY;
}

std::string stripIndex(const std::string& n) {
    size_t b = n.find('[');
    return b == std::string::npos ? n : n.substr(0, b);
}

// What a module declares: its samplers, uniform blocks and vertex inputs,
// and where their decorations' literals are (patchSpirv rewrites them).
struct SpvInfo {
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, size_t> decBinding, decSet, decLocation;  // id -> word index of the literal
    struct Sampler {
        uint32_t var;
        std::string name;
        uint32_t count;  // elements (1 for a scalar)
        bool array;
    };
    struct Block {
        uint32_t var;
        std::string type;
    };
    struct Input {
        uint32_t var;
        std::string name;
        bool integer;
    };
    std::vector<Sampler> samplers;
    std::vector<Block> blocks;
    std::vector<Input> inputs;  // with a Location (not built-ins)
};

bool scanSpirv(const std::vector<uint32_t>& spv, SpvInfo& out) {
    enum {
        OpName = 5, OpTypeInt = 21, OpTypeVector = 23, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypePointer = 32,
        OpConstant = 43, OpVariable = 59, OpDecorate = 71,
        DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34,
        SCUniformConstant = 0, SCInput = 1, SCUniform = 2,
    };
    if (spv.size() < 5 || spv[0] != 0x07230203) return false;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // id -> (storage class, pointee)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;    // id -> (element type, length id)
    std::unordered_map<uint32_t, uint32_t> vectors, constants;             // vector -> component; constant -> value
    std::vector<uint32_t> ints, sampledImages;
    struct Var {
        uint32_t id, type, sc;
    };
    std::vector<Var> vars;
    for (size_t i = 5; i < spv.size();) {
        const uint32_t w0 = spv[i], op = w0 & 0xFFFF, n = w0 >> 16;
        if (n == 0 || i + n > spv.size()) return false;
        switch (op) {
        case OpName: out.names[spv[i + 1]] = reinterpret_cast<const char*>(&spv[i + 2]); break;
        case OpDecorate:
            if (n >= 4 && spv[i + 2] == DecBinding) out.decBinding[spv[i + 1]] = i + 3;
            if (n >= 4 && spv[i + 2] == DecDescriptorSet) out.decSet[spv[i + 1]] = i + 3;
            if (n >= 4 && spv[i + 2] == DecLocation) out.decLocation[spv[i + 1]] = i + 3;
            break;
        case OpTypeInt: ints.push_back(spv[i + 1]); break;
        case OpTypeVector: vectors[spv[i + 1]] = spv[i + 2]; break;
        case OpTypeSampledImage: sampledImages.push_back(spv[i + 1]); break;
        case OpTypeArray: arrays[spv[i + 1]] = {spv[i + 2], spv[i + 3]}; break;
        case OpTypePointer: pointers[spv[i + 1]] = {spv[i + 2], spv[i + 3]}; break;
        case OpConstant:
            if (n >= 4) constants[spv[i + 2]] = spv[i + 3];
            break;
        case OpVariable:
            if (n >= 4) vars.push_back(Var{spv[i + 2], spv[i + 1], spv[i + 3]});
            break;
        default: break;
        }
        i += n;
    }
    auto has = [](const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    for (const Var& v : vars) {
        auto pt = pointers.find(v.type);
        if (pt == pointers.end()) continue;
        const uint32_t pointee = pt->second.second;
        const std::string& name = out.names[v.id];
        if (v.sc == SCUniformConstant) {
            auto ar = arrays.find(pointee);
            const uint32_t elem = ar != arrays.end() ? ar->second.first : pointee;
            if (!has(sampledImages, elem)) continue;
            uint32_t count = 1;
            if (ar != arrays.end()) {
                auto c = constants.find(ar->second.second);
                count = c != constants.end() ? c->second : 1;
            }
            out.samplers.push_back(SpvInfo::Sampler{v.id, name, count, ar != arrays.end()});
        } else if (v.sc == SCUniform) {
            out.blocks.push_back(SpvInfo::Block{v.id, out.names[pointee]});
        } else if (v.sc == SCInput && out.decLocation.count(v.id)) {
            auto vec = vectors.find(pointee);
            const uint32_t comp = vec != vectors.end() ? vec->second : pointee;
            out.inputs.push_back(SpvInfo::Input{v.id, name, has(ints, comp)});
        }
    }
    return true;
}

// The program's bindings, the same in every stage.
struct BindingPlan {
    std::map<std::string, int> samplers;  // sampler uniform -> binding
    std::vector<std::string> blocks;      // named blocks -> B_UBO0 + index
    const std::unordered_map<std::string, int>* attribs = nullptr;
};

// Rewrites a module's DescriptorSet/Binding decorations to the shared
// layout, and (vertex stage) its input locations to the bound ones.
bool patchSpirv(std::vector<uint32_t>& spv, const SpvInfo& info, EShLanguage stage, const BindingPlan& plan, std::string& log) {
    auto setBinding = [&](uint32_t var, int binding, const std::string& what) {
        auto b = info.decBinding.find(var);
        auto s = info.decSet.find(var);
        if (b == info.decBinding.end() || s == info.decSet.end()) {
            log += "glvk: '" + what + "' has no binding decoration\n";
            return false;
        }
        spv[b->second] = uint32_t(binding);
        spv[s->second] = 0;
        return true;
    };
    for (const SpvInfo::Sampler& smp : info.samplers) {
        auto it = plan.samplers.find(smp.name);
        if (it == plan.samplers.end() || !setBinding(smp.var, it->second, smp.name)) return false;
    }
    for (const SpvInfo::Block& blk : info.blocks) {
        int binding = B_DEFAULT_UBO;
        if (blk.type != "gl_DefaultUniformBlock") {
            auto it = std::find(plan.blocks.begin(), plan.blocks.end(), blk.type);
            if (it == plan.blocks.end()) {
                log += "glvk: uniform block '" + blk.type + "' has no binding\n";
                return false;
            }
            binding = B_UBO0 + int(it - plan.blocks.begin());
        }
        if (!setBinding(blk.var, binding, blk.type)) return false;
    }
    if (stage == EShLangVertex && plan.attribs)
        for (const SpvInfo::Input& in : info.inputs) {
            auto it = plan.attribs->find(in.name);
            if (it == plan.attribs->end()) {
                log += "glvk: vertex input '" + in.name + "' has no glBindAttribLocation\n";
                return false;
            }
            spv[info.decLocation.at(in.var)] = uint32_t(it->second);
        }
    return true;
}

VkShaderModule makeModule(const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(D.dev, &ci, nullptr, &m);
    return m;
}

bool makeModules(Program* p) {
    for (int s = 0; s < 3; s++) {
        if (p->spirv[s].empty()) continue;
        p->modules[s] = makeModule(p->spirv[s]);
        if (!p->modules[s]) return false;
    }
    return true;
}

}  // namespace

void destroyProgramObjects(Program* p) {
    programLibraries(p);
    std::vector<VkPipeline> pipes;
    for (auto& kv : p->pipelines) pipes.push_back(kv.second);
    p->pipelines.clear();
    for (VkPipeline& l : p->libPre) pipes.push_back(l), l = VK_NULL_HANDLE;
    for (VkPipeline& l : p->libFrag) pipes.push_back(l), l = VK_NULL_HANDLE;
    VkShaderModule mods[3] = {p->modules[0], p->modules[1], p->modules[2]};
    for (VkShaderModule& m : p->modules) m = VK_NULL_HANDLE;
    deferDelete([pipes, mods] {
        for (VkPipeline pl : pipes)
            if (pl) vkDestroyPipeline(D.dev, pl, nullptr);
        for (VkShaderModule m : mods)
            if (m) vkDestroyShaderModule(D.dev, m, nullptr);
    });
}

// ------------------------------------------------------------------ linking
// A program's GLSL, taken at glLinkProgram (its shaders may be deleted right
// after), and what compiling it makes. Compiling touches nothing of the GL
// state, so it can run on a worker thread (GL_KHR_parallel_shader_compile).
struct LinkInputs {
    std::string src[3];  // vertex, geometry, fragment; empty if missing
    std::unordered_map<std::string, int> attribs;
};
struct LinkOutput {
    bool ok = false;
    std::string log;
    std::vector<uint32_t> spirv[3];
    VkShaderModule modules[3] = {};
    std::vector<UniformInfo> uniforms;
    std::vector<std::string> blockNames;
    std::vector<int> blockSizes;
    int defaultSize = 0;
    uint32_t inputMask = 0, inputInteger = 0, samplerMask = 0;
    bool hasGeometry = false;
};
struct LinkJob {
    LinkInputs in;
    LinkOutput out;
    std::atomic<int> state{0};  // 0 waiting, 1 compiling, 2 compiled
    // the pipeline library parts, made after the compile on the same thread
    // (so a link is done, and the warm-up goes on, before they are)
    std::atomic<int> libState{0};  // 0 not yet, 1 made (or not to be made)
    VkPipeline libPre[2] = {}, libFrag[4] = {};
    std::mutex m;
    std::condition_variable cv;
};

namespace {

std::atomic<bool> s_toldLines{false};

void compileLink(const LinkInputs& in, LinkOutput& o) {
    bool present[3] = {!in.src[0].empty(), !in.src[1].empty(), !in.src[2].empty()};
    if (!present[0] || !present[2]) {
        o.log = "glvk: a program needs a vertex and a fragment shader";
        return;
    }
    bool lineFallback = false;
    if (present[1] && !D.features.geometryShader) {
        // no geometry shaders (MoltenVK): wide lines are drawn as plain
        // 1-pixel lines, by the vertex and fragment shaders alone
        if (!s_toldLines.exchange(true)) logmsg("glvk: no geometry shaders: wide lines are drawn 1 pixel wide");
        present[1] = false;
        lineFallback = true;
    }
    o.hasGeometry = present[1];
    static const EShLanguage kLang[3] = {EShLangVertex, EShLangGeometry, EShLangFragment};
    const EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader* shaders[3] = {nullptr, nullptr, nullptr};
    std::string srcs[3];
    glslang::TProgram program;
    bool ok = true;
    for (int s = 0; s < 3 && ok; s++) {
        if (!present[s]) continue;
        srcs[s] = prepare(in.src[s], kLang[s], o.hasGeometry, lineFallback);
        shaders[s] = new glslang::TShader(kLang[s]);
        const char* str = srcs[s].c_str();
        shaders[s]->setStrings(&str, 1);
        shaders[s]->setEnvInput(glslang::EShSourceGlsl, kLang[s], glslang::EShClientVulkan, 100);
        shaders[s]->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shaders[s]->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        shaders[s]->setEnvInputVulkanRulesRelaxed();
        shaders[s]->setAutoMapBindings(true);
        shaders[s]->setAutoMapLocations(true);
        shaders[s]->setGlobalUniformBlockName("gl_DefaultUniformBlock");
        shaders[s]->setGlobalUniformSet(0);
        shaders[s]->setGlobalUniformBinding(B_DEFAULT_UBO);
        if (!shaders[s]->parse(GetDefaultResources(), 330, ECoreProfile, false, false, msgs)) {
            o.log += std::string("compile: ") + shaders[s]->getInfoLog() + "\n--- source ---\n" + srcs[s];
            ok = false;
            break;
        }
        program.addShader(shaders[s]);
    }
    if (ok && !program.link(msgs)) {
        o.log += std::string("link: ") + program.getInfoLog();
        ok = false;
    }
    if (ok && !program.mapIO()) {
        o.log += std::string("mapIO: ") + program.getInfoLog();
        ok = false;
    }
    if (ok) program.buildReflection(EShReflectionDefault);

    // SPIR-V first: it declares every sampler, block and input (reflection
    // lists only the live ones), and the bindings are planned from it
    SpvInfo infos[3];
    for (int s = 0; ok && s < 3; s++) {
        if (!present[s]) continue;
        glslang::SpvOptions opt;
        opt.generateDebugInfo = false;
        opt.disableOptimizer = true;
        opt.validate = false;
        glslang::GlslangToSpv(*program.getIntermediate(kLang[s]), o.spirv[s], &opt);
        if (!scanSpirv(o.spirv[s], infos[s])) {
            o.log += "glvk: unreadable SPIR-V\n";
            ok = false;
        }
    }
    BindingPlan plan;
    plan.attribs = &in.attribs;
    if (ok) {
        int nextSampler = 0;
        std::string arrayName;
        for (int s = 0; ok && s < 3; s++) {
            for (const SpvInfo::Sampler& smp : infos[s].samplers) {
                if (plan.samplers.count(smp.name)) continue;
                UniformInfo u;
                u.name = smp.name;
                u.block = -2;
                u.glType = GL_SAMPLER_2D;
                u.arraySize = int(std::max(1u, smp.count));
                if (smp.array) {
                    if (!arrayName.empty() || smp.count > kSamplerArrayMax) {
                        o.log += "glvk: more than one sampler array, or one of more than 8\n";
                        ok = false;
                        break;
                    }
                    arrayName = smp.name;
                    u.binding = B_SAMPLER_ARRAY;
                } else {
                    if (nextSampler >= B_SAMPLER_COUNT) {
                        o.log += "glvk: too many samplers\n";
                        ok = false;
                        break;
                    }
                    u.binding = B_SAMPLERS + nextSampler++;
                }
                u.units.assign(size_t(u.arraySize), 0);
                plan.samplers[u.name] = u.binding;
                o.samplerMask |= 1u << u.binding;
                o.uniforms.push_back(u);
            }
            for (const SpvInfo::Block& blk : infos[s].blocks) {
                if (blk.type == "gl_DefaultUniformBlock" ||
                    std::find(plan.blocks.begin(), plan.blocks.end(), blk.type) != plan.blocks.end())
                    continue;
                if (int(plan.blocks.size()) >= B_UBO_COUNT) {
                    o.log += "glvk: too many uniform blocks\n";
                    ok = false;
                    break;
                }
                plan.blocks.push_back(blk.type);
            }
        }
        o.blockNames = plan.blocks;
        o.blockSizes.assign(plan.blocks.size(), 0);
        for (int b = 0; b < program.getNumUniformBlocks(); b++) {
            const glslang::TObjectReflection& blk = program.getUniformBlock(b);
            if (blk.name == "gl_DefaultUniformBlock") o.defaultSize = blk.size;
            for (size_t i = 0; i < plan.blocks.size(); i++)
                if (plan.blocks[i] == blk.name) o.blockSizes[i] = blk.size;
        }
        for (size_t i = 0; i < o.blockSizes.size(); i++)
            if (o.blockSizes[i] <= 0) o.blockSizes[i] = 65536;  // not live in any stage: never read
        for (int i = 0; ok && i < program.getNumUniformVariables(); i++) {
            const glslang::TObjectReflection& r = program.getUniform(i);
            if (isSamplerType(r.glDefineType)) continue;  // from the SPIR-V above
            if (r.index < 0 || r.index >= program.getNumUniformBlocks()) continue;
            if (program.getUniformBlock(r.index).name != "gl_DefaultUniformBlock") continue;  // a named block's member
            UniformInfo u;
            u.name = stripIndex(r.name);
            u.glType = r.glDefineType;
            u.block = -1;
            u.offset = r.offset;
            u.arraySize = std::max(1, r.size);
            u.arrayStride = r.arrayStride;
            if (u.arraySize > 1 && u.arrayStride <= 0) u.arrayStride = 16;
            o.uniforms.push_back(u);
        }
        for (const SpvInfo::Input& inp : infos[0].inputs) {
            auto it = in.attribs.find(inp.name);
            if (it == in.attribs.end()) continue;  // reported by patchSpirv
            o.inputMask |= 1u << it->second;
            if (inp.integer) o.inputInteger |= 1u << it->second;
        }
    }
    for (int s = 0; ok && s < 3; s++)
        if (present[s] && !patchSpirv(o.spirv[s], infos[s], kLang[s], plan, o.log)) ok = false;
    for (glslang::TShader* sh : shaders) delete sh;
    for (int s = 0; ok && s < 3; s++) {
        if (o.spirv[s].empty()) continue;
        o.modules[s] = makeModule(o.spirv[s]);
        if (!o.modules[s]) ok = false;
    }
    if (!ok) {
        for (int s = 0; s < 3; s++) {
            if (o.modules[s]) vkDestroyShaderModule(D.dev, o.modules[s], nullptr);
            o.modules[s] = VK_NULL_HANDLE;
            o.spirv[s].clear();
        }
    }
    o.ok = ok;

}

// ------------------------------------------------------------------ compile threads
std::mutex s_queueMu;
std::condition_variable s_queueCv;
std::deque<std::shared_ptr<LinkJob>> s_queue;
std::vector<std::thread> s_workers;
bool s_quit = false;
int s_threads = -1;  // compile threads; -1: not chosen yet, 0: compile at glLinkProgram

void makeLibraries(LinkJob& j) {
    if (j.out.ok && D.gpl) {
        // both depth clamp settings, and the EFB's sample count
        static const int msaa = [] {
            const char* e = getenv("SMS_MSAA");
            const int n = e ? atoi(e) : 0;
            return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0;
        }();
        j.libPre[1] = makePreRasterLibrary(j.out.modules, true);
        j.libFrag[msaa == 8 ? 3 : msaa == 4 ? 2 : msaa == 2 ? 1 : 0] =
            makeFragmentLibrary(j.out.modules[2], VkSampleCountFlagBits(msaa ? msaa : 1));
        j.libPre[0] = makePreRasterLibrary(j.out.modules, false);
    }
    {
        std::lock_guard<std::mutex> lk(j.m);
        j.libState.store(1);
    }
    j.cv.notify_all();
}

// `libraries`: on a compile thread, which goes on to make the job's pipeline
// library parts; on the GL thread they are left to the draws.
void runJob(LinkJob& j, bool libraries) {
    compileLink(j.in, j.out);
    if (!libraries) j.libState.store(1);
    {
        std::lock_guard<std::mutex> lk(j.m);
        j.state.store(2);
    }
    j.cv.notify_all();
    if (libraries) makeLibraries(j);
}

void worker() {
#ifdef __linux__
    // below the game's threads: compiles ahead of need must not slow a frame
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 5);
#endif
    for (;;) {
        std::shared_ptr<LinkJob> j;
        {
            std::unique_lock<std::mutex> lk(s_queueMu);
            s_queueCv.wait(lk, [] { return s_quit || !s_queue.empty(); });
            if (s_quit) return;
            j = s_queue.front();
            s_queue.pop_front();
        }
        int waiting = 0;
        if (j->state.compare_exchange_strong(waiting, 1)) runJob(*j, true);
    }
}

void startWorkers(int n) {
    while (int(s_workers.size()) < n) s_workers.emplace_back(worker);
}

int defaultThreads() {
    if (const char* e = getenv("SMS_VK_COMPILE_THREADS")) return std::max(0, std::min(16, atoi(e)));
    const unsigned hw = std::thread::hardware_concurrency();
    return int(std::max(1u, std::min(4u, hw > 1 ? hw - 1 : 1u)));
}

void apply(Program* p, LinkOutput& o) {
    p->linked = o.ok;
    p->log = std::move(o.log);
    for (int s = 0; s < 3; s++) {
        p->spirv[s] = std::move(o.spirv[s]);
        p->modules[s] = o.modules[s];
    }
    p->uniforms = std::move(o.uniforms);
    p->blockNames = std::move(o.blockNames);
    p->blockSizes = std::move(o.blockSizes);
    p->inputMask = o.inputMask;
    p->inputInteger = o.inputInteger;
    p->samplerMask = o.samplerMask;
    p->hasGeometry = o.hasGeometry;
    if (o.ok) {
        p->defaultBlock.assign(size_t(std::max(o.defaultSize, 16)), 0);
        p->defaultDirty = true;
    }
}

}  // namespace

void setCompileThreads(GLuint n) {
    s_threads = n == 0xFFFFFFFFu ? defaultThreads() : int(std::min<GLuint>(n, 16));
    startWorkers(s_threads);
}

bool linkProgram(Program* p) {
    if (!s_glslangReady) {
        glslang::InitializeProcess();
        s_glslangReady = true;
    }
    finishLink(p);
    destroyProgramObjects(p);
    p->linked = false;
    p->log.clear();
    p->uniforms.clear();
    p->blockNames.clear();
    p->blockSizes.clear();
    p->defaultBlock.clear();
    p->inputMask = p->inputInteger = p->samplerMask = 0;
    p->hasGeometry = false;
    for (auto& s : p->spirv) s.clear();
    if (!p->id) p->id = s_nextProgramId++;
    auto job = std::make_shared<LinkJob>();
    for (GLuint n : p->shaders) {
        const Shader* sh = g_shaders.get(n);
        if (!sh) continue;
        const int s = sh->type == GL_VERTEX_SHADER ? 0 : sh->type == GL_GEOMETRY_SHADER ? 1 : sh->type == GL_FRAGMENT_SHADER ? 2 : -1;
        if (s >= 0) job->in.src[s] = sh->source;
    }
    job->in.attribs = p->attribLocations;
    if (s_threads < 0) setCompileThreads(0xFFFFFFFFu);
    if (s_threads > 0) {
        p->job = job;
        {
            std::lock_guard<std::mutex> lk(s_queueMu);
            s_queue.push_back(job);
        }
        s_queueCv.notify_one();
        return true;
    }
    runJob(*job, false);
    apply(p, job->out);
    return p->linked;
}

bool linkDone(const Program* p) { return !p->job || p->job->state.load() == 2; }

void takeLibraries(Program* p, LinkJob& j) {
    for (int i = 0; i < 2; i++)
        if (j.libPre[i] && !p->libPre[i]) p->libPre[i] = j.libPre[i], j.libPre[i] = VK_NULL_HANDLE;
    for (int i = 0; i < 4; i++)
        if (j.libFrag[i] && !p->libFrag[i]) p->libFrag[i] = j.libFrag[i], j.libFrag[i] = VK_NULL_HANDLE;
    for (VkPipeline& l : j.libPre)
        if (l) vkDestroyPipeline(D.dev, l, nullptr), l = VK_NULL_HANDLE;
    for (VkPipeline& l : j.libFrag)
        if (l) vkDestroyPipeline(D.dev, l, nullptr), l = VK_NULL_HANDLE;
}

// The parts a compile thread made for p, waiting for them if it is on them.
void programLibraries(Program* p) {
    if (!p->libJob) return;
    std::shared_ptr<LinkJob> j = std::move(p->libJob);
    {
        std::unique_lock<std::mutex> lk(j->m);
        j->cv.wait(lk, [&] { return j->libState.load() == 1; });
    }
    takeLibraries(p, *j);
}

void finishLink(Program* p) {
    if (!p->job) return;
    std::shared_ptr<LinkJob> j = std::move(p->job);
    int waiting = 0;
    if (j->state.compare_exchange_strong(waiting, 1)) {
        runJob(*j, false);  // not started yet: compile it here rather than wait for a thread
    } else {
        std::unique_lock<std::mutex> lk(j->m);
        j->cv.wait(lk, [&] { return j->state.load() == 2; });
    }
    apply(p, j->out);
    if (j->libState.load() == 0) p->libJob = j;  // its library parts are still being made
    else takeLibraries(p, *j);
    if (!p->linked) logmsg("glvk: program link failed: %s", p->log.c_str());
}

void programUniform(Program* p, GLint loc, int comps, GLsizei count, const void* data, bool integer) {
    (void)integer;
    if (!p || loc < 0) return;
    finishLink(p);
    const size_t idx = size_t(loc) >> 8;
    int elem = loc & 0xFF;
    if (idx >= p->uniforms.size()) return;
    UniformInfo& u = p->uniforms[idx];
    if (u.block == -2) {
        const GLint* v = static_cast<const GLint*>(data);
        for (GLsizei k = 0; k < count && elem + k < u.arraySize; k++) u.units[size_t(elem + k)] = v[k];
        p->defaultDirty = true;  // the descriptors are pushed again with the block
        return;
    }
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (GLsizei k = 0; k < count && elem + k < u.arraySize; k++) {
        const size_t at = size_t(u.offset) + size_t(elem + k) * size_t(u.arrayStride);
        if (at + size_t(comps) * 4 > p->defaultBlock.size()) break;
        memcpy(p->defaultBlock.data() + at, src + size_t(k) * size_t(comps) * 4, size_t(comps) * 4);
    }
    p->defaultDirty = true;
}

// ------------------------------------------------------------------ program binaries
namespace {
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    o.insert(o.end(), b, b + 4);
}
void putStr(std::vector<uint8_t>& o, const std::string& s) {
    putU32(o, uint32_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint32_t u32() {
        if (end - p < 4) {
            ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || uint32_t(end - p) < n) {
            ok = false;
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};
}  // namespace

bool programBinary(Program* p, std::vector<uint8_t>& out) {
    finishLink(p);
    if (!p->linked) return false;
    out.assign(kBinaryMagic, kBinaryMagic + 8);
    putU32(out, kBinaryVersion);
    putU32(out, (p->hasGeometry ? 1u : 0u) | (depthFixInShader() ? 2u : 0u) | (D.noLodBias ? 4u : 0u) |
                    (D.features.geometryShader ? 0u : 8u));
    for (int s = 0; s < 3; s++) {
        putU32(out, uint32_t(p->spirv[s].size()));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p->spirv[s].data());
        out.insert(out.end(), b, b + p->spirv[s].size() * 4);
    }
    putU32(out, uint32_t(p->uniforms.size()));
    for (const UniformInfo& u : p->uniforms) {
        putStr(out, u.name);
        putU32(out, uint32_t(u.block));
        putU32(out, uint32_t(u.offset));
        putU32(out, uint32_t(u.arrayStride));
        putU32(out, uint32_t(u.arraySize));
        putU32(out, uint32_t(u.glType));
        putU32(out, uint32_t(u.binding));
    }
    putU32(out, uint32_t(p->blockNames.size()));
    for (size_t i = 0; i < p->blockNames.size(); i++) {
        putStr(out, p->blockNames[i]);
        putU32(out, uint32_t(p->blockSizes[i]));
    }
    putU32(out, uint32_t(p->defaultBlock.size()));
    putU32(out, p->inputMask);
    putU32(out, p->inputInteger);
    putU32(out, p->samplerMask);
    return true;
}

bool loadProgramBinary(Program* p, const void* data, size_t len) {
    finishLink(p);
    destroyProgramObjects(p);
    p->linked = false;
    Reader r{static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len};
    if (len < 16 || memcmp(data, kBinaryMagic, 8) != 0) return false;
    r.p += 8;
    if (r.u32() != kBinaryVersion) return false;
    const uint32_t flags = r.u32();
    if (((flags & 2) != 0) != depthFixInShader() || ((flags & 4) != 0) != D.noLodBias ||
        ((flags & 8) != 0) != !D.features.geometryShader)
        return false;
    p->hasGeometry = (flags & 1) != 0;
    if (p->hasGeometry && !D.features.geometryShader) return false;
    for (int s = 0; s < 3; s++) {
        const uint32_t n = r.u32();
        if (!r.ok || uint64_t(r.end - r.p) < uint64_t(n) * 4) return false;
        p->spirv[s].resize(n);
        memcpy(p->spirv[s].data(), r.p, size_t(n) * 4);
        r.p += size_t(n) * 4;
    }
    p->uniforms.clear();
    const uint32_t nu = r.u32();
    for (uint32_t i = 0; i < nu && r.ok; i++) {
        UniformInfo u;
        u.name = r.str();
        u.block = int(r.u32());
        u.offset = int(r.u32());
        u.arrayStride = int(r.u32());
        u.arraySize = int(r.u32());
        u.glType = int(r.u32());
        u.binding = int(r.u32());
        if (u.block == -2) u.units.assign(size_t(std::max(1, u.arraySize)), 0);
        p->uniforms.push_back(u);
    }
    p->blockNames.clear();
    p->blockSizes.clear();
    const uint32_t nb = r.u32();
    for (uint32_t i = 0; i < nb && r.ok; i++) {
        p->blockNames.push_back(r.str());
        p->blockSizes.push_back(int(r.u32()));
    }
    const uint32_t defSize = r.u32();
    p->inputMask = r.u32();
    p->inputInteger = r.u32();
    p->samplerMask = r.u32();
    if (!r.ok || defSize > (1u << 20) || p->spirv[0].empty() || p->spirv[2].empty()) return false;
    if (!makeModules(p)) return false;
    p->defaultBlock.assign(defSize, 0);
    p->defaultDirty = true;
    if (!p->id) p->id = s_nextProgramId++;
    p->linked = true;
    return true;
}

// A Vulkan GLSL shader of glvk's own (clears): stage 0 vertex, 2 fragment.
std::vector<uint32_t> compileGlsl(int stage, const char* src) {
    if (!s_glslangReady) {
        glslang::InitializeProcess();
        s_glslangReady = true;
    }
    const EShLanguage lang = stage == 0 ? EShLangVertex : stage == 1 ? EShLangGeometry : EShLangFragment;
    const EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    std::vector<uint32_t> spv;
    if (!sh.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs)) {
        logmsg("glvk: internal shader failed: %s", sh.getInfoLog());
        return spv;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(msgs)) {
        logmsg("glvk: internal shader failed to link: %s", program.getInfoLog());
        return spv;
    }
    glslang::SpvOptions opt;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spv, &opt);
    return spv;
}

void shaderShutdown() {
    {
        std::lock_guard<std::mutex> lk(s_queueMu);
        s_quit = true;
        s_queue.clear();
    }
    s_queueCv.notify_all();
    for (std::thread& t : s_workers) t.join();
    s_workers.clear();
    s_quit = false;
    s_threads = -1;
    if (s_glslangReady) glslang::FinalizeProcess();
    s_glslangReady = false;
}

}  // namespace glvk
