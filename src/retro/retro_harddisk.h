/*
  Hatari - libretro hard-disk content adapter

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef HATARI_RETRO_HARDDISK_H
#define HATARI_RETRO_HARDDISK_H

#include <libretro.h>

bool RetroHardDisk_LoadGame(const struct retro_game_info *game);
void RetroHardDisk_UnloadGame(void);

#endif
