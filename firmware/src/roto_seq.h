/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto: ring sequencer for four independent tracks.
 * Each track is a circular sequence of 1..64 steps. Steps are on or off;
 * each on-step fires a note_on on its track. The four tracks share one
 * transport (BPM, running) but each loops independently on its own length
 * (polymeter). */
#ifndef ROTO_SEQ_H
#define ROTO_SEQ_H
#include <stdint.h>

#define ROTO_TRACKS  4
#define ROTO_STEPS  64    /* max steps per ring */

/* One step: on/off and a note (0 = root) */
typedef struct {
    uint8_t on;
    uint8_t note;   /* semitone offset from track root, 0..127 */
} roto_step_t;

/* One track's sequencer state */
typedef struct {
    roto_step_t step[ROTO_STEPS];
    uint8_t  len;        /* active steps: 1..ROTO_STEPS */
    uint8_t  pos;        /* current playhead position 0..len-1 */
    uint8_t  div;        /* clock divisions per step: 1=1/16, 2=1/8, 4=1/4 */
    uint8_t  _pad;
    uint32_t acc;        /* accumulated clock ticks toward the next step */
} roto_track_t;

/* Global sequencer */
typedef struct {
    roto_track_t trk[ROTO_TRACKS];
    uint8_t  running;
    uint8_t  bpm;        /* 40..240 */
    uint8_t  sel;        /* selected / active track 0..3 */
    uint8_t  _pad;
} roto_seq_t;

extern roto_seq_t roto;

/* Call once at boot */
void roto_seq_init(void);

/* Call from the audio ISR, once per sample block.
 * fires note_on(track, note) for every step that triggers this block. */
void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate);

/* Step editing */
void roto_step_toggle(uint32_t trk, uint32_t step);
void roto_len_set(uint32_t trk, uint32_t len);

/* Transport */
void roto_play(void);
void roto_stop(void);

/* Called by roto_seq_tick when a step fires — implemented in seq.c / audio.c
 * to trigger the voice on that track */
void roto_note_on(uint32_t trk, uint8_t note);

/* UI hooks — implemented in roto_ui.c, called from ui_input.c / ui_draw.c */
int  roto_ui_input(uint32_t pressed, uint32_t notes);
int  roto_ui_draw(void);   /* returns 1 if it drew the full screen (caller skips Felucca drawing) */
void roto_ui_audio_level(uint8_t level);

#endif /* ROTO_SEQ_H */
