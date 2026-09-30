add_executable(MaiObsMathTests MaiObsMathTests.c)
target_include_directories(MaiObsMathTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MaiObsMathTests PRIVATE maiagent_obs_math maiagent_graphics_simde)
add_test(NAME MaiObsMathTests COMMAND MaiObsMathTests)

if(TARGET maiagent_graphics)
  add_executable(MaiObsGraphicsLinkTests MaiObsGraphicsLinkTests.c)
  target_link_libraries(MaiObsGraphicsLinkTests PRIVATE maiagent_graphics)
  add_test(NAME MaiObsGraphicsLinkTests COMMAND MaiObsGraphicsLinkTests)
endif()

if(TARGET maiagent_graphics AND NOT IOS)
  add_executable(MaiGraphicsImageRendererTests MaiGraphicsImageRendererTests.c)
  target_link_libraries(MaiGraphicsImageRendererTests PRIVATE maiagent_graphics)
  if(TARGET maiagent_obs_metal)
    add_dependencies(MaiGraphicsImageRendererTests maiagent_obs_metal)
    add_test(NAME MaiGraphicsImageRendererTests
      COMMAND MaiGraphicsImageRendererTests $<TARGET_FILE:maiagent_obs_metal>
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tiny-red.ppm)
  endif()
endif()

if(TARGET maiagent_obs_metal)
  add_executable(MaiObsMetalRenderTests MaiObsMetalRenderTests.c)
  target_link_libraries(MaiObsMetalRenderTests PRIVATE maiagent_graphics)
  add_dependencies(MaiObsMetalRenderTests maiagent_obs_metal)
  add_test(NAME MaiObsMetalRenderTests
    COMMAND MaiObsMetalRenderTests $<TARGET_FILE:maiagent_obs_metal>)
  set_tests_properties(MaiObsMetalRenderTests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(MaiObsEffectTests MaiObsEffectTests.c)
  target_link_libraries(MaiObsEffectTests PRIVATE maiagent_graphics)
  add_dependencies(MaiObsEffectTests maiagent_obs_metal maiagent_graphics_data)
  add_test(NAME MaiObsEffectTests
    COMMAND MaiObsEffectTests $<TARGET_FILE:maiagent_obs_metal>
      ${CMAKE_CURRENT_BINARY_DIR}/../data/default.effect
      ${CMAKE_CURRENT_BINARY_DIR}/../data/solid.effect)
endif()

if(APPLE)
  add_executable(MaiObsLexerTests MaiObsLexerTests.c)
  target_include_directories(MaiObsLexerTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(MaiObsLexerTests PRIVATE maiagent_obs_util)
  target_link_options(MaiObsLexerTests PRIVATE -Wl,-dead_strip)
  add_test(NAME MaiObsLexerTests COMMAND MaiObsLexerTests)
endif()
