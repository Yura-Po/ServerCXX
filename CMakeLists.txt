cmake_minimum_required(VERSION 4.2.1 FATAL_ERROR)

set(PROJECT_NAME Server)
project(${PROJECT_NAME})

add_executable(${PROJECT_NAME}
 src/main.cpp
 )
