/*
  Hatari - libretro core options

  SPDX-License-Identifier: GPL-2.0-or-later

  This is deliberately kept in the libretro frontend layer.  Hatari's
  configuration remains frontend-neutral and receives ordinary configuration
  values through Main's pre-initialization hook.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <libretro.h>

#include "configuration.h"
#include "retro_disk.h"
#include "retro_options.h"

static retro_environment_t environment_cb;

static struct retro_variable variables[] = {
	{ "hatari_machine", "Machine; st|mega st|ste|mega ste|tt|falcon" },
	{ "hatari_memory", "ST-RAM; 512|1024|2048|4096|8192" },
	{ "hatari_monitor", "Monitor; rgb|mono|vga|tv" },
	{ "hatari_cpu", "CPU; 68000|68010|68020|68030|68040" },
	{ "hatari_sound_rate", "Sound rate; 22050|44100|48000" },
	{ "hatari_fast_floppy", "Fast floppy; disabled|enabled" },
	{ "hatari_drive_b", "Drive B; disabled|enabled" },
	{ "hatari_disk_active_drive",
	  "Disk Control target drive; a|b" },
	{ "hatari_write_protect", "Floppy write protection; off|on|auto" },
	{ "hatari_falcon_dsp",
	  "Falcon DSP (only applies to the Falcon machine type); emulated|none|dummy" },
	{ NULL, NULL }
};

static const char *retro_option(const char *key)
{
	struct retro_variable variable = { key, NULL };

	if (!environment_cb || !environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE,
	                                       &variable))
		return NULL;
	return variable.value;
}

static int option_index(const char *value, const char *const *names,
	                       int count, int fallback)
{
	int i;

	if (!value)
		return fallback;
	for (i = 0; i < count; ++i)
		if (!strcasecmp(value, names[i]))
			return i;
	return fallback;
}

static int option_number(const char *value, const int *values, int count,
		int fallback)
{
	char *end;
	long number;
	int i;

	if (!value)
		return fallback;
	number = strtol(value, &end, 10);
	if (*value == '\0' || *end != '\0')
		return fallback;
	for (i = 0; i < count; ++i)
		if (number == values[i])
			return (int)number;
	return fallback;
}

void RetroOptions_SetEnvironment(retro_environment_t cb)
{
	environment_cb = cb;
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

void RetroOptions_Apply(void)
{
	static const char *const machines[] = {
		"st", "mega st", "ste", "mega ste", "tt", "falcon"
	};
	static const char *const monitors[] = { "rgb", "mono", "vga", "tv" };
	static const char *const cpus[] = {
		"68000", "68010", "68020", "68030", "68040"
	};
	const char *value;
	static const int memory_sizes[] = { 512, 1024, 2048, 4096, 8192 };
	static const int sound_rates[] = { 22050, 44100, 48000 };

	value = retro_option("hatari_machine");
	ConfigureParams.System.nMachineType = option_index(value, machines,
			(int)(sizeof(machines) / sizeof(machines[0])), MACHINE_ST);

	/* The DSP is Falcon-only hardware; force it off on every other machine
	   type regardless of the option, matching real hardware and avoiding
	   a stale "emulated" setting silently carrying over from a previous
	   Falcon session into a non-Falcon one. */
	if (ConfigureParams.System.nMachineType == MACHINE_FALCON)
	{
		static const char *const dsp_types[] = { "emulated", "none", "dummy" };
		static const int dsp_values[] = {
#if ENABLE_DSP_EMU
			DSP_TYPE_EMU,
#else
			DSP_TYPE_NONE,
#endif
			DSP_TYPE_NONE, DSP_TYPE_DUMMY
		};
		value = retro_option("hatari_falcon_dsp");
		ConfigureParams.System.nDSPType =
			dsp_values[option_index(value, dsp_types,
				(int)(sizeof(dsp_types) / sizeof(dsp_types[0])), 0)];
	}
	else
		ConfigureParams.System.nDSPType = DSP_TYPE_NONE;

	value = retro_option("hatari_memory");
	ConfigureParams.Memory.STRamSize_KB = option_number(value, memory_sizes,
			(int)(sizeof(memory_sizes) / sizeof(memory_sizes[0])),
			ConfigureParams.Memory.STRamSize_KB);

	value = retro_option("hatari_monitor");
	ConfigureParams.Screen.nMonitorType = option_index(value, monitors,
			(int)(sizeof(monitors) / sizeof(monitors[0])), MONITOR_TYPE_RGB);

	value = retro_option("hatari_cpu");
	ConfigureParams.System.nCpuLevel = option_index(value, cpus,
			(int)(sizeof(cpus) / sizeof(cpus[0])), 0);

	value = retro_option("hatari_sound_rate");
	ConfigureParams.Sound.nPlaybackFreq = option_number(value, sound_rates,
			(int)(sizeof(sound_rates) / sizeof(sound_rates[0])),
			ConfigureParams.Sound.nPlaybackFreq);

	value = retro_option("hatari_fast_floppy");
	ConfigureParams.DiskImage.FastFloppy = value && !strcasecmp(value, "enabled");

	value = retro_option("hatari_drive_b");
	ConfigureParams.DiskImage.EnableDriveB = !value || strcasecmp(value, "disabled");
	RetroDisk_SetDriveBEnabled(ConfigureParams.DiskImage.EnableDriveB);

	value = retro_option("hatari_disk_active_drive");
	RetroDisk_SetActiveDrive(ConfigureParams.DiskImage.EnableDriveB &&
			value && !strcasecmp(value, "b") ? 1 : 0);

	value = retro_option("hatari_write_protect");
	if (value && !strcasecmp(value, "on"))
		ConfigureParams.DiskImage.nWriteProtection = WRITEPROT_ON;
	else if (value && !strcasecmp(value, "auto"))
		ConfigureParams.DiskImage.nWriteProtection = WRITEPROT_AUTO;
	else
		ConfigureParams.DiskImage.nWriteProtection = WRITEPROT_OFF;
}

bool RetroOptions_Update(void)
{
	bool updated = false;

	if (!environment_cb ||
	    !environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) ||
	    !updated)
		return false;
	RetroOptions_Apply();
	return true;
}
