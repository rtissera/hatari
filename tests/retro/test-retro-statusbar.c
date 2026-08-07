/*
 * Mock-driven tests for the libretro LED-interface wiring
 * (src/retro/statusbar.c).
 *
 * Same rationale as test-retro-disk.c: never calls retro_init(). The LED
 * functions only touch the adapter's own static state and the mocked
 * RETRO_ENVIRONMENT_GET_LED_INTERFACE callback - no CPU/TOS dependency.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <libretro.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors src/includes/statusbar.h's drive_index_t/drive_led_t - kept in
   sync manually since this test doesn't link against Hatari's headers. */
typedef enum { DRIVE_LED_A, DRIVE_LED_B, DRIVE_LED_HD } drive_index_t;
typedef enum { LED_STATE_OFF, LED_STATE_ON, LED_STATE_ON_BUSY } drive_led_t;

static void (*lr_set_environment)(retro_environment_t cb);
static void (*enable_hd_led)(drive_led_t state);
static void (*set_floppy_led)(drive_index_t drive, drive_led_t state);
static void (*statusbar_tick)(void);

#define MAX_LED_CALLS 64
static struct { int led; int state; } led_calls[MAX_LED_CALLS];
static int led_call_count;
static bool led_interface_requested;

static void mock_set_led_state(int led, int state)
{
	if (led_call_count < MAX_LED_CALLS)
	{
		led_calls[led_call_count].led = led;
		led_calls[led_call_count].state = state;
	}
	led_call_count++;
}

static struct retro_led_interface mock_led_iface = { mock_set_led_state };

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd)
	{
	 case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	 case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	 case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	 case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	 case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	 case RETRO_ENVIRONMENT_SET_VARIABLES:
	 case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
	 case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
	 case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		return true;
	 case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
		led_interface_requested = true;
		*(struct retro_led_interface *)data = mock_led_iface;
		return true;
	 case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable *)data)->value = NULL;
		return true;
	 default:
		return false;
	}
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
	printf("%-60s%s\n", label, condition ? "OK" : "ERROR");
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
	enable_hd_led = must_dlsym(dlh, "Statusbar_EnableHDLed");
	set_floppy_led = must_dlsym(dlh, "Statusbar_SetFloppyLed");
	statusbar_tick = must_dlsym(dlh, "RetroStatusbar_Tick");

	lr_set_environment(env_cb);
	check(!led_interface_requested,
	      "GET_LED_INTERFACE not queried yet (queried lazily, on first LED use)");

	/* --- Floppy LEDs: direct on/off, no auto-expire --- */
	led_call_count = 0;
	set_floppy_led(DRIVE_LED_A, LED_STATE_ON_BUSY);
	check(led_interface_requested, "GET_LED_INTERFACE queried lazily on first LED call");
	check(led_call_count == 1 && led_calls[0].led == DRIVE_LED_A && led_calls[0].state != 0,
	      "Floppy A LED on: forwarded as led=0 state!=0");

	set_floppy_led(DRIVE_LED_B, LED_STATE_ON);
	check(led_call_count == 2 && led_calls[1].led == DRIVE_LED_B && led_calls[1].state != 0,
	      "Floppy B LED on: forwarded as led=1 state!=0");

	set_floppy_led(DRIVE_LED_A, LED_STATE_OFF);
	check(led_call_count == 3 && led_calls[2].led == DRIVE_LED_A && led_calls[2].state == 0,
	      "Floppy A LED off: forwarded as led=0 state=0");

	/* Floppy LEDs must never auto-expire: ticking shouldn't touch them. */
	{
		int before = led_call_count;
		int i;
		for (i = 0; i < 100; ++i)
			statusbar_tick();
		check(led_call_count == before,
		      "Floppy LEDs unaffected by RetroStatusbar_Tick() (no auto-expire)");
	}

	/* --- HD LED: turns on immediately, then auto-expires after a bounded
	   number of ticks (one retro_run() worth of ticks each) --- */
	led_call_count = 0;
	enable_hd_led(LED_STATE_ON);
	check(led_call_count == 1 && led_calls[0].led == DRIVE_LED_HD && led_calls[0].state != 0,
	      "HD LED on: forwarded as led=2 state!=0 immediately");

	{
		int ticks = 0;
		int max_ticks = 1000; /* generous bound; real value is an internal detail */
		while (led_call_count < 2 && ticks < max_ticks)
		{
			statusbar_tick();
			++ticks;
		}
		check(led_call_count == 2, "HD LED eventually auto-expires (a second LED call arrives)");
		check(led_calls[1].led == DRIVE_LED_HD && led_calls[1].state == 0,
		      "HD LED auto-expire call: led=2 state=0");
		check(ticks < max_ticks, "HD LED auto-expire happened within a bounded number of ticks");
	}

	/* Re-enabling resets the expiry countdown rather than leaving it
	   wherever the previous cycle's countdown was. */
	led_call_count = 0;
	enable_hd_led(LED_STATE_ON);
	statusbar_tick();
	check(led_call_count == 1, "Re-enabling HD LED: still on after a single tick (countdown reset)");

	dlclose(dlh);
	puts("All statusbar/LED tests finished successfully.");
	return 0;
}
