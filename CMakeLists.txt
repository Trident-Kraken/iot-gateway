cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_FLAGS_DEBUG "-g" CACHE STRING "Flags used by the C compiler during Debug builds.")
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "Flags used by the C compiler during Release builds.")

project(iot-gateway
    VERSION 1.0.1
    DESCRIPTION "Linux Industrial IoT TCP Communication Gateway"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_library(MOSQUITTO_LIBRARY NAMES mosquitto REQUIRED)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
