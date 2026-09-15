cmake_minimum_required(VERSION 3.25)
project(field_position_probe LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(field_position_probe
    mod_main.cpp
)
