// glvk: buffers, textures, samplers, renderbuffers, framebuffers, queries,
// sync objects, pixel reads and blits.
#include "glvk_impl.h"

#include <math.h>
#include <string.h>
#include <algorithm>

namespace glvk {

State S;
Names<Buffer> g_buffers;
Names<Texture> g_textures;
Names<Renderbuffer> g_renderbuffers;
Names<Framebuffer> g_framebuffers;
Names<SamplerObj> g_samplers;
Names<VertexArray> g_vertexArrays;
Names<Shader> g_shaders;
Names<Program> g_programs;
Names<Query> g_queries;

// ------------------------------------------------------------------ buffers
static bool bufferBusy(const Buffer* b) { return b->lastUse >= currentSerial() || !serialDone(b->lastUse); }

void destroyBuffer(Buffer* b) {
    if (!b->buf) return;
    VkBuffer buf = b->buf;
    VmaAllocation a = b->alloc;
    deferDelete([buf, a] { vmaDestroyBuffer(D.vma, buf, a); });
    b->buf = VK_NULL_HANDLE;
    b->alloc = VK_NULL_HANDLE;
    b->map = nullptr;
    b->size = 0;
    b->pendingConv.clear();
}

// (Re)allocates b's storage: `readback` buffers are host-cached (the CPU
// reads them), the others host-visible and coherent (the CPU writes them and
// the GPU reads them in place, as GL's persistently mapped buffers).
static bool allocBuffer(Buffer* b, VkDeviceSize size, bool readback) {
    destroyBuffer(b);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<VkDeviceSize>(size, 16);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // System memory: the renderer writes its persistently mapped buffers in
    // many small pieces, which video memory the CPU can write (the BAR) takes
    // several times as long. Small ones share 16 MiB blocks.
    VmaAllocationCreateInfo aci = hostBufferAllocInfo(readback);
    if (size <= (VkDeviceSize(4) << 20)) aci.pool = D.hostPools[readback ? 1 : 0];
    else aci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(D.vma, &bci, &aci, &b->buf, &b->alloc, &info) != VK_SUCCESS) {
        logmsg("Vulkan: could not make a %zu KiB buffer", size_t(size >> 10));
        b->buf = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryPropertyFlags props = 0;
    vmaGetAllocationMemoryProperties(D.vma, b->alloc, &props);
    b->coherent = (props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    b->map = static_cast<uint8_t*>(info.pMappedData);
    b->size = size;
    b->readback = readback;
    b->lastUse = b->lastWrite = 0;
    return true;
}

static Buffer* boundBuffer(GLenum target) {
    GLuint n = 0;
    switch (target) {
    case GL_ARRAY_BUFFER: n = S.arrayBuffer; break;
    case GL_ELEMENT_ARRAY_BUFFER: {
        VertexArray* va = g_vertexArrays.get(S.vao);
        n = va ? va->elementBuffer : 0;
        break;
    }
    case GL_PIXEL_PACK_BUFFER: n = S.packBuffer; break;
    case GL_PIXEL_UNPACK_BUFFER: n = S.unpackBuffer; break;
    case GL_UNIFORM_BUFFER: n = S.uniformBuffer; break;
    default: break;
    }
    return g_buffers.get(n);
}

void bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    Buffer* b = boundBuffer(target);
    if (!b) return;
    const bool readback = usage == GL_STREAM_READ || usage == GL_STATIC_READ || usage == GL_DYNAMIC_READ;
    if (!b->buf || bufferBusy(b) || b->size != VkDeviceSize(size) || b->readback != readback) {
        if (!allocBuffer(b, VkDeviceSize(size), readback)) return;
    }
    b->pendingConv.clear();
    if (data && size > 0) {
        memcpy(b->map, data, size_t(size));
        if (!b->coherent) vmaFlushAllocation(D.vma, b->alloc, 0, VkDeviceSize(size));
    }
}

void bufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    Buffer* b = boundBuffer(target);
    if (!b) return;
    const bool readback = (flags & GL_MAP_READ_BIT) && !(flags & GL_MAP_WRITE_BIT);
    if (!allocBuffer(b, VkDeviceSize(size), readback)) return;
    if (data && size > 0) memcpy(b->map, data, size_t(size));
}

void bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    Buffer* b = boundBuffer(target);
    if (!b || !b->buf || size <= 0 || VkDeviceSize(offset + size) > b->size) return;
    if (!bufferBusy(b)) {
        memcpy(b->map + offset, data, size_t(size));
        if (!b->coherent) vmaFlushAllocation(D.vma, b->alloc, VkDeviceSize(offset), VkDeviceSize(size));
        return;
    }
    // queued draws still read it: copy in order on the GPU
    RingAlloc st = g_staging.alloc(VkDeviceSize(size), 16);
    memcpy(st.ptr, data, size_t(size));
    if (renderingActive()) endRendering();
    VkCommandBuffer cb = mainCmd();
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy region{st.offset, VkDeviceSize(offset), VkDeviceSize(size)};
    vkCmdCopyBuffer(cb, st.buffer, b->buf, 1, &region);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    b->lastUse = b->lastWrite = currentSerial();
}

static void applyConversions(Buffer* b) {
    for (const Buffer::Conv& c : b->pendingConv) {
        if (c.offset + c.count * 4 > b->size) continue;
        uint32_t* p = reinterpret_cast<uint32_t*>(b->map + c.offset);
        if (c.d24) {
            for (VkDeviceSize i = 0; i < c.count; i++) {
                const uint32_t d = p[i] & 0xFFFFFF;
                p[i] = d << 8 | d >> 16;
            }
        } else {
            for (VkDeviceSize i = 0; i < c.count; i++) {
                float f;
                memcpy(&f, &p[i], 4);
                f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                p[i] = uint32_t(double(f) * 4294967295.0 + 0.5);
            }
        }
    }
    b->pendingConv.clear();
}

void* mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    Buffer* b = boundBuffer(target);
    if (!b || !b->buf || VkDeviceSize(offset + length) > b->size) return nullptr;
    if (!(access & GL_MAP_UNSYNCHRONIZED_BIT)) {
        if (access & GL_MAP_READ_BIT) waitSerial(b->lastWrite);
        if (access & GL_MAP_WRITE_BIT) {
            if ((access & GL_MAP_INVALIDATE_BUFFER_BIT) && bufferBusy(b)) allocBuffer(b, b->size, b->readback);
            else waitSerial(b->lastUse);
        }
    }
    if (access & GL_MAP_READ_BIT) {
        vmaInvalidateAllocation(D.vma, b->alloc, VkDeviceSize(offset), VkDeviceSize(length));
        if (!b->pendingConv.empty()) applyConversions(b);
    }
    b->mapOffset = VkDeviceSize(offset);
    b->mapLength = VkDeviceSize(length);
    b->mapAccess = access;
    return b->map + offset;
}

GLboolean unmapBuffer(GLenum target) {
    Buffer* b = boundBuffer(target);
    if (!b || !b->buf) return GL_FALSE;
    if ((b->mapAccess & GL_MAP_WRITE_BIT) && !b->coherent) vmaFlushAllocation(D.vma, b->alloc, b->mapOffset, b->mapLength);
    b->mapAccess = 0;
    return GL_TRUE;
}

// ------------------------------------------------------------------ textures
static VkFormat textureFormat(GLint ifmt) {
    switch (ifmt) {
    case GL_RGBA8: case GL_RGBA: return VK_FORMAT_R8G8B8A8_UNORM;
    case GL_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GL_R8: case GL_RED: return VK_FORMAT_R8_UNORM;
    case GL_RG8: case GL_RG: return VK_FORMAT_R8G8_UNORM;
    case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT32F: case GL_DEPTH_COMPONENT32:
    case GL_DEPTH_COMPONENT16:
        return D.depthFormat;
    case 0x83F0: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;   // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
    case 0x83F1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
    case 0x83F2: return VK_FORMAT_BC2_UNORM_BLOCK;       // DXT3
    case 0x83F3: return VK_FORMAT_BC3_UNORM_BLOCK;       // DXT5
    case 0x8E8C: return VK_FORMAT_BC7_UNORM_BLOCK;       // GL_COMPRESSED_RGBA_BPTC_UNORM
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static uint32_t texelBytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

static bool isCompressed(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static uint32_t blockBytes(VkFormat f) {
    return f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
}

static uint32_t fullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1, m = std::max(w, h);
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

static Texture* boundTexture() { return S.activeUnit < kMaxUnits ? g_textures.get(S.textures[S.activeUnit]) : nullptr; }

// Uploads tightly packed data into a level region. Into the segment's upload
// command buffer when the main one has not used the image yet, else in order.
static void uploadRegion(Image* img, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data,
                         size_t bytes) {
    RingAlloc st = g_staging.alloc(VkDeviceSize(bytes), 16);
    memcpy(st.ptr, data, bytes);
    const bool hoist = img->lastMain < currentSerial();
    if (!hoist && renderingActive()) endRendering();  // no transfers inside rendering
    VkCommandBuffer cb = hoist ? uploadCmd() : mainCmd();
    use(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true, cb);
    VkBufferImageCopy region{};
    region.bufferOffset = st.offset;
    region.imageSubresource = {img->aspect, level, 0, 1};
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, st.buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (hoist)  // ready for the main command buffer's draws
        use(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT, false, cb);
}

static size_t levelBytes(VkFormat f, uint32_t w, uint32_t h) {
    if (isCompressed(f)) return size_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes(f);
    return size_t(w) * h * texelBytes(f);
}

// Makes (or remakes, with more levels) t's image and gives it the pending uploads.
bool realizeTexture(Texture* t) {
    if (!t || t->format == VK_FORMAT_UNDEFINED || !t->w || !t->h || !t->specified) return t && t->img;
    uint32_t top = 0;
    for (uint32_t l = 0; l < 32; l++)
        if (t->specified & (1u << l)) top = l;
    const uint32_t full = fullChain(t->w, t->h);
    uint32_t levels = top + 1;
    if (t->maxLevel < 1000) levels = std::max(levels, std::min(full, uint32_t(t->maxLevel) + 1));
    levels = std::min(levels, full);
    Image* img = t->img;
    if (img && (img->format != t->format || img->w != t->w || img->h != t->h)) {
        destroyImageLater(img);
        img = t->img = nullptr;
    }
    if (img && img->levels <= top) {  // levels added after the first use: a bigger image, the old levels copied over
        Image* bigger = createImage(t->format, t->w, t->h, levels, VK_SAMPLE_COUNT_1_BIT, true);
        if (!bigger) return false;
        const bool hoist = img->lastMain < currentSerial();
        if (!hoist && renderingActive()) endRendering();
        VkCommandBuffer cb = hoist ? uploadCmd() : mainCmd();
        use(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false, cb);
        use(bigger, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true, cb);
        std::vector<VkImageCopy> regions;
        for (uint32_t l = 0; l < img->levels; l++) {
            VkImageCopy c{};
            c.srcSubresource = {img->aspect, l, 0, 1};
            c.dstSubresource = {img->aspect, l, 0, 1};
            c.extent = {std::max(1u, img->w >> l), std::max(1u, img->h >> l), 1};
            regions.push_back(c);
        }
        vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bigger->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       uint32_t(regions.size()), regions.data());
        if (hoist)
            use(bigger, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT, false, cb);
        destroyImageLater(img);
        img = t->img = bigger;
        t->version++;
    }
    if (!img) {
        img = t->img = createImage(t->format, t->w, t->h, levels, VK_SAMPLE_COUNT_1_BIT, true);
        if (!img) return false;
        t->version++;
    }
    if (!t->pending.empty()) {
        for (const Texture::Pending& p : t->pending)
            if (p.level < img->levels) uploadRegion(img, p.level, p.x, p.y, p.w, p.h, p.data.data(), p.data.size());
        t->pending.clear();
    }
    return true;
}

// Takes GL's client-memory pixels (unpack row length and alignment) as tight rows.
static void packRows(const void* pixels, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t>& out) {
    const size_t row = size_t(w) * bpp;
    size_t pitch = size_t(S.unpackRowLength > 0 ? S.unpackRowLength : GLint(w)) * bpp;
    const size_t align = size_t(S.unpackAlign > 0 ? S.unpackAlign : 1);
    pitch = (pitch + align - 1) / align * align;
    out.resize(row * h);
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    if (pitch == row) memcpy(out.data(), src, row * h);
    else
        for (uint32_t y = 0; y < h; y++) memcpy(out.data() + row * y, src + pitch * y, row);
}

// Textures holding uploads until their image is made (their levels known):
// made at the end of the frame, or at once when they hold over 64 MiB, so
// the copies do not pile up in memory (a texture pack sends hundreds of
// replacements before any of them is drawn).
static std::vector<GLuint> s_pendingTex;
static size_t s_pendingBytes = 0;

void realizePending() {
    std::vector<GLuint> names;
    names.swap(s_pendingTex);
    s_pendingBytes = 0;
    for (GLuint n : names)
        if (Texture* t = g_textures.get(n))
            if (!t->pending.empty()) realizeTexture(t);
}

static void storeLevel(Texture* t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>&& data) {
    if (t->img && level < t->img->levels && t->img->format == t->format) {
        uploadRegion(t->img, level, x, y, w, h, data.data(), data.size());
        return;
    }
    // not made yet (or without this level): kept until the first use. A whole
    // level replaces what was kept for it.
    if (x == 0 && y == 0 && w == std::max(1u, t->w >> level) && h == std::max(1u, t->h >> level))
        t->pending.erase(std::remove_if(t->pending.begin(), t->pending.end(),
                                        [level](const Texture::Pending& p) { return p.level == level; }),
                         t->pending.end());
    if (t->pending.empty() && S.activeUnit < kMaxUnits) s_pendingTex.push_back(S.textures[S.activeUnit]);
    s_pendingBytes += data.size();
    t->pending.push_back(Texture::Pending{level, x, y, w, h, std::move(data)});
    if (s_pendingBytes > (size_t(64) << 20)) realizePending();
}

void texImage2D(GLint level, GLint ifmt, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels, bool compressed,
                GLsizei compressedBytes) {
    Texture* t = boundTexture();
    if (!t || w <= 0 || h <= 0 || level < 0 || level > 15) return;
    const VkFormat f = textureFormat(ifmt);
    const uint32_t lw = uint32_t(w), lh = uint32_t(h);
    if (level == 0) {
        if (f != t->format || lw != t->w || lh != t->h) {
            // a new size or format: GL forgets the other levels
            if (t->img) destroyImageLater(t->img);
            t->img = nullptr;
            t->pending.clear();
            t->specified = 0;
            t->format = f;
            t->w = lw;
            t->h = lh;
            t->compressed = compressed;
        } else if (t->img && t->img->lastUse >= currentSerial() && pixels && !(t->specified & ~1u)) {
            // the whole (single-level) image again while this segment uses
            // the old one: a fresh image, so the upload need not wait for it
            destroyImageLater(t->img);
            t->img = nullptr;
        }
    } else if (t->format == VK_FORMAT_UNDEFINED) {
        t->format = f;
        t->w = lw << level;
        t->h = lh << level;
    }
    t->specified |= 1u << level;
    t->version++;
    if (!pixels) return;
    std::vector<uint8_t> data;
    if (compressed) {
        const size_t need = levelBytes(f, lw, lh);
        data.assign(static_cast<const uint8_t*>(pixels), static_cast<const uint8_t*>(pixels) + std::min(size_t(compressedBytes), need));
        data.resize(need);
    } else {
        packRows(pixels, lw, lh, f == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : texelBytes(f), data);
        if (format == GL_BGRA && f == VK_FORMAT_R8G8B8A8_UNORM)
            for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 2]);
        (void)type;
    }
    storeLevel(t, uint32_t(level), 0, 0, lw, lh, std::move(data));
}

void texSubImage2D(GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, const void* pixels, bool compressed,
                   GLsizei compressedBytes) {
    Texture* t = boundTexture();
    if (!t || !pixels || w <= 0 || h <= 0 || level < 0 || t->format == VK_FORMAT_UNDEFINED) return;
    std::vector<uint8_t> data;
    if (compressed) {
        const size_t need = levelBytes(t->format, uint32_t(w), uint32_t(h));
        data.assign(static_cast<const uint8_t*>(pixels), static_cast<const uint8_t*>(pixels) + std::min(size_t(compressedBytes), need));
        data.resize(need);
    } else {
        packRows(pixels, uint32_t(w), uint32_t(h), texelBytes(t->format), data);
        if (format == GL_BGRA && t->format == VK_FORMAT_R8G8B8A8_UNORM)
            for (size_t i = 0; i + 3 < data.size(); i += 4) std::swap(data[i], data[i + 2]);
    }
    storeLevel(t, uint32_t(level), uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h), std::move(data));
}

void texParameter(GLenum pname, GLint i, float f, bool isFloat) {
    Texture* t = boundTexture();
    if (!t) return;
    const GLint v = isFloat ? GLint(f) : i;
    const float fv = isFloat ? f : float(i);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->minFilter = v; break;
    case GL_TEXTURE_MAG_FILTER: t->magFilter = v; break;
    case GL_TEXTURE_WRAP_S: t->wrapS = v; break;
    case GL_TEXTURE_WRAP_T: t->wrapT = v; break;
    case GL_TEXTURE_BASE_LEVEL: t->baseLevel = std::max(0, v); break;
    case GL_TEXTURE_MAX_LEVEL: t->maxLevel = std::max(0, v); break;
    case GL_TEXTURE_MIN_LOD: t->minLod = fv; break;
    case GL_TEXTURE_MAX_LOD: t->maxLod = fv; break;
    case GL_TEXTURE_LOD_BIAS: t->lodBias = fv; break;
    case 0x84FE: t->anisotropy = fv; break;  // GL_TEXTURE_MAX_ANISOTROPY
    default: return;
    }
    t->version++;
}

void generateMipmap() {
    Texture* t = boundTexture();
    if (!t || !t->w || t->compressed) return;
    const uint32_t full = fullChain(t->w, t->h);
    const uint32_t base = uint32_t(std::min<GLint>(t->baseLevel, GLint(full - 1)));
    const uint32_t top = std::min<uint32_t>(full - 1, uint32_t(std::min<GLint>(t->maxLevel, 31)));
    if (top <= base) return;
    for (uint32_t l = base + 1; l <= top; l++) t->specified |= 1u << l;
    t->version++;
    if (!realizeTexture(t)) return;
    Image* img = t->img;
    const bool hoist = img->lastMain < currentSerial();
    VkCommandBuffer cb = hoist ? uploadCmd() : mainCmd();
    if (!hoist && renderingActive()) endRendering();
    for (uint32_t l = base + 1; l <= top && l < img->levels; l++) {
        // each level from the one above (GL's box filter is a 2:1 linear blit)
        VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
        for (int k = 0; k < 2; k++) {
            b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[k].image = img->image;
        }
        b[0].srcAccessMask = img->lastWrite ? img->access : 0;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].oldLayout = img->layout;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].subresourceRange = {img->aspect, l - 1, 1, 0, 1};
        b[1].srcAccessMask = img->lastWrite ? img->access : 0;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].oldLayout = img->layout;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].subresourceRange = {img->aspect, l, 1, 0, 1};
        if (l > base + 1) {  // the level above was written by the previous blit
            b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        vkCmdPipelineBarrier(cb, img->stage | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, b);
        VkImageBlit blit{};
        blit.srcSubresource = {img->aspect, l - 1, 0, 1};
        blit.srcOffsets[1] = {int32_t(std::max(1u, img->w >> (l - 1))), int32_t(std::max(1u, img->h >> (l - 1))), 1};
        blit.dstSubresource = {img->aspect, l, 0, 1};
        blit.dstOffsets[1] = {int32_t(std::max(1u, img->w >> l)), int32_t(std::max(1u, img->h >> l)), 1};
        vkCmdBlitImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
        // the source level goes back to the image's layout below; levels
        // outside [base, top] keep img->layout throughout
        VkImageMemoryBarrier back{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        back.srcQueueFamilyIndex = back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        back.image = img->image;
        back.subresourceRange = {img->aspect, l - 1, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    }
    // levels base..top are now TRANSFER_DST; the rest are still img->layout:
    // bring them all to TRANSFER_DST so the whole image has one layout
    {
        std::vector<VkImageMemoryBarrier> bs;
        for (uint32_t l = 0; l < img->levels; l++) {
            if (l >= base && l <= top) continue;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = img->lastWrite ? img->access : 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = img->layout;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img->image;
            b.subresourceRange = {img->aspect, l, 1, 0, 1};
            bs.push_back(b);
        }
        if (!bs.empty())
            vkCmdPipelineBarrier(cb, img->stage | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, uint32_t(bs.size()), bs.data());
    }
    img->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    img->stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    img->access = VK_ACCESS_TRANSFER_WRITE_BIT;
    img->lastWrite = true;
    img->lastUse = currentSerial();
    if (!hoist) img->lastMain = currentSerial();
    if (hoist)
        use(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT, false, cb);
}

void deleteTexture(GLuint n) {
    Texture* t = g_textures.get(n);
    if (!t) return;
    if (t->img && t->img->lastMain >= currentSerial() && renderingActive()) endRendering();
    destroyImageLater(t->img);
    t->img = nullptr;
    for (GLuint& u : S.textures)
        if (u == n) u = 0;
    g_textures.remove(n);
}

// ------------------------------------------------------------------ samplers
namespace {
struct SamplerKey {
    VkFilter mag, min;
    VkSamplerMipmapMode mip;
    VkSamplerAddressMode u, v;
    float minLod, maxLod, bias, aniso;
    bool operator==(const SamplerKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 1099511628211ull;
        return size_t(h);
    }
};
std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> s_vkSamplers;

VkSamplerAddressMode wrapMode(GLint w) {
    switch (w) {
    case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
bool mipFilter(GLint f) { return f != GL_NEAREST && f != GL_LINEAR; }
}  // namespace

static VkSampler vkSampler(GLint minF, GLint magF, GLint wrapS, GLint wrapT, float minLod, float maxLod, float bias, float aniso) {
    SamplerKey k;
    memset(&k, 0, sizeof k);
    k.mag = magF == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    k.min = (minF == GL_NEAREST || minF == GL_NEAREST_MIPMAP_NEAREST || minF == GL_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST
                                                                                                          : VK_FILTER_LINEAR;
    k.mip = (minF == GL_NEAREST_MIPMAP_LINEAR || minF == GL_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    k.u = wrapMode(wrapS);
    k.v = wrapMode(wrapT);
    // a filter without mipmaps samples the base level only: the view has one
    // level, and the LOD range only picks magnification or minification
    k.minLod = std::max(-1000.0f, std::min(1000.0f, minLod));
    k.maxLod = std::max(k.minLod, std::min(1000.0f, maxLod));
    const float maxBias = D.props.limits.maxSamplerLodBias;
    k.bias = D.noLodBias ? 0.0f : std::max(-maxBias, std::min(maxBias, bias));  // else the shader adds it
    k.aniso = D.features.samplerAnisotropy && aniso > 1.0f ? std::min(aniso, D.maxAnisotropy) : 1.0f;
    auto it = s_vkSamplers.find(k);
    if (it != s_vkSamplers.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = k.mag;
    ci.minFilter = k.min;
    ci.mipmapMode = k.mip;
    ci.addressModeU = k.u;
    ci.addressModeV = k.v;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.mipLodBias = k.bias;
    ci.anisotropyEnable = k.aniso > 1.0f;
    ci.maxAnisotropy = k.aniso;
    ci.minLod = k.minLod;
    ci.maxLod = k.maxLod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(D.dev, &ci, nullptr, &s);
    s_vkSamplers[k] = s;
    return s;
}

void destroySamplers() {
    for (auto& kv : s_vkSamplers) vkDestroySampler(D.dev, kv.second, nullptr);
    s_vkSamplers.clear();
}

static Image* s_black = nullptr;
static VkSampler s_blackSampler = VK_NULL_HANDLE;

static VkDescriptorImageInfo blackTexture() {
    if (!s_black) {
        s_black = createImage(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, false);
        const uint8_t px[4] = {0, 0, 0, 255};
        uploadRegion(s_black, 0, 0, 0, 1, 1, px, 4);
        s_blackSampler = vkSampler(GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT, 0, 0, 0, 1);
    }
    use(s_black, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT, false);
    return VkDescriptorImageInfo{s_blackSampler, imageView(s_black, 0, 1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void destroyBlack() {
    destroyImageLater(s_black);
    s_black = nullptr;
}

// The descriptor for sampling texture `n` on a unit with sampler object `sn`:
// a black texture when GL would call it incomplete.
VkDescriptorImageInfo textureDescriptor(GLuint n, GLuint sn) {
    Texture* t = g_textures.get(n);
    if (!t || !realizeTexture(t) || !t->img) return blackTexture();
    SamplerObj* so = g_samplers.get(sn);
    if (t->cacheVersion != t->version || t->cacheSampler != so || (so && t->cacheSamplerVersion != so->version)) {
        const GLint minF = so ? so->minFilter : t->minFilter;
        const bool mip = mipFilter(minF);
        const uint32_t full = fullChain(t->w, t->h);
        const uint32_t base = uint32_t(std::min<GLint>(t->baseLevel, GLint(t->img->levels - 1)));
        uint32_t top = mip ? std::min<uint32_t>(uint32_t(std::min<GLint>(t->maxLevel, 31)), full - 1) : base;
        bool complete = (t->specified & (1u << base)) != 0;
        for (uint32_t l = base + 1; complete && l <= top; l++) complete = (t->specified & (1u << l)) != 0;
        top = std::min(top, t->img->levels - 1);
        t->cacheComplete = complete;
        if (complete) {
            t->cacheView = imageView(t->img, base, top - base + 1);
            t->cacheVkSampler = so ? vkSampler(so->minFilter, so->magFilter, so->wrapS, so->wrapT, so->minLod, so->maxLod,
                                               so->lodBias, so->anisotropy)
                                   : vkSampler(t->minFilter, t->magFilter, t->wrapS, t->wrapT, t->minLod, t->maxLod,
                                               t->lodBias, t->anisotropy);
        }
        t->cacheVersion = t->version;
        t->cacheSampler = so;
        t->cacheSamplerVersion = so ? so->version : 0;
    }
    if (!t->cacheComplete) return blackTexture();
    use(t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT, false);
    return VkDescriptorImageInfo{t->cacheVkSampler, t->cacheView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

void samplerParameter(GLuint n, GLenum pname, GLint i, float f, bool isFloat) {
    SamplerObj* s = g_samplers.get(n);
    if (!s) return;
    const GLint v = isFloat ? GLint(f) : i;
    const float fv = isFloat ? f : float(i);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: s->minFilter = v; break;
    case GL_TEXTURE_MAG_FILTER: s->magFilter = v; break;
    case GL_TEXTURE_WRAP_S: s->wrapS = v; break;
    case GL_TEXTURE_WRAP_T: s->wrapT = v; break;
    case GL_TEXTURE_MIN_LOD: s->minLod = fv; break;
    case GL_TEXTURE_MAX_LOD: s->maxLod = fv; break;
    case GL_TEXTURE_LOD_BIAS: s->lodBias = fv; break;
    case 0x84FE: s->anisotropy = fv; break;
    default: return;
    }
    s->version++;
}

// ------------------------------------------------------------------ renderbuffers
void renderbufferStorage(GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    Renderbuffer* r = g_renderbuffers.get(S.renderbuffer);
    if (!r || w <= 0 || h <= 0) return;
    destroyImageLater(r->img);
    r->format = textureFormat(GLint(ifmt));
    r->w = uint32_t(w);
    r->h = uint32_t(h);
    r->samples = samples > 1 ? VkSampleCountFlagBits(samples) : VK_SAMPLE_COUNT_1_BIT;
    r->img = createImage(r->format, r->w, r->h, 1, r->samples, true);
    for (Framebuffer* f : g_framebuffers.objs)
        if (f) f->version++;
}

// ------------------------------------------------------------------ framebuffers
// The image and level of an attachment (textures are made here if need be).
Image* attachmentImage(const Attachment& a, uint32_t* level) {
    *level = 0;
    if (a.kind == GL_TEXTURE) {
        Texture* t = g_textures.get(a.name);
        if (!t || !realizeTexture(t) || !t->img) return nullptr;
        *level = uint32_t(a.level) < t->img->levels ? uint32_t(a.level) : 0;
        return t->img;
    }
    if (a.kind == GL_RENDERBUFFER) {
        Renderbuffer* r = g_renderbuffers.get(a.name);
        return r ? r->img : nullptr;
    }
    return nullptr;
}

// The colour (or depth) image a read from framebuffer `fbo` reads.
Image* readImage(GLuint fbo, bool depth, uint32_t* level) {
    *level = 0;
    if (fbo == 0) return depth ? nullptr : g_backbuffer;
    Framebuffer* f = g_framebuffers.get(fbo);
    if (!f) return nullptr;
    return attachmentImage(depth ? f->depth : f->color, level);
}

GLenum checkFramebuffer(GLuint fbo) {
    if (fbo == 0) return GL_FRAMEBUFFER_COMPLETE;
    Framebuffer* f = g_framebuffers.get(fbo);
    if (!f) return GL_FRAMEBUFFER_UNDEFINED;
    uint32_t l;
    Image* c = attachmentImage(f->color, &l);
    Image* d = attachmentImage(f->depth, &l);
    if (!c && !d) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if (c && d && c->samples != d->samples) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
    return GL_FRAMEBUFFER_COMPLETE;
}

// ------------------------------------------------------------------ pixel reads
namespace {
VkBuffer s_readBuf = VK_NULL_HANDLE;
VmaAllocation s_readAlloc = VK_NULL_HANDLE;
uint8_t* s_readMap = nullptr;
VkDeviceSize s_readSize = 0;

uint8_t* readbackSpace(VkDeviceSize bytes) {
    if (bytes <= s_readSize) return s_readMap;
    if (s_readBuf) {
        VkBuffer b = s_readBuf;
        VmaAllocation a = s_readAlloc;
        deferDelete([b, a] { vmaDestroyBuffer(D.vma, b, a); });
    }
    s_readSize = std::max<VkDeviceSize>(bytes, VkDeviceSize(4) << 20);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = s_readSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(D.vma, &bci, &aci, &s_readBuf, &s_readAlloc, &info) != VK_SUCCESS) {
        s_readBuf = VK_NULL_HANDLE;
        s_readSize = 0;
        return nullptr;
    }
    s_readMap = static_cast<uint8_t*>(info.pMappedData);
    return s_readMap;
}
}  // namespace

void destroyReadback() {
    if (s_readBuf) vmaDestroyBuffer(D.vma, s_readBuf, s_readAlloc);
    s_readBuf = VK_NULL_HANDLE;
    s_readSize = 0;
}

void readPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    const bool depth = format == GL_DEPTH_COMPONENT;
    uint32_t level = 0;
    Image* img = readImage(S.readFbo, depth, &level);
    if (!img || w <= 0 || h <= 0 || img->samples != VK_SAMPLE_COUNT_1_BIT) return;
    if ((!depth && (format != GL_RGBA || type != GL_UNSIGNED_BYTE)) || (depth && type != GL_UNSIGNED_INT)) {
        static int told = 0;
        if (told++ < 4) logmsg("glvk: glReadPixels format %#x type %#x is not handled", format, type);
        return;
    }
    const uint32_t lw = std::max(1u, img->w >> level), lh = std::max(1u, img->h >> level);
    // the part inside the image (GL leaves the rest of the destination alone)
    const int32_t x0 = std::max(0, x), y0 = std::max(0, y);
    const int32_t x1 = std::min(int32_t(lw), x + w), y1 = std::min(int32_t(lh), y + h);
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t rw = uint32_t(x1 - x0), rh = uint32_t(y1 - y0);
    if (renderingActive()) endRendering();
    use(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    VkBufferImageCopy region{};
    region.imageSubresource = {img->aspect, level, 0, 1};
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {rw, rh, 1};
    const size_t rowBytes = size_t(w) * 4;  // 4 bytes per pixel, so any pack alignment up to 4 adds nothing
    if (S.packBuffer) {
        Buffer* b = g_buffers.get(S.packBuffer);
        if (!b || !b->buf) return;
        const VkDeviceSize off = VkDeviceSize(reinterpret_cast<uintptr_t>(pixels)) + VkDeviceSize(y0 - y) * rowBytes +
                                 VkDeviceSize(x0 - x) * 4;
        if (off + VkDeviceSize(rh - 1) * rowBytes + rw * 4 > b->size) return;
        region.bufferOffset = off;
        region.bufferRowLength = uint32_t(w);
        vkCmdCopyImageToBuffer(mainCmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buf, 1, &region);
        b->lastUse = b->lastWrite = currentSerial();
        if (depth)
            for (uint32_t r = 0; r < rh; r++)
                b->pendingConv.push_back(Buffer::Conv{off + VkDeviceSize(r) * rowBytes, rw, D.depthIsD24});
        return;
    }
    uint8_t* map = readbackSpace(VkDeviceSize(rw) * rh * 4);
    if (!map) return;
    region.bufferOffset = 0;
    vkCmdCopyImageToBuffer(mainCmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_readBuf, 1, &region);
    waitSerial(currentSerial());
    vmaInvalidateAllocation(D.vma, s_readAlloc, 0, VkDeviceSize(rw) * rh * 4);
    uint8_t* dst = static_cast<uint8_t*>(pixels);
    for (uint32_t r = 0; r < rh; r++) {
        uint8_t* row = dst + size_t(y0 - y + int32_t(r)) * rowBytes + size_t(x0 - x) * 4;
        memcpy(row, map + size_t(r) * rw * 4, size_t(rw) * 4);
        if (depth) {
            Buffer tmp;
            tmp.map = row;
            tmp.size = rw * 4;
            tmp.pendingConv.push_back(Buffer::Conv{0, rw, D.depthIsD24});
            applyConversions(&tmp);
            tmp.map = nullptr;
        }
    }
}

// ------------------------------------------------------------------ blits
void blitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                     GLenum filter) {
    if (renderingActive()) endRendering();
    for (int pass = 0; pass < 2; pass++) {
        const bool depth = pass == 1;
        if (!(mask & (depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT))) continue;
        uint32_t sl = 0, dl = 0;
        Image* src = readImage(S.readFbo, depth, &sl);
        Image* dst = readImage(S.drawFbo, depth, &dl);
        if (!src || !dst || src == dst) continue;
        if (S.scissorTest) {
            static int told = 0;
            if (told++ < 2) logmsg("glvk: a blit with the scissor test on ignores it");
        }
        const int32_t sw = int32_t(std::max(1u, src->w >> sl)), sh = int32_t(std::max(1u, src->h >> sl));
        const int32_t dw = int32_t(std::max(1u, dst->w >> dl)), dh = int32_t(std::max(1u, dst->h >> dl));
        VkCommandBuffer cb = mainCmd();
        const bool same = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0;
        if (!(depth && src->samples != VK_SAMPLE_COUNT_1_BIT)) {
            use(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
            use(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
        }
        if (src->samples != VK_SAMPLE_COUNT_1_BIT) {
            if (!same) {
                static int told = 0;
                if (told++ < 2) logmsg("glvk: a scaled multisampled blit is not handled");
                continue;
            }
            if (depth) {
                // vkCmdResolveImage takes colour only: a render pass over the
                // rectangle, without draws, whose depth resolves (sample 0)
                use(src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, false);
                use(dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
                const int32_t x0 = std::max(0, std::max(sx0, dx0)), y0 = std::max(0, std::max(sy0, dy0));
                const int32_t x1 = std::min(std::min(sx1, sw), dw), y1 = std::min(std::min(sy1, sh), dh);
                if (sx0 != dx0 || sy0 != dy0 || x1 <= x0 || y1 <= y0) continue;  // the EFB resolves in place
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                da.imageView = imageView(src, sl, 1);
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                da.resolveImageView = imageView(dst, dl, 1);
                da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
                ri.layerCount = 1;
                ri.pDepthAttachment = &da;
                D.cmdBeginRendering(cb, &ri);
                D.cmdEndRendering(cb);
                continue;
            }
            VkImageResolve r{};
            r.srcSubresource = {src->aspect, sl, 0, 1};
            r.srcOffset = {std::max(0, sx0), std::max(0, sy0), 0};
            r.dstSubresource = {dst->aspect, dl, 0, 1};
            r.dstOffset = {std::max(0, dx0), std::max(0, dy0), 0};
            r.extent = {uint32_t(std::min(sx1, sw) - std::max(0, sx0)), uint32_t(std::min(sy1, sh) - std::max(0, sy0)), 1};
            vkCmdResolveImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1, &r);
            continue;
        }
        if (same && src->format == dst->format) {
            // clipped to both images, as GL clips a blit
            const int32_t x0 = std::max(std::max(sx0, 0), sx0 - dx0), y0 = std::max(std::max(sy0, 0), sy0 - dy0);
            const int32_t x1 = std::min(std::min(sx1, sw), sx0 + (dw - dx0)), y1 = std::min(std::min(sy1, sh), sy0 + (dh - dy0));
            if (x1 <= x0 || y1 <= y0) continue;
            VkImageCopy c{};
            c.srcSubresource = {src->aspect, sl, 0, 1};
            c.srcOffset = {x0, y0, 0};
            c.dstSubresource = {dst->aspect, dl, 0, 1};
            c.dstOffset = {dx0 + (x0 - sx0), dy0 + (y0 - sy0), 0};
            c.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &c);
            continue;
        }
        if (depth && !D.depthBlit) {
            static int told = 0;
            if (told++ < 2) logmsg("glvk: this device cannot blit depth; a scaled depth blit is skipped");
            continue;
        }
        auto clampX = [](int32_t v, int32_t w) { return std::max(0, std::min(w, v)); };
        VkImageBlit b{};
        b.srcSubresource = {src->aspect, sl, 0, 1};
        b.srcOffsets[0] = {clampX(sx0, sw), clampX(sy0, sh), 0};
        b.srcOffsets[1] = {clampX(sx1, sw), clampX(sy1, sh), 1};
        b.dstSubresource = {dst->aspect, dl, 0, 1};
        b.dstOffsets[0] = {clampX(dx0, dw), clampX(dy0, dh), 0};
        b.dstOffsets[1] = {clampX(dx1, dw), clampX(dy1, dh), 1};
        vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       depth || filter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
}

// ------------------------------------------------------------------ queries
// A GL query is a run of Vulkan occlusion queries: one per render pass
// instance it spans (a Vulkan query cannot outlive one), summed at the read.
// Their slots come from pools of 4096, made as needed, and go back once the
// result has been read (or the GL query restarts or goes away).
namespace {
const uint32_t kPerPool = 4096, kMaxPools = 64;
std::vector<VkQueryPool> s_pools;
std::vector<uint32_t> s_freeSlots;
struct Retired {
    uint64_t serial;
    uint32_t slot;
};
std::vector<Retired> s_retired;  // slots to reuse once their segment has finished
uint32_t s_nextSlot = 0;
int s_openSlot = -1;  // the Vulkan query recording now

VkQueryPool poolOf(uint32_t slot) { return s_pools[slot / kPerPool]; }

void retire(Query* q, uint64_t serial) {
    for (uint32_t s : q->slots) s_retired.push_back(Retired{serial, s});
    q->slots.clear();
}

void recycle() {
    pollCompleted();
    size_t keep = 0;
    for (const Retired& r : s_retired) {
        if (r.serial <= D.completed) {
            D.resetQueryPool(D.dev, poolOf(r.slot), r.slot % kPerPool, 1);
            s_freeSlots.push_back(r.slot);
        } else {
            s_retired[keep++] = r;
        }
    }
    s_retired.resize(keep);
}

uint32_t allocSlot() {
    if (s_freeSlots.empty() && s_nextSlot >= s_pools.size() * kPerPool) recycle();
    if (s_freeSlots.empty()) {
        if (s_nextSlot >= s_pools.size() * kPerPool && s_pools.size() < kMaxPools) {
            VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            ci.queryType = VK_QUERY_TYPE_OCCLUSION;
            ci.queryCount = kPerPool;
            VkQueryPool pool = VK_NULL_HANDLE;
            vkCreateQueryPool(D.dev, &ci, nullptr, &pool);
            D.resetQueryPool(D.dev, pool, 0, kPerPool);
            s_pools.push_back(pool);
        }
        if (s_nextSlot < s_pools.size() * kPerPool) return s_nextSlot++;
        while (s_freeSlots.empty() && !s_retired.empty()) {  // all in use: wait for the oldest retired ones
            waitSerial(s_retired.front().serial);
            recycle();
        }
        if (s_freeSlots.empty()) {
            logmsg("glvk: out of occlusion queries (%u in use)", kPerPool * kMaxPools);
            abort();
        }
    }
    uint32_t s = s_freeSlots.back();
    s_freeSlots.pop_back();
    return s;
}
}  // namespace

void beginQueryPiece() {
    Query* q = g_queries.get(S.query);
    if (!q || s_openSlot >= 0) return;
    const uint32_t slot = allocSlot();
    vkCmdBeginQuery(mainCmd(), poolOf(slot), slot % kPerPool,
                    D.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    q->slots.push_back(slot);
    s_openSlot = int(slot);
}

void endQueryPiece() {
    if (s_openSlot < 0) return;
    vkCmdEndQuery(mainCmd(), poolOf(uint32_t(s_openSlot)), uint32_t(s_openSlot) % kPerPool);
    s_openSlot = -1;
}

void beginQuery(GLuint id) {
    Query* q = g_queries.get(id);
    if (!q) return;
    retire(q, std::max(q->serial, currentSerial()));
    q->active = true;
    q->haveResult = false;
    S.query = id;
    if (renderingActive()) beginQueryPiece();
}

void endQuery() {
    Query* q = g_queries.get(S.query);
    if (renderingActive()) endQueryPiece();
    if (q) {
        q->serial = currentSerial();
        q->active = false;
    }
    S.query = 0;
}

GLuint queryResult(GLuint id, bool available) {
    Query* q = g_queries.get(id);
    if (!q) return 0;
    if (available) return q->haveResult || serialDone(q->serial) ? 1 : 0;
    if (!q->haveResult) {
        waitSerial(q->serial);
        uint64_t sum = 0;
        for (uint32_t s : q->slots) {
            uint64_t v = 0;
            vkGetQueryPoolResults(D.dev, poolOf(s), s % kPerPool, 1, sizeof v, &v, sizeof v,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            sum += v;
        }
        q->result = sum;
        q->haveResult = true;
        retire(q, q->serial);  // read: the slots can go back
    }
    return q->result > 0xFFFFFFFFull ? 0xFFFFFFFFu : GLuint(q->result);
}

void deleteQuery(GLuint id) {
    Query* q = g_queries.get(id);
    if (!q) return;
    if (S.query == id) endQuery();
    retire(q, std::max(q->serial, currentSerial()));
    g_queries.remove(id);
}

void destroyQueryPools() {
    for (VkQueryPool p : s_pools) vkDestroyQueryPool(D.dev, p, nullptr);
    s_pools.clear();
    s_freeSlots.clear();
    s_retired.clear();
    s_nextSlot = 0;
}

void queriesSegmentStart() { s_openSlot = -1; }

void destroyAllObjects() {
    for (size_t i = 1; i < g_buffers.objs.size(); i++)
        if (g_buffers.objs[i]) destroyBuffer(g_buffers.objs[i]);
    for (size_t i = 1; i < g_textures.objs.size(); i++)
        if (g_textures.objs[i]) destroyImageLater(g_textures.objs[i]->img), g_textures.objs[i]->img = nullptr;
    for (size_t i = 1; i < g_renderbuffers.objs.size(); i++)
        if (g_renderbuffers.objs[i]) destroyImageLater(g_renderbuffers.objs[i]->img), g_renderbuffers.objs[i]->img = nullptr;
    for (size_t i = 1; i < g_programs.objs.size(); i++)
        if (g_programs.objs[i]) destroyProgramObjects(g_programs.objs[i]);
    destroyBlack();
    destroyReadback();
    destroyQueryPools();
}

// ------------------------------------------------------------------ sync objects
// A fence covers the commands so far, which are submitted now: it signals
// as soon as the GPU has done them, not at the end of the frame.
GLsync fenceSync() {
    Sync* s = new Sync{currentSerial()};
    submit();
    return reinterpret_cast<GLsync>(s);
}

GLenum clientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout) {
    Sync* y = reinterpret_cast<Sync*>(s);
    if (!y) return GL_WAIT_FAILED;
    if (serialDone(y->serial)) return GL_ALREADY_SIGNALED;
    if ((flags & GL_SYNC_FLUSH_COMMANDS_BIT) && y->serial > D.submitted) submit();
    if (timeout == 0) return GL_TIMEOUT_EXPIRED;
    waitSerial(y->serial);
    return GL_CONDITION_SATISFIED;
}

void deleteSync(GLsync s) { delete reinterpret_cast<Sync*>(s); }

}  // namespace glvk
