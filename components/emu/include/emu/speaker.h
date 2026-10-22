#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fills dst with up to `frames` 16-bit stereo frames (L, R), returns how
 * many it had
 */
typedef int (*speaker_render_t)(int16_t *dst, int frames);

/* Stream sound to the speaker at `rate` Hz. Runs its own task on core 0.
 * MaTouch: the MAX98357A amp takes over GPIO19/20, so the native USB port
 * goes away. Tab5: the ES8388 codec.
 */
bool speaker_start(int rate, speaker_render_t render);

/* The volume (0-100, the setup screen's slider; the samples are scaled by
 * its square, so the slider feels even) and mute, both live. Kept in NVS
 * ("speaker", the same for every project on the board): speaker_load() at
 * boot, with the volume as a share of full scale (%) until one is saved;
 * speaker_save() writes them if they changed.
 */
void speaker_load(int default_percent);
int speaker_volume(void);
bool speaker_muted(void);
void speaker_set_volume(int volume);
void speaker_set_muted(bool muted);
void speaker_save(void);

/* Frames the speaker had to make up so far (the machine was late: an
 * underrun, heard as a small dropout)
 */
uint32_t speaker_missing_frames(void);
