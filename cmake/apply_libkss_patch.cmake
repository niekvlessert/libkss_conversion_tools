if(NOT DEFINED LIBKSS_DIR OR NOT DEFINED KSS_PATCH_FILE)
  message(FATAL_ERROR
    "apply_libkss_patch.cmake requires LIBKSS_DIR and KSS_PATCH_FILE")
endif()

set(_kssplay_source "${LIBKSS_DIR}/src/kssplay.c")
if(NOT EXISTS "${_kssplay_source}")
  message(FATAL_ERROR "libkss source was not found at ${LIBKSS_DIR}")
endif()
if(NOT EXISTS "${KSS_PATCH_FILE}")
  message(FATAL_ERROR "libkss patch was not found at ${KSS_PATCH_FILE}")
endif()

# A clean libkss checkout is patched during configure.  This marker also
# makes repeated configure/build cycles a no-op after the first application.
file(READ "${_kssplay_source}" _kssplay_text)
string(FIND "${_kssplay_text}" "mbwave_frequency_from_base" _patch_marker)
if(_patch_marker GREATER -1)
  message(STATUS "libkss MoonSound/MBWave patch already applied")
  return()
endif()

find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
  message(FATAL_ERROR
    "git is required to apply ${KSS_PATCH_FILE} to the clean libkss checkout")
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${LIBKSS_DIR}" apply --ignore-whitespace --check "${KSS_PATCH_FILE}"
  RESULT_VARIABLE _check_result
  OUTPUT_VARIABLE _check_output
  ERROR_VARIABLE _check_error
)
if(NOT _check_result EQUAL 0)
  message(FATAL_ERROR
    "The libkss patch cannot be applied cleanly.\n"
    "This checkout may contain incompatible local changes.\n"
    "git apply output:\n${_check_output}${_check_error}")
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${LIBKSS_DIR}" apply --ignore-whitespace "${KSS_PATCH_FILE}"
  RESULT_VARIABLE _apply_result
  OUTPUT_VARIABLE _apply_output
  ERROR_VARIABLE _apply_error
)
if(NOT _apply_result EQUAL 0)
  message(FATAL_ERROR
    "Applying the libkss patch failed.\n"
    "git apply output:\n${_apply_output}${_apply_error}")
endif()

message(STATUS "Applied libkss MoonSound/MBWave patch")
