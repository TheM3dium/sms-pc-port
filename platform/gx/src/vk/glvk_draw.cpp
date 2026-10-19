// glvk: rendering, pipelines, draws and clears, and the GL entry points.
#include "glvk.h"
#include "glvk_impl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>

namespace glvk {

std::vector<uint32_t> compileGlsl(int stage, const char* src);  // glvk_shader.cpp

namespace {

// ------------------------------------------------------------------ rendering
struct RenderTarget {
    Image* color = nullptr;
    Image* depth = nullptr;
    uint32_t colorLevel = 0, depthLevel = 0;
    uint32_t w = 0, h = 0;
    bool operator==(const RenderTarget& o) const {
        return color == o.color && depth == o.depth && colorLevel == o.colorLevel && depthLevel == o.depthLevel;
    }
};
bool s_rendering = false;
RenderTarget s_rt;

bool resolveTarget(GLuint fbo, RenderTarget& rt) {
    rt = RenderTarget();
    if (fbo == 0) {
        rt.color = g_backbuffer;
    } else {
        Framebuffer* f = g_framebuffers.get(fbo);
        if (!f) return false;
        rt.color = attachmentImage(f->color, &rt.colorLevel);
        rt.depth = attachmentImage(f->depth, &rt.depthLevel);
    }
    if (!rt.color && !rt.depth) return false;
    uint32_t w = ~0u, h = ~0u;
    if (rt.color) {
        w = std::min(w, std::max(1u, rt.color->w >> rt.colorLevel));
        h = std::min(h, std::max(1u, rt.color->h >> rt.colorLevel));
    }
    if (rt.depth) {
        w = std::min(w, std::max(1u, rt.depth->w >> rt.depthLevel));
        h = std::min(h, std::max(1u, rt.depth->h >> rt.depthLevel));
    }
    rt.w = w;
    rt.h = h;
    return true;
}

void beginRendering(const RenderTarget& rt, const VkClearValue* clearColor, const VkClearValue* clearDepth) {
    if (s_rendering) endRendering();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (rt.color) {
        const VkImageLayout before = rt.color->layout;
        use(rt.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
        ca.imageView = imageView(rt.color, rt.colorLevel, 1);
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR
                  : before == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clearColor) ca.clearValue = *clearColor;
    }
    if (rt.depth) {
        const VkImageLayout before = rt.depth->layout;
        use(rt.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
        da.imageView = imageView(rt.depth, rt.depthLevel, 1);
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR
                  : before == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clearDepth) da.clearValue = *clearDepth;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rt.w, rt.h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = rt.color ? 1 : 0;
    ri.pColorAttachments = rt.color ? &ca : nullptr;
    ri.pDepthAttachment = rt.depth ? &da : nullptr;
    D.cmdBeginRendering(mainCmd(), &ri);
    g_stats.renderPasses++;
    s_rendering = true;
    s_rt = rt;
    if (S.query) beginQueryPiece();
}

// Rendering into the draw framebuffer, begun if it is not already.
bool ensureRendering() {
    RenderTarget rt;
    if (!resolveTarget(S.drawFbo, rt)) return false;
    if (s_rendering && rt == s_rt) return true;
    beginRendering(rt, nullptr, nullptr);
    return true;
}

// ------------------------------------------------------------------ command-buffer state
struct CmdCache {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool vpValid = false, scValid = false, bcValid = false, pcValid = false;
    VkViewport vp{};
    VkRect2D sc{};
    float bc[4] = {};
    float pc[4] = {};
    bool biasValid = false;
    float bias[8] = {};
    int cull = -1, front = -1, dtest = -1, dwrite = -1, dcmp = -1;
    VkBuffer vb[kMaxAttribs + 1] = {};
    VkDeviceSize vbOff[kMaxAttribs + 1] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    VkIndexType ibType = VK_INDEX_TYPE_UINT32;
    bool descValid = false;
    uint32_t descMask = 0;
    VkDescriptorBufferInfo ubo[B_SAMPLER_ARRAY] = {};
    VkDescriptorImageInfo imgs[kSamplerArrayMax + B_SAMPLER_COUNT] = {};
    uint32_t constVersion = 0;
    VkDescriptorBufferInfo constAt{};
};
CmdCache s_cc;
uint32_t s_constVersion = 1;  // bumped by glVertexAttrib*
uint32_t s_framePipelines = 0;      // made since the last present
double s_framePipelineSeconds = 0;
uint32_t s_pipelineCreates = 0;

// ------------------------------------------------------------------ vertex layouts
struct LayoutKey {
    GLuint vao;
    uint32_t version, inputMask, inputInteger;
    bool operator==(const LayoutKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct LayoutKeyHash {
    size_t operator()(const LayoutKey& k) const {
        return size_t((uint64_t(k.vao) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.version) << 20) ^ (uint64_t(k.inputMask) << 40) ^
                      k.inputInteger);
    }
};
struct VertexLayout {
    uint64_t hash = 0;  // of what pipelines depend on
    uint32_t nbind = 0, nattr = 0;
    VkVertexInputBindingDescription binds[kMaxAttribs + 1];
    VkVertexInputAttributeDescription attrs[kMaxAttribs];
    GLuint bufs[kMaxAttribs];
    VkDeviceSize offs[kMaxAttribs];
    uint32_t narray = 0;  // array bindings; the constant one (if any) follows
    bool hasConst = false;
    uint32_t constMask = 0, constInteger = 0;
};
std::unordered_map<LayoutKey, VertexLayout*, LayoutKeyHash> s_layouts;

uint32_t typeBytes(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    default: return 4;
    }
}

VkFormat attribFormat(const VertexAttrib& a) {
    const int n = std::max(1, std::min(4, int(a.size)));
    static const VkFormat f32[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat u32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const VkFormat s32[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT};
    static const VkFormat u8n[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat u8s[4] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED};
    static const VkFormat u8i[4] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT};
    static const VkFormat s8n[4] = {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM};
    static const VkFormat u16n[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
    static const VkFormat s16n[4] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat s16s[4] = {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    switch (a.type) {
    case GL_FLOAT: return f32[n - 1];
    case GL_UNSIGNED_INT: return u32[n - 1];
    case GL_INT: return s32[n - 1];
    case GL_UNSIGNED_BYTE: return a.integer ? u8i[n - 1] : a.normalized ? u8n[n - 1] : u8s[n - 1];
    case GL_BYTE: return s8n[n - 1];
    case GL_UNSIGNED_SHORT: return u16n[n - 1];
    case GL_SHORT: return a.normalized ? s16n[n - 1] : s16s[n - 1];
    default: return f32[n - 1];
    }
}

uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

const VertexLayout* vertexLayout(const Program* p) {
    static const VertexArray s_none;
    const VertexArray* va = g_vertexArrays.get(S.vao);
    if (!va) va = &s_none;
    LayoutKey key{S.vao, va->version, p->inputMask, p->inputInteger};
    static LayoutKey lastKey{~0u, 0, 0, 0};
    static const VertexLayout* last = nullptr;
    if (last && key == lastKey) return last;
    auto it = s_layouts.find(key);
    if (it != s_layouts.end()) {
        lastKey = key;
        return last = it->second;
    }
    VertexLayout* L = new VertexLayout;
    uint64_t h = fnv(&p->inputMask, 4);
    for (uint32_t loc = 0; loc < kMaxAttribs; loc++) {
        if (!(p->inputMask & (1u << loc))) continue;
        const VertexAttrib& a = va->attribs[loc];
        if (!a.enabled || !g_buffers.get(a.buffer)) {
            L->constMask |= 1u << loc;
            if (p->inputInteger & (1u << loc)) L->constInteger |= 1u << loc;
            continue;
        }
        const uint32_t stride = a.stride ? uint32_t(a.stride) : uint32_t(std::max(1, int(a.size))) * typeBytes(a.type);
        uint32_t b = 0;
        for (; b < L->narray; b++)
            if (L->bufs[b] == a.buffer && L->binds[b].stride == stride) break;
        if (b == L->narray) {
            L->binds[b] = VkVertexInputBindingDescription{b, stride, VK_VERTEX_INPUT_RATE_VERTEX};
            L->bufs[b] = a.buffer;
            L->offs[b] = VkDeviceSize(a.offset);  // the lowest offset of the binding's attributes
            L->narray++;
        } else if (VkDeviceSize(a.offset) < L->offs[b]) {
            L->offs[b] = VkDeviceSize(a.offset);
        }
    }
    for (uint32_t loc = 0; loc < kMaxAttribs; loc++) {
        if (!(p->inputMask & (1u << loc)) || (L->constMask & (1u << loc))) continue;
        const VertexAttrib& a = va->attribs[loc];
        const uint32_t stride = a.stride ? uint32_t(a.stride) : uint32_t(std::max(1, int(a.size))) * typeBytes(a.type);
        uint32_t b = 0;
        while (b < L->narray && !(L->bufs[b] == a.buffer && L->binds[b].stride == stride)) b++;
        L->attrs[L->nattr++] = VkVertexInputAttributeDescription{loc, b, attribFormat(a), uint32_t(VkDeviceSize(a.offset) - L->offs[b])};
    }
    if (L->constMask) {
        L->hasConst = true;
        const uint32_t cb = L->narray;
        L->binds[cb] = VkVertexInputBindingDescription{cb, 16 * kMaxAttribs, VK_VERTEX_INPUT_RATE_INSTANCE};
        for (uint32_t loc = 0; loc < kMaxAttribs; loc++)
            if (L->constMask & (1u << loc))
                L->attrs[L->nattr++] = VkVertexInputAttributeDescription{
                    loc, cb, (L->constInteger & (1u << loc)) ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, loc * 16};
    }
    L->nbind = L->narray + (L->hasConst ? 1 : 0);
    h = fnv(L->binds, sizeof(VkVertexInputBindingDescription) * L->nbind, h);
    h = fnv(L->attrs, sizeof(VkVertexInputAttributeDescription) * L->nattr, h);
    L->hash = h;
    s_layouts[key] = L;
    lastKey = key;
    return last = L;
}

// ------------------------------------------------------------------ pipelines
struct PipeKey {
    uint64_t layout;
    uint32_t colorFormat, depthFormat;
    uint8_t topology, samples, blend, writeMask;
    uint8_t srcRGB, dstRGB, srcA, dstA, opRGB, opA, logic, logicOp;
    uint8_t depthClamp, cull, front, depthTest, depthWrite, depthCmp, pad0, pad1;
};

VkBlendFactor blendFactor(GLenum f) {
    switch (f) {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blendOp(GLenum e) {
    switch (e) {
    case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GL_MIN: return VK_BLEND_OP_MIN;
    case GL_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkCompareOp compareOp(GLenum f) {
    switch (f) {
    case GL_NEVER: return VK_COMPARE_OP_NEVER;
    case GL_LESS: return VK_COMPARE_OP_LESS;
    case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
VkCullModeFlags cullMode() {
    if (!S.cullFace) return VK_CULL_MODE_NONE;
    return S.cullMode == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : S.cullMode == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK
                                                                                           : VK_CULL_MODE_BACK_BIT;
}
// GL's signed area has the opposite sign of Vulkan's for the same window
// coordinates (glvk does not flip y), so its counter-clockwise is Vulkan's clockwise.
VkFrontFace frontFace() { return S.frontFace == GL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; }
VkPrimitiveTopology topology(GLenum mode) {
    switch (mode) {
    case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

PipeKey pipeKey(const VertexLayout* vl, VkPrimitiveTopology topo) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.layout = vl->hash;
    k.colorFormat = s_rt.color ? uint32_t(s_rt.color->format) : 0;
    k.depthFormat = s_rt.depth ? uint32_t(s_rt.depth->format) : 0;
    k.samples = uint8_t(s_rt.color ? s_rt.color->samples : s_rt.depth ? s_rt.depth->samples : 1);
    k.topology = uint8_t(topo);
    k.writeMask = uint8_t((S.colorMask[0] ? 1 : 0) | (S.colorMask[1] ? 2 : 0) | (S.colorMask[2] ? 4 : 0) | (S.colorMask[3] ? 8 : 0));
    k.logic = S.logicOp && D.features.logicOp;
    if (k.logic) k.logicOp = uint8_t(S.logicOpMode - GL_CLEAR);
    k.blend = S.blend && !k.logic;
    if (k.blend) {
        k.srcRGB = uint8_t(blendFactor(S.blendSrcRGB));
        k.dstRGB = uint8_t(blendFactor(S.blendDstRGB));
        k.srcA = uint8_t(blendFactor(S.blendSrcA));
        k.dstA = uint8_t(blendFactor(S.blendDstA));
        k.opRGB = uint8_t(blendOp(S.blendEqRGB));
        k.opA = uint8_t(blendOp(S.blendEqA));
    }
    k.depthClamp = S.depthClamp && D.features.depthClamp;
    if (!D.eds) {
        k.cull = uint8_t(cullMode());
        k.front = uint8_t(frontFace());
        k.depthTest = S.depthTest;
        k.depthWrite = S.depthMask;
        k.depthCmp = uint8_t(compareOp(S.depthFunc));
    }
    return k;
}

VkPipeline createPipeline(Program* p, const VertexLayout* vl, const PipeKey& k) {
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t ns = 0;
    const VkShaderStageFlagBits kStage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (int s = 0; s < 3; s++) {
        if (!p->modules[s]) continue;
        stages[ns] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = kStage[s];
        stages[ns].module = p->modules[s];
        stages[ns].pName = "main";
        ns++;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = vl->nbind;
    vi.pVertexBindingDescriptions = vl->binds;
    vi.vertexAttributeDescriptionCount = vl->nattr;
    vi.pVertexAttributeDescriptions = vl->attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VkPrimitiveTopology(k.topology);
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.pNext = D.depthClipControl ? &clip : nullptr;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = k.depthClamp;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VkCullModeFlags(k.cull);
    rs.frontFace = VkFrontFace(k.front);
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(k.samples ? k.samples : 1);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depthTest;
    ds.depthWriteEnable = k.depthWrite;
    ds.depthCompareOp = VkCompareOp(k.depthCmp);
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = k.blend;
    att.srcColorBlendFactor = VkBlendFactor(k.srcRGB);
    att.dstColorBlendFactor = VkBlendFactor(k.dstRGB);
    att.colorBlendOp = VkBlendOp(k.opRGB);
    att.srcAlphaBlendFactor = VkBlendFactor(k.srcA);
    att.dstAlphaBlendFactor = VkBlendFactor(k.dstA);
    att.alphaBlendOp = VkBlendOp(k.opA);
    att.colorWriteMask = k.writeMask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = k.logic;
    cb.logicOp = VkLogicOp(k.logicOp);
    cb.attachmentCount = k.colorFormat ? 1 : 0;
    cb.pAttachments = &att;
    uint32_t nd = 0;
    const VkDynamicState* dyn = pipelineDynamicStates(&nd);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = nd;
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.colorFormat);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.colorFormat ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = VkFormat(k.depthFormat);
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = ns;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = D.pipeLayout;
    VkPipeline pipe = VK_NULL_HANDLE;
    const double t0 = now();
    VkResult r = vkCreateGraphicsPipelines(D.dev, D.pipelineCache, 1, &ci, nullptr, &pipe);
    g_stats.pipelineSeconds += now() - t0;
    s_framePipelines++;
    s_framePipelineSeconds += now() - t0;
    if (r != VK_SUCCESS) {
        static int told = 0;
        if (told++ < 8) logmsg("Vulkan: vkCreateGraphicsPipelines failed (%d) for program %u", int(r), p->id);
        return VK_NULL_HANDLE;
    }
    s_pipelineCreates++;
    g_stats.pipelines++;
    return pipe;
}

// ------------------------------------------------------------------ pipeline libraries
// With VK_EXT_graphics_pipeline_library, a pipeline is linked (fast, no
// compile) from four parts: the vertex input (per vertex layout and
// topology), the program's pre-rasterisation and fragment shaders (made once
// per program, on a compile thread right after it links), and the fragment
// output (per target formats and blend state).
std::unordered_map<uint64_t, VkPipeline> s_vertexLibs, s_outputLibs;

VkPipeline createLibrary(VkGraphicsPipelineCreateInfo& ci, VkGraphicsPipelineLibraryFlagsEXT part, void* more) {
    VkGraphicsPipelineLibraryCreateInfoEXT li{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    li.flags = part;
    li.pNext = more;
    ci.pNext = &li;
    ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    uint32_t nd = 0;
    const VkDynamicState* dyn = pipelineDynamicStates(&nd);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = nd;
    dy.pDynamicStates = dyn;
    ci.pDynamicState = &dy;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(D.dev, D.pipelineCache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
        static std::atomic<int> told{0};
        if (told++ < 4) logmsg("Vulkan: a pipeline library (part %#x) failed", unsigned(part));
        return VK_NULL_HANDLE;
    }
    return pipe;
}

VkPipeline vertexInputLibrary(const VertexLayout* vl, VkPrimitiveTopology topo) {
    const uint64_t key = vl->hash * 31 + uint64_t(topo);
    auto it = s_vertexLibs.find(key);
    if (it != s_vertexLibs.end()) return it->second;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = vl->nbind;
    vi.pVertexBindingDescriptions = vl->binds;
    vi.vertexAttributeDescriptionCount = vl->nattr;
    vi.pVertexAttributeDescriptions = vl->attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    return s_vertexLibs[key] = createLibrary(ci, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, nullptr);
}

VkPipeline outputLibrary(const PipeKey& k) {
    struct OutKey {
        uint32_t color, depth;
        uint8_t samples, blend, srcRGB, dstRGB, srcA, dstA, opRGB, opA, writeMask, logic, logicOp, pad;
    } ok{k.colorFormat, k.depthFormat, k.samples, k.blend, k.srcRGB, k.dstRGB, k.srcA, k.dstA, k.opRGB, k.opA, k.writeMask,
         k.logic, k.logicOp, 0};
    const uint64_t key = fnv(&ok, sizeof ok);
    auto it = s_outputLibs.find(key);
    if (it != s_outputLibs.end()) return it->second;
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = k.blend;
    att.srcColorBlendFactor = VkBlendFactor(k.srcRGB);
    att.dstColorBlendFactor = VkBlendFactor(k.dstRGB);
    att.colorBlendOp = VkBlendOp(k.opRGB);
    att.srcAlphaBlendFactor = VkBlendFactor(k.srcA);
    att.dstAlphaBlendFactor = VkBlendFactor(k.dstA);
    att.alphaBlendOp = VkBlendOp(k.opA);
    att.colorWriteMask = k.writeMask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = k.logic;
    cb.logicOp = VkLogicOp(k.logicOp);
    cb.attachmentCount = k.colorFormat ? 1 : 0;
    cb.pAttachments = &att;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(k.samples ? k.samples : 1);
    VkFormat color = VkFormat(k.colorFormat);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.colorFormat ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = VkFormat(k.depthFormat);
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pColorBlendState = &cb;
    ci.pMultisampleState = &ms;
    return s_outputLibs[key] = createLibrary(ci, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, &ri);
}

int sampleIndex(uint32_t samples) { return samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0; }

VkPipeline linkPipeline(Program* p, const VertexLayout* vl, const PipeKey& k) {
    const int clamp = k.depthClamp ? 1 : 0, si = sampleIndex(k.samples);
    programLibraries(p);
    if (!p->libPre[clamp]) p->libPre[clamp] = makePreRasterLibrary(p->modules, clamp != 0);
    if (!p->libFrag[si]) p->libFrag[si] = makeFragmentLibrary(p->modules[2], VkSampleCountFlagBits(k.samples ? k.samples : 1));
    VkPipeline libs[4] = {vertexInputLibrary(vl, VkPrimitiveTopology(k.topology)), p->libPre[clamp], p->libFrag[si],
                          outputLibrary(k)};
    for (VkPipeline l : libs)
        if (!l) return createPipeline(p, vl, k);  // a part failed: the whole pipeline at once
    VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    li.libraryCount = 4;
    li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &li;
    ci.layout = D.pipeLayout;
    VkPipeline pipe = VK_NULL_HANDLE;
    const double t0 = now();
    VkResult r = vkCreateGraphicsPipelines(D.dev, D.pipelineCache, 1, &ci, nullptr, &pipe);
    g_stats.pipelineSeconds += now() - t0;
    s_framePipelines++;
    s_framePipelineSeconds += now() - t0;
    if (r != VK_SUCCESS) return createPipeline(p, vl, k);
    s_pipelineCreates++;
    g_stats.pipelines++;
    return pipe;
}

VkPipeline pipelineFor(Program* p, const VertexLayout* vl, VkPrimitiveTopology topo) {
    const PipeKey k = pipeKey(vl, topo);
    const uint64_t h = fnv(&k, sizeof k);
    auto it = p->pipelines.find(h);
    if (it != p->pipelines.end()) return it->second;
    VkPipeline pipe = D.gpl ? linkPipeline(p, vl, k) : createPipeline(p, vl, k);
    p->pipelines[h] = pipe;
    return pipe;
}

// ------------------------------------------------------------------ dynamic state
VkRect2D scissorRect() {
    if (!S.scissorTest) return VkRect2D{{0, 0}, {s_rt.w, s_rt.h}};
    const int32_t x0 = std::max(0, S.scissor[0]), y0 = std::max(0, S.scissor[1]);
    const int32_t x1 = std::min(int32_t(s_rt.w), S.scissor[0] + std::max(0, S.scissor[2]));
    const int32_t y1 = std::min(int32_t(s_rt.h), S.scissor[1] + std::max(0, S.scissor[3]));
    VkRect2D r{{x0, y0}, {0, 0}};
    if (x1 > x0 && y1 > y0) r.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0)};
    return r;
}

bool applyDynamicState(VkCommandBuffer cb) {
    if (S.viewport[2] <= 0 || S.viewport[3] <= 0) return false;
    VkViewport vp{float(S.viewport[0]), float(S.viewport[1]), float(S.viewport[2]), float(S.viewport[3]),
                  float(S.depthRange[0]), float(S.depthRange[1])};
    if (!s_cc.vpValid || memcmp(&vp, &s_cc.vp, sizeof vp) != 0) {
        vkCmdSetViewport(cb, 0, 1, &vp);
        s_cc.vp = vp;
        s_cc.vpValid = true;
    }
    const VkRect2D sc = scissorRect();
    if (sc.extent.width == 0 || sc.extent.height == 0) return false;
    if (!s_cc.scValid || memcmp(&sc, &s_cc.sc, sizeof sc) != 0) {
        vkCmdSetScissor(cb, 0, 1, &sc);
        s_cc.sc = sc;
        s_cc.scValid = true;
    }
    if (!s_cc.bcValid || memcmp(S.blendColor, s_cc.bc, sizeof s_cc.bc) != 0) {
        vkCmdSetBlendConstants(cb, S.blendColor);
        memcpy(s_cc.bc, S.blendColor, sizeof s_cc.bc);
        s_cc.bcValid = true;
    }
    if (D.eds) {
        const int cull = int(cullMode()), front = int(frontFace()), dt = S.depthTest, dw = S.depthMask,
                  dc = int(compareOp(S.depthFunc));
        if (cull != s_cc.cull) D.cmdSetCullMode(cb, VkCullModeFlags(s_cc.cull = cull));
        if (front != s_cc.front) D.cmdSetFrontFace(cb, VkFrontFace(s_cc.front = front));
        if (dt != s_cc.dtest) D.cmdSetDepthTestEnable(cb, VkBool32(s_cc.dtest = dt));
        if (dw != s_cc.dwrite) D.cmdSetDepthWriteEnable(cb, VkBool32(s_cc.dwrite = dw));
        if (dc != s_cc.dcmp) D.cmdSetDepthCompareOp(cb, VkCompareOp(s_cc.dcmp = dc));
    }
    const float pc[4] = {S.pointSize, 0.0f, 0.0f, 0.0f};
    if (!s_cc.pcValid || memcmp(pc, s_cc.pc, sizeof pc) != 0) {
        vkCmdPushConstants(cb, D.pipeLayout, D.pushStages, 0, sizeof pc, pc);
        memcpy(s_cc.pc, pc, sizeof pc);
        s_cc.pcValid = true;
    }
    return true;
}

// The program's descriptors: textures are resolved (and put in the layout
// for sampling) before rendering begins, since that can need a barrier.
struct DescSet {
    float bias[8];  // the sampler array's LOD biases (without sampler LOD bias)
    VkDescriptorBufferInfo ubo[B_SAMPLER_ARRAY];
    VkDescriptorImageInfo imgs[kSamplerArrayMax + B_SAMPLER_COUNT];  // the array's elements, then the scalars
    uint32_t mask;  // bindings used
    uint32_t arrayCount;
};

void gatherDescriptors(Program* p, DescSet& ds) {
    memset(ds.bias, 0, sizeof ds.bias);
    ds.mask = 1u << B_DEFAULT_UBO;
    ds.arrayCount = 0;
    if (p->defaultDirty || p->defaultBoundSerial != currentSerial()) {
        const VkDeviceSize align = std::max<VkDeviceSize>(D.props.limits.minUniformBufferOffsetAlignment, 16);
        RingAlloc a = g_dynamic.alloc(p->defaultBlock.size(), align);
        memcpy(a.ptr, p->defaultBlock.data(), p->defaultBlock.size());
        p->defaultBound = VkDescriptorBufferInfo{a.buffer, a.offset, VkDeviceSize(p->defaultBlock.size())};
        p->defaultBoundSerial = currentSerial();
        p->defaultDirty = false;
    }
    ds.ubo[B_DEFAULT_UBO] = p->defaultBound;
    for (size_t i = 0; i < p->blockNames.size() && i < size_t(B_UBO_COUNT); i++) {
        const GLuint bp = p->blockBindings[i];
        const State::UboRange& r = S.ubo[bp < kMaxUboBindings ? bp : 0];
        Buffer* b = g_buffers.get(r.buffer);
        if (!b || !b->buf) continue;
        VkDeviceSize range = r.size > 0 ? VkDeviceSize(r.size) : b->size - VkDeviceSize(r.offset);
        range = std::min<VkDeviceSize>(range, VkDeviceSize(p->blockSizes[i]));
        ds.ubo[B_UBO0 + i] = VkDescriptorBufferInfo{b->buf, VkDeviceSize(r.offset), range};
        ds.mask |= 1u << (B_UBO0 + i);
        b->lastUse = currentSerial();
    }
    for (const UniformInfo& u : p->uniforms) {
        if (u.block != -2) continue;
        for (int e = 0; e < u.arraySize; e++) {
            const int unit = u.units[size_t(e)];
            const GLuint tex = unit >= 0 && unit < kMaxUnits ? S.textures[unit] : 0;
            const GLuint smp = unit >= 0 && unit < kMaxUnits ? S.samplers[unit] : 0;
            const int slot = u.binding == B_SAMPLER_ARRAY ? e : kSamplerArrayMax + (u.binding - B_SAMPLERS);
            ds.imgs[slot] = textureDescriptor(tex, smp);
            if (D.noLodBias && u.binding == B_SAMPLER_ARRAY && e < 8) {
                const SamplerObj* so = g_samplers.get(smp);
                const Texture* t = g_textures.get(tex);
                ds.bias[e] = so ? so->lodBias : t ? t->lodBias : 0.0f;
            }
        }
        if (u.binding == B_SAMPLER_ARRAY) ds.arrayCount = uint32_t(u.arraySize);
        ds.mask |= 1u << u.binding;
    }
}

void pushDescriptors(VkCommandBuffer cb, const DescSet& ds) {
    bool same = s_cc.descValid && (s_cc.descMask & ds.mask) == ds.mask;
    for (uint32_t b = 0; same && b < B_SAMPLER_ARRAY; b++)
        if ((ds.mask & (1u << b)) && memcmp(&ds.ubo[b], &s_cc.ubo[b], sizeof ds.ubo[b]) != 0) same = false;
    if (same && (ds.mask & (1u << B_SAMPLER_ARRAY)) &&
        memcmp(ds.imgs, s_cc.imgs, sizeof(VkDescriptorImageInfo) * ds.arrayCount) != 0)
        same = false;
    for (uint32_t b = B_SAMPLERS; same && b < B_COUNT; b++) {
        const int slot = kSamplerArrayMax + int(b - B_SAMPLERS);
        if ((ds.mask & (1u << b)) && memcmp(&ds.imgs[slot], &s_cc.imgs[slot], sizeof ds.imgs[slot]) != 0) same = false;
    }
    if (same) return;
    VkWriteDescriptorSet w[B_COUNT];
    uint32_t n = 0;
    for (uint32_t b = 0; b < B_COUNT; b++) {
        if (!(ds.mask & (1u << b))) continue;
        w[n] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[n].dstBinding = b;
        if (b < B_SAMPLER_ARRAY) {
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[n].descriptorCount = 1;
            w[n].pBufferInfo = &ds.ubo[b];
            s_cc.ubo[b] = ds.ubo[b];
        } else if (b == B_SAMPLER_ARRAY) {
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[n].descriptorCount = std::max(1u, ds.arrayCount);
            w[n].pImageInfo = ds.imgs;
            memcpy(s_cc.imgs, ds.imgs, sizeof(VkDescriptorImageInfo) * kSamplerArrayMax);
        } else {
            const int slot = kSamplerArrayMax + int(b - B_SAMPLERS);
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[n].descriptorCount = 1;
            w[n].pImageInfo = &ds.imgs[slot];
            s_cc.imgs[slot] = ds.imgs[slot];
        }
        n++;
    }
    D.cmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, D.pipeLayout, 0, n, w);
    s_cc.descValid = true;
    s_cc.descMask = ds.mask;
}

bool bindVertexInput(VkCommandBuffer cb, const VertexLayout* vl) {
    for (uint32_t b = 0; b < vl->narray; b++) {
        Buffer* buf = g_buffers.get(vl->bufs[b]);
        if (!buf || !buf->buf) return false;
        buf->lastUse = currentSerial();
        if (s_cc.vb[b] != buf->buf || s_cc.vbOff[b] != vl->offs[b]) {
            vkCmdBindVertexBuffers(cb, b, 1, &buf->buf, &vl->offs[b]);
            s_cc.vb[b] = buf->buf;
            s_cc.vbOff[b] = vl->offs[b];
        }
    }
    if (vl->hasConst) {
        if (s_cc.constVersion != s_constVersion || !s_cc.constAt.buffer) {
            RingAlloc a = g_dynamic.alloc(16 * kMaxAttribs, 16);
            memcpy(a.ptr, S.attrib, 16 * kMaxAttribs);
            s_cc.constAt = VkDescriptorBufferInfo{a.buffer, a.offset, 16 * kMaxAttribs};
            s_cc.constVersion = s_constVersion;
        }
        const uint32_t b = vl->narray;
        if (s_cc.vb[b] != s_cc.constAt.buffer || s_cc.vbOff[b] != s_cc.constAt.offset) {
            vkCmdBindVertexBuffers(cb, b, 1, &s_cc.constAt.buffer, &s_cc.constAt.offset);
            s_cc.vb[b] = s_cc.constAt.buffer;
            s_cc.vbOff[b] = s_cc.constAt.offset;
        }
    }
    return true;
}

// Everything a draw needs, up to the draw command. False: nothing to draw.
bool prepareDraw(GLenum mode, VkCommandBuffer* outCb) {
    Program* p = g_programs.get(S.program);
    if (!p) return false;
    finishLink(p);
    if (!p->linked) return false;
    DescSet ds;
    gatherDescriptors(p, ds);
    if (!ensureRendering()) return false;
    const VertexLayout* vl = vertexLayout(p);
    VkPipeline pipe = pipelineFor(p, vl, topology(mode));
    if (!pipe) return false;
    VkCommandBuffer cb = mainCmd();
    if (!applyDynamicState(cb)) return false;
    if (s_cc.pipeline != pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        s_cc.pipeline = pipe;
    }
    pushDescriptors(cb, ds);
    if (D.noLodBias && (!s_cc.biasValid || memcmp(ds.bias, s_cc.bias, sizeof ds.bias) != 0)) {
        vkCmdPushConstants(cb, D.pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof ds.bias, ds.bias);
        memcpy(s_cc.bias, ds.bias, sizeof ds.bias);
        s_cc.biasValid = true;
    }
    if (!bindVertexInput(cb, vl)) return false;
    *outCb = cb;
    return true;
}

bool bindIndices(VkCommandBuffer cb, GLenum type) {
    VertexArray* va = g_vertexArrays.get(S.vao);
    Buffer* b = va ? g_buffers.get(va->elementBuffer) : nullptr;
    if (!b || !b->buf) return false;
    b->lastUse = currentSerial();
    const VkIndexType t = type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (s_cc.ib != b->buf || s_cc.ibType != t) {
        vkCmdBindIndexBuffer(cb, b->buf, 0, t);
        s_cc.ib = b->buf;
        s_cc.ibType = t;
    }
    return true;
}

void drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint base) {
    if (count <= 0) return;
    VkCommandBuffer cb;
    if (!prepareDraw(mode, &cb) || !bindIndices(cb, type)) return;
    const uint32_t isz = type == GL_UNSIGNED_SHORT ? 2 : 4;
    vkCmdDrawIndexed(cb, uint32_t(count), 1, uint32_t(reinterpret_cast<uintptr_t>(indices) / isz), base, 0);
    drawRecorded();
}

// ------------------------------------------------------------------ clears
VkPipelineLayout s_clearLayout = VK_NULL_HANDLE;
VkShaderModule s_clearVs = VK_NULL_HANDLE, s_clearFs = VK_NULL_HANDLE;
std::unordered_map<uint64_t, VkPipeline> s_clearPipes;

VkPipeline clearPipeline(VkFormat color, VkFormat depth, VkSampleCountFlagBits samples, uint32_t mask) {
    const uint64_t key = uint64_t(color) | uint64_t(depth) << 20 | uint64_t(samples) << 40 | uint64_t(mask) << 48;
    auto it = s_clearPipes.find(key);
    if (it != s_clearPipes.end()) return it->second;
    if (!s_clearLayout) {
        VkPushConstantRange r{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &r;
        vkCreatePipelineLayout(D.dev, &lci, nullptr, &s_clearLayout);
        auto mod = [](const std::vector<uint32_t>& spv) {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = spv.size() * 4;
            ci.pCode = spv.data();
            VkShaderModule m = VK_NULL_HANDLE;
            if (!spv.empty()) vkCreateShaderModule(D.dev, &ci, nullptr, &m);
            return m;
        };
        s_clearVs = mod(compileGlsl(0,
                                    "#version 450\n"
                                    "void main() { vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
                                    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n"));
        s_clearFs = mod(compileGlsl(2,
                                    "#version 450\n"
                                    "layout(push_constant) uniform PC { vec4 color; } pc;\n"
                                    "layout(location = 0) out vec4 o;\n"
                                    "void main() { o = pc.color; }\n"));
    }
    if (!s_clearVs || !s_clearFs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = s_clearVs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = s_clearFs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = mask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_clearLayout;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(D.dev, D.pipelineCache, 1, &ci, nullptr, &pipe);
    s_clearPipes[key] = pipe;
    return pipe;
}

void clear(GLbitfield mask) {
    RenderTarget rt;
    if (!resolveTarget(S.drawFbo, rt)) return;
    const bool anyColor = S.colorMask[0] || S.colorMask[1] || S.colorMask[2] || S.colorMask[3];
    const bool fullColor = S.colorMask[0] && S.colorMask[1] && S.colorMask[2] && S.colorMask[3];
    const bool doColor = (mask & GL_COLOR_BUFFER_BIT) && rt.color && anyColor;
    const bool doDepth = (mask & GL_DEPTH_BUFFER_BIT) && rt.depth && S.depthMask;
    if (!doColor && !doDepth) return;
    VkRect2D rect{{0, 0}, {rt.w, rt.h}};
    if (S.scissorTest) {
        const int32_t x0 = std::max(0, S.scissor[0]), y0 = std::max(0, S.scissor[1]);
        const int32_t x1 = std::min(int32_t(rt.w), S.scissor[0] + S.scissor[2]);
        const int32_t y1 = std::min(int32_t(rt.h), S.scissor[1] + S.scissor[3]);
        if (x1 <= x0 || y1 <= y0) return;
        rect = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    const bool whole = rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width == rt.w && rect.extent.height == rt.h;
    VkClearValue cc{}, dc{};
    memcpy(cc.color.float32, S.clearColor, sizeof cc.color.float32);
    dc.depthStencil.depth = float(std::max(0.0, std::min(1.0, S.clearDepth)));
    if (whole && (!doColor || fullColor) && !(s_rendering && rt == s_rt)) {
        // a whole clear at the start of rendering: the attachments' load op
        beginRendering(rt, doColor ? &cc : nullptr, doDepth ? &dc : nullptr);
        return;
    }
    if (!s_rendering || !(rt == s_rt)) beginRendering(rt, nullptr, nullptr);
    VkCommandBuffer cb = mainCmd();
    VkClearAttachment atts[2];
    uint32_t n = 0;
    if (doColor && fullColor) atts[n++] = VkClearAttachment{VK_IMAGE_ASPECT_COLOR_BIT, 0, cc};
    if (doDepth) atts[n++] = VkClearAttachment{VK_IMAGE_ASPECT_DEPTH_BIT, 0, dc};
    VkClearRect cr{rect, 0, 1};
    if (n) vkCmdClearAttachments(cb, n, atts, 1, &cr);
    if (doColor && !fullColor) {
        // vkCmdClearAttachments ignores the colour mask (glClear does not):
        // a triangle over the rectangle, outside any occlusion query
        const uint32_t wm = (S.colorMask[0] ? 1u : 0u) | (S.colorMask[1] ? 2u : 0u) | (S.colorMask[2] ? 4u : 0u) |
                            (S.colorMask[3] ? 8u : 0u);
        VkPipeline pipe = clearPipeline(rt.color->format, rt.depth ? rt.depth->format : VK_FORMAT_UNDEFINED, rt.color->samples, wm);
        if (!pipe) return;
        endQueryPiece();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkViewport vp{0, 0, float(rt.w), float(rt.h), 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &rect);
        vkCmdPushConstants(cb, s_clearLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, S.clearColor);
        vkCmdDraw(cb, 3, 1, 0, 0);
        // Binding a pipeline writes its static state over the dynamic state
        // set before (cull, depth, blend constants), and the main layout's
        // pushed state is gone: everything is set again at the next draw.
        s_cc = CmdCache();
        if (S.query) beginQueryPiece();
    }
}

}  // namespace

// ------------------------------------------------------------------ shared with the other files
const VkDynamicState* pipelineDynamicStates(uint32_t* count) {
    static const VkDynamicState kDyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                                          VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_CULL_MODE,
                                          VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                          VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    *count = D.eds ? 8 : 3;
    return kDyn;
}

// A program's pre-rasterisation shaders (vertex, geometry) as a library;
// thread-safe (compile threads make them right after a program links).
VkPipeline makePreRasterLibrary(const VkShaderModule modules[3], bool depthClamp) {
    VkPipelineShaderStageCreateInfo stages[2];
    uint32_t ns = 0;
    const VkShaderStageFlagBits kStage[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    for (int s = 0; s < 2; s++) {
        if (!modules[s]) continue;
        stages[ns] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = kStage[s];
        stages[ns].module = modules[s];
        stages[ns].pName = "main";
        ns++;
    }
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.pNext = D.depthClipControl ? &clip : nullptr;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = depthClamp && D.features.depthClamp;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = ns;
    ci.pStages = stages;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.layout = D.pipeLayout;
    return createLibrary(ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &ri);
}

VkPipeline makeFragmentLibrary(VkShaderModule fs, VkSampleCountFlagBits samples) {
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st.module = fs;
    st.pName = "main";
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 1;
    ci.pStages = &st;
    ci.pDepthStencilState = &ds;
    ci.pMultisampleState = &ms;
    ci.layout = D.pipeLayout;
    return createLibrary(ci, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &ri);
}

void endRendering() {
    if (!s_rendering) return;
    endQueryPiece();
    D.cmdEndRendering(mainCmd());
    s_rendering = false;
}

bool renderingActive() { return s_rendering; }

void onSegmentStart() {
    s_rendering = false;
    s_cc = CmdCache();
    queriesSegmentStart();
}

void beforeSubmit() {
    if (s_rendering) endRendering();
}

void destroyDrawObjects() {
    for (auto* m : {&s_vertexLibs, &s_outputLibs}) {
        for (auto& kv : *m)
            if (kv.second) vkDestroyPipeline(D.dev, kv.second, nullptr);
        m->clear();
    }
    for (auto& kv : s_clearPipes) vkDestroyPipeline(D.dev, kv.second, nullptr);
    s_clearPipes.clear();
    if (s_clearVs) vkDestroyShaderModule(D.dev, s_clearVs, nullptr);
    if (s_clearFs) vkDestroyShaderModule(D.dev, s_clearFs, nullptr);
    if (s_clearLayout) vkDestroyPipelineLayout(D.dev, s_clearLayout, nullptr);
    s_clearVs = s_clearFs = VK_NULL_HANDLE;
    s_clearLayout = VK_NULL_HANDLE;
    for (auto& kv : s_layouts) delete kv.second;
    s_layouts.clear();
}

uint32_t pipelineCreates() { return s_pipelineCreates; }

// SMS_GX_HITCH_MS=n: a frame whose pipelines took over n/4 ms says so (the
// renderer's own hitch line has the frame's total).
void pipelineFrameEnd(uint32_t frame) {
    static double limit = -1;
    if (limit < 0) {
        const char* e = getenv("SMS_GX_HITCH_MS");
        limit = e ? atof(e) / 4000.0 : 0;
    }
    if (limit > 0 && s_framePipelineSeconds > limit)
        logmsg("Vulkan: present %u made %u pipelines in %.1f ms", frame, s_framePipelines, s_framePipelineSeconds * 1000.0);
    s_framePipelines = 0;
    s_framePipelineSeconds = 0;
}

// ------------------------------------------------------------------ GL entry points
namespace {

const GLenum kBinaryFormat = 0x474C564B;  // 'GLVK'

std::vector<std::string> extensions() {
    std::vector<std::string> e = {"GL_ARB_shader_image_load_store", "GL_ARB_buffer_storage", "GL_ARB_sampler_objects",
                                  "GL_KHR_parallel_shader_compile"};
    if (D.features.textureCompressionBC) {
        e.push_back("GL_EXT_texture_compression_s3tc");
        e.push_back("GL_ARB_texture_compression_bptc");
    }
    if (D.features.samplerAnisotropy) e.push_back("GL_ARB_texture_filter_anisotropic");
    return e;
}

#define GLAPI_FN(ret, name) ret APIENTRY name

GLAPI_FN(GLenum, x_glGetError)() { return GL_NO_ERROR; }

GLAPI_FN(const GLubyte*, x_glGetString)(GLenum name) {
    static std::string vendor, renderer, version;
    switch (name) {
    case GL_VENDOR:
        vendor = "glvk";
        return reinterpret_cast<const GLubyte*>(vendor.c_str());
    case GL_RENDERER: {
        renderer = std::string(D.name) + " (Vulkan)";
        return reinterpret_cast<const GLubyte*>(renderer.c_str());
    }
    case GL_VERSION: {
        char buf[160];
        snprintf(buf, sizeof buf, "3.3 glvk 1 (Vulkan %u.%u.%u, driver %08x)", VK_VERSION_MAJOR(D.apiVersion),
                 VK_VERSION_MINOR(D.apiVersion), VK_VERSION_PATCH(D.props.apiVersion), D.props.driverVersion);
        version = buf;
        return reinterpret_cast<const GLubyte*>(version.c_str());
    }
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>("3.30");
    default: return reinterpret_cast<const GLubyte*>("");
    }
}

GLAPI_FN(const GLubyte*, x_glGetStringi)(GLenum name, GLuint i) {
    static std::vector<std::string> ext;
    if (ext.empty()) ext = extensions();
    if (name != GL_EXTENSIONS || i >= ext.size()) return nullptr;
    return reinterpret_cast<const GLubyte*>(ext[i].c_str());
}

bool* capFlag(GLenum cap) {
    switch (cap) {
    case GL_BLEND: return &S.blend;
    case GL_DEPTH_TEST: return &S.depthTest;
    case GL_CULL_FACE: return &S.cullFace;
    case GL_SCISSOR_TEST: return &S.scissorTest;
    case GL_COLOR_LOGIC_OP: return &S.logicOp;
    case GL_CLIP_DISTANCE0: return &S.clip[0];
    case GL_CLIP_DISTANCE1: return &S.clip[1];
    case GL_DEPTH_CLAMP: return &S.depthClamp;
    case GL_STENCIL_TEST: return &S.stencilTest;
    default: return nullptr;
    }
}

GLAPI_FN(void, x_glGetIntegerv)(GLenum pname, GLint* v) {
    if (bool* flag = capFlag(pname)) {
        *v = *flag ? 1 : 0;
        return;
    }
    switch (pname) {
    case GL_NUM_EXTENSIONS: *v = GLint(extensions().size()); break;
    case GL_MAJOR_VERSION: *v = 3; break;
    case GL_MINOR_VERSION: *v = 3; break;
    case GL_MAX_SAMPLES: {
        const VkSampleCountFlags c = D.props.limits.framebufferColorSampleCounts & D.props.limits.framebufferDepthSampleCounts;
        *v = 1;
        for (int n = 64; n > 1; n >>= 1)
            if (c & VkSampleCountFlags(n)) {
                *v = n;
                break;
            }
        break;
    }
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *v = GLint(std::max<VkDeviceSize>(D.props.limits.minUniformBufferOffsetAlignment, 16)); break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = GLint(S.packBuffer); break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *v = GLint(S.unpackBuffer); break;
    case GL_ARRAY_BUFFER_BINDING: *v = GLint(S.arrayBuffer); break;
    case GL_UNIFORM_BUFFER_BINDING: *v = GLint(S.uniformBuffer); break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
        VertexArray* va = g_vertexArrays.get(S.vao);
        *v = va ? GLint(va->elementBuffer) : 0;
        break;
    }
    case 0x84FF: *v = GLint(D.maxAnisotropy); break;  // GL_MAX_TEXTURE_MAX_ANISOTROPY
    case GL_NUM_PROGRAM_BINARY_FORMATS: *v = 1; break;
    case GL_PROGRAM_BINARY_FORMATS: *v = GLint(kBinaryFormat); break;
    case GL_CURRENT_PROGRAM: *v = GLint(S.program); break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = GLint(S.drawFbo); break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = GLint(S.readFbo); break;
    case GL_RENDERBUFFER_BINDING: *v = GLint(S.renderbuffer); break;
    case GL_VIEWPORT: memcpy(v, S.viewport, sizeof S.viewport); break;
    case GL_SCISSOR_BOX: memcpy(v, S.scissor, sizeof S.scissor); break;
    case GL_ACTIVE_TEXTURE: *v = GLint(GL_TEXTURE0 + S.activeUnit); break;
    case GL_TEXTURE_BINDING_2D: *v = S.activeUnit < kMaxUnits ? GLint(S.textures[S.activeUnit]) : 0; break;
    case GL_SAMPLER_BINDING: *v = S.activeUnit < kMaxUnits ? GLint(S.samplers[S.activeUnit]) : 0; break;
    case GL_UNPACK_ROW_LENGTH: *v = S.unpackRowLength; break;
    case GL_UNPACK_ALIGNMENT: *v = S.unpackAlign; break;
    case GL_PACK_ALIGNMENT: *v = S.packAlign; break;
    case GL_PACK_ROW_LENGTH: *v = S.packRowLength; break;
    case GL_VERTEX_ARRAY_BINDING: *v = GLint(S.vao); break;
    case GL_COLOR_WRITEMASK:
        for (int i = 0; i < 4; i++) v[i] = S.colorMask[i] ? 1 : 0;
        break;
    case GL_DEPTH_WRITEMASK: *v = S.depthMask ? 1 : 0; break;
    case GL_DEPTH_FUNC: *v = GLint(S.depthFunc); break;
    case GL_BLEND_SRC_RGB: *v = GLint(S.blendSrcRGB); break;
    case GL_BLEND_DST_RGB: *v = GLint(S.blendDstRGB); break;
    case GL_BLEND_SRC_ALPHA: *v = GLint(S.blendSrcA); break;
    case GL_BLEND_DST_ALPHA: *v = GLint(S.blendDstA); break;
    case GL_BLEND_EQUATION_RGB: *v = GLint(S.blendEqRGB); break;
    case GL_BLEND_EQUATION_ALPHA: *v = GLint(S.blendEqA); break;
    case GL_CULL_FACE_MODE: *v = GLint(S.cullMode); break;
    case GL_FRONT_FACE: *v = GLint(S.frontFace); break;
    case GL_MAX_TEXTURE_SIZE: *v = GLint(D.props.limits.maxImageDimension2D); break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = kMaxUnits; break;
    case GL_MAX_VERTEX_ATTRIBS: *v = kMaxAttribs; break;
    case GL_MAX_UNIFORM_BUFFER_BINDINGS: *v = kMaxUboBindings; break;
    case GL_MAX_UNIFORM_BLOCK_SIZE: *v = GLint(std::min<uint32_t>(D.props.limits.maxUniformBufferRange, 65536)); break;
    case GL_MAX_VIEWPORT_DIMS:
        v[0] = GLint(D.props.limits.maxViewportDimensions[0]);
        v[1] = GLint(D.props.limits.maxViewportDimensions[1]);
        break;
    default: *v = 0; break;
    }
}

void setCap(GLenum cap, bool on) {
    if (bool* flag = capFlag(cap)) *flag = on;
}
GLAPI_FN(void, x_glEnable)(GLenum cap) { setCap(cap, true); }
GLAPI_FN(void, x_glDisable)(GLenum cap) { setCap(cap, false); }
GLAPI_FN(void, x_glViewport)(GLint x, GLint y, GLsizei w, GLsizei h) {
    S.viewport[0] = x;
    S.viewport[1] = y;
    S.viewport[2] = w;
    S.viewport[3] = h;
}
GLAPI_FN(void, x_glScissor)(GLint x, GLint y, GLsizei w, GLsizei h) {
    S.scissor[0] = x;
    S.scissor[1] = y;
    S.scissor[2] = w;
    S.scissor[3] = h;
}
GLAPI_FN(void, x_glBlendFuncSeparate)(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    S.blendSrcRGB = sr;
    S.blendDstRGB = dr;
    S.blendSrcA = sa;
    S.blendDstA = da;
}
GLAPI_FN(void, x_glBlendEquationSeparate)(GLenum rgb, GLenum a) {
    S.blendEqRGB = rgb;
    S.blendEqA = a;
}
GLAPI_FN(void, x_glBlendColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    S.blendColor[0] = r;
    S.blendColor[1] = g;
    S.blendColor[2] = b;
    S.blendColor[3] = a;
}
GLAPI_FN(void, x_glLogicOp)(GLenum op) { S.logicOpMode = op; }
GLAPI_FN(void, x_glDepthFunc)(GLenum f) { S.depthFunc = f; }
GLAPI_FN(void, x_glDepthMask)(GLboolean m) { S.depthMask = m != GL_FALSE; }
GLAPI_FN(void, x_glColorMask)(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    S.colorMask[0] = r != GL_FALSE;
    S.colorMask[1] = g != GL_FALSE;
    S.colorMask[2] = b != GL_FALSE;
    S.colorMask[3] = a != GL_FALSE;
}
GLAPI_FN(void, x_glCullFace)(GLenum m) { S.cullMode = m; }
GLAPI_FN(void, x_glFrontFace)(GLenum m) { S.frontFace = m; }
GLAPI_FN(void, x_glClearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    S.clearColor[0] = r;
    S.clearColor[1] = g;
    S.clearColor[2] = b;
    S.clearColor[3] = a;
}
GLAPI_FN(void, x_glClearDepth)(GLdouble d) { S.clearDepth = d; }
GLAPI_FN(void, x_glClear)(GLbitfield mask) { clear(mask); }
GLAPI_FN(void, x_glDepthRange)(GLdouble n, GLdouble f) {
    S.depthRange[0] = n;
    S.depthRange[1] = f;
}
GLAPI_FN(void, x_glLineWidth)(GLfloat w) { S.lineWidth = w; }
GLAPI_FN(void, x_glPointSize)(GLfloat s) { S.pointSize = s; }
GLAPI_FN(void, x_glFinish)() { waitSerial(currentSerial()); }

// buffers
GLAPI_FN(void, x_glGenBuffers)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_buffers.gen();
}
GLAPI_FN(void, x_glDeleteBuffers)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        Buffer* b = g_buffers.get(names[i]);
        if (!b) continue;
        destroyBuffer(b);
        if (S.arrayBuffer == names[i]) S.arrayBuffer = 0;
        if (S.packBuffer == names[i]) S.packBuffer = 0;
        if (S.uniformBuffer == names[i]) S.uniformBuffer = 0;
        g_buffers.remove(names[i]);
    }
}
GLAPI_FN(void, x_glBindBuffer)(GLenum target, GLuint b) {
    switch (target) {
    case GL_ARRAY_BUFFER: S.arrayBuffer = b; break;
    case GL_ELEMENT_ARRAY_BUFFER: {
        VertexArray* va = g_vertexArrays.get(S.vao);
        if (va && va->elementBuffer != b) {
            va->elementBuffer = b;
            va->version++;
        }
        break;
    }
    case GL_PIXEL_PACK_BUFFER: S.packBuffer = b; break;
    case GL_PIXEL_UNPACK_BUFFER: S.unpackBuffer = b; break;
    case GL_UNIFORM_BUFFER: S.uniformBuffer = b; break;
    default: break;
    }
}
GLAPI_FN(void, x_glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage) { bufferData(target, size, data, usage); }
GLAPI_FN(void, x_glBufferStorage)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    bufferStorage(target, size, data, flags);
}
GLAPI_FN(void, x_glBufferSubData)(GLenum target, GLintptr off, GLsizeiptr size, const void* data) {
    bufferSubData(target, off, size, data);
}
GLAPI_FN(void, x_glBindBufferBase)(GLenum target, GLuint index, GLuint b) {
    if (target != GL_UNIFORM_BUFFER || index >= kMaxUboBindings) return;
    S.ubo[index] = State::UboRange{b, 0, 0};
    S.uniformBuffer = b;
}
GLAPI_FN(void, x_glBindBufferRange)(GLenum target, GLuint index, GLuint b, GLintptr off, GLsizeiptr size) {
    if (target != GL_UNIFORM_BUFFER || index >= kMaxUboBindings) return;
    S.ubo[index] = State::UboRange{b, off, size};
    S.uniformBuffer = b;
}
GLAPI_FN(void*, x_glMapBufferRange)(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    return mapBufferRange(target, off, len, access);
}
GLAPI_FN(GLboolean, x_glUnmapBuffer)(GLenum target) { return unmapBuffer(target); }

// vertex arrays
GLAPI_FN(void, x_glGenVertexArrays)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_vertexArrays.gen();
}
GLAPI_FN(void, x_glBindVertexArray)(GLuint v) { S.vao = v; }
GLAPI_FN(void, x_glDeleteVertexArrays)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        if (S.vao == names[i]) S.vao = 0;
        g_vertexArrays.remove(names[i]);
    }
}
void attribPointer(GLuint i, GLint size, GLenum type, bool norm, bool integer, GLsizei stride, const void* ptr) {
    VertexArray* va = g_vertexArrays.get(S.vao);
    if (!va || i >= kMaxAttribs) return;
    VertexAttrib& a = va->attribs[i];
    a.size = size;
    a.type = type;
    a.normalized = norm;
    a.integer = integer;
    a.stride = stride;
    a.offset = reinterpret_cast<size_t>(ptr);
    a.buffer = S.arrayBuffer;
    va->version++;
}
GLAPI_FN(void, x_glVertexAttribPointer)(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
    attribPointer(i, size, type, norm != GL_FALSE, false, stride, ptr);
}
GLAPI_FN(void, x_glVertexAttribIPointer)(GLuint i, GLint size, GLenum type, GLsizei stride, const void* ptr) {
    attribPointer(i, size, type, false, true, stride, ptr);
}
void attribArray(GLuint i, bool on) {
    VertexArray* va = g_vertexArrays.get(S.vao);
    if (!va || i >= kMaxAttribs || va->attribs[i].enabled == on) return;
    va->attribs[i].enabled = on;
    va->version++;
}
GLAPI_FN(void, x_glEnableVertexAttribArray)(GLuint i) { attribArray(i, true); }
GLAPI_FN(void, x_glDisableVertexAttribArray)(GLuint i) { attribArray(i, false); }
GLAPI_FN(void, x_glVertexAttrib4f)(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (i >= kMaxAttribs) return;
    const float v[4] = {x, y, z, w};
    memcpy(S.attrib[i], v, sizeof v);
    S.attribInteger &= ~(1u << i);
    s_constVersion++;
}
GLAPI_FN(void, x_glVertexAttribI4ui)(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w) {
    if (i >= kMaxAttribs) return;
    const GLuint v[4] = {x, y, z, w};
    memcpy(S.attrib[i], v, sizeof v);
    S.attribInteger |= 1u << i;
    s_constVersion++;
}

// draws
GLAPI_FN(void, x_glDrawArrays)(GLenum mode, GLint first, GLsizei count) {
    if (count <= 0) return;
    VkCommandBuffer cb;
    if (!prepareDraw(mode, &cb)) return;
    vkCmdDraw(cb, uint32_t(count), 1, uint32_t(first), 0);
    drawRecorded();
}
GLAPI_FN(void, x_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    drawElements(mode, count, type, indices, 0);
}
GLAPI_FN(void, x_glDrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint base) {
    drawElements(mode, count, type, indices, base);
}
GLAPI_FN(void, x_glMultiDrawElementsBaseVertex)(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                               GLsizei drawcount, const GLint* base) {
    if (drawcount <= 0) return;
    VkCommandBuffer cb;
    if (!prepareDraw(mode, &cb) || !bindIndices(cb, type)) return;
    const uint32_t isz = type == GL_UNSIGNED_SHORT ? 2 : 4;
    for (GLsizei i = 0; i < drawcount; i++)
        if (count[i] > 0)
            vkCmdDrawIndexed(cb, uint32_t(count[i]), 1, uint32_t(reinterpret_cast<uintptr_t>(indices[i]) / isz), base[i], 0);
    drawRecorded();
}

// shaders and programs
GLAPI_FN(GLuint, x_glCreateShader)(GLenum type) {
    GLuint n = g_shaders.gen();
    g_shaders.get(n)->type = type;
    return n;
}
GLAPI_FN(void, x_glShaderSource)(GLuint s, GLsizei count, const GLchar* const* str, const GLint* len) {
    Shader* sh = g_shaders.get(s);
    if (!sh) return;
    sh->source.clear();
    for (GLsizei i = 0; i < count; i++) {
        if (!str[i]) continue;
        if (len && len[i] >= 0) sh->source.append(str[i], size_t(len[i]));
        else sh->source.append(str[i]);
    }
}
GLAPI_FN(void, x_glCompileShader)(GLuint) {}  // compiled with the program it is linked into
GLAPI_FN(void, x_glGetShaderiv)(GLuint, GLenum pname, GLint* v) {
    *v = pname == GL_COMPILE_STATUS ? GL_TRUE : pname == GL_INFO_LOG_LENGTH ? 1 : 0;
}
GLAPI_FN(void, x_glGetShaderInfoLog)(GLuint, GLsizei size, GLsizei* len, GLchar* log) {
    if (len) *len = 0;
    if (log && size > 0) log[0] = 0;
}
GLAPI_FN(void, x_glDeleteShader)(GLuint s) { g_shaders.remove(s); }
GLAPI_FN(GLuint, x_glCreateProgram)() { return g_programs.gen(); }
GLAPI_FN(void, x_glAttachShader)(GLuint p, GLuint s) {
    if (Program* pr = g_programs.get(p)) pr->shaders.push_back(s);
}
GLAPI_FN(void, x_glLinkProgram)(GLuint p) {
    Program* pr = g_programs.get(p);
    if (!pr) return;
    if (!linkProgram(pr)) logmsg("glvk: program link failed: %s", pr->log.c_str());
}
GLAPI_FN(void, x_glGetProgramiv)(GLuint p, GLenum pname, GLint* v) {
    Program* pr = g_programs.get(p);
    *v = 0;
    if (!pr) return;
    if (pname == 0x91B1) {  // GL_COMPLETION_STATUS_KHR: without waiting
        *v = linkDone(pr) ? GL_TRUE : GL_FALSE;
        return;
    }
    finishLink(pr);
    switch (pname) {
    case GL_LINK_STATUS: *v = pr->linked ? GL_TRUE : GL_FALSE; break;
    case GL_INFO_LOG_LENGTH: *v = GLint(pr->log.size() + 1); break;
    case GL_PROGRAM_BINARY_LENGTH: {
        std::vector<uint8_t> bin;
        *v = programBinary(pr, bin) ? GLint(bin.size()) : 0;
        break;
    }
    default: break;
    }
}
GLAPI_FN(void, x_glGetProgramInfoLog)(GLuint p, GLsizei size, GLsizei* len, GLchar* log) {
    Program* pr = g_programs.get(p);
    if (pr) finishLink(pr);
    const std::string s = pr ? pr->log : std::string();
    const GLsizei n = size > 0 ? GLsizei(std::min(s.size(), size_t(size - 1))) : 0;
    if (log && size > 0) {
        memcpy(log, s.data(), size_t(n));
        log[n] = 0;
    }
    if (len) *len = n;
}
GLAPI_FN(void, x_glUseProgram)(GLuint p) { S.program = p; }
GLAPI_FN(void, x_glDeleteProgram)(GLuint p) {
    Program* pr = g_programs.get(p);
    if (!pr) return;
    finishLink(pr);
    destroyProgramObjects(pr);
    if (S.program == p) S.program = 0;
    g_programs.remove(p);
}
GLAPI_FN(void, x_glBindAttribLocation)(GLuint p, GLuint i, const GLchar* name) {
    if (Program* pr = g_programs.get(p)) pr->attribLocations[name] = int(i);
}
GLAPI_FN(void, x_glBindFragDataLocation)(GLuint, GLuint, const GLchar*) {}  // one output, at location 0
GLAPI_FN(GLint, x_glGetUniformLocation)(GLuint p, const GLchar* name) {
    Program* pr = g_programs.get(p);
    if (!pr || !name) return -1;
    finishLink(pr);
    std::string n = name;
    int elem = 0;
    size_t b = n.find('[');
    if (b != std::string::npos) {
        elem = atoi(n.c_str() + b + 1);
        n = n.substr(0, b);
    }
    for (size_t i = 0; i < pr->uniforms.size(); i++)
        if (pr->uniforms[i].name == n && elem >= 0 && elem < pr->uniforms[i].arraySize) return GLint(i << 8) | elem;
    return -1;
}
GLAPI_FN(GLuint, x_glGetUniformBlockIndex)(GLuint p, const GLchar* name) {
    Program* pr = g_programs.get(p);
    if (!pr) return GL_INVALID_INDEX;
    finishLink(pr);
    for (size_t i = 0; i < pr->blockNames.size(); i++)
        if (pr->blockNames[i] == name) return GLuint(i);
    return GL_INVALID_INDEX;
}
GLAPI_FN(void, x_glUniformBlockBinding)(GLuint p, GLuint idx, GLuint binding) {
    Program* pr = g_programs.get(p);
    if (pr) finishLink(pr);
    if (pr && idx < GLuint(B_UBO_COUNT)) pr->blockBindings[idx] = binding;
}
Program* current() { return g_programs.get(S.program); }
GLAPI_FN(void, x_glUniform1i)(GLint loc, GLint v) { programUniform(current(), loc, 1, 1, &v, true); }
GLAPI_FN(void, x_glUniform1iv)(GLint loc, GLsizei n, const GLint* v) { programUniform(current(), loc, 1, n, v, true); }
GLAPI_FN(void, x_glUniform2iv)(GLint loc, GLsizei n, const GLint* v) { programUniform(current(), loc, 2, n, v, true); }
GLAPI_FN(void, x_glUniform4iv)(GLint loc, GLsizei n, const GLint* v) { programUniform(current(), loc, 4, n, v, true); }
GLAPI_FN(void, x_glUniform1f)(GLint loc, GLfloat v) { programUniform(current(), loc, 1, 1, &v, false); }
GLAPI_FN(void, x_glUniform2f)(GLint loc, GLfloat x, GLfloat y) {
    const float v[2] = {x, y};
    programUniform(current(), loc, 2, 1, v, false);
}
GLAPI_FN(void, x_glUniform1fv)(GLint loc, GLsizei n, const GLfloat* v) { programUniform(current(), loc, 1, n, v, false); }
GLAPI_FN(void, x_glUniform2fv)(GLint loc, GLsizei n, const GLfloat* v) { programUniform(current(), loc, 2, n, v, false); }
GLAPI_FN(void, x_glUniform4fv)(GLint loc, GLsizei n, const GLfloat* v) { programUniform(current(), loc, 4, n, v, false); }
GLAPI_FN(void, x_glGetProgramBinary)(GLuint p, GLsizei size, GLsizei* len, GLenum* format, void* out) {
    Program* pr = g_programs.get(p);
    std::vector<uint8_t> bin;
    if (len) *len = 0;
    if (!pr || !programBinary(pr, bin) || GLsizei(bin.size()) > size) return;
    memcpy(out, bin.data(), bin.size());
    if (len) *len = GLsizei(bin.size());
    if (format) *format = kBinaryFormat;
}
GLAPI_FN(void, x_glProgramBinary)(GLuint p, GLenum format, const void* data, GLsizei len) {
    Program* pr = g_programs.get(p);
    if (!pr) return;
    if (format != kBinaryFormat || !loadProgramBinary(pr, data, size_t(len))) pr->linked = false;
}
GLAPI_FN(void, x_glProgramParameteri)(GLuint, GLenum, GLint) {}
GLAPI_FN(void, x_glMaxShaderCompilerThreadsKHR)(GLuint n) { setCompileThreads(n); }

// textures
GLAPI_FN(void, x_glGenTextures)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_textures.gen();
}
GLAPI_FN(void, x_glDeleteTextures)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) deleteTexture(names[i]);
}
GLAPI_FN(void, x_glBindTexture)(GLenum, GLuint t) {
    if (S.activeUnit < kMaxUnits) S.textures[S.activeUnit] = t;
}
GLAPI_FN(void, x_glActiveTexture)(GLenum unit) { S.activeUnit = unit - GL_TEXTURE0; }
GLAPI_FN(void, x_glTexImage2D)(GLenum, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type,
                               const void* pixels) {
    texImage2D(level, ifmt, w, h, format, type, pixels, false, 0);
}
GLAPI_FN(void, x_glTexSubImage2D)(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum,
                                  const void* pixels) {
    texSubImage2D(level, x, y, w, h, format, pixels, false, 0);
}
GLAPI_FN(void, x_glCompressedTexImage2D)(GLenum, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint, GLsizei bytes,
                                         const void* data) {
    texImage2D(level, GLint(ifmt), w, h, 0, 0, data, true, bytes);
}
GLAPI_FN(void, x_glCompressedTexSubImage2D)(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLsizei bytes,
                                            const void* data) {
    texSubImage2D(level, x, y, w, h, 0, data, true, bytes);
}
GLAPI_FN(void, x_glTexParameteri)(GLenum, GLenum pname, GLint v) { texParameter(pname, v, 0, false); }
GLAPI_FN(void, x_glTexParameterf)(GLenum, GLenum pname, GLfloat v) { texParameter(pname, 0, v, true); }
GLAPI_FN(void, x_glGenerateMipmap)(GLenum) { generateMipmap(); }

// samplers
GLAPI_FN(void, x_glGenSamplers)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_samplers.gen();
}
GLAPI_FN(void, x_glDeleteSamplers)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        for (GLuint& u : S.samplers)
            if (u == names[i]) u = 0;
        g_samplers.remove(names[i]);
    }
}
GLAPI_FN(void, x_glBindSampler)(GLuint unit, GLuint s) {
    if (unit < kMaxUnits) S.samplers[unit] = s;
}
GLAPI_FN(void, x_glSamplerParameteri)(GLuint s, GLenum pname, GLint v) { samplerParameter(s, pname, v, 0, false); }
GLAPI_FN(void, x_glSamplerParameterf)(GLuint s, GLenum pname, GLfloat v) { samplerParameter(s, pname, 0, v, true); }

// framebuffers and renderbuffers
GLAPI_FN(void, x_glGenFramebuffers)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_framebuffers.gen();
}
GLAPI_FN(void, x_glDeleteFramebuffers)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        if (S.drawFbo == names[i]) S.drawFbo = 0;
        if (S.readFbo == names[i]) S.readFbo = 0;
        g_framebuffers.remove(names[i]);
    }
}
GLAPI_FN(void, x_glBindFramebuffer)(GLenum target, GLuint f) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) S.drawFbo = f;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) S.readFbo = f;
}
Framebuffer* targetFbo(GLenum target) { return g_framebuffers.get(target == GL_READ_FRAMEBUFFER ? S.readFbo : S.drawFbo); }
void attach(GLenum target, GLenum attachment, GLenum kind, GLuint name, GLint level) {
    Framebuffer* f = targetFbo(target);
    if (!f) return;
    Attachment a{name ? kind : 0, name, level};
    if (attachment == GL_COLOR_ATTACHMENT0) f->color = a;
    else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = a;
    f->version++;
}
GLAPI_FN(void, x_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum, GLuint tex, GLint level) {
    attach(target, attachment, GL_TEXTURE, tex, level);
}
GLAPI_FN(void, x_glFramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum, GLuint rb) {
    attach(target, attachment, GL_RENDERBUFFER, rb, 0);
}
GLAPI_FN(GLenum, x_glCheckFramebufferStatus)(GLenum target) {
    return checkFramebuffer(target == GL_READ_FRAMEBUFFER ? S.readFbo : S.drawFbo);
}
GLAPI_FN(void, x_glBlitFramebuffer)(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                    GLbitfield mask, GLenum filter) {
    blitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
GLAPI_FN(void, x_glReadPixels)(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    readPixels(x, y, w, h, format, type, pixels);
}
GLAPI_FN(void, x_glPixelStorei)(GLenum pname, GLint v) {
    switch (pname) {
    case GL_PACK_ALIGNMENT: S.packAlign = v; break;
    case GL_UNPACK_ALIGNMENT: S.unpackAlign = v; break;
    case GL_UNPACK_ROW_LENGTH: S.unpackRowLength = v; break;
    case GL_PACK_ROW_LENGTH: S.packRowLength = v; break;
    default: break;
    }
}
GLAPI_FN(void, x_glGenRenderbuffers)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_renderbuffers.gen();
}
GLAPI_FN(void, x_glBindRenderbuffer)(GLenum, GLuint r) { S.renderbuffer = r; }
GLAPI_FN(void, x_glRenderbufferStorage)(GLenum, GLenum ifmt, GLsizei w, GLsizei h) { renderbufferStorage(0, ifmt, w, h); }
GLAPI_FN(void, x_glRenderbufferStorageMultisample)(GLenum, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    renderbufferStorage(samples, ifmt, w, h);
}

// queries and sync objects
GLAPI_FN(void, x_glGenQueries)(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g_queries.gen();
}
GLAPI_FN(void, x_glDeleteQueries)(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) deleteQuery(names[i]);
}
GLAPI_FN(void, x_glBeginQuery)(GLenum, GLuint id) { beginQuery(id); }
GLAPI_FN(void, x_glEndQuery)(GLenum) { endQuery(); }
GLAPI_FN(void, x_glGetQueryObjectuiv)(GLuint id, GLenum pname, GLuint* v) {
    *v = queryResult(id, pname == GL_QUERY_RESULT_AVAILABLE);
}
GLAPI_FN(GLsync, x_glFenceSync)(GLenum, GLbitfield) { return fenceSync(); }
GLAPI_FN(GLenum, x_glClientWaitSync)(GLsync s, GLbitfield flags, GLuint64 timeout) { return clientWaitSync(s, flags, timeout); }
GLAPI_FN(void, x_glDeleteSync)(GLsync s) { deleteSync(s); }

struct ProcEntry {
    const char* name;
    void* fn;
};
#define P(n) {#n, reinterpret_cast<void*>(&x_##n)}
const ProcEntry kProcs[] = {
    P(glGetError), P(glGetString), P(glGetStringi), P(glGetIntegerv), P(glEnable), P(glDisable), P(glViewport), P(glScissor),
    P(glBlendFuncSeparate), P(glBlendEquationSeparate), P(glBlendColor), P(glLogicOp), P(glDepthFunc), P(glDepthMask),
    P(glColorMask), P(glCullFace), P(glFrontFace), P(glClearColor), P(glClearDepth), P(glClear), P(glDepthRange),
    P(glLineWidth), P(glPointSize), P(glFinish), P(glGenBuffers), P(glDeleteBuffers), P(glBindBuffer), P(glBufferData),
    P(glBufferStorage), P(glBufferSubData), P(glBindBufferBase), P(glBindBufferRange), P(glMapBufferRange), P(glUnmapBuffer),
    P(glGenVertexArrays), P(glBindVertexArray), P(glDeleteVertexArrays), P(glVertexAttribPointer), P(glVertexAttribIPointer),
    P(glEnableVertexAttribArray), P(glDisableVertexAttribArray), P(glVertexAttrib4f), P(glVertexAttribI4ui), P(glDrawArrays),
    P(glDrawElements), P(glDrawElementsBaseVertex), P(glMultiDrawElementsBaseVertex), P(glCreateShader), P(glShaderSource),
    P(glCompileShader), P(glGetShaderiv), P(glGetShaderInfoLog), P(glDeleteShader), P(glCreateProgram), P(glAttachShader),
    P(glLinkProgram), P(glGetProgramiv), P(glGetProgramInfoLog), P(glUseProgram), P(glDeleteProgram), P(glBindAttribLocation),
    P(glBindFragDataLocation), P(glGetUniformLocation), P(glGetUniformBlockIndex), P(glUniformBlockBinding), P(glUniform1i),
    P(glUniform1iv), P(glUniform2iv), P(glUniform4iv), P(glUniform1f), P(glUniform2f), P(glUniform1fv), P(glUniform2fv),
    P(glUniform4fv), P(glGetProgramBinary), P(glProgramBinary), P(glProgramParameteri), P(glMaxShaderCompilerThreadsKHR),
    P(glGenTextures), P(glDeleteTextures),
    P(glBindTexture), P(glActiveTexture), P(glTexImage2D), P(glTexSubImage2D), P(glCompressedTexImage2D),
    P(glCompressedTexSubImage2D), P(glTexParameteri), P(glTexParameterf), P(glGenerateMipmap), P(glGenSamplers),
    P(glDeleteSamplers), P(glBindSampler), P(glSamplerParameteri), P(glSamplerParameterf), P(glGenFramebuffers),
    P(glDeleteFramebuffers), P(glBindFramebuffer), P(glFramebufferTexture2D), P(glFramebufferRenderbuffer),
    P(glCheckFramebufferStatus), P(glBlitFramebuffer), P(glReadPixels), P(glPixelStorei), P(glGenRenderbuffers),
    P(glBindRenderbuffer), P(glRenderbufferStorage), P(glRenderbufferStorageMultisample), P(glGenQueries), P(glDeleteQueries),
    P(glBeginQuery), P(glEndQuery), P(glGetQueryObjectuiv), P(glFenceSync), P(glClientWaitSync), P(glDeleteSync),
};
#undef P

}  // namespace

void* getProc(const char* name) {
    for (const ProcEntry& e : kProcs)
        if (!strcmp(e.name, name)) return e.fn;
    return nullptr;
}

}  // namespace glvk
