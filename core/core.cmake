# Shared by the desktop build and the Android NDK build.
set(JAG_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(MUSASHI ${JAG_ROOT}/third_party/Musashi)

add_library(jagcore STATIC
  ${JAG_ROOT}/core/jaguar.cpp
  ${JAG_ROOT}/core/risc.cpp
  ${JAG_ROOT}/core/video.cpp
  ${JAG_ROOT}/core/blitter.cpp
  ${JAG_ROOT}/core/savedata.cpp
  ${JAG_ROOT}/core/cdrom.cpp
  ${JAG_ROOT}/core/memtrack.cpp
  ${JAG_ROOT}/core/enhance.cpp
  ${JAG_ROOT}/core/zipfile.cpp
  ${MUSASHI}/m68kcpu.c
  ${MUSASHI}/m68kops.c
  ${MUSASHI}/m68kdasm.c
  ${MUSASHI}/softfloat/softfloat.c
)
target_include_directories(jagcore PUBLIC ${JAG_ROOT}/core ${MUSASHI})
target_compile_definitions(jagcore PUBLIC MUSASHI_CNF="m68kconf_jag.h")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jagcore PRIVATE $<$<COMPILE_LANGUAGE:C>:-w> -O2)
endif()
