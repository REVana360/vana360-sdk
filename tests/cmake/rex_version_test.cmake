if(NOT DEFINED REX_SOURCE_DIR)
    message(FATAL_ERROR "REX_SOURCE_DIR is required")
endif()

include("${REX_SOURCE_DIR}/cmake/rex_version.cmake")

find_program(GIT_EXECUTABLE git REQUIRED)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef fixture_suffix)
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/rex-version-fixture-${fixture_suffix}")
if(EXISTS "${fixture}" OR IS_SYMLINK "${fixture}")
    message(FATAL_ERROR "fixture path already exists: ${fixture}")
endif()
file(MAKE_DIRECTORY "${fixture}")
file(REAL_PATH "${fixture}" fixture_real)
file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" fixture_parent_real)
file(RELATIVE_PATH fixture_relative "${fixture_parent_real}" "${fixture_real}")
if("${fixture_relative}" MATCHES "^\\.\\.")
    message(FATAL_ERROR "fixture resolved outside the build directory: ${fixture_real}")
endif()
if(IS_ABSOLUTE "${fixture_relative}")
    message(FATAL_ERROR "fixture resolved as an absolute relative path: ${fixture_real}")
endif()
string(REGEX MATCH "^rex-version-fixture-[0-9a-f]+$" fixture_name_match
       "${fixture_relative}")
if(NOT fixture_name_match)
    message(FATAL_ERROR "fixture resolved outside the build directory: ${fixture_real}")
endif()

function(run_git)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} ${ARGN}
        WORKING_DIRECTORY "${fixture}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${output} ${error}")
    endif()
endfunction()

run_git(init --quiet --initial-branch=main)
run_git(config user.name "ReXGlue version test")
run_git(config user.email "version-test@example.invalid")
file(WRITE "${fixture}/version.txt" "baseline\n")
run_git(add version.txt)
run_git(commit --quiet -m baseline)
run_git(tag v1.2.0)

file(APPEND "${fixture}/version.txt" "child\n")
run_git(add version.txt)
run_git(commit --quiet -m child)
run_git(tag upstream/v1.4.0)

# A fork baseline reachable behind an upstream exact tag remains authoritative.
rex_resolve_version(resolved FLOOR_MAJOR 1 FLOOR_MINOR 2 SOURCE_DIR "${fixture}")
if(NOT resolved MATCHES "^1\\.2\\.0\\.1-dev\\.g[0-9a-f]+$")
    message(FATAL_ERROR "fork tag did not outrank upstream tag: ${resolved}")
endif()

run_git(tag --delete v1.2.0)
rex_resolve_version(resolved FLOOR_MAJOR 1 FLOOR_MINOR 2 SOURCE_DIR "${fixture}")
if(NOT resolved STREQUAL "1.4.0")
    message(FATAL_ERROR "upstream fallback did not resolve its tag: ${resolved}")
endif()

file(REAL_PATH "${fixture}" fixture_real_after)
if(NOT fixture_real_after STREQUAL fixture_real)
    message(FATAL_ERROR "fixture path changed before cleanup: ${fixture_real_after}")
endif()
file(REMOVE_RECURSE "${fixture_real}")
