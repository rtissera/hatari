/*
  Hatari - libretro core options

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef HATARI_RETRO_OPTIONS_H
#define HATARI_RETRO_OPTIONS_H

#include <libretro.h>

void RetroOptions_SetEnvironment(retro_environment_t cb);
void RetroOptions_Apply(void);
bool RetroOptions_Update(void);

/* Which RetroPad port (0-3), if any, drives the given Atari JOYID_* slot
   (src/includes/configuration.h). Returns -1 if none is configured. */
int RetroOptions_JoystickPortFor(int joyid);

#endif
