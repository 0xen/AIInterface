# StageLatest.cmake -- assembles the runnable application into <source>/latest.
#
# Run as `cmake -P` from a POST_BUILD step on `avatar`; see
# src/avatar/CMakeLists.txt. That step exists only when AII_STAGE_LATEST is ON,
# which since 19 Sep 2026 it is not by default -- staging is for release
# packaging. Nothing here runs, warns or checks anything on an ordinary build;
# LATEST_DIR does not have to exist beforehand, the copies below create it.
# Arguments:
#
#   MANIFEST    a file written by file(GENERATE) listing exactly what the app
#               needs, one entry per line, in one of two forms:
#                 copy|<dest relative to latest>|<absolute source file>
#                 glob|<dest directory, "." for the root>|<absolute pattern>
#   LATEST_DIR  where to assemble it
#   CONFIG      the configuration being staged, for BUILD-INFO.txt
#   SOURCE_DIR  the checkout, for BUILD-INFO.txt
#
# Why this is not `copy_directory`. The build tree accumulates: a shader whose
# .hlsl was deleted three commits ago is still sitting in
# build/bin/Release/data/shaders, and an executable built while an option was
# on is not removed when the option goes off (README.md says so). Copying the
# folder would carry all of that into a directory whose name promises it is
# current. So the manifest is the authority on what belongs here, and anything
# this script staged on a previous build and no longer wants is deleted.
#
# It deletes only what it staged before -- that is what .aii-staged.txt is for.
# The application writes pre-prompt.md next to its own executable and the user
# may put scripts there; a build must not eat those.

if(NOT MANIFEST OR NOT LATEST_DIR)
  message(FATAL_ERROR "StageLatest: MANIFEST and LATEST_DIR are required")
endif()

file(STRINGS "${MANIFEST}" _lines)

set(_wanted "")   # destination paths, relative to LATEST_DIR
set(_sources "")  # absolute source path for each, same order

foreach(_line IN LISTS _lines)
  if(_line STREQUAL "")
    continue()
  endif()
  string(REPLACE "|" ";" _parts "${_line}")
  list(LENGTH _parts _n)
  if(_n LESS 3)
    message(FATAL_ERROR "StageLatest: malformed manifest line '${_line}'")
  endif()
  list(GET _parts 0 _kind)
  list(GET _parts 1 _dest)
  list(GET _parts 2 _src)

  if(_kind STREQUAL "copy")
    list(APPEND _wanted "${_dest}")
    list(APPEND _sources "${_src}")
  elseif(_kind STREQUAL "glob")
    file(GLOB _found "${_src}")
    if(NOT _found)
      message(FATAL_ERROR "StageLatest: nothing matched '${_src}'")
    endif()
    foreach(_f IN LISTS _found)
      get_filename_component(_name "${_f}" NAME)
      if(_dest STREQUAL ".")
        list(APPEND _wanted "${_name}")
      else()
        list(APPEND _wanted "${_dest}/${_name}")
      endif()
      list(APPEND _sources "${_f}")
    endforeach()
  else()
    message(FATAL_ERROR "StageLatest: unknown manifest verb '${_kind}'")
  endif()
endforeach()

# ---- copy what changed ----------------------------------------------------
list(LENGTH _wanted _count)
math(EXPR _last "${_count} - 1")
foreach(_i RANGE ${_last})
  list(GET _wanted ${_i} _dest)
  list(GET _sources ${_i} _src)
  if(NOT EXISTS "${_src}")
    message(FATAL_ERROR "StageLatest: ${_src} does not exist; the build did not produce what latest/ expects")
  endif()
  set(_out "${LATEST_DIR}/${_dest}")
  get_filename_component(_out_dir "${_out}" DIRECTORY)
  file(MAKE_DIRECTORY "${_out_dir}")
  file(COPY_FILE "${_src}" "${_out}" ONLY_IF_DIFFERENT)
endforeach()

# ---- delete what a previous build staged and this one does not want -------
set(_ledger "${LATEST_DIR}/.aii-staged.txt")
set(_previous "")
if(EXISTS "${_ledger}")
  file(STRINGS "${_ledger}" _previous)
endif()

set(_orphan_dirs "")
foreach(_old IN LISTS _previous)
  if(_old STREQUAL "")
    continue()
  endif()
  if(NOT _old IN_LIST _wanted)
    message(STATUS "latest/: removing stale ${_old}")
    file(REMOVE "${LATEST_DIR}/${_old}")
    get_filename_component(_d "${LATEST_DIR}/${_old}" DIRECTORY)
    list(APPEND _orphan_dirs "${_d}")
  endif()
endforeach()

# Any directory a removal emptied goes too, so latest/ never shows an empty
# data/shaders and implies something is there.
if(_orphan_dirs)
  list(REMOVE_DUPLICATES _orphan_dirs)
  foreach(_d IN LISTS _orphan_dirs)
    while(IS_DIRECTORY "${_d}" AND NOT "${_d}" STREQUAL "${LATEST_DIR}")
      file(GLOB _rest "${_d}/*")
      if(_rest)
        break()
      endif()
      file(REMOVE_RECURSE "${_d}")
      get_filename_component(_d "${_d}" DIRECTORY)
    endwhile()
  endforeach()
endif()

list(SORT _wanted)
string(JOIN "\n" _ledger_text ${_wanted})
file(WRITE "${_ledger}" "${_ledger_text}\n")

# ---- say what this is -----------------------------------------------------
# A binary and the assets it seeds can drift apart, and the only defence is
# being able to see which build this is.
set(_commit "unknown")
find_package(Git QUIET)
if(GIT_EXECUTABLE)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short HEAD
                  OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR _commit STREQUAL "")
    set(_commit "unknown")
  endif()
endif()
file(TIMESTAMP "${LATEST_DIR}/avatar.exe" _built UTC)

file(WRITE "${LATEST_DIR}/BUILD-INFO.txt"
"AIInterface -- staged by the build, do not edit.

configuration : ${CONFIG}
commit        : ${_commit}
source tree   : ${SOURCE_DIR}
avatar.exe    : ${_built} UTC

This folder holds the most recent build, whichever configuration that was.
It is not relocatable: the model, VOICEVOX and asset directories are compiled
in as absolute paths into the source tree above, so it runs on this machine
only.
")
