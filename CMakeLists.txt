cmake_minimum_required(VERSION 3.20)

project(Feather VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FEATHER_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FeatherSystem")

set(FEATHER_CORE_SOURCES
  FeatherSystem/FeatherRuntime/FSTime.cpp
)

set(FEATHER_SCHEDULER_SOURCES
  FeatherSystem/FeatherRuntime/FSScheduler.cpp
)

set(FEATHER_EVENT_SOURCES
  FeatherSystem/FeatherRuntime/FSEvents.cpp
)

set(FEATHER_FACADE_SOURCES
  FeatherSystem/Feather.cpp
)

set(FEATHER_CORE_HEADERS
  FeatherSystem/Core/Clock.hpp
)

set(FEATHER_SCHEDULER_HEADERS
  FeatherSystem/Scheduler/Scheduler.hpp
)

set(FEATHER_EVENT_HEADERS
  FeatherSystem/Events/Event.hpp
)

set(FEATHER_PLATFORM_HEADERS
  FeatherSystem/Platform/Platform.hpp
)

set(FEATHER_UI_HEADERS
  FeatherSystem/UI/Display.hpp
  FeatherSystem/FeatherUI/FSDisplay.hpp
)

set(FEATHER_COMPAT_HEADERS
  FeatherSystem/FeatherRuntime/FSEvent.hpp
  FeatherSystem/FeatherRuntime/FSEvents.hpp
  FeatherSystem/FeatherRuntime/FSScheduler.hpp
  FeatherSystem/FeatherRuntime/FSTime.hpp
)

function(feather_configure_public_target target_name)
  target_include_directories(${target_name} PUBLIC
    "$<BUILD_INTERFACE:${FEATHER_PUBLIC_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  )
endfunction()

function(feather_configure_warning_target target_name)
  target_compile_options(${target_name} PRIVATE
    -Wall -Wextra -Wpedantic
  )
endfunction()

add_library(FeatherCore STATIC ${FEATHER_CORE_SOURCES})
add_library(Feather::Core ALIAS FeatherCore)
set_target_properties(FeatherCore PROPERTIES
  OUTPUT_NAME FeatherCore
  EXPORT_NAME Core
  POSITION_INDEPENDENT_CODE ON
)
feather_configure_public_target(FeatherCore)
feather_configure_warning_target(FeatherCore)

add_library(FeatherScheduler STATIC ${FEATHER_SCHEDULER_SOURCES})
add_library(Feather::Scheduler ALIAS FeatherScheduler)
set_target_properties(FeatherScheduler PROPERTIES
  OUTPUT_NAME FeatherScheduler
  EXPORT_NAME Scheduler
  POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(FeatherScheduler PUBLIC FeatherCore)
feather_configure_public_target(FeatherScheduler)
feather_configure_warning_target(FeatherScheduler)

add_library(FeatherEvents STATIC ${FEATHER_EVENT_SOURCES})
add_library(Feather::Events ALIAS FeatherEvents)
set_target_properties(FeatherEvents PROPERTIES
  EXPORT_NAME Events
)
target_link_libraries(FeatherEvents PUBLIC FeatherScheduler)
feather_configure_public_target(FeatherEvents)
feather_configure_warning_target(FeatherEvents)

set(FEATHER_ALL_RUNTIME_SOURCES
  ${FEATHER_FACADE_SOURCES}
  ${FEATHER_CORE_SOURCES}
  ${FEATHER_SCHEDULER_SOURCES}
)

add_library(FeatherStatic STATIC ${FEATHER_ALL_RUNTIME_SOURCES})
add_library(Feather::Feather ALIAS FeatherStatic)
add_library(Feather::Static ALIAS FeatherStatic)

add_library(FeatherShared SHARED ${FEATHER_ALL_RUNTIME_SOURCES})
add_library(Feather::Shared ALIAS FeatherShared)

set_target_properties(FeatherStatic FeatherShared PROPERTIES
  OUTPUT_NAME Feather
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}"
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)
set_target_properties(FeatherStatic PROPERTIES EXPORT_NAME Feather)
set_target_properties(FeatherShared PROPERTIES EXPORT_NAME Shared)

feather_configure_public_target(FeatherStatic)
feather_configure_public_target(FeatherShared)
feather_configure_warning_target(FeatherStatic)
feather_configure_warning_target(FeatherShared)

add_library(FeatherUI INTERFACE)
add_library(Feather::UI ALIAS FeatherUI)
set_target_properties(FeatherUI PROPERTIES
  EXPORT_NAME UI
)
target_link_libraries(FeatherUI INTERFACE FeatherStatic)
target_include_directories(FeatherUI INTERFACE
  "$<BUILD_INTERFACE:${FEATHER_PUBLIC_INCLUDE_DIR}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

option(FEATHER_BUILD_DEMO "Build feather_demo sample executable" OFF)
if(FEATHER_BUILD_DEMO)
  add_executable(feather_demo Test/test1.cpp)
  target_link_libraries(feather_demo PRIVATE Feather::Feather)
endif()

install(TARGETS
    FeatherCore
    FeatherScheduler
    FeatherEvents
    FeatherStatic
    FeatherShared
    FeatherUI
  EXPORT FeatherTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES FeatherSystem/Feather.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather
)
install(FILES ${FEATHER_CORE_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/Core
)
install(FILES ${FEATHER_SCHEDULER_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/Scheduler
)
install(FILES ${FEATHER_EVENT_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/Events
)
install(FILES ${FEATHER_PLATFORM_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/Platform
)
install(FILES FeatherSystem/UI/Display.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/UI
)
install(FILES FeatherSystem/FeatherUI/FSDisplay.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/FeatherUI
)
install(FILES ${FEATHER_COMPAT_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather/FeatherRuntime
)

install(EXPORT FeatherTargets
  FILE FeatherTargets.cmake
  NAMESPACE Feather::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Feather
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FeatherConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Feather"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FeatherConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Feather"
)
