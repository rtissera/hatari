/*
  Hatari - main_retro.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

extern retro_environment_t environment_cb;
extern retro_video_refresh_t video_refresh_cb;
extern retro_input_state_t input_state_cb;
extern unsigned retro_controller_devices[2];

static inline bool Retro_ControllerConnected(unsigned port)
{
	return port < 2 &&
	       (retro_controller_devices[port] & RETRO_DEVICE_MASK) ==
	       RETRO_DEVICE_JOYPAD;
}

static inline int16_t Retro_InputState(unsigned port, unsigned device,
		unsigned index, unsigned id)
{
	return input_state_cb ? input_state_cb(port, device, index, id) : 0;
}
