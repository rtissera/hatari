/*
  Hatari - libretro hard-disk content adapter

  SPDX-License-Identifier: GPL-2.0-or-later

  This adapter only changes the frontend-facing content boundary.  The
  existing ACSI/IDE/SCSI implementations remain responsible for
  validating, opening, and accessing the image; a "hatari_harddisk_bus"
  core option picks which one device 0 of the single hard-disk content
  slot attaches to (real ST hardware has no single canonical bus for a
  plain image file, so this can't be inferred from the file itself the
  way floppy vs. ACSI is inferred from extension).
*/

#include "sysdeps.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libretro.h>

#include "configuration.h"
#include "hdc.h"
#include "ide.h"
#include "main_retro.h"
#include "ncr5380.h"
#include "retro_harddisk.h"
#include "vfs.h"

enum harddisk_bus { BUS_ACSI, BUS_IDE, BUS_SCSI };

static CNF_SCSIDEV previous_acsi;
static CNF_IDEDEV previous_ide;
static CNF_SCSIDEV previous_scsi;
static bool previous_boot_from_harddisk;
static bool mounted;
static enum harddisk_bus mounted_bus;
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

static enum harddisk_bus selected_bus(void)
{
	struct retro_variable variable = { "hatari_harddisk_bus", NULL };

	if (environment_cb &&
	    environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) &&
	    variable.value)
	{
		if (!strcasecmp(variable.value, "ide"))
			return BUS_IDE;
		if (!strcasecmp(variable.value, "scsi"))
			return BUS_SCSI;
	}
	return BUS_ACSI;
}

static void set_device_path(enum harddisk_bus bus, const char *path)
{
	switch (bus)
	{
	 case BUS_IDE:
		ConfigureParams.Ide[0].bUseDevice = true;
		ConfigureParams.Ide[0].nBlockSize = 512;
		ConfigureParams.Ide[0].nByteSwap = BYTESWAP_AUTO;
		ConfigureParams.Ide[0].nDeviceType = 0; /* BDRV_TYPE_HD (ide.c) */
		snprintf(ConfigureParams.Ide[0].sDeviceFile,
		         sizeof(ConfigureParams.Ide[0].sDeviceFile), "%s", path);
		break;
	 case BUS_SCSI:
		ConfigureParams.Scsi[0].bUseDevice = true;
		ConfigureParams.Scsi[0].nBlockSize = 512;
		ConfigureParams.Scsi[0].nScsiVersion = 1;
		snprintf(ConfigureParams.Scsi[0].sDeviceFile,
		         sizeof(ConfigureParams.Scsi[0].sDeviceFile), "%s", path);
		break;
	 case BUS_ACSI:
	 default:
		ConfigureParams.Acsi[0].bUseDevice = true;
		ConfigureParams.Acsi[0].nBlockSize = 512;
		ConfigureParams.Acsi[0].nScsiVersion = 1;
		snprintf(ConfigureParams.Acsi[0].sDeviceFile,
		         sizeof(ConfigureParams.Acsi[0].sDeviceFile), "%s", path);
		break;
	}
}

static bool device_path_fits(enum harddisk_bus bus, const char *path)
{
	size_t size;
	switch (bus)
	{
	 case BUS_IDE: size = sizeof(ConfigureParams.Ide[0].sDeviceFile); break;
	 case BUS_SCSI: size = sizeof(ConfigureParams.Scsi[0].sDeviceFile); break;
	 case BUS_ACSI: default: size = sizeof(ConfigureParams.Acsi[0].sDeviceFile); break;
	}
	return strlen(path) < size;
}

static void restore_previous(enum harddisk_bus bus)
{
	switch (bus)
	{
	 case BUS_IDE: ConfigureParams.Ide[0] = previous_ide; break;
	 case BUS_SCSI: ConfigureParams.Scsi[0] = previous_scsi; break;
	 case BUS_ACSI: default: ConfigureParams.Acsi[0] = previous_acsi; break;
	}
	ConfigureParams.HardDisk.bBootFromHardDisk = previous_boot_from_harddisk;
}

static bool bus_init(enum harddisk_bus bus)
{
	switch (bus)
	{
	 case BUS_IDE:
		Ide_UnInit();
		Ide_Init();
		return ConfigureParams.Ide[0].bUseDevice;
	 case BUS_SCSI:
		Ncr5380_UnInit();
		return Ncr5380_Init() && ConfigureParams.Scsi[0].bUseDevice;
	 case BUS_ACSI:
	 default:
		HDC_UnInit();
		return HDC_Init() && bAcsiEmuOn;
	}
}

static void bus_uninit(enum harddisk_bus bus)
{
	switch (bus)
	{
	 case BUS_IDE: Ide_UnInit(); break;
	 case BUS_SCSI: Ncr5380_UnInit(); break;
	 case BUS_ACSI: default: HDC_UnInit(); break;
	}
}

static void unmount_image(void)
{
	if (!mounted)
		return;

	bus_uninit(mounted_bus);
	RetroVfs_Release(original_path, materialized_path, true);
	original_path[0] = '\0';
	restore_previous(mounted_bus);
	bus_init(mounted_bus);
	mounted = false;
}

bool RetroHardDisk_LoadGame(const struct retro_game_info *game)
{
	enum harddisk_bus bus;

	if (!game || !game->path || !is_harddisk_image(game->path))
		return false;

	RetroHardDisk_UnloadGame();
	bus = selected_bus();
	previous_acsi = ConfigureParams.Acsi[0];
	previous_ide = ConfigureParams.Ide[0];
	previous_scsi = ConfigureParams.Scsi[0];
	previous_boot_from_harddisk = ConfigureParams.HardDisk.bBootFromHardDisk;

	if (!device_path_fits(bus, game->path))
		return false;
	set_device_path(bus, game->path);
	ConfigureParams.HardDisk.bBootFromHardDisk = true;

	if (!bus_init(bus))
	{
		char temporary[PATH_MAX];
		if (!RetroVfs_MakeTemplate("hatari-libretro-hd-XXXXXX", "", temporary,
		                           sizeof(temporary)))
		{
			restore_previous(bus);
			bus_init(bus);
			return false;
		}
		if (!RetroVfs_Materialize(game->path, temporary, sizeof(temporary),
		                          RETRO_HARDDISK_MATERIALIZE_LIMIT, 0,
		                          materialized_path,
		                          sizeof(materialized_path)))
		{
			restore_previous(bus);
			bus_init(bus);
			return false;
		}
		if (!device_path_fits(bus, materialized_path))
		{
			RetroVfs_Release(NULL, materialized_path, false);
			restore_previous(bus);
			bus_init(bus);
			return false;
		}
		set_device_path(bus, materialized_path);
		if (!bus_init(bus))
		{
			RetroVfs_Release(NULL, materialized_path, false);
			restore_previous(bus);
			bus_init(bus);
			return false;
		}
	}

	if (snprintf(original_path, sizeof(original_path), "%s", game->path) >=
	    (int)sizeof(original_path))
	{
		bus_uninit(bus);
		RetroVfs_Release(NULL, materialized_path, false);
		restore_previous(bus);
		bus_init(bus);
		return false;
	}

	mounted = true;
	mounted_bus = bus;
	return true;
}

void RetroHardDisk_UnloadGame(void)
{
	unmount_image();
}
