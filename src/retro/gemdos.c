/*
  Hatari - libretro GEMDOS host-directory content adapter

  SPDX-License-Identifier: GPL-2.0-or-later

  Real ST hardware has no concept of a host directory appearing as a
  drive - this is purely an Hatari convenience feature
  (ConfigureParams.HardDisk.bUseHardDiskDirectories/szHardDiskDirectories,
  already wired unconditionally into Main_InitSubsystems() via
  GemDOS_InitDrives()) - but it's the single most common way ST software
  is actually run today, since it needs no disk image at all.

  libretro's content model hands the core exactly one file, never a
  directory, and core options are dropdown-only (no freeform path entry a
  user could practically fill in). So content here is a small "loader"
  text file (extension .gemdos) whose single line is the host directory
  path to mount - the same "pointer file" precedent M3U playlists already
  set for floppy content. This can only work against a real host
  filesystem: a directory has no VFS equivalent to materialize the way a
  single file does, so both the loader file and the directory it points
  to are read directly via the host filesystem, not through libretro's
  VFS interface.
*/

#include "sysdeps.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <libretro.h>

#include "configuration.h"
#include "gemdos.h"
#include "retro_gemdos.h"

static bool previous_use_directories;
static char previous_directory[FILENAME_MAX];
static bool previous_boot_from_harddisk;
static bool mounted;

static bool has_extension(const char *path, const char *extension)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot + 1, extension);
}

static bool is_gemdos_loader(const char *path)
{
	return has_extension(path, "gemdos");
}

static bool read_loader_path(const char *loader_path, char *out, size_t out_size)
{
	FILE *file;
	size_t length;

	file = fopen(loader_path, "r");
	if (!file)
		return false;
	if (!fgets(out, (int)out_size, file))
	{
		fclose(file);
		return false;
	}
	fclose(file);

	length = strlen(out);
	while (length && (out[length - 1] == '\n' || out[length - 1] == '\r'))
		out[--length] = '\0';
	return length > 0;
}

static bool directory_exists(const char *path)
{
	struct stat info;
	return stat(path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

static void apply_directory(bool use_directories, const char *directory,
		bool boot_from_harddisk)
{
	ConfigureParams.HardDisk.bUseHardDiskDirectories = use_directories;
	snprintf(ConfigureParams.HardDisk.szHardDiskDirectories[0],
	         sizeof(ConfigureParams.HardDisk.szHardDiskDirectories[0]), "%s",
	         directory);
	ConfigureParams.HardDisk.bBootFromHardDisk = boot_from_harddisk;
}

static void unmount(void)
{
	if (!mounted)
		return;

	GemDOS_UnInitDrives();
	apply_directory(previous_use_directories, previous_directory,
	                 previous_boot_from_harddisk);
	if (ConfigureParams.HardDisk.bUseHardDiskDirectories)
		GemDOS_InitDrives();
	mounted = false;
}

bool RetroGemDos_LoadGame(const struct retro_game_info *game)
{
	char directory[FILENAME_MAX];

	if (!game || !game->path || !is_gemdos_loader(game->path))
		return false;
	if (!read_loader_path(game->path, directory, sizeof(directory)) ||
	    !directory_exists(directory))
		return false;

	RetroGemDos_UnloadGame();
	previous_use_directories = ConfigureParams.HardDisk.bUseHardDiskDirectories;
	snprintf(previous_directory, sizeof(previous_directory), "%s",
	         ConfigureParams.HardDisk.szHardDiskDirectories[0]);
	previous_boot_from_harddisk = ConfigureParams.HardDisk.bBootFromHardDisk;

	apply_directory(true, directory, true);
	GemDOS_UnInitDrives();
	GemDOS_InitDrives();

	mounted = true;
	return true;
}

void RetroGemDos_UnloadGame(void)
{
	unmount();
}
