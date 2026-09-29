# OBS uses SIMDe as a header-only imported target. Keep that include path local
# and reproducible instead of depending on an OBS installation or the host SDK.
add_library(maiagent_graphics_simde INTERFACE)
target_include_directories(maiagent_graphics_simde INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simde/include)

set(obs_graphics_source ${CMAKE_CURRENT_SOURCE_DIR})
add_library(maiagent_obs_graphics_compile OBJECT
  ${obs_graphics_source}/effect-parser.c
  ${obs_graphics_source}/effect.c
  ${obs_graphics_source}/graphics-imports.c
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

if(APPLE AND NOT IOS)
  set(ffmpeg_source ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ffmpeg)
  set(ffmpeg_runtime_dir ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-runtime)
  set(ffmpeg_runtime_script
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg-runtime.sh)
  set(ffmpeg_runtime_archives
    ${ffmpeg_runtime_dir}/libavformat/libavformat.a
    ${ffmpeg_runtime_dir}/libavcodec/libavcodec.a
    ${ffmpeg_runtime_dir}/libswresample/libswresample.a
    ${ffmpeg_runtime_dir}/libswscale/libswscale.a
    ${ffmpeg_runtime_dir}/libavutil/libavutil.a)
  add_custom_command(OUTPUT ${ffmpeg_runtime_archives}
    ${ffmpeg_runtime_dir}/install/include/libavutil/avconfig.h
    COMMAND bash ${ffmpeg_runtime_script} ${ffmpeg_source} ${ffmpeg_runtime_dir}
      ${CMAKE_C_COMPILER} ${CMAKE_SYSTEM_PROCESSOR} ${CMAKE_AR} ${CMAKE_RANLIB}
    DEPENDS ${ffmpeg_source}/configure ${ffmpeg_runtime_script}
    VERBATIM)
  add_custom_target(maiagent_ffmpeg_runtime
    DEPENDS ${ffmpeg_runtime_archives}
      ${ffmpeg_runtime_dir}/install/include/libavutil/avconfig.h)
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

# The standalone macOS runtime uses OBS util's lexer, allocator, file and
# platform implementations. The platform-nix.c obsconfig.h include is unused
# on macOS, so provide a generated empty header instead of OBS's app config.
if(APPLE AND NOT IOS)
  enable_language(OBJC)
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
    ${obs_util_source}/platform-cocoa.m
    ${obs_util_source}/utf8.c)
  target_compile_definitions(maiagent_obs_util PRIVATE MAI_GRAPHICS_STANDALONE)
  target_include_directories(maiagent_obs_util PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(maiagent_obs_util PRIVATE
    "-framework Cocoa" "-framework CoreServices" "-framework IOKit")
endif()

if(APPLE AND NOT IOS)
  add_library(maiagent_graphics SHARED
    $<TARGET_OBJECTS:maiagent_obs_graphics_compile>
    compat/MaiObsStandalone.c)
  add_library(MaiAgent::Graphics ALIAS maiagent_graphics)
  target_include_directories(maiagent_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  target_include_directories(maiagent_graphics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  target_link_libraries(maiagent_graphics PUBLIC
    maiagent_obs_math maiagent_obs_util maiagent_graphics_simde Threads::Threads
    PRIVATE ${ffmpeg_runtime_archives}
      "-framework AudioToolbox" "-framework CoreMedia"
      "-framework CoreVideo" "-framework VideoToolbox")
  add_dependencies(maiagent_graphics maiagent_ffmpeg_runtime)

  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    enable_language(Swift)
    file(GLOB metal_sources CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/metal/*.swift)
    add_library(maiagent_obs_metal MODULE ${metal_sources})
    target_compile_options(maiagent_obs_metal PRIVATE
      -import-objc-header
      ${CMAKE_CURRENT_SOURCE_DIR}/metal/libobs-metal-Bridging-Header.h)
    target_include_directories(maiagent_obs_metal PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/compat)
    target_link_libraries(maiagent_obs_metal PRIVATE
      maiagent_graphics maiagent_graphics_simde
      "-framework AppKit" "-framework Metal" "-framework CoreVideo"
      "-framework QuartzCore")
  endif()

endif()

add_custom_target(maiagent_obs_graphics_check ALL)
add_dependencies(maiagent_obs_graphics_check
  maiagent_obs_graphics_compile maiagent_obs_math)
if(APPLE AND NOT IOS)
  add_dependencies(maiagent_obs_graphics_check maiagent_obs_util)
endif()
