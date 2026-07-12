cmake_minimum_required(VERSION 3.10)

project(sng2kss_conversion_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Keep the reference KSS emulator with this project so the command-line test
# player does not depend on a separately installed libkss.
add_subdirectory(3rd_party/libkss EXCLUDE_FROM_ALL)

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(KSSPLAYER_AUDIO_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
  set(KSSPLAYER_AUDIO_TARGET SDL2::SDL2-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(KSSPLAYER_AUDIO_TARGET PkgConfig::SDL2)
endif()

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

add_executable(vigamup2kss
  src/vigamup2kss.c
  src/zx_compressor.c
  src/zx0/optimize.c
  src/zx0/compress.c
  src/zx0/memory.c
)
target_include_directories(vigamup2kss PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(vigamup2kss PRIVATE -O2)
endif()

add_executable(vigamup2rom src/vigamup2rom.c)
target_include_directories(vigamup2rom PRIVATE src)

add_executable(kssplayer src/kssplayer.c)
target_include_directories(kssplayer PRIVATE 3rd_party/libkss/src
                                             3rd_party/libkss/modules)
target_compile_definitions(kssplayer PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(kssplayer PRIVATE kss ${KSSPLAYER_AUDIO_TARGET})

if(UNIX)
  target_link_libraries(sng2kss PRIVATE m)
  target_link_libraries(pro2kss PRIVATE m)
  target_link_libraries(sngpro2kss PRIVATE m)
  target_link_libraries(vigamup2kss PRIVATE m)
  target_link_libraries(kssplayer PRIVATE m)
endif()
