cmake_minimum_required(VERSION 3.25)
project(low_hp_alarm LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin(low_hp_alarm
    mod_main.cpp
)
