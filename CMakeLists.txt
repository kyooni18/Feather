cmake_minimum_required(VERSION 3.20)

if(APPLE AND (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL ""))
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build for on macOS" FORCE)
endif()

project(Feather VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FEATHER_BUILD_SHARED "Build Feather as a shared library" OFF)
option(FEATHER_BUILD_EXAMPLE "Build the Feather example executable" ON)
option(FEATHER_BUILD_DEMO "Build the Feather demo executable" ON)
option(FEATHER_BUILD_FAST_DEMO "Build the fast Feather demo executable" ON)
option(FEATHER_BUILD_TEST_CLI "Build the Feather test CLI executable" ON)
option(FEATHER_BUILD_MEMORY_TEST "Build the Feather memory test executable" ON)
option(FEATHER_BUILD_SYSTEM_SCORE_TEST
       "Build the Feather extended system score diagnostic executable" ON)
option(FEATHER_BUILD_RESOURCE_TRACKING
       "Build the optional Feather memory tracking library" ON)
option(FEATHER_BUILD_RESOURCE_TRACKING_TEST
       "Build the Feather resource tracking test executable" ON)
option(FEATHER_INSTALL_PKGCONFIG "Install pkg-config metadata for Feather" ON)

file(GLOB_RECURSE FEATHER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/System/*.c"
)

set(FEATHER_RESOURCE_TRACKING_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/System/FeatherRuntime/FSResourceTracker.c")
set(FEATHER_SYSTEM_SCORE_TEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Test/Diagnostics/feather_system_score_test.c")
list(REMOVE_ITEM FEATHER_SOURCES "${FEATHER_RESOURCE_TRACKING_SOURCE}")

file(GLOB_RECURSE FEATHER_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/System/*.h"
)

if(FEATHER_BUILD_SHARED)
    add_library(Feather SHARED ${FEATHER_SOURCES})
else()
    add_library(Feather STATIC ${FEATHER_SOURCES})
endif()

add_library(Feather::Feather ALIAS Feather)

target_include_directories(Feather
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/System>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(FEATHER_BUILD_SHARED)
    target_compile_definitions(Feather PRIVATE FEATHER_BUILDING_LIBRARY)
else()
    target_compile_definitions(Feather PUBLIC FEATHER_STATIC_DEFINE)
endif()

set_target_properties(Feather PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(APPLE AND FEATHER_BUILD_SHARED)
    set_target_properties(Feather PROPERTIES
        BUILD_WITH_INSTALL_NAME_DIR ON
        INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}"
    )
endif()

if(FEATHER_BUILD_RESOURCE_TRACKING)
    if(FEATHER_BUILD_SHARED)
        add_library(FeatherResourceTracking SHARED ${FEATHER_RESOURCE_TRACKING_SOURCE})
    else()
        add_library(FeatherResourceTracking STATIC ${FEATHER_RESOURCE_TRACKING_SOURCE})
    endif()

    add_library(Feather::FeatherResourceTracking ALIAS FeatherResourceTracking)

    target_link_libraries(FeatherResourceTracking PUBLIC Feather::Feather)
    target_include_directories(FeatherResourceTracking
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/System>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    if(FEATHER_BUILD_SHARED)
        target_compile_definitions(FeatherResourceTracking PRIVATE FEATHER_BUILDING_LIBRARY)
    else()
        target_compile_definitions(FeatherResourceTracking PUBLIC FEATHER_STATIC_DEFINE)
    endif()

    set_target_properties(FeatherResourceTracking PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )

    if(APPLE AND FEATHER_BUILD_SHARED)
        set_target_properties(FeatherResourceTracking PROPERTIES
            BUILD_WITH_INSTALL_NAME_DIR ON
            INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}"
        )
    endif()
endif()

function(feather_add_tool target_name source_file output_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE Feather::Feather)
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME ${output_name}
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
endfunction()

if(FEATHER_BUILD_EXAMPLE)
    feather_add_tool(feather_example Test/Examples/feather_example.c Feather)
endif()

if(FEATHER_BUILD_DEMO)
    feather_add_tool(feather_demo Test/Diagnostics/feather_demo.c FeatherDemo)
endif()

if(FEATHER_BUILD_FAST_DEMO)
    feather_add_tool(feather_fast_demo Test/Diagnostics/feather_fast_demo.c FeatherFastDemo)
endif()

if(FEATHER_BUILD_TEST_CLI)
    feather_add_tool(feather_test_cli Test/Smoke/feather_test_cli.c FeatherTestCLI)
endif()

if(FEATHER_BUILD_MEMORY_TEST AND FEATHER_BUILD_RESOURCE_TRACKING)
    add_executable(feather_memory_test Test/Diagnostics/feather_memory_test.c)
    target_link_libraries(feather_memory_test PRIVATE Feather::FeatherResourceTracking)
    set_target_properties(feather_memory_test PROPERTIES
        OUTPUT_NAME FeatherMemoryTest
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
elseif(FEATHER_BUILD_MEMORY_TEST)
    message(WARNING "FEATHER_BUILD_MEMORY_TEST requires FEATHER_BUILD_RESOURCE_TRACKING; skipping FeatherMemoryTest")
endif()

if(FEATHER_BUILD_SYSTEM_SCORE_TEST AND EXISTS "${FEATHER_SYSTEM_SCORE_TEST_SOURCE}")
    add_executable(feather_system_score_test
        "${FEATHER_SYSTEM_SCORE_TEST_SOURCE}"
    )
    if(FEATHER_BUILD_RESOURCE_TRACKING)
        target_link_libraries(feather_system_score_test
            PRIVATE Feather::FeatherResourceTracking
        )
        target_compile_definitions(feather_system_score_test
            PRIVATE FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING=1
        )
    else()
        target_link_libraries(feather_system_score_test PRIVATE Feather::Feather)
    endif()
    set_target_properties(feather_system_score_test PROPERTIES
        OUTPUT_NAME FeatherSystemScoreTest
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
elseif(FEATHER_BUILD_SYSTEM_SCORE_TEST)
    message(WARNING "FEATHER_BUILD_SYSTEM_SCORE_TEST requested but source file is missing; skipping FeatherSystemScoreTest")
endif()

if(FEATHER_BUILD_RESOURCE_TRACKING AND FEATHER_BUILD_RESOURCE_TRACKING_TEST)
    add_executable(feather_resource_tracking_test
        Test/Diagnostics/feather_resource_tracking_test.c
    )
    target_link_libraries(feather_resource_tracking_test
        PRIVATE Feather::FeatherResourceTracking
    )
    set_target_properties(feather_resource_tracking_test PROPERTIES
        OUTPUT_NAME FeatherResourceTrackingTest
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
endif()

install(TARGETS Feather
    EXPORT FeatherTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(FEATHER_BUILD_RESOURCE_TRACKING)
    install(TARGETS FeatherResourceTracking
        EXPORT FeatherTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(FEATHER_BUILD_RESOURCE_TRACKING)
    add_executable(feather_full_system_benchmark
        Test/Diagnostics/feather_full_system_benchmark.c
    )

    target_link_libraries(feather_full_system_benchmark
        PRIVATE Feather::FeatherResourceTracking
    )

    set_target_properties(feather_full_system_benchmark PROPERTIES
        OUTPUT_NAME FeatherFullSystemBenchmark
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
endif()

foreach(FEATHER_HEADER IN LISTS FEATHER_HEADERS)
    file(RELATIVE_PATH FEATHER_HEADER_RELATIVE
        "${CMAKE_CURRENT_SOURCE_DIR}/System"
        "${FEATHER_HEADER}"
    )
    get_filename_component(FEATHER_HEADER_DIRECTORY "${FEATHER_HEADER_RELATIVE}" DIRECTORY)
    install(FILES "${FEATHER_HEADER}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${FEATHER_HEADER_DIRECTORY}"
    )
endforeach()

install(EXPORT FeatherTargets
    FILE FeatherTargets.cmake
    NAMESPACE Feather::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Feather
)

if(FEATHER_INSTALL_PKGCONFIG)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Feather.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Feather.pc"
        @ONLY
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/Feather.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FeatherConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Feather
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Feather
)
