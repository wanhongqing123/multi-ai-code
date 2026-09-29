add_library(maiagent_graphics STATIC
  src/MaiGraphics.cpp
  src/MaiGraphicsEffectParser.cpp
  src/MaiGraphicsTaskRunner.cpp
)

target_include_directories(maiagent_graphics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(maiagent_graphics PUBLIC maiagent)
