#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fills dst with up to `frames` 16-bit stereo frames (L, R), returns how
 * many it had
 */
typedef int (*speaker_render_t)(int16_t *dst, int frames);

/* Stream sound to the speaker at `rate` Hz. Runs its own task on core 0;
 * volume in %. MaTouch: the MAX98357A amp takes over GPIO19/20, so the
 * native USB port goes away. Tab5: the ES8388 codec.
 */
bool speaker_start(int rate, speaker_render_t render, int volume);
