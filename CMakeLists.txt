cmake_minimum_required(VERSION 3.20)

project(Feather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(Feather STATIC
  FeatherSystem/Feather/Feather.cpp
  FeatherSystem/FeatherRuntime/FSScheduler.cpp
  FeatherSystem/FeatherRuntime/FSTime.cpp
)

set_target_properties(Feather PROPERTIES
  OUTPUT_NAME Feather
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_include_directories(Feather PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/FeatherSystem>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/FeatherSystem/FeatherRuntime>"
)

target_compile_options(Feather PRIVATE
  -Wall -Wextra -Wpedantic
)

add_executable(feather_demo Test/test1.cpp)
target_link_libraries(feather_demo PRIVATE Feather)

install(TARGETS Feather
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(FILES FeatherSystem/Feather/Feather.hpp
  DESTINATION include/Feather
)
install(FILES
  FeatherSystem/FeatherRuntime/FSScheduler.hpp
  FeatherSystem/FeatherRuntime/FSTime.hpp
  DESTINATION include/Feather/FeatherRuntime
)
