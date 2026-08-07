
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <libretro.h>

#include "main.h"
#include "main_retro.h"
#include "configuration.h"
#include "hatari-glue.h"
#include "dialog.h"
#include "floppy.h"
#include "memorySnapShot.h"
#include "m68000.h"
#include "retro_options.h"
#include "reset.h"
#include "retro_disk.h"
#include "retro_harddisk.h"
#include "retro_statusbar.h"
#include "vfs.h"
#include "screen.h"
#include "stMemory.h"
#include "sound.h"
#include "tos.h"
#include "vdi.h"
#include "version.h"

static bool has_cpu_config_changed = true;
static bool cpu_has_run;
static unsigned last_video_width;
static unsigned last_video_height;
static char pending_state_path[PATH_MAX];
static char system_directory[PATH_MAX];
static char save_directory[PATH_MAX];
static char tos_path[PATH_MAX];

static void retro_set_memory_maps(void)
{
	static struct retro_memory_descriptor descriptors[2];
	struct retro_memory_map map;
	unsigned count = 0;

	if (!environment_cb)
		return;
	memset(descriptors, 0, sizeof(descriptors));
	if (STRam && STRamEnd)
	{
		descriptors[count].ptr = STRam;
		descriptors[count].len = STRamEnd;
		descriptors[count].flags = RETRO_MEMDESC_SYSTEM_RAM |
		                            RETRO_MEMDESC_BIGENDIAN;
		++count;
	}
	if (TTmemory && TTmem_size)
	{
		descriptors[count].ptr = TTmemory;
		descriptors[count].start = 0x01000000;
		descriptors[count].len = TTmem_size;
		descriptors[count].flags = RETRO_MEMDESC_SYSTEM_RAM |
		                            RETRO_MEMDESC_BIGENDIAN;
		++count;
	}
	map.descriptors = descriptors;
	map.num_descriptors = count;
	environment_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}

/* Hatari's native snapshot code operates on files.  Keep that implementation
 * as the single source of truth and bridge it to libretro's memory API here. */
static bool snapshot_path(char *path, size_t path_size)
{
	const char *directory = save_directory[0] ? save_directory : "/tmp";
	size_t length;
	int fd;

	length = strlen(directory);
	if (length + sizeof("/hatari-libretro-state-XXXXXX") > path_size)
		return false;
	if (snprintf(path, path_size, "%s%s%s", directory,
	             length && directory[length - 1] == '/' ? "" : "/",
	             "hatari-libretro-state-XXXXXX") >= (int)path_size)
		return false;

	fd = mkstemp(path);
	if (fd < 0 && save_directory[0])
	{
		if (snprintf(path, path_size, "/tmp/hatari-libretro-state-XXXXXX") >=
		    (int)path_size)
			return false;
		fd = mkstemp(path);
	}
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
	static unsigned serialization_quirks = RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
	static struct retro_controller_description controller_types[] = {
		{ "RetroPad", RETRO_DEVICE_JOYPAD }
	};
	static struct retro_controller_info controller_info[] = {
		{ controller_types, 1 },
		{ controller_types, 1 },
		{ NULL, 0 }
	};
	static struct retro_input_descriptor input_descriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick 0 Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick 0 Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick 0 Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick 0 Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Joystick 0 Fire 1" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Joystick 0 Fire 2" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Joystick 0 Fire 3" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Joystick 0 Analog X" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Joystick 0 Analog Y" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick 1 Left" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick 1 Right" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick 1 Up" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick 1 Down" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Joystick 1 Fire 1" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Joystick 1 Fire 2" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Joystick 1 Fire 3" },
		{ 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Joystick 1 Analog X" },
		{ 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Joystick 1 Analog Y" },
		{ 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X, "Mouse X" },
		{ 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y, "Mouse Y" },
		{ 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT, "Mouse Left Button" },
		{ 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE, "Mouse Middle Button" },
		{ 0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT, "Mouse Right Button" },
		{ 0 }
	};
	static const char subsystem_extensions[] =
		"st|msa|dim|stx|scp|kfs|ipf|zip|gz";
	static struct retro_subsystem_rom_info subsystem_roms[] = {
		{ "Floppy disk 1", subsystem_extensions, true, false, true, NULL, 0 },
		{ "Floppy disk 2 (drive A swap list)", subsystem_extensions, true, false, false, NULL, 0 }
	};
	static struct retro_subsystem_info subsystems[] = {
		{ "Hatari floppy disk swap list (drive A)", "floppies", subsystem_roms, 2, 1 },
		{ NULL, NULL, NULL, 0, 0 }
	};

	environment_cb = cb;

	/* Hatari only supports 32-bit bits per pixel */
	cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixelformat);

	/* Hatari can start without game disks */
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
	cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)controller_info);
	cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)input_descriptors);
	RetroOptions_SetEnvironment(cb);
	cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, subsystems);
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
	const char *save_path = NULL;
	last_video_width = 0;
	last_video_height = 0;
	save_directory[0] = '\0';

	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,
	                                     &directory) && directory && *directory)
	{
		if (snprintf(system_directory, sizeof(system_directory), "%s", directory)
		    >= (int)sizeof(system_directory) ||
		    snprintf(tos_path, sizeof(tos_path), "%s/%s", system_directory,
		             "tos.img") >= (int)sizeof(tos_path))
			tos_path[0] = '\0';
	}
	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,
	                                     &save_path) && save_path && *save_path)
		snprintf(save_directory, sizeof(save_directory), "%s", save_path);
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
	retro_set_memory_maps();
	has_cpu_config_changed = true;
	cpu_has_run = false;
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
	if (width <= 0 || height <= 0)
	{
		/* screen_width/height are only ever set by Screen_SetVideoSize(),
		   which only runs once the emulated CPU programs a video mode -
		   i.e. after TOS has booted, inside retro_run(). Every libretro
		   frontend calls retro_get_system_av_info() once right after
		   retro_init(), before any retro_run(), so this 0x0 case is the
		   normal first call, not a fallback for something exceptional.
		   Report Screen_Init()'s own nMaxWidth/nMaxHeight instead of an
		   invalid 0x0/NaN geometry; it runs during Main_InitSubsystems(),
		   before the TOS check, so it's already populated here. */
		width = ConfigureParams.Screen.nMaxWidth;
		height = ConfigureParams.Screen.nMaxHeight;
	}
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

/* Dispatches one frame of 68k emulation, taking the "config just changed,
   do a full (re)init" path when needed. Shared by retro_run() and by the
   serialize functions below, which need this to have happened at least
   once before a snapshot is meaningful (see ensure_cpu_started()). */
static void cpu_dispatch(void)
{
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
	cpu_has_run = true;
}

/* Hatari's CPU/FPU core selects its emulation function tables lazily,
   inside cpu_dispatch()'s first call - before that,
   M68000_MemorySnapShot_Capture() crashes on a null FPU jump-table entry
   (fpp_from_exten_fmovem). Some libretro frontends probe serialize
   support immediately after retro_load_game(), before any retro_run(), so
   run the same dispatch a real first retro_run() would instead of
   crashing or permanently reporting "unsupported" for the session. */
static void ensure_cpu_started(void)
{
	/* Without a successfully loaded TOS image, cpu_dispatch() would hit
	   the same PC==0 reset-vector crash retro_run() does (see main_retro.c
	   history) - there's nothing meaningful to snapshot in that state
	   anyway, so just report "not yet serializable" instead. */
	if (!cpu_has_run && bTosImageLoaded)
		cpu_dispatch();
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
	cpu_dispatch();
	RetroStatusbar_Tick();

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

	ensure_cpu_started();
	if (!cpu_has_run)
		return 0;
	if (snapshot_read(&state, &size))
		free(state);
	return size;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
	void *state = NULL;
	size_t state_size;

	ensure_cpu_started();
	if (!cpu_has_run || !data || !snapshot_read(&state, &state_size) ||
	    size < state_size)
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
	ensure_cpu_started();
	if (!cpu_has_run || !data || !size || !snapshot_path(pending_state_path,
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
	/* Don't call RetroDisk_UnloadGame() here: RetroDisk_LoadGame() already
	   does equivalent cleanup itself (ejects both drives, clears the swap
	   list), and unlike this function, it doesn't reset the pending
	   initial-image preference a just-prior disk_set_initial_image() call
	   may have stashed - real RetroArch calls that before retro_load_game()
	   runs, and calling RetroDisk_UnloadGame() here was wiping it out
	   before RetroDisk_LoadGame() ever got to consult it. */
	RetroHardDisk_UnloadGame();
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
	if (game_type != 1 || !info || !num_info)
		return false;
	/* See the comment in retro_load_game(): RetroDisk_LoadGameSpecial()
	   already does the equivalent cleanup itself without touching the
	   pending initial-image preference. */
	RetroHardDisk_UnloadGame();
	return RetroDisk_LoadGameSpecial(info, num_info);
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
