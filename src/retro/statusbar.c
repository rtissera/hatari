/*
  Hatari - statusbar.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Code to draw statusbar area, floppy leds etc.
*/
const char Statusbar_fileid[] = "Hatari statusbar.c";

#include <assert.h>
#include <libretro.h>
#include "main.h"
#include "main_retro.h"
#include "configuration.h"
#include "retro_statusbar.h"
#include "screenSnapShot.h"
#include "statusbar.h"
#include "tos.h"
#include "video.h"
#include "sound.h"
#include "avi_record.h"
#include "vdi.h"
#include "fdc.h"
#include "stMemory.h"
#include "blitter.h"
#include "str.h"
#include "lilo.h"

/* drive_index_t (DRIVE_LED_A/B/HD) already matches the LED index scheme
   used below one-for-one, so it's passed straight through as the
   frontend-facing LED number - RetroArch shows these as "LED 0"/"LED
   1"/"LED 2" with no built-in labels (custom skins can name them). */
static struct retro_led_interface led_interface;
static bool led_interface_queried;
static bool led_interface_ok;

/* SDL frontend (src/sdl/statusbar.c) shows the HD LED for 500ms after each
   Statusbar_EnableHDLed() call; matched here via a frame countdown ticked
   from retro_run(), since libretro has no wall-clock equivalent handy in
   this adapter. Assumes a ~50Hz emulated frame rate (PAL ST); close enough
   for a purely cosmetic activity blip. */
#define HD_LED_EXPIRE_FRAMES 25
static bool hd_led_active;
static int hd_led_expire_frames;

static void ensure_led_interface(void)
{
	if (led_interface_queried)
		return;
	led_interface_queried = true;
	led_interface_ok = environment_cb &&
		environment_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &led_interface) &&
		led_interface.set_led_state;
}


/**
 * Return statusbar height for given width and height
 */
int Statusbar_GetHeightForSize(int width, int height)
{
	return 0;
}

/**
 * Set screen height used for statusbar height calculation.
 *
 * Return height of statusbar that should be added to the screen
 * height when screen is (re-)created, or zero if statusbar will
 * not be shown
 */
int Statusbar_SetHeight(int width, int height)
{
	return Statusbar_GetHeightForSize(width, height);
}

/**
 * Return height of statusbar set with Statusbar_SetHeight()
 */
int Statusbar_GetHeight(void)
{
	return 0;
}


/**
 * Enable HD drive led, it will be automatically disabled after a while.
 */
void Statusbar_EnableHDLed(drive_led_t state)
{
	ensure_led_interface();
	if (!led_interface_ok)
		return;
	led_interface.set_led_state(DRIVE_LED_HD, state != LED_STATE_OFF);
	hd_led_active = (state != LED_STATE_OFF);
	hd_led_expire_frames = HD_LED_EXPIRE_FRAMES;
}

/**
 * Set given floppy drive led state, anything enabling led with this
 * needs also to take care of disabling it.
 */
void Statusbar_SetFloppyLed(drive_index_t drive, drive_led_t state)
{
	assert(drive == DRIVE_LED_A || drive == DRIVE_LED_B);
	ensure_led_interface();
	if (!led_interface_ok)
		return;
	led_interface.set_led_state((int)drive, state != LED_STATE_OFF);
}

/**
 * Expire the HD LED's transient "on" state; called once per retro_run().
 */
void RetroStatusbar_Tick(void)
{
	if (!hd_led_active)
		return;
	if (--hd_led_expire_frames <= 0)
	{
		hd_led_active = false;
		if (led_interface_ok)
			led_interface.set_led_state(DRIVE_LED_HD, 0);
	}
}


/**
 * Set TOS etc information and initial help message
 */
void Statusbar_InitialSetup(void)
{
}


/**
 * Queue new statusbar message 'msg' to be shown for 'msecs' milliseconds
 */
void Statusbar_AddMessage(const char *msg, uint32_t msecs)
{
}

/**
 * Retrieve/update default statusbar information
 */
void Statusbar_UpdateInfo(void)
{
}
