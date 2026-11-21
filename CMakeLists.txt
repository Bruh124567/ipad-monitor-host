cmake_minimum_required(VERSION 3.20)
project(ipad_monitor_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ipad_host src/main.cpp src/usbmux.cpp)
target_compile_definitions(ipad_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ipad_host PRIVATE ws2_32 winmm)
if(MSVC)
  target_compile_options(ipad_host PRIVATE /W4 /utf-8)
endif()
