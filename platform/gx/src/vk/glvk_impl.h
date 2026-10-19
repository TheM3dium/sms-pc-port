// glvk internals: the Vulkan device, the GL objects and the GL state.
//
// GL objects keep GL's semantics on top of Vulkan's:
//  - Commands go into one command buffer at a time (a segment), submitted at
//    present and wherever the GL caller needs a result (a synchronous read, a
//    fence wait, a query result). Each submission signals the next value
//    (serial) of one timeline semaphore; anything a segment used is kept
//    until its serial has passed.
//  - Images keep their layout and last access, and every use goes through
//    use(), which adds the barrier GL's implicit ordering stands for.
//  - Texture uploads go into the segment's upload command buffer (submitted
//    just before its main one) when the image is not used yet in the
//    segment, so they need not break a render pass; otherwise in order.
//  - Rendering (dynamic rendering) begins at the first draw or clear into a
//    framebuffer and ends at anything that cannot happen inside it.
#ifndef SMS_GX_GLVK_IMPL_H
#define SMS_GX_GLVK_IMPL_H

#include "volk.h"
#include "vk_mem_alloc.h"
#include <GL/glcorearb.h>

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gx {
void logmsg(const char* fmt, ...);
}

namespace glvk {

using gx::logmsg;

enum { kMaxUnits = 16, kMaxAttribs = 16, kMaxUboBindings = 8 };

// The descriptor set every program uses (push descriptors), whatever its
// GLSL declared: glslang's bindings are patched to these in the SPIR-V.
enum {
    B_DEFAULT_UBO = 0,  // the loose uniforms (gl_DefaultUniformBlock)
    B_UBO0 = 1,         // named uniform blocks, in order of appearance
    B_UBO_COUNT = 2,
    B_SAMPLER_ARRAY = 3,  // one sampler array, up to 8 elements
    B_SAMPLERS = 4,       // scalar samplers, in order of appearance
    B_SAMPLER_COUNT = 8,
    B_COUNT = B_SAMPLERS + B_SAMPLER_COUNT,
    kSamplerArrayMax = 8,
};

// ------------------------------------------------------------------ device
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator vma = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceFeatures features{};  // the ones enabled
    uint32_t apiVersion = 0;
    char name[256] = {};

    // optional features in use
    bool eds = false;              // extended dynamic state: cull, front face, depth test/write/compare, topology
    bool depthClipControl = false; // GL's [-1, 1] clip depth without a shader fix-up
    bool portability = false;      // MoltenVK (VK_KHR_portability_subset)
    bool noLodBias = false;        // samplers cannot bias the LOD (Metal): the shaders do (texture's bias argument)
    bool gpl = false;              // graphics pipeline libraries with fast linking: shaders built once per program
    bool displayTiming = false;    // VK_GOOGLE_display_timing: which presents the compositor has shown (MoltenVK)
    bool hostQueryReset = true;
    PFN_vkCmdBeginRendering cmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
    PFN_vkCmdSetCullMode cmdSetCullMode = nullptr;
    PFN_vkCmdSetFrontFace cmdSetFrontFace = nullptr;
    PFN_vkCmdSetDepthTestEnable cmdSetDepthTestEnable = nullptr;
    PFN_vkCmdSetDepthWriteEnable cmdSetDepthWriteEnable = nullptr;
    PFN_vkCmdSetDepthCompareOp cmdSetDepthCompareOp = nullptr;
    PFN_vkCmdSetPrimitiveTopology cmdSetPrimitiveTopology = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    PFN_vkResetQueryPool resetQueryPool = nullptr;

    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool depthIsD24 = false;      // 24-bit unorm depth, as GL's DEPTH_COMPONENT24
    bool depthBlit = false;       // vkCmdBlitImage works on depthFormat
    float maxAnisotropy = 1.0f;

    VmaPool hostPools[2] = {};  // GL buffers: written by the CPU, read by it (pixel pack buffers)

    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;  // the last serial submitted
    uint64_t completed = 0;  // the last serial known to have finished

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::string pipelineCachePath;
    size_t pipelineCacheSavedSize = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderStageFlags pushStages = 0;  // of push constant bytes 0-15 (the point size)
};
extern Device D;
VmaAllocationCreateInfo hostBufferAllocInfo(bool readback);

// The serial the commands being recorded now will signal.
inline uint64_t currentSerial() { return D.submitted + 1; }
uint64_t pollCompleted();
// Waits until `serial` has finished, submitting the current segment first if
// it is that one.
void waitSerial(uint64_t serial);
bool serialDone(uint64_t serial);
// Submits what has been recorded (the upload and main command buffers).
void submit();
void deferDelete(std::function<void()> fn);  // runs once the current segment has finished
// After a draw: submits the segment once it holds enough work, so the GPU
// starts on a frame while the rest is recorded (as a GL driver flushes).
void drawRecorded();

// SMS_GX_STATS: what glvk spent, logged with the renderer's stats.
struct Stats {
    uint32_t submits = 0, draws = 0, pipelines = 0, renderPasses = 0, frames = 0;
    uint32_t dropped = 0;  // frames not presented: no drawable free without waiting (vsync off, MoltenVK)
    double waitSeconds = 0, submitSeconds = 0;
    double pipelineSeconds = 0;  // making pipelines
};
extern Stats g_stats;
double now();
void collectGarbage();

VkCommandBuffer mainCmd();
VkCommandBuffer uploadCmd();  // the segment's upload command buffer (begun on demand)

// Host-visible memory handed out linearly and recycled once the segments
// that used it have finished (staging data, uniform blocks, constant
// attributes).
struct RingAlloc {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* ptr;
};
struct Ring {
    struct Block {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        uint8_t* map = nullptr;
        VkDeviceSize size = 0, used = 0;
        uint64_t serial = 0;  // the last segment that used it
    };
    VkBufferUsageFlags usage = 0;
    VkDeviceSize blockSize = 0;
    std::vector<Block> blocks;  // blocks[cur] is filled now
    int cur = -1;
    RingAlloc alloc(VkDeviceSize bytes, VkDeviceSize align);
    void destroy();
};
extern Ring g_staging, g_dynamic;

// ------------------------------------------------------------------ images
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t w = 0, h = 0, levels = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags access = 0;
    bool lastWrite = false;
    uint64_t lastUse = 0;   // the last serial whose commands used it
    uint64_t lastMain = 0;  // the last serial whose main command buffer used it
    std::unordered_map<uint32_t, VkImageView> views;
    bool owned = true;  // false for swapchain images
};
Image* createImage(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, VkSampleCountFlagBits samples, bool attachment);
void destroyImageLater(Image* img);
VkImageView imageView(Image* img, uint32_t baseLevel, uint32_t levelCount);
// Puts img in `layout` for a use at `stage` with `access`, with the barrier
// that orders it after the previous use, into cb (the main command buffer
// unless given). Ends rendering first when cb is the main command buffer and
// a barrier is needed (barriers cannot go inside it).
void use(Image* img, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool write,
         VkCommandBuffer cb = VK_NULL_HANDLE);
bool isDepthFormat(VkFormat f);

// ------------------------------------------------------------------ GL objects
struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0;
    bool readback = false;   // host-cached memory, for pixel pack buffers
    bool coherent = true;
    uint64_t lastUse = 0;    // the last serial that read or wrote it on the GPU
    uint64_t lastWrite = 0;  // the last serial that wrote it on the GPU
    // depth readbacks into it, converted to GL's 32-bit depth at the next map
    struct Conv {
        VkDeviceSize offset, count;
        bool d24;
    };
    std::vector<Conv> pendingConv;
    // the mapped range (glMapBufferRange)
    VkDeviceSize mapOffset = 0, mapLength = 0;
    GLbitfield mapAccess = 0;
};

struct Texture {
    // what GL was told
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;     // level 0
    uint32_t specified = 0;    // the levels given (bit per level)
    bool compressed = false;
    // parameters
    GLint minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR;
    GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    GLint baseLevel = 0, maxLevel = 1000;
    float minLod = -1000.0f, maxLod = 1000.0f, lodBias = 0.0f, anisotropy = 1.0f;
    // the image (made at the first use: levels known by then)
    Image* img = nullptr;
    struct Pending {
        uint32_t level, x, y, w, h;
        std::vector<uint8_t> data;  // tightly packed rows, or compressed blocks
    };
    std::vector<Pending> pending;
    uint32_t version = 1;  // bumped when img, the levels or a parameter changes (descriptor caches)
    // the descriptor last made for it, and what it was made from
    struct SamplerObj* cacheSampler = nullptr;
    uint32_t cacheVersion = 0, cacheSamplerVersion = 0;
    VkImageView cacheView = VK_NULL_HANDLE;
    VkSampler cacheVkSampler = VK_NULL_HANDLE;
    bool cacheComplete = false;
};

struct Renderbuffer {
    Image* img = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

struct Attachment {
    GLenum kind = 0;  // 0, GL_TEXTURE or GL_RENDERBUFFER
    GLuint name = 0;
    GLint level = 0;
};
struct Framebuffer {
    Attachment color, depth;
    uint32_t version = 0;
};

struct SamplerObj {
    GLint minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR;
    GLint wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    float minLod = -1000.0f, maxLod = 1000.0f, lodBias = 0.0f, anisotropy = 1.0f;
    uint32_t version = 1;
};

struct VertexAttrib {
    bool enabled = false;
    bool integer = false;
    bool normalized = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    size_t offset = 0;
    GLuint buffer = 0;
};
struct VertexArray {
    VertexAttrib attribs[kMaxAttribs];
    GLuint elementBuffer = 0;
    uint32_t version = 1;
};

struct Shader {
    GLenum type = 0;
    std::string source;
};

struct UniformInfo {
    std::string name;
    int block = -1;       // -1: the default block; samplers: -2
    int offset = 0;       // in the default block
    int arrayStride = 0;  // 0 for non-arrays
    int arraySize = 1;
    int glType = 0;       // GL_FLOAT_VEC4, GL_SAMPLER_2D, ...
    int binding = -1;     // samplers: the set's binding (B_SAMPLER_ARRAY or B_SAMPLERS + n)
    std::vector<int> units;  // samplers: the unit of each element
};

struct VertexLayout;
struct LinkJob;
struct Program {
    std::vector<GLuint> shaders;
    std::unordered_map<std::string, int> attribLocations;  // glBindAttribLocation
    bool linked = false;
    std::string log;
    VkShaderModule modules[3] = {};  // vertex, geometry, fragment
    std::vector<uint32_t> spirv[3];
    std::vector<UniformInfo> uniforms;
    std::vector<std::string> blockNames;  // named uniform blocks: binding B_UBO0 + index
    std::vector<int> blockSizes;
    GLuint blockBindings[B_UBO_COUNT] = {0, 0};  // glUniformBlockBinding
    std::vector<uint8_t> defaultBlock;
    bool defaultDirty = true;
    VkDescriptorBufferInfo defaultBound{};  // where the default block was last written
    uint64_t defaultBoundSerial = 0;
    uint32_t inputMask = 0;   // the vertex shader's input locations
    uint32_t inputInteger = 0;  // those of integer type
    uint32_t samplerMask = 0; // bindings of samplers it uses (bit per binding)
    bool hasGeometry = false;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    // with graphics pipeline libraries: the shader parts, which pipelines link
    VkPipeline libPre[2] = {};   // pre-rasterisation shaders, without and with depth clamp
    VkPipeline libFrag[4] = {};  // fragment shader, by sample count (1, 2, 4, 8)
    uint32_t id = 0;
    std::shared_ptr<LinkJob> job;     // a link still compiling on a worker thread
    std::shared_ptr<LinkJob> libJob;  // a link whose pipeline library parts are still being made
};

struct Query {
    std::vector<uint32_t> slots;  // the Vulkan queries of this GL query, summed
    uint64_t serial = 0;          // the segment that ended it
    bool active = false;
    bool haveResult = false;      // read (and its slots given back)
    uint64_t result = 0;
};

struct Sync {
    uint64_t serial;
};

// ------------------------------------------------------------------ GL state
struct State {
    GLuint drawFbo = 0, readFbo = 0;
    GLuint program = 0, vao = 0;
    GLuint arrayBuffer = 0, packBuffer = 0, unpackBuffer = 0, uniformBuffer = 0, renderbuffer = 0;
    struct UboRange {
        GLuint buffer = 0;
        GLintptr offset = 0;
        GLsizeiptr size = 0;
    } ubo[kMaxUboBindings];
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    bool scissorTest = false, blend = false, depthTest = false, cullFace = false, logicOp = false;
    bool clip[2] = {false, false}, depthClamp = false, stencilTest = false;
    GLenum blendEqRGB = GL_FUNC_ADD, blendEqA = GL_FUNC_ADD;
    GLenum blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
    float blendColor[4] = {0, 0, 0, 0};
    GLenum depthFunc = GL_LESS;
    bool depthMask = true;
    bool colorMask[4] = {true, true, true, true};
    GLenum cullMode = GL_BACK, frontFace = GL_CCW, logicOpMode = GL_COPY;
    float clearColor[4] = {0, 0, 0, 0};
    double clearDepth = 1.0;
    float pointSize = 1.0f, lineWidth = 1.0f;
    double depthRange[2] = {0.0, 1.0};
    GLint packAlign = 4, unpackAlign = 4, unpackRowLength = 0, packRowLength = 0;
    GLuint activeUnit = 0;
    GLuint textures[kMaxUnits] = {};
    GLuint samplers[kMaxUnits] = {};
    float attrib[kMaxAttribs][4];
    uint32_t attribInteger = 0;  // the current values set with glVertexAttribI*
    GLuint query = 0;            // the active GL_SAMPLES_PASSED query
};
extern State S;

// Name tables: GL names index these (0 is never a generated name).
template <typename T> struct Names {
    std::vector<T*> objs{nullptr};
    std::vector<GLuint> free;
    GLuint gen() {
        GLuint n;
        if (!free.empty()) {
            n = free.back();
            free.pop_back();
        } else {
            n = GLuint(objs.size());
            objs.push_back(nullptr);
        }
        objs[n] = new T();
        return n;
    }
    T* get(GLuint n) const { return n < objs.size() ? objs[n] : nullptr; }
    void remove(GLuint n) {
        if (n == 0 || n >= objs.size() || !objs[n]) return;
        delete objs[n];
        objs[n] = nullptr;
        free.push_back(n);
    }
};
extern Names<Buffer> g_buffers;
extern Names<Texture> g_textures;
extern Names<Renderbuffer> g_renderbuffers;
extern Names<Framebuffer> g_framebuffers;
extern Names<SamplerObj> g_samplers;
extern Names<VertexArray> g_vertexArrays;
extern Names<Shader> g_shaders;    // shaders and programs share one name space in GL;
extern Names<Program> g_programs;  // here each has its own, which no caller can tell apart
extern Names<Query> g_queries;

// ------------------------------------------------------------------ framebuffer 0
extern Image* g_backbuffer;  // framebuffer 0's colour (null until sized)
void resizeBackbuffer(uint32_t w, uint32_t h);

// ------------------------------------------------------------------ helpers (glvk_gl.cpp)
void endRendering();
bool renderingActive();
void onSegmentStart();  // a new main command buffer: forget what was bound in the last
void beforeSubmit();    // ends rendering and the open query piece
bool realizeTexture(Texture* t);
void realizePending();  // makes the images of textures still holding uploads
void destroyBuffer(Buffer* b);

// ------------------------------------------------------------------ shaders (glvk_shader.cpp)
bool linkProgram(Program* p);
// GL_KHR_parallel_shader_compile: glLinkProgram hands the compile to a worker
// thread; anything that needs the program waits for it (finishLink).
void finishLink(Program* p);
void programLibraries(Program* p);  // takes (or waits for) a compile thread's library parts
bool linkDone(const Program* p);
void setCompileThreads(GLuint n);
void programUniform(Program* p, GLint loc, int comps, GLsizei count, const void* data, bool integer);
bool programBinary(Program* p, std::vector<uint8_t>& out);
bool loadProgramBinary(Program* p, const void* data, size_t len);
void destroyProgramObjects(Program* p);
void shaderShutdown();

// ------------------------------------------------------------------ objects (glvk_objects.cpp)
void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void bufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
GLboolean unmapBuffer(GLenum target);
void texImage2D(GLint level, GLint ifmt, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels, bool compressed,
                GLsizei compressedBytes);
void texSubImage2D(GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, const void* pixels, bool compressed,
                   GLsizei compressedBytes);
void texParameter(GLenum pname, GLint i, float f, bool isFloat);
void generateMipmap();
void deleteTexture(GLuint n);
VkDescriptorImageInfo textureDescriptor(GLuint tex, GLuint sampler);
void samplerParameter(GLuint n, GLenum pname, GLint i, float f, bool isFloat);
void destroySamplers();
void destroyBlack();
void renderbufferStorage(GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h);
Image* attachmentImage(const Attachment& a, uint32_t* level);
Image* readImage(GLuint fbo, bool depth, uint32_t* level);
GLenum checkFramebuffer(GLuint fbo);
void readPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels);
void destroyReadback();
void blitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                     GLenum filter);
void beginQueryPiece();
void endQueryPiece();
void beginQuery(GLuint id);
void endQuery();
GLuint queryResult(GLuint id, bool available);
void deleteQuery(GLuint id);
void queriesSegmentStart();
void destroyQueryPools();
GLsync fenceSync();
GLenum clientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout);
void deleteSync(GLsync s);

// ------------------------------------------------------------------ draws (glvk_draw.cpp)
void destroyDrawObjects();
// Graphics pipeline libraries (D.gpl): a program's shader parts.
VkPipeline makePreRasterLibrary(const VkShaderModule modules[3], bool depthClamp);
VkPipeline makeFragmentLibrary(VkShaderModule fs, VkSampleCountFlagBits samples);
const VkDynamicState* pipelineDynamicStates(uint32_t* count);
uint32_t pipelineCreates();
void pipelineFrameEnd(uint32_t frame);
// Every GL object's Vulkan objects (at shutdown).
void destroyAllObjects();

// ------------------------------------------------------------------ swapchain (glvk_device.cpp)
bool presentFrame();

}  // namespace glvk

#endif
