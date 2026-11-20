cmake_minimum_required(VERSION 3.16)
project(ridgerac C)
# Ridge Racer (System 22, MAME `ridgerac`, World RR2 Ver.B) on the SHARED runtime: ../raverace/src + ../engine, with OUR lifted 68K
# (tools/import.sh -> gen/rr_lifted_*.c, the rr_* ABI), one table (src/rg_game.c) and the DSP / sound MCU interpreted (Victory Lap's
# ../victlap/src/vl_interp.c, used from there: HARD RULE 3, no copies).
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "gcc required (the lifted code uses computed labels and statement expressions)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-fno-strict-aliasing -fwrapv)
set(RR ${CMAKE_CURRENT_SOURCE_DIR}/../raverace)
# PORTABLE (default ON): pin glibc symbol versions so a binary built here runs on older systems -- the Steam Deck included
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${RR}/include/glibc_compat.h)
endif()
include_directories(${RR}/include gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.cmake)
file(GLOB RG_LIFTED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/rr_lifted_*.c)
if(NOT RG_LIFTED)
  message(FATAL_ERROR "gen/rr_lifted_*.c missing: run tools/import.sh first (Ghidra + ../tools/gen/lift68k.py, see the README)")
endif()
set_source_files_properties(${RG_LIFTED} PROPERTIES COMPILE_OPTIONS "-O1;-foptimize-sibling-calls;-w;-DLIFT_LAST_PC")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# Windows (MinGW-w64): Rave Racer's block -- win_compat.h, no console, everything static, OpenGL resolved at run time
# (engine/gl_dyn.c) so the bundled Mesa can stand in, Winsock for the link.
if(WIN32)
  add_compile_options(-include ${RR}/include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS})
  set(RG_GL_LIB "")
  set(RG_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
  set(RG_NET_LIB ws2_32)
else()
  set(RG_GL_LIB OpenGL::GL)
  set(RG_GL_SRC "")
  set(RG_NET_LIB "")
endif()

# the readable-C harness hooks rr_main.c / rr_mem.c call; Ridge Racer has no readable functions, so they are inert
set(RG_RD ${RR}/src/rd/rd_core.c src/rg_rd_stub.c)

set(RG_CORE ${RR}/src/rr_romzip.c ${NAMCO22_ENGINE_DIR}/romzip.c ${RR}/src/rr_main.c ${NAMCO22_ENGINE_LINK_SRC} ${NAMCO22_ENGINE_LIFT_SRC}
    ${RR}/src/rr_mem.c ${RR}/src/rr_hw.c ${RR}/src/rr_env.c src/rg_game.c ${RR}/src/rr_link.c ${RR}/src/rr_net.c ${NAMCO22_ENGINE_NET_SRC} ${RR}/src/rr_dsp.c
    ${NAMCO22_ENGINE_C25_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/../victlap/src/vl_interp.c ${RR}/src/rr_scene.c ${RR}/src/rr_gl.c ${NAMCO22_ENGINE_GL_SRC}
    ${RR}/src/rr_host.c ${RR}/src/rr_ui.c ${RR}/src/rr_input.c ${NAMCO22_ENGINE_DIR}/twin_stick.c ${NAMCO22_ENGINE_FFB_SRC} ${RR}/src/rr_audio.c ${NAMCO22_ENGINE_SND_SRC}
    ${RR}/src/snd/snd_board.c ${RG_LIFTED} ${RG_RD})

# THE GAME
add_executable(rg ${RR}/src/rr_win.c ${RG_CORE} ${RG_GL_SRC})
target_include_directories(rg PRIVATE ${SDL2_INCLUDE_DIRS} ${RR}/src/snd)
if(WIN32)
  target_link_options(rg PRIVATE -Wl,--stack,67108864)   # 64 MB: the lifted program nests C calls (Windows' default is 1 MB)
endif()
target_link_libraries(rg ${SDL2_LIBRARIES} ${RG_GL_LIB} ZLIB::ZLIB ${RG_NET_LIB} m)
