/*
  Hatari - libretro VFS helper

  SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef HATARI_RETRO_VFS_H
#define HATARI_RETRO_VFS_H

#include <libretro.h>

void RetroVfs_SetEnvironment(retro_environment_t cb);
bool RetroVfs_MakeTemplate(const char *name, char *path, size_t path_size);
bool RetroVfs_Materialize(const char *source, char *template_path,
		int64_t size_limit, char *destination, size_t destination_size);
void RetroVfs_Writeback(const char *source, const char *materialized);
void RetroVfs_Release(const char *source, char *materialized, bool writeback);

#endif
