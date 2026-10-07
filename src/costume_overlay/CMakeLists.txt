cmake_minimum_required(VERSION 3.25)
project(costume_overlay LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(costume_overlay
    mod_main.cpp
)
