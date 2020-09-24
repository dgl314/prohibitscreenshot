cmake_minimum_required(VERSION 3.13)

set(SRC ./prohibitscreenshot.cc)
add_definitions(-g3)
add_library(prohibitscreenshot SHARED ${SRC})
#set_target_properties(prohibitscreenshot PROPERTIES VERSION 1.1.0)
