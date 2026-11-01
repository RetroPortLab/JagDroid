cmake_minimum_required(VERSION 3.18)
project(jagdroid_native C CXX)
set(CMAKE_CXX_STANDARD 17)

# Shared emulator core lives at the repository root.
include(${CMAKE_CURRENT_LIST_DIR}/../../../../../core/core.cmake)

add_library(jagdroid SHARED jni_bridge.cpp)
target_link_libraries(jagdroid PRIVATE jagcore android log aaudio z)
