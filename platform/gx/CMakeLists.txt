cmake_minimum_required(VERSION 3.16)
project(sms_gx C CXX)

# GX/GD for the Super Mario Sunshine PC port: OpenGL 3.3 core backend.
set(SMS_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../decomp" CACHE PATH "Path to the decompilation")
# The decomp's game headers and the SDK's (libs/dolphin/include).
set(SMS_GX_DECOMP_INCLUDE ${SMS_DECOMP_DIR}/include ${SMS_DECOMP_DIR}/libs/dolphin/include)
option(SMS_GX_BUILD_TESTS "Build the headless self-test" ON)

set(SMS_GX_SOURCES
    src/gl_loader.cpp
    src/gx_api.cpp
    src/gx_bcenc.cpp
    src/gx_fifo.cpp
    src/gx_glthread.cpp
    src/gx_hdr.cpp
    src/gx_hires.cpp
    src/gx_overlay.cpp
    src/gx_platform.cpp
    src/gx_prompts.cpp
    src/gx_render.cpp
    src/gx_settings.cpp
    src/gx_settings_extra.cpp
    src/gx_shader.cpp
    src/gx_texture.cpp
    src/gx_trace.cpp
    src/gx_vert.cpp)

# GD is compiled from the decompiled SDK sources: it only emits command bytes.
set(SMS_GX_GD_SOURCES
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDBase.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDGeometry.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDLight.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDPixel.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDTev.c
    ${SMS_DECOMP_DIR}/libs/dolphin/src/gd/GDTransform.c)
set_source_files_properties(${SMS_GX_GD_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gd_host_prefix.h;-w;-fcommon")

# Game sources must see port_include/dolphin/gx/GXVert.h before the decomp's.
set(SMS_GX_OVERRIDE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/port_include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SMS_GX_OVERRIDE_INCLUDE_DIR "${SMS_GX_OVERRIDE_INCLUDE_DIR}" PARENT_SCOPE)
endif()

# The port's dolphin/types.h override (u32 stays 32 bits on LP64 hosts), so the
# GX objects this library lays over GXTexObj etc. match the game's layout.
if(DEFINED SMS_PORT_SRC)
    set(SMS_GX_TYPES_INCLUDE ${SMS_PORT_SRC}/port_include)
endif()

add_library(sms_gx STATIC ${SMS_GX_SOURCES} ${SMS_GX_GD_SOURCES})
target_include_directories(sms_gx
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/port_include ${SMS_GX_TYPES_INCLUDE} ${SMS_GX_DECOMP_INCLUDE}
    # port_fpu.h (the SDK's float-to-unsigned conversion, port_cvt_fp2unsigned)
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
# macOS has no system GL/glcorearb.h; ship Khronos headers under third_party/.
# The GL thread's waits run the main run loop there (CoreFoundation).
if(APPLE)
    target_include_directories(sms_gx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_link_libraries(sms_gx PUBLIC "-framework CoreFoundation")
endif()
target_compile_features(sms_gx PRIVATE cxx_std_17)
# The SDK functions here that compute in floats round every operation as the
# DOL's objects do, which MWCC built without contraction: no host compiler may
# fuse a multiply-add (GCC and Clang do on arm64 by default; x86 builds
# without -mfma never fuse, so their code does not change).
target_compile_options(sms_gx PRIVATE -ffp-contract=off)
set_target_properties(sms_gx PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Window (SDL2) and headless (EGL) context creation; both optional so the
# library still builds where only a host-supplied context is used.
# On macOS x86_64, prefer a universal SDL2.framework — Homebrew's bottle is
# often arm64-only and will not link into an x86_64 binary.
set(SMS_SDL2_FRAMEWORK "" CACHE PATH "Path to SDL2.framework (universal)")
if(APPLE)
  set(_sms_sdl2_fw "")
  foreach(_cand
      "${SMS_SDL2_FRAMEWORK}"
      "${CMAKE_BINARY_DIR}/SDL2.framework"
      "${CMAKE_SOURCE_DIR}/build/deps/SDL2.framework"
      "/Library/Frameworks/SDL2.framework")
    if(_cand AND EXISTS "${_cand}/SDL2")
      set(_sms_sdl2_fw "${_cand}")
      break()
    endif()
  endforeach()
  if(_sms_sdl2_fw)
    get_filename_component(_sms_sdl2_fw_dir "${_sms_sdl2_fw}" DIRECTORY)
    add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
    # SDL.h includes <SDL2/SDL_main.h>, which resolves through -F.
    target_include_directories(SDL2::SDL2 INTERFACE "${_sms_sdl2_fw}/Headers")
    target_compile_options(SDL2::SDL2 INTERFACE "-F${_sms_sdl2_fw_dir}")
    target_link_options(SDL2::SDL2 INTERFACE "LINKER:-F${_sms_sdl2_fw_dir}" "LINKER:-framework,SDL2")
    message(STATUS "sms_gx: using SDL2.framework at ${_sms_sdl2_fw}")
    set(SMS_SDL2_FRAMEWORK_DIR "${_sms_sdl2_fw_dir}" PARENT_SCOPE)
  endif()
endif()
if(NOT TARGET SDL2::SDL2)
  find_package(SDL2 CONFIG QUIET)
endif()
find_library(SMS_GX_EGL_LIB EGL)
find_path(SMS_GX_EGL_INCLUDE EGL/egl.h)
if(TARGET SDL2::SDL2)
    target_compile_definitions(sms_gx PRIVATE SMS_GX_HAVE_SDL2=1)
    target_link_libraries(sms_gx PUBLIC SDL2::SDL2)
    message(STATUS "sms_gx: SDL2 window support")
    # The cheat menu (F9), the warp menu (F8), the quit prompt (Escape), the save state messages and extensions'
    # text in the window.
    target_sources(sms_gx PRIVATE src/gx_cheat_menu.cpp src/gx_warp_menu.cpp src/gx_quit_prompt.cpp
                   src/gx_savestate_overlay.cpp src/gx_ext_overlay.cpp)
else()
    message(STATUS "sms_gx: SDL2 not found - no window support")
endif()
if(WIN32)
    # HDR output: a Direct3D 11 swap chain shares the frame with OpenGL (gx_hdr.cpp)
    target_link_libraries(sms_gx PUBLIC d3d11 dxgi)
endif()
if(SMS_GX_EGL_LIB AND SMS_GX_EGL_INCLUDE)
    target_compile_definitions(sms_gx PRIVATE SMS_GX_HAVE_EGL=1)
    target_link_libraries(sms_gx PUBLIC ${SMS_GX_EGL_LIB})
    message(STATUS "sms_gx: EGL headless support")
else()
    message(STATUS "sms_gx: EGL not found - no headless context")
endif()

# The optional Vulkan backend (src/vk, SMS_GX_BACKEND=vulkan): the GL subset
# the renderer draws with, on Vulkan. The Vulkan loader is opened at run time
# (volk), so the game still starts where there is none and falls back to GL.
# On by default on Linux and macOS (MoltenVK), where it is tested;
# -DSMS_GX_VULKAN=ON on Windows.
# Its dependencies are downloaded at configure time (or taken from the
# tarballs in SMS_GX_VULKAN_DEPS); if one cannot be had, the game is built
# without the backend, as before it existed, and configure only warns.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    set(_sms_vulkan_default ON)
else()
    set(_sms_vulkan_default OFF)
endif()
option(SMS_GX_VULKAN "Build the optional Vulkan backend" ${_sms_vulkan_default})
set(SMS_GX_VULKAN_DEPS "" CACHE PATH "Folder with the Vulkan backend's dependency tarballs (offline builds)")

# Sets <var> to the extracted source folder of tarball <file> (sha256 <hash>)
# from <url>, or to "" when it cannot be had.
function(sms_gx_fetch var file url hash)
    set(${var} "" PARENT_SCOPE)
    set(_root ${CMAKE_BINARY_DIR}/_vkdeps)
    set(_dir ${_root}/${file}.d)
    if(EXISTS ${_dir}/.ok)
        file(GLOB _top LIST_DIRECTORIES true ${_dir}/*)
        list(FILTER _top EXCLUDE REGEX "/\\.ok$")
        set(${var} ${_top} PARENT_SCOPE)
        return()
    endif()
    set(_tar ${_root}/${file})
    if(SMS_GX_VULKAN_DEPS AND EXISTS ${SMS_GX_VULKAN_DEPS}/${file})
        set(_tar ${SMS_GX_VULKAN_DEPS}/${file})
    else()
        file(DOWNLOAD ${url} ${_tar} STATUS _status TIMEOUT 120 INACTIVITY_TIMEOUT 30)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            message(WARNING "sms_gx: could not download ${url} (${_status})")
            file(REMOVE ${_tar})
            return()
        endif()
    endif()
    file(SHA256 ${_tar} _got)
    if(NOT _got STREQUAL hash)
        message(WARNING "sms_gx: ${file} does not match its checksum")
        return()
    endif()
    file(REMOVE_RECURSE ${_dir})
    file(ARCHIVE_EXTRACT INPUT ${_tar} DESTINATION ${_dir})
    file(GLOB _top LIST_DIRECTORIES true ${_dir}/*)
    list(LENGTH _top _n)
    if(NOT _n EQUAL 1)
        message(WARNING "sms_gx: ${file} did not extract as expected")
        return()
    endif()
    file(TOUCH ${_dir}/.ok)
    if(_tar STREQUAL ${_root}/${file})
        file(REMOVE ${_tar})
    endif()
    set(${var} ${_top} PARENT_SCOPE)
endfunction()

if(SMS_GX_VULKAN)
    sms_gx_fetch(_vk_headers vulkan-headers-1.3.296.tar.gz
        https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.296.tar.gz
        e204e0b3c19f622d197df945737f5db913d6621830999b8578d34e80a7c90585)
    sms_gx_fetch(_vk_volk volk-1.3.296.0.tar.gz
        https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
        8ffd0e81e29688f4abaa39e598937160b098228f37503903b10d481d4862ab85)
    sms_gx_fetch(_vk_vma vma-3.1.0.tar.gz
        https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
        ae134ecc37c55634f108e926f85d5d887b670360e77cd107affaf3a9539595f2)
    sms_gx_fetch(_vk_glslang glslang-15.0.0.tar.gz
        https://github.com/KhronosGroup/glslang/archive/refs/tags/15.0.0.tar.gz
        c31c8c2e89af907507c0631273989526ee7d5cdf7df95ececd628fd7b811e064)
    if(NOT _vk_headers OR NOT _vk_volk OR NOT _vk_vma OR NOT _vk_glslang)
        message(WARNING "sms_gx: building without the Vulkan backend (its dependencies could not be had); the game draws with OpenGL")
        set(SMS_GX_VULKAN OFF)
    endif()
endif()
if(SMS_GX_VULKAN)
    # glslang: the GLSL front end and SPIR-V back end only
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ALLOW_EXTERNAL_SPIRV_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(_sms_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    if(NOT TARGET glslang)
        add_subdirectory(${_vk_glslang} ${CMAKE_BINARY_DIR}/_vkdeps/glslang-build EXCLUDE_FROM_ALL)
    endif()
    set(BUILD_SHARED_LIBS ${_sms_shared})
    target_sources(sms_gx PRIVATE src/vk/glvk_device.cpp src/vk/glvk_draw.cpp src/vk/glvk_objects.cpp
                   src/vk/glvk_shader.cpp src/vk/glvk_vma.cpp ${_vk_volk}/volk.c)
    target_include_directories(sms_gx PRIVATE src/vk ${_vk_headers}/include ${_vk_volk}
                               ${_vk_vma}/include ${_vk_glslang})
    target_compile_definitions(sms_gx PRIVATE SMS_GX_HAVE_VULKAN=1 VK_NO_PROTOTYPES=1)
    target_link_libraries(sms_gx PRIVATE glslang SPIRV glslang-default-resource-limits ${CMAKE_DL_LIBS})
    # third-party code keeps its own warnings
    set_source_files_properties(src/vk/glvk_vma.cpp ${_vk_volk}/volk.c PROPERTIES COMPILE_OPTIONS "-w")
    message(STATUS "sms_gx: Vulkan backend (SMS_GX_BACKEND=vulkan)")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sms_gx PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-function -fno-strict-aliasing>)
endif()

if(SMS_GX_BUILD_TESTS)
    # GXDrawSphere calls MSL's sinf and cosf, which the port links from
    # platform/misc/msl_math.c; the standalone tests link it themselves.
    # Built as the port builds it (the top-level CMakeLists.txt): SSE maths on
    # x86, never x87 extended precision, and no contraction. Source file
    # properties are per directory, so the tests' copy needs its own.
    set(SMS_GX_MSL_MATH ${CMAKE_CURRENT_SOURCE_DIR}/../misc/msl_math.c)
    # The cheat menu reads the port_cheat_* flags and the warp menu the warp
    # list, which the port defines (platform/misc).
    set(SMS_GX_PORT_CHEATS ${CMAKE_CURRENT_SOURCE_DIR}/../misc/port_cheats.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../misc/port_warp.cpp)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../misc/port_warp.cpp PROPERTIES
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../src/port_include)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$" OR CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(_sms_gx_msl_opts "-msse2;-mfpmath=sse;-ffp-contract=off")
    else()
        set(_sms_gx_msl_opts "-ffp-contract=off")
    endif()
    set_source_files_properties(${SMS_GX_MSL_MATH} PROPERTIES COMPILE_OPTIONS "${_sms_gx_msl_opts}"
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    add_executable(gx_selftest tests/gx_selftest.cpp ${SMS_GX_MSL_MATH} ${SMS_GX_PORT_CHEATS})
    target_include_directories(gx_selftest PRIVATE ${SMS_GX_DECOMP_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(APPLE)
        target_include_directories(gx_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    endif()
    target_link_libraries(gx_selftest PRIVATE sms_gx)
    target_compile_features(gx_selftest PRIVATE cxx_std_17)
    enable_testing()
    add_test(NAME gx_selftest COMMAND gx_selftest --headless)
    set_tests_properties(gx_selftest PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(gx_hires_test tests/gx_hires_test.cpp ${SMS_GX_MSL_MATH} ${SMS_GX_PORT_CHEATS})
    target_include_directories(gx_hires_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gx_hires_test PRIVATE sms_gx)
    target_compile_features(gx_hires_test PRIVATE cxx_std_17)
    add_test(NAME gx_hires_test COMMAND gx_hires_test)
    add_test(NAME gx_hires_fallback_test COMMAND gx_hires_test --fallback)
    add_test(NAME gx_hires_sync_test COMMAND gx_hires_test --sync)
    add_test(NAME gx_hires_no_preload_test COMMAND gx_hires_test --no-preload)
    add_test(NAME gx_hires_all_test COMMAND gx_hires_test --all)
    set_tests_properties(gx_hires_test gx_hires_fallback_test gx_hires_sync_test gx_hires_no_preload_test
        gx_hires_all_test PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(gx_windowtest tests/gx_windowtest.cpp)
    target_include_directories(gx_windowtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(gx_windowtest PRIVATE cxx_std_17)
    add_test(NAME gx_windowtest COMMAND gx_windowtest)
    if(TARGET SDL2::SDL2)
        add_executable(gx_borderlesstest tests/gx_borderlesstest.cpp)
        target_include_directories(gx_borderlesstest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(gx_borderlesstest PRIVATE cxx_std_17)
        target_link_libraries(gx_borderlesstest PRIVATE SDL2::SDL2)
        add_executable(gx_windowsmoke tests/gx_windowtest.cpp ${SMS_GX_MSL_MATH} ${SMS_GX_PORT_CHEATS})
        target_include_directories(gx_windowsmoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_features(gx_windowsmoke PRIVATE cxx_std_17)
        target_compile_definitions(gx_windowsmoke PRIVATE SMS_GX_HAVE_SDL2=1)
        target_link_libraries(gx_windowsmoke PRIVATE sms_gx)
    endif()
endif()
