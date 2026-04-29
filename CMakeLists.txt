cmake_minimum_required(VERSION 3.16)

project(Server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# PostgreSQL (libpqxx)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Argon2
pkg_check_modules(ARGON2 REQUIRED libargon2)

# OpenSSL
find_package(OpenSSL REQUIRED)

add_executable(Server
    src/main.cpp
    src/auth.cpp
)

# INCLUDE
target_include_directories(Server PRIVATE
    ${PQXX_INCLUDE_DIRS}
    ${ARGON2_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    src
)

# LINK LIBS
target_link_libraries(Server
    ${PQXX_LIBRARIES}
    ${ARGON2_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
)