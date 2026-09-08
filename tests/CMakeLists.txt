# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# Keep fork and upstream release-tag namespaces collision-safe.
add_test(
    NAME version_tag_namespace
    COMMAND ${CMAKE_COMMAND}
        -DREX_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/tests/cmake/rex_version_test.cmake)
set_tests_properties(version_tag_namespace PROPERTIES LABELS "unit" TIMEOUT 30)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)
