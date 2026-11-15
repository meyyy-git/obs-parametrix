# Standalone build of the editor preview (no OBS needed). Point CMAKE_PREFIX_PATH at a Qt 6
# install, for example the one the plugin build downloads into .deps:
#   cmake -S src/editor -B build_editor -DCMAKE_PREFIX_PATH=.deps/obs-deps-qt6-2025-07-11-x64
#   cmake --build build_editor --config Release
cmake_minimum_required(VERSION 3.28...3.30)
project(px_editor_preview LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_subdirectory(../core core)

add_executable(editor-preview ../../tools/editor-preview.cpp editor.cpp editor_views.cpp)
target_include_directories(editor-preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(editor-preview PRIVATE px_core Qt6::Widgets)
