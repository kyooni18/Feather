cmake_minimum_required(VERSION 3.20)

project(Feather VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FEATHER_SOURCES
  FeatherSystem/Feather.cpp
  FeatherSystem/FeatherRuntime/FSScheduler.cpp
  FeatherSystem/FeatherRuntime/FSTime.cpp
)

add_library(FeatherStatic STATIC ${FEATHER_SOURCES})
add_library(FeatherShared SHARED ${FEATHER_SOURCES})
add_library(Feather::Static ALIAS FeatherStatic)
add_library(Feather::Shared ALIAS FeatherShared)

set_target_properties(FeatherStatic FeatherShared PROPERTIES
  OUTPUT_NAME Feather
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}"
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(FeatherStatic PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/FeatherSystem>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_include_directories(FeatherShared PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/FeatherSystem>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_compile_options(FeatherStatic PRIVATE
  -Wall -Wextra -Wpedantic
)
target_compile_options(FeatherShared PRIVATE
  -Wall -Wextra -Wpedantic
)

option(FEATHER_BUILD_DEMO "Build feather_demo sample executable" OFF)
if(FEATHER_BUILD_DEMO)
  add_executable(feather_demo Test/test1.cpp)
  target_link_libraries(feather_demo PRIVATE FeatherStatic)
endif()

install(TARGETS FeatherStatic FeatherShared
  EXPORT FeatherTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES FeatherSystem/Feather.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Feather
)
install(FILES
  FeatherSystem/FeatherRuntime/FSEvent.hpp
  FeatherSystem/FeatherRuntime/FSScheduler.hpp
  FeatherSystem/FeatherRuntime/FSTime.hpp
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
