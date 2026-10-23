// The radar's stage maps (src/radar.cpp): top-down pictures of each stage,
// made from the disc by tools/radar/gen_maps.py.
//
// The folder (SMS_RADAR_DIR, else assets/maps beside or two folders above
// the working directory, as the game runs from build/<os>-<arch>/) holds
// maps.txt, one line per map:
//     <col key> <file> <x0> <z0> <pixels per unit> <width> <height> <scenes>
// Each folder in mods/maps (beside or two folders above, the same way) is a
// pack of maps for a mod's stages in the same form, made from that mod's disc
// (Super Mario Eclipse's: tools/mods/get.py eclipse installs it in
// mods/maps/eclipse); the keys tell the stages apart, so all are read.
// The game hashes the stage's loaded collision (/scene/map/map.col) and asks
// for that map; the picture is decoded on a thread of its own, so a stage
// never waits for it, and only the last one asked for is kept, with an
// overview of it (the whole picture at most 1536 pixels on a side, each
// pixel the average of those under it) for the full map.
#include "gx_internal.h"

#include <algorithm>
#if defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 101500
// macOS before 10.15 has no std::filesystem: subfolders() uses POSIX.
#define SMS_RADAR_POSIX_FS 1
#include <dirent.h>
#include <sys/stat.h>
#else
#include <filesystem>
#endif
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third_party/stb_image.h"

// as src/port_include/port_radar.h
struct PortRadarMap {
    const uint8_t* rgba;  // row-major RGBA, w x h
    int w, h;
    float x0, z0;   // world x and z of the picture's top left corner
    float density;  // pixels per world unit
    const uint8_t* overview;  // the whole picture smaller, for the full map: row-major RGBA
    int ow, oh;
};

namespace {

struct Entry {
    unsigned long long key;
    std::string path;  // the picture
    float x0, z0, density;
};

std::once_flag s_once;
std::vector<Entry> s_entries;

std::mutex s_lock;
unsigned long long s_want;  // the map asked for last
unsigned long long s_have;  // the map in s_pixels (or that failed)
const Entry* s_haveEntry;   // null if it failed
std::vector<uint8_t> s_pixels;
int s_w, s_h;
std::vector<uint8_t> s_overview;  // at most kOverview on its longest side
int s_ow, s_oh;
const int kOverview = 1536;
bool s_loading;

// The names of base's folders; false if base is not a folder.
bool subfolders(const std::string& base, std::vector<std::string>* names) {
#if SMS_RADAR_POSIX_FS
    DIR* d = opendir(base.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d)) {
        struct stat st;
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
            stat((base + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            names->push_back(e->d_name);
    }
    closedir(d);
    return true;
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return false;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) names->push_back(it->path().filename().string());
    return true;
#endif
}

bool readable(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

// The maps listed in dir/maps.txt; how many.
size_t readManifest(const std::string& dir) {
    FILE* f = fopen((dir + "/maps.txt").c_str(), "r");
    if (!f) return 0;
    size_t n = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char file[256];
        unsigned long long key;
        int w, h;
        Entry e;
        if (sscanf(line, "%llx %255s %f %f %f %d %d", &key, file, &e.x0, &e.z0, &e.density, &w, &h) != 7) continue;
        e.key = key;
        e.path = dir + "/" + file;
        s_entries.push_back(e);
        n++;
    }
    fclose(f);
    return n;
}

void init() {
    const char* env = getenv("SMS_RADAR_DIR");
    const char* dirs[] = {env, "assets/maps", "../../assets/maps"};
    bool found = false;
    for (const char* d : dirs) {
        if (d && *d && readable(std::string(d) + "/maps.txt")) {
            gx::logmsg("radar: %zu stage maps in %s", readManifest(d), d);
            found = true;
            break;
        }
    }
    if (!found) gx::logmsg("radar: no stage maps (assets/maps/maps.txt; SMS_RADAR_DIR sets the folder)");
    // the mods' packs, from the first mods/maps there is
    for (const char* base : {"mods/maps", "../../mods/maps"}) {
        std::vector<std::string> packs;
        if (!subfolders(base, &packs)) continue;
        std::sort(packs.begin(), packs.end());
        for (const std::string& p : packs) {
            const std::string dir = std::string(base) + "/" + p;
            if (const size_t n = readManifest(dir)) gx::logmsg("radar: %zu stage maps in %s", n, dir.c_str());
        }
        break;
    }
}

const Entry* find(unsigned long long key) {
    for (const Entry& e : s_entries)
        if (e.key == key) return &e;
    return nullptr;
}

void load(const Entry* e) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load(e->path.c_str(), &w, &h, &n, 4);
    std::lock_guard<std::mutex> g(s_lock);
    s_loading = false;
    if (s_want != e->key) {  // another stage by now: that one starts on the next ask
        if (px) stbi_image_free(px);
        return;
    }
    s_have = e->key;
    if (!px) {
        gx::logmsg("radar: cannot read %s", e->path.c_str());
        s_haveEntry = nullptr;
        return;
    }
    s_pixels.assign(px, px + size_t(w) * h * 4);
    s_w = w;
    s_h = h;
    s_haveEntry = e;
    stbi_image_free(px);
    // the overview: each of its pixels the average of the picture's under it
    const int side = w > h ? w : h;
    s_ow = side > kOverview ? int(int64_t(w) * kOverview / side) : w;
    s_oh = side > kOverview ? int(int64_t(h) * kOverview / side) : h;
    s_ow = s_ow < 1 ? 1 : s_ow;
    s_oh = s_oh < 1 ? 1 : s_oh;
    s_overview.assign(size_t(s_ow) * s_oh * 4, 0);
    for (int oy = 0; oy < s_oh; oy++) {
        const int y0 = int(int64_t(oy) * h / s_oh), y1 = std::max(y0 + 1, int(int64_t(oy + 1) * h / s_oh));
        for (int ox = 0; ox < s_ow; ox++) {
            const int x0 = int(int64_t(ox) * w / s_ow), x1 = std::max(x0 + 1, int(int64_t(ox + 1) * w / s_ow));
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int y = y0; y < y1; y++) {
                const uint8_t* p = &s_pixels[(size_t(y) * w + x0) * 4];
                for (int x = x0; x < x1; x++, p += 4)
                    for (int k = 0; k < 4; k++) sum[k] += p[k];
            }
            const uint32_t n = uint32_t((y1 - y0) * (x1 - x0));
            uint8_t* o = &s_overview[(size_t(oy) * s_ow + ox) * 4];
            for (int k = 0; k < 4; k++) o[k] = uint8_t((sum[k] + n / 2) / n);
        }
    }
}

}  // namespace

// 1 with the stage's map in *out, 0 while it loads, -1 with no map for it.
// The pixels stay until another map is asked for.
extern "C" int port_radar_map(unsigned long long colKey, PortRadarMap* out) {
    std::call_once(s_once, init);
    const Entry* e = find(colKey);
    if (!e) return -1;
    std::lock_guard<std::mutex> g(s_lock);
    if (s_have == colKey) {
        if (!s_haveEntry) return -1;
        out->rgba = s_pixels.data();
        out->w = s_w;
        out->h = s_h;
        out->x0 = s_haveEntry->x0;
        out->z0 = s_haveEntry->z0;
        out->density = s_haveEntry->density;
        out->overview = s_overview.data();
        out->ow = s_ow;
        out->oh = s_oh;
        return 1;
    }
    s_want = colKey;
    if (!s_loading) {  // one decode at a time
        s_loading = true;
        s_have = 0;
        s_haveEntry = nullptr;
        s_pixels.clear();
        s_pixels.shrink_to_fit();
        s_overview.clear();
        s_overview.shrink_to_fit();
        std::thread(load, e).detach();
    }
    return 0;
}
