cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(psb-editor VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "LEGO Universe PSB particle system editor")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(FetchContent)
FetchContent_Declare(lu_assets
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lu-assets
)
FetchContent_MakeAvailable(lu_assets)

FetchContent_Declare(tool_common
    GIT_REPOSITORY https://github.com/LU-Rebuilt/tool-common.git
    GIT_TAG main
)
FetchContent_MakeAvailable(tool_common)

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)
find_package(OpenGL REQUIRED)

add_executable(psb_editor
    src/main.cpp
    src/main_window.cpp src/main_window.h
    src/particle_gl_widget.cpp src/particle_gl_widget.h
    src/texture_editor.cpp src/texture_editor.h
)
target_link_libraries(psb_editor PRIVATE qt_common lu_assets Qt6::Widgets Qt6::OpenGLWidgets OpenGL::GL)
target_include_directories(psb_editor PRIVATE src)
