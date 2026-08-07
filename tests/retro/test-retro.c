/*
 * Test that we can successfully load the libretro core and run it
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <libretro.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*lr_set_environment)(retro_environment_t cb);
static void (*lr_set_video_refresh)(retro_video_refresh_t cb);
static void (*lr_set_input_poll)(retro_input_poll_t cb);
static void (*lr_set_input_state)(retro_input_state_t cb);
static unsigned int (*lr_api_version)(void);
static void (*lr_init)(void);
static void (*lr_deinit)(void);
static void (*lr_run)(void);
static bool (*lr_load_game)(const struct retro_game_info *game);
static void *(*lr_get_memory_data)(unsigned id);
static size_t (*lr_get_memory_size)(unsigned id);
static void (*lr_get_system_av_info)(struct retro_system_av_info *info);
static size_t (*lr_serialize_size)(void);

static bool screen_refreshed;
static const struct retro_subsystem_info *captured_subsystems;


static void *test_dlsym(void *dlh, const char *symname)
{
	void *fun;

	fun = dlsym(dlh, symname);
	if (!fun)
	{
		fprintf(stderr, "Failed to get function symbol '%s':\n%s\n",
		        symname, dlerror());
		exit(EXIT_FAILURE);
	}

	return fun;
}


static void init_funcs(void *dlh)
{
	lr_set_environment = test_dlsym(dlh, "retro_set_environment");
	lr_set_video_refresh= test_dlsym(dlh, "retro_set_video_refresh");
	lr_set_input_poll = test_dlsym(dlh, "retro_set_input_poll");
	lr_set_input_state = test_dlsym(dlh, "retro_set_input_state");
	lr_api_version = test_dlsym(dlh, "retro_api_version");
	lr_init = test_dlsym(dlh, "retro_init");
	lr_deinit = test_dlsym(dlh, "retro_deinit");
	lr_run = test_dlsym(dlh, "retro_run");
	lr_load_game = test_dlsym(dlh, "retro_load_game");
	lr_get_memory_data = test_dlsym(dlh, "retro_get_memory_data");
	lr_get_memory_size = test_dlsym(dlh, "retro_get_memory_size");
	lr_get_system_av_info = test_dlsym(dlh, "retro_get_system_av_info");
	lr_serialize_size = test_dlsym(dlh, "retro_serialize_size");
}


static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	 case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	 case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	 case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	 case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	 case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	 case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	 case RETRO_ENVIRONMENT_SET_VARIABLES:
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
	 case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
	 case RETRO_ENVIRONMENT_SET_GEOMETRY:
		return true;
	 case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
		captured_subsystems = (const struct retro_subsystem_info *)data;
		return true;
	 case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
	 case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
	 case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
	 case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		return false;
	 case RETRO_ENVIRONMENT_GET_VARIABLE:
	 {
		struct retro_variable *variable = (struct retro_variable *)data;
		/* hatari_midi_capture=enabled here specifically exercises
		   retro_init()'s pre-init hook (RetroOptions_Apply(), called
		   before Main_InitSubsystems()/CycInt exist) taking the
		   MIDI-enabled path with MIDI pre-configured on *before* the
		   very first retro_init() ever runs - the exact scenario that
		   crashed before RetroOptions_Update() (not Apply()) became the
		   only place allowed to call Midi_UnInit()/Midi_Init(). Every
		   other option falls back to its default (NULL). */
		variable->value = !strcmp(variable->key, "hatari_midi_capture") ?
			"enabled" : NULL;
		return true;
	 }
	 default:
		fprintf(stderr, "Unexpected env setting 0x%x\n", cmd);
		return false;
	}
}

static void refresh_cb(const void *data, unsigned width, unsigned height,
                       size_t pitch)
{
	assert(data && width && height && pitch);
	screen_refreshed = true;
}

static int16_t input_state_cb(unsigned int port, unsigned intdevice,
                              unsigned int index, unsigned int id)
{
	return 0;
}

int main(int argc, char *argv[])
{
	void *dlh;

	if (argc != 2 || argv[1][0] == '-')
	{
		printf("Usage: %s <path-to-libretro-hatari.so>\n", argv[0]);
		return 0;
	}

	printf("Testing dlopen:\t\t\t\t");
	dlh = dlopen(argv[1], RTLD_NOW);
	if (!dlh)
	{
		puts("ERROR");
		fprintf(stderr, "Failed to open '%s':\n%s\n", argv[1],
		        dlerror());
		return EXIT_FAILURE;
	}
	puts("OK");

	init_funcs(dlh);

	printf("Testing retro_api_version:\t\t");
	if (lr_api_version() != RETRO_API_VERSION)
	{
		puts("ERROR");
		fprintf(stderr, "Wrong RETRO_API_VERSION!\n");
		return EXIT_FAILURE;
	}
	puts("OK");

	printf("Setting retro_set_environment:\t\t");
	lr_set_environment(env_cb);
	puts("OK");

	printf("Subsystem metadata:			");
	if (!captured_subsystems || captured_subsystems->id != 1 ||
	    !captured_subsystems->roms || captured_subsystems->num_roms != 2)
	{
		puts("ERROR");
		return EXIT_FAILURE;
	}
	puts("OK");

	printf("Setting retro_set_video_refresh:\t");
	lr_set_video_refresh(refresh_cb);
	puts("OK");

	printf("Setting retro_set_input_state:\t\t");
	lr_set_input_state(input_state_cb);
	puts("OK");

	printf("Memory API before init:			");
	if (lr_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) != NULL ||
	    lr_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) != 0)
	{
		puts("ERROR");
		return EXIT_FAILURE;
	}
	puts("OK");

	printf("Rejecting data-only content:		");
	{
		struct retro_game_info data_only = { NULL, "data", 4, NULL };
		if (lr_load_game(&data_only))
		{
			puts("ERROR");
			return EXIT_FAILURE;
		}
	}
	puts("OK");

	/*
	 * retro_init() itself is safe to call without a TOS image: the
	 * libretro frontend's own Main_ErrorExit() (main_retro.c) just logs
	 * and sets a flag instead of exit()ing like the SDL frontend's does,
	 * and Dialog_MainDlg() is stubbed to a no-op here, so the
	 * "bring up a GUI to pick another TOS" path Main_InitSubsystems()
	 * takes on load failure never actually opens anything. Confirmed by
	 * running it repeatedly under gdb with no crash.
	 *
	 * retro_run() is a different story: it eventually crashes (SIGSEGV
	 * inside m68k_run_1_ce(), PC == 0) because the reset vector was never
	 * loaded from a real TOS ROM - confirmed via gdb backtrace. The first
	 * call can survive by luck (whether the CPU reaches address 0 within
	 * that frame's cycle budget), but the second reliably doesn't. So
	 * retro_run() still isn't called here; only the parts of the
	 * lifecycle that don't need the CPU to actually execute are.
	 */
	printf("Initializing core (no TOS image):\t");
	lr_init();
	puts("OK");

	printf("retro_get_system_av_info before retro_run:\t");
	{
		struct retro_system_av_info av_info;
		lr_get_system_av_info(&av_info);
		/* Every frontend calls this once right after retro_init(), before
		   any retro_run() - i.e. before TOS has had a chance to program a
		   video mode. Screen dimensions must already have a sane fallback
		   at that point, not the 0x0 (and resulting NaN aspect ratio) that
		   Screen_GetDimension() reports before the first mode change. */
		if (av_info.geometry.base_width == 0 ||
		    av_info.geometry.base_height == 0 ||
		    !(av_info.geometry.aspect_ratio > 0.0f))
		{
			puts("ERROR");
			return EXIT_FAILURE;
		}
	}
	puts("OK");

	printf("retro_serialize_size without a TOS image (no crash):\t");
	{
		/* Without a loaded TOS, ensure_cpu_started() must not attempt a
		   real CPU dispatch (that hits the same PC==0 reset-vector crash
		   as retro_run() without TOS) just because a frontend probed
		   serialize support before ever calling retro_run(). */
		if (lr_serialize_size() != 0)
		{
			puts("ERROR");
			return EXIT_FAILURE;
		}
	}
	puts("OK");

	printf("Testing retro_deinit (retro_run was never called):\t");
	lr_deinit();
	puts("OK");

	dlclose(dlh);

	puts("All tests finished successfully.");

	return 0;
}
