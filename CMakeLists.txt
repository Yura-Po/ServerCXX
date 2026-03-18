cmake_minimum_required(VERSION 3.28.3 FATAL_ERROR)

set(PROJECT_NAME Server)
set(CMAKE_BUILD_TYPE Debug)
project(${PROJECT_NAME})

add_executable(${PROJECT_NAME}
 src/main.cpp
 )
