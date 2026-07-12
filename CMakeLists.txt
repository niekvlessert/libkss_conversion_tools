cmake_minimum_required(VERSION 3.10)

project(sng2kss_conversion_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_executable(sng2kss src/sng2kss.c src/sng2kss_converter.c)
target_include_directories(sng2kss PRIVATE src)

add_executable(pro2kss
  src/pro2kss.c
  src/pro_tracker_converter.c
  src/zx_compressor.c
  src/zx0/optimize.c
  src/zx0/compress.c
  src/zx0/memory.c
)
target_include_directories(pro2kss PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(pro2kss PRIVATE -O2)
endif()

add_executable(sngpro2kss
  src/sngpro2kss.c
  src/sngpro2kss_converter.c
  src/sng2kss_converter.c
  src/pro_tracker_converter.c
  src/zx_compressor.c
  src/zx0/optimize.c
  src/zx0/compress.c
  src/zx0/memory.c
)
target_include_directories(sngpro2kss PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(sngpro2kss PRIVATE -O2)
endif()

if(UNIX)
  target_link_libraries(sng2kss PRIVATE m)
  target_link_libraries(pro2kss PRIVATE m)
  target_link_libraries(sngpro2kss PRIVATE m)
endif()
