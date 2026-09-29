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
  ${obs_graphics_source}/shader-parser.c
  ${obs_graphics_source}/texture-render.c
)
target_include_directories(maiagent_obs_graphics_compile PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(maiagent_obs_graphics_compile PRIVATE maiagent_graphics_simde)
# This is a source-compatibility gate, not a linked OBS runtime.

if(APPLE AND NOT IOS)
  set(ffmpeg_source ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ffmpeg)
  set(ffmpeg_config_dir ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-config)
  set(ffmpeg_config_script
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/configure-ffmpeg-headers.sh)
  add_custom_command(OUTPUT ${ffmpeg_config_dir}/libavutil/avconfig.h
    COMMAND bash ${ffmpeg_config_script} ${ffmpeg_source} ${ffmpeg_config_dir}
      ${CMAKE_SYSTEM_PROCESSOR} ${CMAKE_C_COMPILER}
    DEPENDS ${ffmpeg_source}/configure ${ffmpeg_config_script}
    VERBATIM)
  add_custom_target(maiagent_ffmpeg_headers
    DEPENDS ${ffmpeg_config_dir}/libavutil/avconfig.h)
  target_sources(maiagent_obs_graphics_compile PRIVATE
    ${obs_graphics_source}/graphics-ffmpeg.c)
  target_include_directories(maiagent_obs_graphics_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${ffmpeg_config_dir}
    ${ffmpeg_source})
  add_dependencies(maiagent_obs_graphics_compile maiagent_ffmpeg_headers)
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

# OBS's C-family lexer is the first upstream component linked and exercised at
# runtime. On Apple platforms, platform-nix.c supplies the allocation failure
# hooks used by bmem.c. Its obsconfig.h include is unused on this platform.
if(APPLE)
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
    ${obs_util_source}/platform-nix.c
    ${obs_util_source}/utf8.c)
  target_include_directories(maiagent_obs_util PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_custom_target(maiagent_obs_graphics_check ALL)
add_dependencies(maiagent_obs_graphics_check
  maiagent_obs_graphics_compile maiagent_obs_math)
if(APPLE)
  add_dependencies(maiagent_obs_graphics_check maiagent_obs_util)
endif()
