cmake_minimum_required(VERSION 3.16)
project(jagdroid C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/core/core.cmake)

# Headless desktop runner for testing (not built for Android)
if(NOT ANDROID)
  add_executable(jagrun desktop/jagrun.cpp)
  target_link_libraries(jagrun PRIVATE jagcore z)

  # Tracing variant: records the 68000's recent PCs to diagnose crashes.
  get_target_property(JAG_SRCS jagcore SOURCES)
  add_executable(jagtrace desktop/jagtrace.cpp ${JAG_SRCS})
  target_include_directories(jagtrace PRIVATE core third_party/Musashi)
  target_compile_definitions(jagtrace PRIVATE MUSASHI_CNF="m68kconf_jag.h" JAG_TRACE=1)
  target_compile_options(jagtrace PRIVATE $<$<COMPILE_LANGUAGE:C>:-w> -O2)
  target_link_libraries(jagtrace PRIVATE z)
endif()
