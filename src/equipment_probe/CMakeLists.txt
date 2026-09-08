cmake_minimum_required(VERSION 3.25)
project(equipment_probe LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(equipment_probe
    mod_main.cpp
)
