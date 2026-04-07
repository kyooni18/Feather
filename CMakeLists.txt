cmake_minimum_required(VERSION 3.20)

if(APPLE AND (NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL ""))
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build for on macOS" FORCE)
endif()

project(Feather VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FEATHER_BUILD_SHARED "Build Feather as a shared library" OFF)
option(FEATHER_BUILD_EXAMPLE "Build the Feather example executable" ON)
option(FEATHER_BUILD_DEMO "Build the Feather demo executable" OFF)
option(FEATHER_BUILD_FAST_DEMO "Build the fast Feather demo executable" OFF)
option(FEATHER_BUILD_TEST_CLI "Build the Feather test CLI executable" ON)
option(FEATHER_BUILD_MEMORY_TEST "Build the Feather memory test executable" OFF)
option(FEATHER_BUILD_SYSTEM_SCORE_TEST
       "Build the Feather extended system score diagnostic executable" OFF)
option(FEATHER_BUILD_RESOURCE_TRACKING
       "Build the optional Feather memory tracking library" ON)
option(FEATHER_BUILD_RESOURCE_TRACKING_TEST
       "Build the Feather resource tracking test executable" ON)
option(FEATHER_BUILD_FULL_SYSTEM_BENCHMARK
       "Build the Feather long-running full system benchmark executable" OFF)
option(FEATHER_BUILD_CPP_API "Build the Feather C++ scheduler API library" ON)
option(FEATHER_BUILD_CPP_TEST "Build the Feather C++ scheduler smoke test" ON)
option(FEATHER_INSTALL_PKGCONFIG "Install pkg-config metadata for Feather" ON)

file(GLOB_RECURSE FEATHER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/System/*.cpp"
)

set(FEATHER_RESOURCE_TRACKING_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/System/FeatherRuntime/FSResourceTracker.cpp")
set(FEATHER_SYSTEM_SCORE_TEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/Test/Diagnostics/feather_system_score_test.cpp")
set(FEATHER_CPP_API_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/System/FeatherCpp.cpp")
list(REMOVE_ITEM FEATHER_SOURCES "${FEATHER_RESOURCE_TRACKING_SOURCE}")
list(REMOVE_ITEM FEATHER_SOURCES "${FEATHER_CPP_API_SOURCE}")

file(GLOB_RECURSE FEATHER_CPP_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/System/*.hpp"
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
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
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
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
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

if(FEATHER_BUILD_CPP_API)
    add_library(FeatherCpp STATIC
        "${FEATHER_CPP_API_SOURCE}"
    )
    add_library(Feather::FeatherCpp ALIAS FeatherCpp)
    target_link_libraries(FeatherCpp PUBLIC Feather::Feather)
    target_include_directories(FeatherCpp
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/System>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    set_target_properties(FeatherCpp PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

function(feather_add_tool target_name source_file output_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE Feather::Feather)
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME ${output_name}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endfunction()

if(FEATHER_BUILD_EXAMPLE)
    feather_add_tool(feather_example Test/Examples/feather_example.cpp Feather)
endif()

if(FEATHER_BUILD_DEMO)
    feather_add_tool(feather_demo Test/Diagnostics/feather_demo.cpp FeatherDemo)
endif()

if(FEATHER_BUILD_FAST_DEMO)
    feather_add_tool(feather_fast_demo Test/Diagnostics/feather_fast_demo.cpp FeatherFastDemo)
endif()

if(FEATHER_BUILD_TEST_CLI)
    feather_add_tool(feather_test_cli Test/Smoke/feather_test_cli.cpp FeatherTestCLI)
endif()

if(FEATHER_BUILD_MEMORY_TEST AND FEATHER_BUILD_RESOURCE_TRACKING)
    add_executable(feather_memory_test Test/Diagnostics/feather_memory_test.cpp)
    target_link_libraries(feather_memory_test PRIVATE Feather::FeatherResourceTracking)
    set_target_properties(feather_memory_test PROPERTIES
        OUTPUT_NAME FeatherMemoryTest
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
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
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
elseif(FEATHER_BUILD_SYSTEM_SCORE_TEST)
    message(WARNING "FEATHER_BUILD_SYSTEM_SCORE_TEST requested but source file is missing; skipping FeatherSystemScoreTest")
endif()

if(FEATHER_BUILD_RESOURCE_TRACKING AND FEATHER_BUILD_RESOURCE_TRACKING_TEST)
    add_executable(feather_resource_tracking_test
        Test/Diagnostics/feather_resource_tracking_test.cpp
    )
    target_link_libraries(feather_resource_tracking_test
        PRIVATE Feather::FeatherResourceTracking
    )
    set_target_properties(feather_resource_tracking_test PROPERTIES
        OUTPUT_NAME FeatherResourceTrackingTest
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

if(FEATHER_BUILD_CPP_API AND FEATHER_BUILD_CPP_TEST)
    add_executable(feather_cpp_scheduler_test
        Test/Smoke/feather_cpp_scheduler_test.cpp
    )
    target_link_libraries(feather_cpp_scheduler_test PRIVATE Feather::FeatherCpp)
    set_target_properties(feather_cpp_scheduler_test PROPERTIES
        OUTPUT_NAME FeatherCppSchedulerTest
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
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

if(FEATHER_BUILD_CPP_API)
    install(TARGETS FeatherCpp
        EXPORT FeatherTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(FEATHER_BUILD_RESOURCE_TRACKING AND FEATHER_BUILD_FULL_SYSTEM_BENCHMARK)
    add_executable(feather_full_system_benchmark
        Test/Diagnostics/feather_full_system_benchmark.cpp
    )

    target_link_libraries(feather_full_system_benchmark
        PRIVATE Feather::FeatherResourceTracking
    )

    set_target_properties(feather_full_system_benchmark PROPERTIES
        OUTPUT_NAME FeatherFullSystemBenchmark
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

foreach(FEATHER_CPP_HEADER IN LISTS FEATHER_CPP_HEADERS)
    file(RELATIVE_PATH FEATHER_CPP_HEADER_RELATIVE
        "${CMAKE_CURRENT_SOURCE_DIR}/System"
        "${FEATHER_CPP_HEADER}"
    )
    get_filename_component(FEATHER_CPP_HEADER_DIRECTORY "${FEATHER_CPP_HEADER_RELATIVE}" DIRECTORY)
    install(FILES "${FEATHER_CPP_HEADER}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${FEATHER_CPP_HEADER_DIRECTORY}"
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
