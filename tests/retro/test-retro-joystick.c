/*
 * Mock-driven tests for the libretro joystick-port mapping
 * (src/retro/options.c's RetroOptions_JoystickPortFor()).
 *
 * Same rationale as test-retro-disk.c: never calls retro_init(). The
 * mapping logic only touches the adapter's own static state and the
 * mocked RETRO_ENVIRONMENT_GET_VARIABLE callback - no CPU/TOS dependency.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <libretro.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors src/includes/configuration.h's JOYID_* order - kept in sync
   manually since this test doesn't link against Hatari's headers. */
enum { JOYID_JOYSTICK0, JOYID_JOYSTICK1, JOYID_JOYPADA, JOYID_JOYPADB,
       JOYID_PARPORT1, JOYID_PARPORT2 };

static void (*lr_set_environment)(retro_environment_t cb);
static void (*lr_options_apply)(void);
static int (*lr_port_for)(int joyid);

/* NULL means "not overridden" - env_cb reports that as a NULL value,
   same convention as retro_option()'s callers use for "use the option's
   own declared default" elsewhere in this adapter. */
static const char *port_targets[4] = { NULL, NULL, NULL, NULL };

static bool env_cb(unsigned cmd, void *data)
{
	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE)
	{
		struct retro_variable *v = (struct retro_variable *)data;
		if (!strcmp(v->key, "hatari_joystick_port1")) v->value = port_targets[0];
		else if (!strcmp(v->key, "hatari_joystick_port2")) v->value = port_targets[1];
		else if (!strcmp(v->key, "hatari_joystick_port3")) v->value = port_targets[2];
		else if (!strcmp(v->key, "hatari_joystick_port4")) v->value = port_targets[3];
		else v->value = NULL;
		return true;
	}
	return true;
}

static void *must_dlsym(void *dlh, const char *name)
{
	void *fn = dlsym(dlh, name);
	if (!fn)
	{
		fprintf(stderr, "Failed to get symbol '%s': %s\n", name, dlerror());
		exit(EXIT_FAILURE);
	}
	return fn;
}

static void check(bool condition, const char *label)
{
	printf("%-70s%s\n", label, condition ? "OK" : "ERROR");
	if (!condition)
		exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	void *dlh;

	if (argc != 2)
	{
		printf("Usage: %s <path-to-libretro-hatari.so>\n", argv[0]);
		return 0;
	}

	dlh = dlopen(argv[1], RTLD_NOW);
	if (!dlh)
	{
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return EXIT_FAILURE;
	}
	lr_set_environment = must_dlsym(dlh, "retro_set_environment");
	lr_options_apply = must_dlsym(dlh, "RetroOptions_Apply");
	lr_port_for = must_dlsym(dlh, "RetroOptions_JoystickPortFor");

	lr_set_environment(env_cb);

	/* --- Defaults: preserves the adapter's historical id^=1 swap
	   (RetroPad 1 -> ST joystick 1, RetroPad 2 -> ST joystick 0), with
	   ports 3/4 and the STE pads/parallel ports unmapped. This is the
	   exact case a prior version of this fix got wrong: NULL (option
	   not overridden) was treated as "none" for every port instead of
	   falling back to each port's own declared default. --- */
	lr_options_apply();
	check(lr_port_for(JOYID_JOYSTICK0) == 1, "Default: ST joystick 0 <- RetroPad 2");
	check(lr_port_for(JOYID_JOYSTICK1) == 0, "Default: ST joystick 1 <- RetroPad 1");
	check(lr_port_for(JOYID_JOYPADA) == -1, "Default: STE pad A unmapped");
	check(lr_port_for(JOYID_JOYPADB) == -1, "Default: STE pad B unmapped");
	check(lr_port_for(JOYID_PARPORT1) == -1, "Default: parallel port 1 unmapped");
	check(lr_port_for(JOYID_PARPORT2) == -1, "Default: parallel port 2 unmapped");

	/* --- Override: retarget RetroPad 1 to "none" and RetroPad 3 to an
	   STE pad; RetroPad 2's default must be untouched by this. --- */
	port_targets[0] = "none";
	port_targets[2] = "joypada";
	lr_options_apply();
	check(lr_port_for(JOYID_JOYSTICK1) == -1, "Override: RetroPad 1 retargeted to none");
	check(lr_port_for(JOYID_JOYSTICK0) == 1, "Override: RetroPad 2's default is untouched");
	check(lr_port_for(JOYID_JOYPADA) == 2, "Override: STE pad A <- RetroPad 3");

	/* --- Two RetroPad ports assigned to the same Atari target: no
	   crash, and the mapping table only ever reflects one winner. --- */
	port_targets[0] = "joystick0";
	port_targets[1] = "joystick0";
	port_targets[2] = "none";
	lr_options_apply();
	check(lr_port_for(JOYID_JOYSTICK0) == 1,
	      "Duplicate target: later port in iteration order wins, no crash");

	dlclose(dlh);
	puts("All joystick-port mapping tests finished successfully.");
	return 0;
}
