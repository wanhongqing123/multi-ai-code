add_library(maiagent_graphics STATIC
  src/MaiGraphics.cpp
  src/MaiGraphicsEffectParser.cpp
  src/MaiGraphicsTaskRunner.cpp
)

target_include_directories(maiagent_graphics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(maiagent_graphics PUBLIC maiagent)

# OBS uses SIMDe as a header-only imported target. Keep that include path local
# and reproducible instead of depending on an OBS installation or the host SDK.
add_library(maiagent_graphics_simde INTERFACE)
target_include_directories(maiagent_graphics_simde INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/simde/include)

set(obs_graphics_source ${CMAKE_CURRENT_SOURCE_DIR}/obs-studio/libobs/graphics)
add_library(maiagent_obs_graphics_compile OBJECT
  ${obs_graphics_source}/axisang.c
  ${obs_graphics_source}/bounds.c
  ${obs_graphics_source}/effect-parser.c
  ${obs_graphics_source}/effect.c
  ${obs_graphics_source}/graphics-imports.c
  ${obs_graphics_source}/graphics.c
  ${obs_graphics_source}/image-file.c
  ${obs_graphics_source}/math-extra.c
  ${obs_graphics_source}/matrix3.c
  ${obs_graphics_source}/matrix4.c
  ${obs_graphics_source}/plane.c
  ${obs_graphics_source}/quat.c
  ${obs_graphics_source}/shader-parser.c
  ${obs_graphics_source}/texture-render.c
  ${obs_graphics_source}/vec2.c
  ${obs_graphics_source}/vec3.c
  ${obs_graphics_source}/vec4.c
)
target_include_directories(maiagent_obs_graphics_compile PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/obs-studio/libobs)
target_link_libraries(maiagent_obs_graphics_compile PRIVATE maiagent_graphics_simde)
# This is a source-compatibility gate, not a linked OBS runtime. Upstream
# graphics-ffmpeg.c needs a header outside the requested OBS source subset.
add_dependencies(maiagent_graphics maiagent_obs_graphics_compile)
