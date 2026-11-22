cmake_minimum_required(VERSION 3.20)
project(vdctl CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vdctl vdctl.cpp)
target_compile_definitions(vdctl PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(vdctl PRIVATE setupapi newdev)
if(MSVC)
  target_compile_options(vdctl PRIVATE /W4 /utf-8)
  target_link_options(vdctl PRIVATE /ENTRY:wmainCRTStartup /SUBSYSTEM:CONSOLE)
endif()
