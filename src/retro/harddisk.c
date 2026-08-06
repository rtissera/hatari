/*
  Hatari - libretro hard-disk content adapter

  SPDX-License-Identifier: GPL-2.0-or-later

  This adapter only changes the frontend-facing content boundary.  The
  existing ACSI implementation remains responsible for validating, opening,
  and accessing the image.
*/

#include "sysdeps.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libretro.h>

#include "configuration.h"
#include "hdc.h"
#include "retro_harddisk.h"
#include "vfs.h"

static CNF_SCSIDEV previous_acsi;
static bool previous_boot_from_harddisk;
static bool mounted;
static char original_path[FILENAME_MAX];
static char materialized_path[FILENAME_MAX];

#define RETRO_HARDDISK_MATERIALIZE_LIMIT (256 * 1024 * 1024)

static bool has_extension(const char *path, const char *extension)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot + 1, extension);
}

static bool is_harddisk_image(const char *path)
{
	return has_extension(path, "hd") || has_extension(path, "hdf") ||
	       has_extension(path, "hdi") || has_extension(path, "vhd") ||
	       has_extension(path, "sthd");
}

static bool mount_image(void)
{
	HDC_UnInit();
	return HDC_Init() && bAcsiEmuOn;
}

static void unmount_image(void)
{
	if (!mounted)
		return;

	HDC_UnInit();
	RetroVfs_Release(original_path, materialized_path, true);
	original_path[0] = '\0';
	ConfigureParams.Acsi[0] = previous_acsi;
	ConfigureParams.HardDisk.bBootFromHardDisk = previous_boot_from_harddisk;
	HDC_Init();
	mounted = false;
}

bool RetroHardDisk_LoadGame(const struct retro_game_info *game)
{
	if (!game || !game->path || !is_harddisk_image(game->path))
		return false;

	RetroHardDisk_UnloadGame();
	previous_acsi = ConfigureParams.Acsi[0];
	previous_boot_from_harddisk = ConfigureParams.HardDisk.bBootFromHardDisk;

	ConfigureParams.Acsi[0].bUseDevice = true;
	ConfigureParams.Acsi[0].nBlockSize = 512;
	ConfigureParams.Acsi[0].nScsiVersion = 1;
	if (snprintf(ConfigureParams.Acsi[0].sDeviceFile,
	             sizeof(ConfigureParams.Acsi[0].sDeviceFile), "%s", game->path)
	    >= (int)sizeof(ConfigureParams.Acsi[0].sDeviceFile))
	{
		ConfigureParams.Acsi[0] = previous_acsi;
		return false;
	}
	ConfigureParams.HardDisk.bBootFromHardDisk = true;

	if (!mount_image())
	{
		char temporary[] = "/tmp/hatari-libretro-hd-XXXXXX";
		if (!RetroVfs_Materialize(game->path, temporary,
		                          RETRO_HARDDISK_MATERIALIZE_LIMIT,
		                          materialized_path,
		                          sizeof(materialized_path)))
		{
			ConfigureParams.Acsi[0] = previous_acsi;
			ConfigureParams.HardDisk.bBootFromHardDisk = previous_boot_from_harddisk;
			HDC_Init();
			return false;
		}
		ConfigureParams.Acsi[0].bUseDevice = true;
		if (snprintf(ConfigureParams.Acsi[0].sDeviceFile,
		             sizeof(ConfigureParams.Acsi[0].sDeviceFile), "%s",
		             materialized_path) >=
		    (int)sizeof(ConfigureParams.Acsi[0].sDeviceFile) ||
		    !mount_image())
		{
			RetroVfs_Release(NULL, materialized_path, false);
			ConfigureParams.Acsi[0] = previous_acsi;
			ConfigureParams.HardDisk.bBootFromHardDisk = previous_boot_from_harddisk;
			HDC_Init();
			return false;
		}
	}

	if (snprintf(original_path, sizeof(original_path), "%s", game->path) >=
	    (int)sizeof(original_path))
	{
		HDC_UnInit();
		RetroVfs_Release(NULL, materialized_path, false);
		ConfigureParams.Acsi[0] = previous_acsi;
		ConfigureParams.HardDisk.bBootFromHardDisk = previous_boot_from_harddisk;
		HDC_Init();
		return false;
	}

	mounted = true;
	return true;
}

void RetroHardDisk_UnloadGame(void)
{
	unmount_image();
}
