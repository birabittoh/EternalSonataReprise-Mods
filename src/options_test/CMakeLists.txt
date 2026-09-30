cmake_minimum_required(VERSION 3.25)
project(options_test LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(options_test
    mod_main.cpp
)
