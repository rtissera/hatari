#ifndef HATARI_RETRO_GEMDOS_H
#define HATARI_RETRO_GEMDOS_H

#include <libretro.h>

/* Content is a small "loader" text file (extension .gemdos) whose single
   line is the absolute host path of a directory to expose as an Atari
   GEMDOS drive. Returns false (without side effects) for any other
   content, so it's safe to try before/after the other content loaders
   in retro_load_game(). */
bool RetroGemDos_LoadGame(const struct retro_game_info *game);
void RetroGemDos_UnloadGame(void);

#endif
