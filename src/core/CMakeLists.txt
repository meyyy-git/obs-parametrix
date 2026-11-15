# Parametrix DSP core. No OBS dependency, so it also builds standalone for tests:
#   cmake -S src/core -B build_core && cmake --build build_core --config Release
#   ctest --test-dir build_core -C Release --output-on-failure
cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(px_core LANGUAGES CXX)
  set(PX_CORE_STANDALONE ON)
endif()

add_library(
  px_core
  STATIC
  px_params.cpp
  px_settings.cpp
  px_chain.cpp
  mod_eq.cpp
  mod_dynamics.cpp
  mod_fx.cpp
  mod_denoise.cpp
  mod_pitch.cpp
)
target_include_directories(px_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(px_core PUBLIC cxx_std_17)
set_target_properties(px_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(px_core PUBLIC /utf-8)
endif()

if(PX_CORE_STANDALONE)
  enable_testing()
  add_executable(px_tests ../../tests/test_core.cpp)
  target_link_libraries(px_tests PRIVATE px_core)
  add_test(NAME core COMMAND px_tests "${CMAKE_CURRENT_SOURCE_DIR}/../../data/presets")

  add_executable(px-render ../../tools/px-render.cpp)
  target_link_libraries(px-render PRIVATE px_core)
endif()
