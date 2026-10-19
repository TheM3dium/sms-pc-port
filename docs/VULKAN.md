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

The backend is built in by default on Linux only for now (macOS has been tried, below; Windows not yet): `SMS_GX_VULKAN=ON ./build.sh` (CMake `-DSMS_GX_VULKAN=ON`) builds it elsewhere, `SMS_GX_VULKAN=OFF` leaves it out.
Its dependencies (Vulkan-Headers, volk, VMA, glslang) are downloaded at configure time and checked against their SHA-256, or taken from the tarballs in `-DSMS_GX_VULKAN_DEPS=dir`; when one cannot be had, configure warns and the game is built without the backend, as before it existed.
Without the backend, `renderer = vulkan` logs that and draws with OpenGL.

## Pictures

`tools/vulkan/compare.py RUN...` plays a scripted run twice, headless with the deterministic clock (as `tools/regress/regress.py` does), once with each backend, and compares the XFB captures pixel by pixel:

```sh
python3 tools/vulkan/compare.py title plaza tour --disc /path/GMSE01.iso --out /tmp/compare
python3 tools/vulkan/compare.py tour --env SMS_MSAA=4 --env SMS_GX_SCALE=2
```

`title` and `plaza` are the regression runs; `tour` is a new game into Delfino Plaza and the pilot script [`plaza-tour.txt`](../tools/vulkan/plaza-tour.txt), which runs and sweeps the camera at the airstrip and then warps through the plaza's episodes (0, 1, 2, 5, 6, 7, 8 and 9; 3 and 4 do not load from a warp with either backend).
`--out` writes OpenGL | Vulkan | difference x16 for each field.
`--against OTHER_EXE` compares OpenGL with another build's OpenGL instead (one of main, say): with the backend in, OpenGL's pictures are the same as main's in every field of these runs (with MSAA, NVIDIA's own run-to-run noise of a few pixels by one level, which main shows against itself too).

On a GTX 1060 (NVIDIA 580) every field of those runs is within 0.15% of its pixels of OpenGL's, most within 0.01%; the title's logo frames differ in 6 to 10 pixels by one level.
With 4x MSAA up to 0.8% of the pixels differ, at triangle edges (by up to 26 levels: where each API puts the samples and how it resolves them). The 0.15% holds with the HD texture pack (with `SMS_TEXTURE_PACK_SYNC=1`, which makes its loading repeatable), widescreen and 60 fps, and for every present filter (compared in the window, `SMS_WINDOW_SHOTS`).
What remains comes from the drivers' shader compilers, not from glvk: NVIDIA compiles GLSL and SPIR-V through different front ends, which round a few interpolated values the other way (Mesa's llvmpipe and lavapipe differ from each other the same way, in more pixels).

The game's timeline stays the same: the GPU reads the game depends on (pollution counters, copy write-backs, peeks) return the same values.

## Frame rate

`tools/vulkan/bench.py` plays the plaza tour in a 1280x960 window with vsync off and the deterministic clock, which runs the game as fast as it can draw, alternating backends:

```sh
python3 tools/vulkan/bench.py --disc /path/GMSE01.iso --rounds 2 --scale 1
```

| GTX 1060, Linux, frame time over the tour (loading left out) | OpenGL | Vulkan |
| --- | --- | --- |
| internal resolution 1x | 3.23 ms (310 fps) | 3.02 ms (331 fps), 6.9% faster |
| internal resolution 3x | 4.05 ms (247 fps) | 3.61 ms (277 fps), 12.2% faster |
| 1x, `frame_rate = 60` | 3.01 ms (332 fps) | 2.77 ms (361 fps), 8.5% faster |

Headless, the plaza takes 4.8 ms a frame with Vulkan and 5.0 with OpenGL.
The renderer's own timers (`SMS_GX_STATS=n`) also log, with Vulkan, the submits, GPU waits, render passes, pipelines made and the memory heaps every n frames.

Memory: about 70 MiB more than OpenGL (glslang is in the executable), and less video memory.

Smoothness: shaders compile on threads of their own (`GL_KHR_parallel_shader_compile`, which the shader warm-up uses as it does with OpenGL), and with `VK_EXT_graphics_pipeline_library` (NVIDIA, AMD, Intel and Mesa drivers have it) each program's shader parts are made on those threads right after it links, so a draw with a new state only links a pipeline, without compiling. With the caches of an earlier session, playing the tour in real time (60 fps, `SMS_GX_HITCH_MS=34`), the frames over 34 ms were the boot, loading screens and each stage's first frame (its textures arriving: 52 ms with Vulkan, 65 ms with OpenGL at the plaza) with both backends; OpenGL also spent about a second compiling at boot and 0.3 s at the plaza's first load, Vulkan none. Without any cache, both compile at boot (about a second each), and Vulkan finishes the known programs at the plaza's first load (0.6 s).

## How glvk works

- Commands go into one command buffer at a time, submitted at present, at every fence (`glFenceSync` submits what came before it, so it signals as soon as the GPU has done that) and every 200 draws (`SMS_VK_FLUSH_DRAWS`), as a GL driver flushes; a timeline semaphore counts the submissions, and anything a submission used is kept until its count has passed.
- Rendering (dynamic rendering) begins at the first draw or clear into a framebuffer and ends at anything that cannot happen inside it. Images keep their layout and last access, and each use adds the barrier GL's ordering implies.
- Texture uploads go into an upload command buffer submitted before the frame's, when the frame has not used the image yet, so they do not break rendering; a texture's image is made once its levels are known (at its first use, or at the end of the frame).
- GLSL is compiled by glslang in its "Vulkan rules relaxed" mode, made for this (loose uniforms become one std140 block); the SPIR-V's bindings are patched to one descriptor set layout every program shares (push descriptors), and its vertex inputs to `glBindAttribLocation`'s locations. Program binaries (the port's shader cache) are the SPIR-V and its reflection, in their own file (`gx-programs-vk-*.bin`); pipelines are kept in `vk-pipelines-*.bin` beside it.
- Pipelines are made per program and state (blend, colour mask, vertex layout, topology, formats); cull, depth and blend constants are dynamic. With graphics pipeline libraries they are linked from four parts: the vertex input, the program's pre-rasterisation and fragment shaders (made on its compile thread), and the fragment output; without, each is made whole.
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
| `SMS_VK_COMPILE_THREADS=n` | shader compile threads (default: the CPU's threads less one, up to 4; 0: compile at link) |
| `SMS_VK_PIPELINE_LIBRARY=0` | make every pipeline whole, without graphics pipeline libraries |
| `SMS_VK_LIBRARY=path` | the Vulkan library to load instead of the system's loader |
| `SMS_VK_PORTABILITY_TEST=1` | behave as on MoltenVK (no geometry shaders, logic ops or sampler LOD bias; float depth) on a desktop GPU |

## macOS (MoltenVK)

Tried on an Apple M4 (macOS 15.7, MoltenVK 1.4.2, x86_64 game under Rosetta), against OpenGL 4.1 (Apple's, on Metal):

- Pictures (`compare.py title plaza tour --window`): the title is identical; every plaza and tour field is within 0.4% of its pixels of OpenGL's, most within 0.02%. The most, about 0.3% in the plaza's fields from 4600, lie on horizontal bands of the floor where its mip levels meet (trilinear filtering's blend, by up to 26 levels; the same with the shaders adding the LOD bias), the rest are single pixels.
- Frame rate (`bench.py`, 1280x960): 2.18 ms (459 fps) against OpenGL's 2.33 ms (428 fps) at 1x, 7% faster; 2.48 ms (403 fps) against 3.22 ms (311 fps) at 3x, 30% faster.
- With vsync off, Metal hands out a swapchain's drawables only as the compositor lets them go, and MoltenVK waits for one (`nextDrawable`) as it encodes the blit into it, which held the game to the compositor's pace (6.2 ms a frame, whatever was drawn). glvk asks MoltenVK which presents were shown (`VK_GOOGLE_display_timing`) and does not present a frame while too many are not (the Vulkan stats count those frames), as Apple's OpenGL drops frames the compositor has no time for; about 155 frames a second still reach the 120 Hz screen. With vsync on nothing changes.

- `SMS_GX_VULKAN=ON ./build.sh` on macOS builds the backend in and fetches MoltenVK 1.4.2 (a universal `libMoltenVK.dylib`, checked against its SHA-256) into `build/deps/`, copies it beside `sms`, and puts it in `SMS.app/Contents/Frameworks`. Without `SMS_GX_VULKAN=ON`, the Mac build is as before.
- glvk loads that dylib directly (no Vulkan loader needed): `libMoltenVK.dylib` or `libvulkan.1.dylib` beside the executable or in `../Frameworks`, else `SMS_VK_LIBRARY`, else the loader. SDL is pointed at the same library before it makes the window.
- MoltenVK has no graphics pipeline libraries: pipelines are made whole there (the first time a state is drawn, as before the libraries).
- What Metal lacks, and what glvk does instead (each of these can be tried on a desktop GPU with `SMS_VK_PORTABILITY_TEST=1`, which gave the same pictures as the normal path in the runs above):
  - no geometry shaders: wide lines are drawn 1 pixel wide (the line programs lose their geometry shader and keep their outputs' names);
  - no sampler LOD bias: the fragment shaders add each texture's bias in `texture()` (push constants);
  - no 24-bit depth on Apple GPUs: 32-bit float depth, converted to GL's 24-bit values for peeks;
  - no logic ops: drawn as plain blending.
- To check on a Mac:
  1. `SMS_GX_VULKAN=ON ./build.sh` (Rosetta runs the x86_64 game and MoltenVK's x86_64 slice).
  2. `SMS_GX_BACKEND=vulkan SMS_VK_VALIDATION=1 ./run.sh` — the log should name MoltenVK (`Vulkan: using .../libMoltenVK.dylib`, then `Vulkan 1.x on Apple M...`, `portability subset`). The validation layer comes with the Vulkan SDK; without it the variable only logs that.
  3. Play the title and Delfino Plaza; compare with OpenGL in a window, since macOS has no headless OpenGL: `python3 tools/vulkan/compare.py title plaza --window` and `python3 tools/vulkan/bench.py`.
  4. Things to look at: the sun's lens flare (depth peeks with float depth), Delfino's goop counters, anything the game draws as wide lines, and the frame rate against OpenGL 4.1.
