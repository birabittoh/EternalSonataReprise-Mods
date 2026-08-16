cmake_minimum_required(VERSION 3.25)
project(demo_characters LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(demo_characters
    mod_main.cpp
)
