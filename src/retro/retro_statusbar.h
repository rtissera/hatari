#ifndef HATARI_RETRO_STATUSBAR_H
#define HATARI_RETRO_STATUSBAR_H

/* Called once per retro_run() to expire the HD LED's "on for a while"
   state (see Statusbar_EnableHDLed() in statusbar.c). */
void RetroStatusbar_Tick(void);

#endif
