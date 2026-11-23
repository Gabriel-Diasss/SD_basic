#ifndef WAV_PLAYER_H
#define WAV_PLAYER_H

#include "ff.h"

FRESULT wav_play(const char *path);   /* bloqueante, retorna ao terminar */
void wav_set_volume(uint16_t vol);    /* 0 a 256 (256 = 100%) */

#endif
