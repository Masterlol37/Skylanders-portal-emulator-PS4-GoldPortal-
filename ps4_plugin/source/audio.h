// Portal speaker -> DualShock 4 speaker.
//
// Trap Team sends the portal speaker's sound as 64 byte interrupt OUT packets of
// signed 16 bit mono PCM at 8 kHz (recorded from a real Trap Team portal). We
// buffer it, upsample to 48 kHz and play it on the controller speaker.
#pragma once
#include <stdint.h>

// Bytes of free space in the speaker buffer. The game's audio packets are only
// accepted when they fit, which paces the game at real time like the real portal.
int  audio_space(void);
// Queues PCM data (starts the audio output on first use). Returns bytes taken.
int  audio_push(const uint8_t *data, int len);
