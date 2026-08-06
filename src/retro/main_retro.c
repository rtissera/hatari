
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <libretro.h>

#include "main.h"
#include "main_retro.h"
#include "hatari-glue.h"
#include "dialog.h"
#include "floppy.h"
#include "memorySnapShot.h"
#include "m68000.h"
#include "retro_options.h"
#include "reset.h"
#include "retro_disk.h"
#include "retro_harddisk.h"
#include "vfs.h"
#include "screen.h"
#include "stMemory.h"
#include "sound.h"
#include "tos.h"
#include "vdi.h"
#include "version.h"

static bool has_cpu_config_changed = true;
static unsigned last_video_width;
static unsigned last_video_height;
static char pending_state_path[PATH_MAX];
static char system_directory[PATH_MAX];
static char tos_path[PATH_MAX];

/* Hatari's native snapshot code operates on files.  Keep that implementation
 * as the single source of truth and bridge it to libretro's memory API here. */
static bool snapshot_path(char *path, size_t path_size)
{
	int fd;

	if (path_size < sizeof("/tmp/hatari-libretro-state-XXXXXX"))
		return false;

	if (snprintf(path, path_size, "/tmp/hatari-libretro-state-XXXXXX") >= (int)path_size)
		return false;

	fd = mkstemp(path);
	if (fd < 0)
		return false;
	close(fd);
	unlink(path);
	return true;
}

static bool snapshot_read(void **data, size_t *size)
{
	char path[PATH_MAX];
	FILE *file;
	long length;
	void *buffer;

	if (!snapshot_path(path, sizeof(path)))
		return false;

	MemorySnapShot_Capture_Immediate(path, false);
	file = fopen(path, "rb");
	if (!file)
		return false;
	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		unlink(path);
		return false;
	}
	length = ftell(file);
	if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		unlink(path);
		return false;
	}
	buffer = malloc((size_t)length);
	if (!buffer || fread(buffer, 1, (size_t)length, file) != (size_t)length)
	{
		free(buffer);
		fclose(file);
		unlink(path);
		return false;
	}
	fclose(file);
	unlink(path);
	*data = buffer;
	*size = (size_t)length;
	return true;
}

retro_environment_t environment_cb;
retro_video_refresh_t video_refresh_cb;
retro_input_poll_t input_poll_cb;
retro_input_state_t input_state_cb;
unsigned retro_controller_devices[2] = {
	RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD
};


RETRO_API void retro_set_environment(retro_environment_t cb)
{
	static enum retro_pixel_format pixelformat = RETRO_PIXEL_FORMAT_XRGB8888;
	static bool no_game = true;
	static struct retro_controller_description controller_types[] = {
		{ "RetroPad", RETRO_DEVICE_JOYPAD }
	};
	static struct retro_controller_info controller_info[] = {
		{ controller_types, 1 },
		{ controller_types, 1 }
	};
	static struct retro_input_descriptor input_descriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick 0 Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick 0 Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick 0 Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick 0 Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Joystick 0 Fire 1" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Joystick 0 Fire 2" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Joystick 0 Fire 3" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick 1 Left" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick 1 Right" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick 1 Up" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick 1 Down" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Joystick 1 Fire 1" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Joystick 1 Fire 2" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Joystick 1 Fire 3" },
		{ 0 }
	};

	environment_cb = cb;

	/* Hatari only supports 32-bit bits per pixel */
	cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixelformat);

	/* Hatari can start without game disks */
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)controller_info);
	cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_descriptors);
	RetroOptions_SetEnvironment(cb);
	RetroDisk_SetEnvironment(cb);
	RetroVfs_SetEnvironment(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_refresh_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_init(void)
{
	char name[] = "hatari";
	char tos_option[] = "--tos";
	char *argv[1] = { name };
	int argc = 1;
	const char *directory = NULL;
	last_video_width = 0;
	last_video_height = 0;

	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
	                                     &directory) && directory && *directory)
	{
		if (snprintf(system_directory, sizeof(system_directory), "%s", directory)
		    >= (int)sizeof(system_directory) ||
		    snprintf(tos_path, sizeof(tos_path), "%s/%s", system_directory,
		             "tos.img") >= (int)sizeof(tos_path))
			tos_path[0] = '\0';
	}
	if (tos_path[0])
	{
		static char *argv_with_tos[3];
		argv_with_tos[0] = name;
		argv_with_tos[1] = tos_option;
		argv_with_tos[2] = tos_path;
		argc = 3;
		Main_SetPreInitHook(RetroOptions_Apply);
		Main_Init(argc, argv_with_tos);
	}
	else
	{
		Main_SetPreInitHook(RetroOptions_Apply);
		Main_Init(argc, argv);
	}
	has_cpu_config_changed = true;
}

RETRO_API void retro_deinit(void)
{
	if (pending_state_path[0])
		unlink(pending_state_path);
	pending_state_path[0] = '\0';
	Main_SetPreInitHook(NULL);
	Main_UnInit();
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "hatari";
	info->library_version = HATARI_VERSION;
	info->need_fullpath = true;
	info->valid_extensions = "st|msa|dim|stx|scp|kfs|ipf|zip|gz|m3u|m3u8|hd|hdf|hdi|vhd|sthd";
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	uint32_t *pixels;
	int width, height, pitch;

	memset(info, 0, sizeof(*info));

	Screen_GetDimension(&pixels, &width, &height, &pitch);
	info->geometry.base_width = width;
	info->geometry.base_height = height;
	info->geometry.max_width = MAX_VDI_WIDTH;
	info->geometry.max_height = MAX_VDI_HEIGHT;
	info->geometry.aspect_ratio = (float)width / (float)height;
	info->timing.fps = 50.0f;

	info->timing.sample_rate = nAudioFrequency;
}

RETRO_API void retro_reset(void)
{
	Reset_Warm();
}

RETRO_API void retro_run(void)
{
	int width, height, pitch;
	uint32_t *pixels;

	if (input_poll_cb)
		input_poll_cb();
	if (RetroOptions_Update())
	{
		Configuration_Apply(true);
		has_cpu_config_changed = true;
	}
	M68000_UnsetSpecial(SPCFLAG_BRK);

	if (has_cpu_config_changed)
	{
		has_cpu_config_changed = false;
		UAE_Set_Quit_Reset(false);
		m68k_go(true);
	}
	else
	{
		quit_program = 0;
		m68k_run();
	}

	Screen_GetDimension(&pixels, &width, &height, &pitch);
	if (video_refresh_cb && pixels && width > 0 && height > 0)
	{
		if (environment_cb &&
		    ((unsigned)width != last_video_width ||
		     (unsigned)height != last_video_height))
		{
			struct retro_game_geometry geometry = {
				(unsigned)width, (unsigned)height,
				MAX_VDI_WIDTH, MAX_VDI_HEIGHT,
				(float)width / (float)height
			};
			environment_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
			last_video_width = (unsigned)width;
			last_video_height = (unsigned)height;
		}
		video_refresh_cb(pixels, (unsigned)width, (unsigned)height, (size_t)pitch);
	}

	/* Restore requests are completed by the CPU loop.  The file is no longer
	 * needed once that loop has returned. */
	if (pending_state_path[0])
	{
		unlink(pending_state_path);
		pending_state_path[0] = '\0';
	}
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port < 2)
		retro_controller_devices[port] = device;
}

RETRO_API size_t retro_serialize_size(void)
{
	void *state;
	size_t size = 0;

	if (snapshot_read(&state, &size))
		free(state);
	return size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
	void *state = NULL;
	size_t state_size;

	if (!data || !snapshot_read(&state, &state_size) || size < state_size)
	{
		free(state);
		return false;
	}
	memcpy(data, state, state_size);
	free(state);
	return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	FILE *file;

	if (pending_state_path[0])
	{
		unlink(pending_state_path);
		pending_state_path[0] = '\0';
	}
	if (!data || !size || !snapshot_path(pending_state_path,
	                                     sizeof(pending_state_path)))
		return false;
	file = fopen(pending_state_path, "wb");
	if (!file || fwrite(data, 1, size, file) != size)
	{
		if (file)
			fclose(file);
		unlink(pending_state_path);
		pending_state_path[0] = '\0';
		return false;
	}
	fclose(file);
	MemorySnapShot_Restore(pending_state_path, false);
	return true;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	/* The native floppy and hard-disk backends open filenames themselves.
	 * Keep the full-path contract explicit until those backends share a VFS
	 * capable file abstraction. */
	if (game && !game->path)
		return false;
	if (RetroHardDisk_LoadGame(game))
		return true;
	if (game && game->path &&
	    (strrchr(game->path, '.') &&
	     (!strcasecmp(strrchr(game->path, '.') + 1, "hd") ||
	      !strcasecmp(strrchr(game->path, '.') + 1, "hdf") ||
	      !strcasecmp(strrchr(game->path, '.') + 1, "hdi") ||
	      !strcasecmp(strrchr(game->path, '.') + 1, "vhd") ||
	      !strcasecmp(strrchr(game->path, '.') + 1, "sthd"))))
		return false;
	return RetroDisk_LoadGame(game);
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

RETRO_API void retro_unload_game(void)
{
	for (int drive = 0; drive < MAX_FLOPPYDRIVES; drive++)
		Floppy_EjectDiskFromDrive(drive);
	RetroDisk_UnloadGame();
	RetroHardDisk_UnloadGame();
}

RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_PAL;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM && STRam && STRamEnd)
		return STRam;
	return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM && STRam)
		return STRamEnd;
	return 0;
}


void Main_RequestQuit(int exitval)
{
}

/**
 * Set exit value and enable quit flag
 */
void Main_SetQuitValue(int exitval)
{
	bQuitProgram = true;
	M68000_SetSpecial(SPCFLAG_BRK);
}


/**
 * Error exit wrapper
 */
void Main_ErrorExit(const char *msg1, const char *msg2, int errval)
{
	if (msg1)
	{
		if (msg2)
			Log_Printf(LOG_ERROR, "%s - %s\n", msg1, msg2);
		else
			Log_Printf(LOG_ERROR, "%s\n", msg1);
	}

	bQuitProgram = true;
	M68000_SetSpecial(SPCFLAG_BRK);
}


bool DlgAlert_Query(const char *text)
{
	Log_Printf(LOG_DEBUG, "DlgAlert_Query: %s\n", text);
	return false;
}

bool DlgAlert_Notice(const char *text)
{
	Log_Printf(LOG_DEBUG, "DlgAlert_Notice: %s\n", text);
	return false;
}

void Dialog_HaltDlg(void)
{
}

int Dialog_MainDlg(bool *bReset, bool *bLoadedSnapshot)
{
	*bReset = false;
	*bLoadedSnapshot = false;
	return 0;
}

char* DlgFloppy_ShortCutSel(const char *path_and_name, char **zip_path)
{
	return NULL;
}
