# The Vulkan backend

`renderer = vulkan` in [`settings.txt`](../settings.txt) (or `SMS_GX_BACKEND=vulkan`, or `--vulkan` after the image) draws the game with Vulkan instead of OpenGL.
OpenGL stays the default; when Vulkan cannot start (no driver, too old a device), the game says why in its log and starts with OpenGL.

It is the same renderer either way.
`platform/gx` speaks the OpenGL 3.3 subset in [`gl_funcs.h`](../platform/gx/src/gl_funcs.h) and fetches every entry point through one get-proc function; with Vulkan, the platform layer hands it `glvk::getProc` (`platform/gx/src/vk/`), which implements that subset on Vulkan.
The GX state, EFB copies and their write-backs, peeks, pixel metrics, the texture packs, the GL thread, post-processing (FXAA, FSR 1, NIS, sharpening) and the overlays are shared code, so both backends draw the same pictures and every renderer change reaches both.

## What it needs

Vulkan 1.1 or newer with dynamic rendering, push descriptors and timeline semaphores, which every Vulkan 1.3 driver has (NVIDIA, AMD and Intel on Linux and Windows, Mesa's drivers, MoltenVK).
Extended dynamic state and `VK_EXT_depth_clip_control` are used when present.
The Vulkan loader is opened at run time (volk), so the game still starts on a system without one.

## Pictures

`tools/vulkan/compare.py RUN...` plays a scripted run twice, headless with the deterministic clock (as `tools/regress/regress.py` does), once with each backend, and compares the XFB captures pixel by pixel:

```sh
python3 tools/vulkan/compare.py title plaza tour --disc /path/GMSE01.iso --out /tmp/compare
python3 tools/vulkan/compare.py tour --env SMS_MSAA=4 --env SMS_GX_SCALE=2
```

`title` and `plaza` are the regression runs; `tour` is a new game into Delfino Plaza and the pilot script [`plaza-tour.txt`](../tools/vulkan/plaza-tour.txt), which runs and sweeps the camera at the airstrip and then warps through the plaza's episodes (0, 1, 2, 5, 6, 7, 8 and 9; 3 and 4 do not load from a warp with either backend).
`--out` writes OpenGL | Vulkan | difference x16 for each field.

On a GTX 1060 (NVIDIA 580) every field of those runs is within 0.15% of its pixels of OpenGL's, most within 0.01%; the title's logo frames differ in 6 to 10 pixels by one level.
The same holds with 4x MSAA, the HD texture pack (with `SMS_TEXTURE_PACK_SYNC=1`, which makes its loading repeatable), widescreen and 60 fps, and for every present filter (compared in the window, `SMS_WINDOW_SHOTS`).
What remains comes from the drivers' shader compilers, not from glvk: NVIDIA compiles GLSL and SPIR-V through different front ends, which round a few interpolated values the other way (Mesa's llvmpipe and lavapipe differ from each other the same way, in more pixels).
Edge pixels with MSAA follow from where each API puts the samples.

The game's timeline stays the same: the GPU reads the game depends on (pollution counters, copy write-backs, peeks) return the same values.

## Frame rate

`tools/vulkan/bench.py` plays the plaza tour in a 1280x960 window with vsync off and the deterministic clock, which runs the game as fast as it can draw, alternating backends:

```sh
python3 tools/vulkan/bench.py --disc /path/GMSE01.iso --rounds 2 --scale 1
```

| GTX 1060, Linux, frame time over the tour (loading left out) | OpenGL | Vulkan |
| --- | --- | --- |
| internal resolution 1x | 3.28 ms (305 fps) | 3.01 ms (332 fps), 8.8% faster |
| internal resolution 3x | 3.90 ms (256 fps) | 3.64 ms (274 fps), 7.0% faster |

Headless, the plaza takes 4.8 ms a frame with Vulkan and 5.0 with OpenGL.
The renderer's own timers (`SMS_GX_STATS=n`) also log, with Vulkan, the submits, GPU waits, render passes, pipelines made and the memory heaps every n frames.

Memory: about 70 MiB more than OpenGL (glslang is in the executable), and less video memory.

## How glvk works

- Commands go into one command buffer at a time, submitted at present, at every fence (`glFenceSync` submits what came before it, so it signals as soon as the GPU has done that) and every 200 draws (`SMS_VK_FLUSH_DRAWS`), as a GL driver flushes; a timeline semaphore counts the submissions, and anything a submission used is kept until its count has passed.
- Rendering (dynamic rendering) begins at the first draw or clear into a framebuffer and ends at anything that cannot happen inside it. Images keep their layout and last access, and each use adds the barrier GL's ordering implies.
- Texture uploads go into an upload command buffer submitted before the frame's, when the frame has not used the image yet, so they do not break rendering; a texture's image is made once its levels are known (at its first use, or at the end of the frame).
- GLSL is compiled by glslang in its "Vulkan rules relaxed" mode, made for this (loose uniforms become one std140 block); the SPIR-V's bindings are patched to one descriptor set layout every program shares (push descriptors), and its vertex inputs to `glBindAttribLocation`'s locations. Program binaries (the port's shader cache) are the SPIR-V and its reflection, in their own file (`gx-programs-vk-*.bin`); pipelines are kept in `vk-pipelines-*.bin` beside it.
- Pipelines are made per program and state (blend, colour mask, vertex layout, topology, formats); cull, depth and blend constants are dynamic.
- GL's clip-space depth is kept with `VK_EXT_depth_clip_control` (else the vertex shaders map it). GL's window coordinates and Vulkan's framebuffer ones are the same rows in memory, so nothing is flipped except at present; GL's front faces are Vulkan's clockwise ones.
- Masked clears are drawn (`vkCmdClearAttachments` ignores the colour mask). GL queries are runs of Vulkan occlusion queries, one per render pass they span.
- With MSAA, colour resolves with `vkCmdResolveImage` and depth through an empty render pass that resolves sample 0.

## Variables

| Variable | Effect |
| --- | --- |
| `SMS_GX_BACKEND=vulkan` | the Vulkan backend (`renderer = vulkan`); `--vulkan` / `--opengl` after the image |
| `SMS_VK_DEVICE=n` or a name | the GPU, by index or part of its name (default: a discrete one) |
| `SMS_VK_VALIDATION=1` | the Khronos validation layer (when installed), its messages in the log |
| `SMS_VK_FRAMES=n` | frames in flight, 1 to 4 (default 2) |
| `SMS_VK_FLUSH_DRAWS=n` | submit every n draws (default 200; 0: only at fences and presents) |
| `SMS_VK_LIBRARY=path` | the Vulkan library to load instead of the system's loader |
| `SMS_VK_PORTABILITY_TEST=1` | behave as on MoltenVK (no geometry shaders, logic ops or sampler LOD bias; float depth) on a desktop GPU |

## macOS (MoltenVK)

The groundwork is in; it still has to be built and tried on a Mac.

- `./build.sh` on macOS fetches MoltenVK 1.4.2 (a universal `libMoltenVK.dylib`, checked against its SHA-256) into `build/deps/`, copies it beside `sms`, and puts it in `SMS.app/Contents/Frameworks`. `SMS_GX_VULKAN=OFF ./build.sh` leaves the backend out.
- glvk loads that dylib directly (no Vulkan loader needed): `libMoltenVK.dylib` or `libvulkan.1.dylib` beside the executable or in `../Frameworks`, else `SMS_VK_LIBRARY`, else the loader. SDL is pointed at the same library before it makes the window.
- What Metal lacks, and what glvk does instead (each of these can be tried on a desktop GPU with `SMS_VK_PORTABILITY_TEST=1`, which gave the same pictures as the normal path in the runs above):
  - no geometry shaders: wide lines are drawn 1 pixel wide (the line programs lose their geometry shader and keep their outputs' names);
  - no sampler LOD bias: the fragment shaders add each texture's bias in `texture()` (push constants);
  - no 24-bit depth on Apple GPUs: 32-bit float depth, converted to GL's 24-bit values for peeks;
  - no logic ops: drawn as plain blending.
- To check on a Mac:
  1. `./build.sh` (Rosetta runs the x86_64 game and MoltenVK's x86_64 slice).
  2. `SMS_GX_BACKEND=vulkan SMS_VK_VALIDATION=1 ./run.sh` — the log should name MoltenVK (`Vulkan: using .../libMoltenVK.dylib`, then `Vulkan 1.x on Apple M...`, `portability subset`). The validation layer comes with the Vulkan SDK; without it the variable only logs that.
  3. Play the title and Delfino Plaza; compare with OpenGL in a window, since macOS has no headless OpenGL: `python3 tools/vulkan/compare.py title plaza --window` and `python3 tools/vulkan/bench.py`.
  4. Things to look at: the sun's lens flare (depth peeks with float depth), Delfino's goop counters, anything the game draws as wide lines, and the frame rate against OpenGL 4.1.
