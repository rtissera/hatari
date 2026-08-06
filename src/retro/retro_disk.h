/*
  Hatari - libretro disk control

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef HATARI_RETRO_DISK_H
#define HATARI_RETRO_DISK_H

#include <libretro.h>

void RetroDisk_SetEnvironment(retro_environment_t cb);
bool RetroDisk_LoadGame(const struct retro_game_info *game);
bool RetroDisk_LoadGameSpecial(const struct retro_game_info *info,
		size_t num_info);
void RetroDisk_UnloadGame(void);

#endif
