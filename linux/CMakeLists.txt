cmake_minimum_required(VERSION 3.10)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Defaulting to Debug build.")
  set(CMAKE_BUILD_TYPE Debug)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

project(sling-linux C)

set(SINTER_STATIC_HEAP 0)
add_subdirectory(../deps/sinter sinter EXCLUDE_FROM_ALL)

option(WITH_SOCKS "" OFF)
option(DOCUMENTATION "" OFF)
option(WITH_STATIC_LIBRARIES "" ON)
add_subdirectory(../deps/mosquitto mosquitto EXCLUDE_FROM_ALL)

add_executable(sling
  src/main.c
)

target_compile_options(sling
  PRIVATE -Wall -Wextra -Wswitch-enum -std=c11 -pedantic -Werror -fwrapv -g
  PRIVATE -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
  PRIVATE $<$<CONFIG:Debug>:-Og>
  PRIVATE $<$<CONFIG:Release>:-O2>
)

target_include_directories(sling
  PRIVATE include
)

target_link_libraries(sling libmosquitto_static)

add_executable(sinter_host
  src/sinter_host.c
  ../common/sling_sinter.c
)

target_compile_options(sinter_host
  PRIVATE -Wall -Wextra -Wswitch-enum -std=c11 -pedantic -Werror -fwrapv -g
  PRIVATE -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
  PRIVATE $<$<CONFIG:Debug>:-Og>
  PRIVATE $<$<CONFIG:Release>:-O2>
)

target_include_directories(sinter_host
  PRIVATE include
)

target_link_libraries(sinter_host sinter)
