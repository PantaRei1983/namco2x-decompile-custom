# host.cmake -- THE SYSTEM 23 HOST, shared by every System 23 / Evolution 2 game (HARD RULE 3). A game's CMakeLists includes this and adds
# its own table (src/tc2_game.c here, ../crszone/src/csz_game.c), its lifted program and its two translated H8 programs.
set(S23_HOST_DIR ${CMAKE_CURRENT_LIST_DIR})
set(S23_HOST_SRC ${S23_HOST_DIR}/src/s23_ss_begin.c)   # FIRST: the start of the game's state (save states, src/s23_savestate.c)
# S23_HOST_TAIL goes LAST in a game's sources (after its lifted program): the end of the state, then the save-state code itself
# then THE HOST'S OWN SIDE, outside the saved state: the window, the GPU, audio out, input, the menu, the settings, the outputs, the profiler
# -- Windows / SDL / GL handles, sockets, thread locks and the frame pacer's clock belong to the running process (a load that put back
# another process's values crashed: the user's F9 captures, 2026-11-09)
set(S23_HOST_TAIL ${S23_HOST_DIR}/src/s23_ss_end.c ${S23_HOST_DIR}/src/s23_savestate.c)
foreach(f src/s23_window.c src/eng/eng_ui.c src/eng/eng_crosshair.c src/eng/eng_cfg.c src/eng/eng_display.c src/eng/ss22_input.c src/eng/eng_ffb.c
          src/s23_input.c src/s23_gl.c src/s23_audio.c src/s23_out.c src/s23_prof.c src/s23_subs.c)
  list(APPEND S23_HOST_TAIL ${S23_HOST_DIR}/${f})
endforeach()
foreach(f src/s23_cpu.c src/s23_mem.c src/s23_ctl.c src/s23_chips.c src/s23_screen.c src/s23_text.c src/s23_main.c
          src/s23_c435.c src/s23_render.c src/h8_periph.c src/s23_h8.c src/s23_rom.c
          src/eng/romzip.c src/eng/c352.c src/s23_c422.c src/s23_guns.c)
  list(APPEND S23_HOST_SRC ${S23_HOST_DIR}/${f})
endforeach()
set(S23_HOST_INCLUDE ${S23_HOST_DIR}/include ${S23_HOST_DIR}/src ${S23_HOST_DIR}/src/eng)
