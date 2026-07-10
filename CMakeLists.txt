cmake_minimum_required(VERSION 3.10)

project(libkss_conversion_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(external/libkss)

add_executable(sng2kss src/sng2kss.c)
target_include_directories(sng2kss PRIVATE external/libkss/src)
target_link_libraries(sng2kss PRIVATE kss)

if(UNIX)
  target_link_libraries(sng2kss PRIVATE m)
endif()
