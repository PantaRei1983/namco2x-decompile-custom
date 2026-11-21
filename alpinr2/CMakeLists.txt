cmake_minimum_required(VERSION 3.16)
project(alpinr2 C)

# Alpine Racer 2 on the shared Super 22 engine -- a FEASIBILITY TEST (see src/a2_game.c). The 68K program is lifted by
# ../tools/gen/lift68k.py from Ghidra's p-code:
#   python ../tools/gen/lift68k.py alpinr2.pcode gen a2 8 alpinr2_main.bin
# The master DSP and the sound MCU are interpreted (src/a2_interp.c, src/a2_snd_interp.c).
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "gcc required")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-fno-strict-aliasing -fwrapv)

include_directories(include gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.cmake)

file(GLOB A2_LIFTED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/a2_lifted_*.c)
set_source_files_properties(${A2_LIFTED} PROPERTIES COMPILE_OPTIONS "-O1;-w;-DLIFT_LAST_PC")
set_source_files_properties(src/a2_snd_interp.c src/a2_interp.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})
if(WIN32)
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/../include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS} ws2_32)
  set(A2_GL_LIB "")
  set(A2_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
else()
  set(A2_GL_LIB OpenGL::GL)
  set(A2_GL_SRC "")
endif()

add_executable(a2 src/a2_game.c src/a2_interp.c src/a2_snd_interp.c
    ${NAMCO22_ENGINE_NET_SRC}
    ${NAMCO22_ENGINE_LIFT_SRC} ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_ENGINE_SND_SRC}
    ${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SS22_SRC} ${NAMCO22_ENGINE_SS22_HOST_SRC} ${NAMCO22_ENGINE_UI_SRC} ${NAMCO22_ENGINE_ROMZIP_SRC} ${A2_GL_SRC}
    ${A2_LIFTED})
target_compile_options(a2 PRIVATE -Wall)
target_link_libraries(a2 ${SDL2_LIBRARIES} ${A2_GL_LIB} ZLIB::ZLIB m)

