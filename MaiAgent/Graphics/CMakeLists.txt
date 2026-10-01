# OBS uses SIMDe as a header-only imported target. Keep that include path local
# and reproducible instead of depending on an OBS installation or the host SDK.
add_library(maiagent_graphics_simde INTERFACE)
target_include_directories(maiagent_graphics_simde INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simde/include)

set(obs_graphics_source ${CMAKE_CURRENT_SOURCE_DIR})
if(IOS)
  set(obs_graphics_imports_source ${CMAKE_CURRENT_SOURCE_DIR}/compat/MaiMetalStaticImports.c)
else()
  set(obs_graphics_imports_source ${obs_graphics_source}/graphics-imports.c)
endif()
add_library(maiagent_obs_graphics_compile OBJECT
  ${obs_graphics_source}/effect-parser.c
  ${obs_graphics_source}/effect.c
  ${obs_graphics_imports_source}
  ${obs_graphics_source}/graphics.c
  ${obs_graphics_source}/image-file.c
  ${obs_graphics_source}/libnsgif/gif.c
  ${obs_graphics_source}/libnsgif/lzw.c
  ${obs_graphics_source}/shader-parser.c
  ${obs_graphics_source}/texture-render.c
)
target_include_directories(maiagent_obs_graphics_compile PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(maiagent_obs_graphics_compile PRIVATE maiagent_graphics_simde)
# Keep the upstream sources separate from the public link target so the same
# objects can also serve as a compile check on platforms without a backend.

file(GLOB obs_effect_files CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/data/*.effect)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/data/.staged
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/data
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${obs_effect_files} ${CMAKE_CURRENT_BINARY_DIR}/data
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/data/.staged
  DEPENDS ${obs_effect_files}
  VERBATIM)
add_custom_target(maiagent_graphics_data ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/data/.staged)

if(APPLE AND NOT IOS)
  set(ffmpeg_source ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ffmpeg)
  set(ffmpeg_runtime_dir ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-runtime)
  set(ffmpeg_runtime_script
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg-runtime.sh)
  set(ffmpeg_runtime_archives
    ${ffmpeg_runtime_dir}/libmaifftools.a
    ${ffmpeg_runtime_dir}/libavdevice/libavdevice.a
    ${ffmpeg_runtime_dir}/libavfilter/libavfilter.a
    ${ffmpeg_runtime_dir}/libavformat/libavformat.a
    ${ffmpeg_runtime_dir}/libavcodec/libavcodec.a
    ${ffmpeg_runtime_dir}/libswresample/libswresample.a
    ${ffmpeg_runtime_dir}/libswscale/libswscale.a
    ${ffmpeg_runtime_dir}/libavutil/libavutil.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libdav1d.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libx264.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libmp3lame.a)
  add_custom_command(OUTPUT ${ffmpeg_runtime_archives}
    ${ffmpeg_runtime_dir}/install/include/libavutil/avconfig.h
    COMMAND bash ${ffmpeg_runtime_script} ${ffmpeg_source} ${ffmpeg_runtime_dir}
      ${CMAKE_C_COMPILER} ${CMAKE_SYSTEM_PROCESSOR} ${CMAKE_AR} ${CMAKE_RANLIB}
    DEPENDS ${ffmpeg_source}/configure ${ffmpeg_runtime_script}
      ${ffmpeg_source}/../build-media-codecs.sh
      ${ffmpeg_source}/fftools/Makefile
      ${ffmpeg_source}/fftools/ffmpeg.c ${ffmpeg_source}/fftools/ffprobe.c
      ${ffmpeg_source}/fftools/cmdutils.c ${ffmpeg_source}/fftools/opt_common.c
      ${ffmpeg_source}/fftools/mai_fftools_embed.h
      ${ffmpeg_source}/fftools/mai_fftools_log.c
      ${ffmpeg_source}/../dav1d/meson.build
      ${ffmpeg_source}/../x264/configure
      ${ffmpeg_source}/../lame/configure
    VERBATIM)
  add_custom_target(maiagent_ffmpeg_runtime
    DEPENDS ${ffmpeg_runtime_archives}
      ${ffmpeg_runtime_dir}/install/include/libavutil/avconfig.h)
  add_library(maiagent_embedded_fftools STATIC IMPORTED GLOBAL)
  set_target_properties(maiagent_embedded_fftools PROPERTIES
    IMPORTED_LOCATION ${ffmpeg_runtime_dir}/libmaifftools.a)
  add_dependencies(maiagent_embedded_fftools maiagent_ffmpeg_runtime)
  target_link_libraries(maiagent_embedded_fftools INTERFACE
    ${ffmpeg_runtime_dir}/libavdevice/libavdevice.a
    ${ffmpeg_runtime_dir}/libavfilter/libavfilter.a
    ${ffmpeg_runtime_dir}/libavformat/libavformat.a
    ${ffmpeg_runtime_dir}/libavcodec/libavcodec.a
    ${ffmpeg_runtime_dir}/libswresample/libswresample.a
    ${ffmpeg_runtime_dir}/libswscale/libswscale.a
    ${ffmpeg_runtime_dir}/libavutil/libavutil.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libdav1d.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libx264.a
    ${ffmpeg_runtime_dir}/vendor-install/lib/libmp3lame.a
    z
    "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMedia"
    "-framework CoreVideo" "-framework VideoToolbox")
  target_sources(maiagent_obs_graphics_compile PRIVATE
    ${obs_graphics_source}/graphics-ffmpeg.c)
  target_include_directories(maiagent_obs_graphics_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${ffmpeg_runtime_dir}/install/include)
  add_dependencies(maiagent_obs_graphics_compile maiagent_ffmpeg_runtime)
endif()

# Link the original OBS vector/matrix implementations so the SIMDe path is
# exercised on ARM at runtime, not merely accepted by the compiler.
add_library(maiagent_obs_math STATIC
  ${obs_graphics_source}/axisang.c
  ${obs_graphics_source}/bounds.c
  ${obs_graphics_source}/math-extra.c
  ${obs_graphics_source}/matrix3.c
  ${obs_graphics_source}/matrix4.c
  ${obs_graphics_source}/plane.c
  ${obs_graphics_source}/quat.c
  ${obs_graphics_source}/vec2.c
  ${obs_graphics_source}/vec3.c
  ${obs_graphics_source}/vec4.c)
target_include_directories(maiagent_obs_math PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(maiagent_obs_math PRIVATE maiagent_graphics_simde)

# The standalone runtime uses OBS util's lexer, allocator, file and platform
# implementations. platform-nix.c includes obsconfig.h without using it on
# these targets, so provide a generated empty header.
if(APPLE OR ANDROID)
  if(APPLE AND NOT IOS)
    enable_language(OBJC)
  endif()
  set(obs_util_source ${CMAKE_CURRENT_SOURCE_DIR}/util)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/obsconfig.h")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/obsconfig.h
      "/* OBS platform-nix compatibility for the MaiAgent Graphics subset. */\n")
  endif()
  add_library(maiagent_obs_util STATIC
    ${obs_util_source}/base.c
    ${obs_util_source}/bmem.c
    ${obs_util_source}/cf-lexer.c
    ${obs_util_source}/cf-parser.c
    ${obs_util_source}/dstr.c
    ${obs_util_source}/lexer.c
    ${obs_util_source}/platform.c
    ${obs_util_source}/platform-nix.c
    ${obs_util_source}/utf8.c)
  if(IOS)
    target_sources(maiagent_obs_util PRIVATE compat/MaiObsIosPlatform.c)
  elseif(APPLE)
    target_sources(maiagent_obs_util PRIVATE
      ${obs_util_source}/platform-cocoa.m)
    target_link_libraries(maiagent_obs_util PRIVATE
      "-framework Cocoa" "-framework CoreServices" "-framework IOKit")
  endif()
  target_compile_definitions(maiagent_obs_util PRIVATE MAI_GRAPHICS_STANDALONE)
  target_include_directories(maiagent_obs_util PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(WIN32)
  set(obs_pthreads_source ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/w32-pthreads)
  add_library(maiagent_obs_pthreads SHARED ${obs_pthreads_source}/pthread.c)
  target_compile_definitions(maiagent_obs_pthreads PRIVATE __CLEANUP_C PTW32_BUILD)
  target_include_directories(maiagent_obs_pthreads PUBLIC ${obs_pthreads_source})

  if(NOT MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "Windows Graphics requires MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR")
  endif()
  target_sources(maiagent_obs_graphics_compile PRIVATE
    ${obs_graphics_source}/graphics-ffmpeg.c)
  target_include_directories(maiagent_obs_graphics_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR}
    ${obs_pthreads_source})

  set(obs_util_source ${CMAKE_CURRENT_SOURCE_DIR}/util)
  add_library(maiagent_obs_util STATIC
    ${obs_util_source}/base.c
    ${obs_util_source}/bmem.c
    ${obs_util_source}/cf-lexer.c
    ${obs_util_source}/cf-parser.c
    ${obs_util_source}/dstr.c
    ${obs_util_source}/lexer.c
    ${obs_util_source}/platform.c
    ${obs_util_source}/platform-windows.c
    ${obs_util_source}/threading-windows.c
    ${obs_util_source}/utf8.c)
  target_compile_definitions(maiagent_obs_util PRIVATE
    MAI_GRAPHICS_STANDALONE UNICODE _UNICODE)
  target_include_directories(maiagent_obs_util PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${obs_pthreads_source})
  target_link_libraries(maiagent_obs_util PRIVATE
    maiagent_obs_pthreads winmm shell32 shlwapi psapi rpcrt4)

  add_library(maiagent_graphics SHARED
    $<TARGET_OBJECTS:maiagent_obs_graphics_compile>
    compat/MaiObsStandalone.c)
  add_library(MaiAgent::Graphics ALIAS maiagent_graphics)
  target_include_directories(maiagent_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR})
  target_include_directories(maiagent_graphics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat ${obs_pthreads_source})
  target_link_libraries(maiagent_graphics PUBLIC
    maiagent_obs_math maiagent_obs_util maiagent_graphics_simde
    maiagent_obs_pthreads Threads::Threads
    PRIVATE d3d11 dxgi d3dcompiler)
  add_dependencies(maiagent_graphics maiagent_graphics_data)

  add_library(maiagent_obs_d3d11 MODULE
    d3d11/d3d11-duplicator.cpp
    d3d11/d3d11-indexbuffer.cpp
    d3d11/d3d11-rebuild.cpp
    d3d11/d3d11-samplerstate.cpp
    d3d11/d3d11-shader.cpp
    d3d11/d3d11-shaderprocessor.cpp
    d3d11/d3d11-stagesurf.cpp
    d3d11/d3d11-subsystem.cpp
    d3d11/d3d11-texture2d.cpp
    d3d11/d3d11-texture3d.cpp
    d3d11/d3d11-vertexbuffer.cpp
    d3d11/d3d11-zstencilbuffer.cpp)
  target_include_directories(maiagent_obs_d3d11 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${obs_pthreads_source})
  target_compile_definitions(maiagent_obs_d3d11 PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 _AMD64_)
  target_link_libraries(maiagent_obs_d3d11 PRIVATE
    maiagent_graphics d3d9 d3d11 d3dcompiler dxgi shcore)
endif()

if(APPLE AND NOT IOS)
  add_library(maiagent_graphics SHARED
    $<TARGET_OBJECTS:maiagent_obs_graphics_compile>
    compat/MaiObsStandalone.c)
  add_library(MaiAgent::Graphics ALIAS maiagent_graphics)
  target_include_directories(maiagent_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${ffmpeg_runtime_dir}/install/include)
  target_include_directories(maiagent_graphics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  target_link_libraries(maiagent_graphics PUBLIC
    maiagent_obs_math maiagent_obs_util maiagent_graphics_simde Threads::Threads
    PRIVATE ${ffmpeg_runtime_archives}
      z
      "-framework AudioToolbox" "-framework CoreMedia"
      "-framework CoreVideo" "-framework VideoToolbox")
  add_dependencies(maiagent_graphics maiagent_ffmpeg_runtime)
  add_dependencies(maiagent_graphics maiagent_graphics_data)

  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    find_program(maiagent_swiftc swiftc REQUIRED)
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE maiagent_macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB metal_sources CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/metal/*.swift)
    set(metal_module ${CMAKE_CURRENT_BINARY_DIR}/libmaiagent_obs_metal.so)
    add_custom_command(OUTPUT ${metal_module}
      COMMAND ${maiagent_swiftc}
        -target arm64-apple-macos13.0 -sdk ${maiagent_macos_sdk}
        -emit-library -module-name MaiAgentObsMetal
        -import-objc-header ${CMAKE_CURRENT_SOURCE_DIR}/metal/libobs-metal-Bridging-Header.h
        -I ${CMAKE_CURRENT_SOURCE_DIR}
        -Xcc -I${CMAKE_CURRENT_SOURCE_DIR}/compat
        -Xcc -I${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simde/include
        -L $<TARGET_FILE_DIR:maiagent_graphics> -lmaiagent_graphics
        -Xlinker -rpath -Xlinker @loader_path
        -framework AppKit -framework Metal -framework CoreVideo -framework QuartzCore
        -o ${metal_module} ${metal_sources}
      DEPENDS maiagent_graphics ${metal_sources}
        ${CMAKE_CURRENT_SOURCE_DIR}/metal/libobs-metal-Bridging-Header.h
      VERBATIM)
    add_custom_target(maiagent_obs_metal_build ALL DEPENDS ${metal_module})
    add_library(maiagent_obs_metal MODULE IMPORTED GLOBAL)
    set_target_properties(maiagent_obs_metal PROPERTIES IMPORTED_LOCATION ${metal_module})
    add_dependencies(maiagent_obs_metal maiagent_obs_metal_build)
  endif()

endif()

if(IOS)
  if(NOT MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "iOS Graphics requires MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR")
  endif()
  target_sources(maiagent_obs_graphics_compile PRIVATE
    ${obs_graphics_source}/graphics-ffmpeg.c)
  target_include_directories(maiagent_obs_graphics_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR})
  target_compile_definitions(maiagent_obs_graphics_compile PRIVATE
    MAI_GRAPHICS_STATIC_METAL)

  set(mai_ios_swift_target "${CMAKE_OSX_ARCHITECTURES}-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    string(APPEND mai_ios_swift_target "-simulator")
  endif()
  string(REGEX REPLACE "(^|[ \t])-target[ \t]+[^ \t]+" ""
    mai_ios_swift_flags "${CMAKE_Swift_FLAGS}")
  string(STRIP "${mai_ios_swift_flags}" mai_ios_swift_flags)
  set(CMAKE_Swift_FLAGS "${mai_ios_swift_flags} -target ${mai_ios_swift_target}"
    CACHE STRING "Swift target for iOS Graphics" FORCE)
  enable_language(Swift)
  file(GLOB metal_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/metal/*.swift)
  add_library(maiagent_obs_metal_ios STATIC ${metal_sources})
  target_compile_options(maiagent_obs_metal_ios PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:-import-objc-header>"
    "$<$<COMPILE_LANGUAGE:Swift>:${CMAKE_CURRENT_SOURCE_DIR}/metal/libobs-metal-Bridging-Header.h>")
  target_include_directories(maiagent_obs_metal_ios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  target_link_libraries(maiagent_obs_metal_ios PRIVATE maiagent_graphics_simde)

  add_library(maiagent_graphics STATIC
    $<TARGET_OBJECTS:maiagent_obs_graphics_compile>
    compat/MaiObsStandalone.c)
  set_target_properties(maiagent_graphics PROPERTIES LINKER_LANGUAGE C)
  add_library(MaiAgent::Graphics ALIAS maiagent_graphics)
  target_include_directories(maiagent_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR})
  target_include_directories(maiagent_graphics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  target_link_libraries(maiagent_graphics PUBLIC
    maiagent_obs_metal_ios maiagent_obs_math maiagent_obs_util
    maiagent_graphics_simde Threads::Threads c++
    "-framework UIKit" "-framework Metal"
    "-framework CoreVideo" "-framework QuartzCore")
  add_dependencies(maiagent_graphics maiagent_graphics_data)
endif()

if(ANDROID)
  if(NOT MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "Android Graphics requires MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR")
  endif()
  target_sources(maiagent_obs_graphics_compile PRIVATE
    ${obs_graphics_source}/graphics-ffmpeg.c)
  target_include_directories(maiagent_obs_graphics_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR})

  add_library(maiagent_graphics SHARED
    $<TARGET_OBJECTS:maiagent_obs_graphics_compile>)
  add_library(MaiAgent::Graphics ALIAS maiagent_graphics)
  target_include_directories(maiagent_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${MAIAGENT_GRAPHICS_FFMPEG_INCLUDE_DIR})
  target_link_libraries(maiagent_graphics PUBLIC
    maiagent_obs_math maiagent_obs_util maiagent_graphics_simde Threads::Threads
    PRIVATE ${CMAKE_DL_LIBS} log)

  add_library(maiagent_obs_gles MODULE
    opengl/android/gl-android-egl.c
    opengl/gl-helpers.c
    opengl/gl-indexbuffer.c
    opengl/gl-shader.c
    opengl/gl-shaderparser.c
    opengl/gl-stagesurf.c
    opengl/gl-subsystem.c
    opengl/gl-texture2d.c
    opengl/gl-texture3d.c
    opengl/gl-texturecube.c
    opengl/gl-vertexbuffer.c
    opengl/gl-zstencil.c)
  target_include_directories(maiagent_obs_gles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/opengl/android)
  target_link_libraries(maiagent_obs_gles PRIVATE
    maiagent_graphics maiagent_graphics_simde EGL GLESv3 android log)
endif()

add_custom_target(maiagent_obs_graphics_check ALL)
add_dependencies(maiagent_obs_graphics_check
  maiagent_obs_graphics_compile maiagent_obs_math)
if(APPLE OR ANDROID)
  add_dependencies(maiagent_obs_graphics_check maiagent_obs_util)
endif()
