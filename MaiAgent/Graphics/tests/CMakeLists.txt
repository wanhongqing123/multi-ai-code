add_executable(MaiObsMathTests MaiObsMathTests.c)
target_include_directories(MaiObsMathTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MaiObsMathTests PRIVATE maiagent_obs_math maiagent_graphics_simde)
add_test(NAME MaiObsMathTests COMMAND MaiObsMathTests)

if(APPLE)
  add_executable(MaiObsLexerTests MaiObsLexerTests.c)
  target_include_directories(MaiObsLexerTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(MaiObsLexerTests PRIVATE maiagent_obs_util)
  target_link_options(MaiObsLexerTests PRIVATE -Wl,-dead_strip)
  add_test(NAME MaiObsLexerTests COMMAND MaiObsLexerTests)
endif()
