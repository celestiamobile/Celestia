set(INTEGRATION_TEST_SOURCES
  3ds_load_test.cpp
  cmod_bin_ascii_roundtrip_test.cpp)

test_case(integration "${INTEGRATION_TEST_SOURCES}")

file(COPY "${CMAKE_SOURCE_DIR}/test/data/icosphere.3ds"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
file(COPY "${CMAKE_SOURCE_DIR}/test/data/testmodel.cmod"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT ENABLE_GLES)
  add_executable(autoexposure_gpu $<TARGET_OBJECTS:doctest_main> autoexposure_test.cpp)
  target_link_libraries(autoexposure_gpu PRIVATE celestia "-framework OpenGL")
  target_include_directories(autoexposure_gpu PRIVATE "${_CELESTIA_TESTS_BASE_DIR}/doctest")
  target_compile_definitions(autoexposure_gpu PRIVATE
    DOCTEST_CONFIG_USE_STD_HEADERS GL_SILENCE_DEPRECATION)
  target_compile_options(autoexposure_gpu PRIVATE -fexceptions)
  doctest_discover_tests(autoexposure_gpu
    TEST_PREFIX "Automatic exposure GPU/"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
