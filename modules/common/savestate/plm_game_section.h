/* Force-included into a decomp port's game sources only (module.cmake): every global and static the game defines goes
   into two dedicated sections, so its whole state is one memory range that save states copy (plm_savestate.cpp).
   Section names are C identifiers so the linker provides __start_/__stop_ bounds. */
#pragma clang section bss = "plm_game_bss" data = "plm_game_data"
