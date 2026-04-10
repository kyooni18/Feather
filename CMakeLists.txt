cmake_minimum_required(VERSION 3.20)

project(Feather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(feather_demo
  FeatherSystem/FeatherRuntime/FSScheduler.cpp
)

target_include_directories(feather_demo PRIVATE
  FeatherSystem
  FeatherSystem/FeatherRuntime
)

# Optional: nicer warnings (works for AppleClang/Clang/GCC)
target_compile_options(feather_demo PRIVATE
  -Wall -Wextra -Wpedantic
)