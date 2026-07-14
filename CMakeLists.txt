cmake_minimum_required(VERSION 3.10)

project(sng2kss_conversion_tools C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Optional raw OPL4 backend. The source tree is intentionally supplied by the
# caller because it is also used by the standalone libmoonsound project.
set(KSP_MOONSOUND_DIR "" CACHE PATH
    "libmoonsound source directory (enables YMF278B audio in kspplayer)")

add_library(ksp STATIC
  src/ksp/ksp.c
  src/ksp/engine.c
  src/ksp/mbwave.c
  src/zx_compressor.c
  src/zx_decompressor.c
  src/zx0/optimize.c
  src/zx0/compress.c
  src/zx0/memory.c
)
target_include_directories(ksp PUBLIC include PRIVATE src)
if(UNIX)
  target_link_libraries(ksp PUBLIC m)
endif()

add_executable(kspack tools/kspack.c)
target_link_libraries(kspack PRIVATE ksp)
target_include_directories(kspack PRIVATE include)

add_executable(kspvalidate tools/kspvalidate.c)
target_link_libraries(kspvalidate PRIVATE ksp)

add_executable(kspmaterialize tools/kspmaterialize.c)
target_link_libraries(kspmaterialize PRIVATE ksp)
target_include_directories(kspmaterialize PRIVATE include)
target_include_directories(kspvalidate PRIVATE include)

add_executable(zx0pack tools/zx0pack.c)
target_link_libraries(zx0pack PRIVATE ksp)
target_include_directories(zx0pack PRIVATE src)

add_executable(mbwave_extract tools/mbwave_extract.c)

add_executable(mbwave2ksp tools/mbwave2ksp.c)
target_link_libraries(mbwave2ksp PRIVATE ksp)
target_include_directories(mbwave2ksp PRIVATE include)

add_executable(ksp_selftest tests/ksp_selftest.c)
target_link_libraries(ksp_selftest PRIVATE ksp)
target_include_directories(ksp_selftest PRIVATE include)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME ksp_selftest COMMAND ksp_selftest)
endif()

# Keep the reference KSS emulator with this project so the command-line test
# player does not depend on a separately installed libkss.
set(KSS_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/libkss-moonsound.patch")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          "-DLIBKSS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/libkss"
          "-DKSS_PATCH_FILE=${KSS_PATCH_FILE}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_libkss_patch.cmake"
  RESULT_VARIABLE KSS_PATCH_RESULT
  OUTPUT_VARIABLE KSS_PATCH_OUTPUT
  ERROR_VARIABLE KSS_PATCH_ERROR
)
if(NOT KSS_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR
    "Could not prepare the bundled libkss checkout.\n"
    "${KSS_PATCH_OUTPUT}${KSS_PATCH_ERROR}")
endif()
if(KSS_PATCH_OUTPUT)
  message(STATUS "${KSS_PATCH_OUTPUT}")
endif()
add_subdirectory(3rd_party/libkss EXCLUDE_FROM_ALL)

if(KSP_MOONSOUND_DIR)
  set(KSP_LIBVGM_DIR "${KSP_MOONSOUND_DIR}/src_libvgm")
  target_sources(kss PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/moonsound_opl4.c"
    "${KSP_MOONSOUND_DIR}/src/mwm_parser.c"
    "${KSP_LIBVGM_DIR}/emu/SoundEmu.c"
    "${KSP_LIBVGM_DIR}/emu/Resampler.c"
    "${KSP_LIBVGM_DIR}/emu/panning.c"
    "${KSP_LIBVGM_DIR}/emu/dac_control.c"
    "${KSP_LIBVGM_DIR}/emu/logging.c"
    "${KSP_LIBVGM_DIR}/emu/cores/ymf278b.c"
    "${KSP_LIBVGM_DIR}/emu/cores/262intf.c"
    "${KSP_LIBVGM_DIR}/emu/cores/nukedopl3.c"
  )
  target_include_directories(kss PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${KSP_MOONSOUND_DIR}/src"
    "${KSP_LIBVGM_DIR}"
    "${KSP_LIBVGM_DIR}/emu"
    "${KSP_LIBVGM_DIR}/emu/cores"
  )
  target_compile_definitions(kss PRIVATE
    KSS_HAVE_MOONSOUND
    VGM_LITTLE_ENDIAN
    SNDDEV_SELECT
    SNDDEV_YMF278B
    SNDDEV_YMF262
    EC_YMF262_NUKED
  )

  add_executable(kss2wav_moonsound src/kss2wav_moonsound.c)
  target_include_directories(kss2wav_moonsound PRIVATE
    3rd_party/libkss/src
    3rd_party/libkss/modules
    src
  )
  target_link_libraries(kss2wav_moonsound PRIVATE kss)
endif()

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

add_executable(kspplayer src/kssplayer.c)
# Keep the old executable name as a compatibility target. Both names use the
# same KSP/KSS and MoonSound-capable implementation.
add_executable(kssplayer src/kssplayer.c)
foreach(KSSPLAYER_TARGET kspplayer kssplayer)
  target_include_directories(${KSSPLAYER_TARGET} PRIVATE
    3rd_party/libkss/src
    3rd_party/libkss/modules)
  target_compile_definitions(${KSSPLAYER_TARGET} PRIVATE SDL_MAIN_HANDLED)
  target_link_libraries(${KSSPLAYER_TARGET} PRIVATE kss ksp
    ${KSSPLAYER_AUDIO_TARGET})
  if(KSP_MOONSOUND_DIR)
    target_compile_definitions(${KSSPLAYER_TARGET} PRIVATE KSS_HAVE_MOONSOUND)
    target_compile_definitions(${KSSPLAYER_TARGET} PRIVATE
      KSP_DEFAULT_YRW801_ROM="${KSP_MOONSOUND_DIR}/yrw801.rom")
    target_include_directories(${KSSPLAYER_TARGET} PRIVATE src)
  endif()
endforeach()

if(UNIX)
  target_link_libraries(sng2kss PRIVATE m)
  target_link_libraries(pro2kss PRIVATE m)
  target_link_libraries(sngpro2kss PRIVATE m)
  target_link_libraries(vigamup2kss PRIVATE m)
  target_link_libraries(kspplayer PRIVATE m)
  target_link_libraries(kssplayer PRIVATE m)
endif()
