/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto: ring sequencer (2026-11-24).
 * Four independent circular sequencers. Each track loops on its own step
 * length (polymeter). The clock is a 1/16-note pulse derived from BPM;
 * each track's DIV controls how many 1/16 pulses equal one of its steps.
 *
 * roto_seq_tick() runs in the audio ISR. Everything else runs in the UI task.
 * The only shared state is roto_t (byte-sized fields = atomic writes on
 * pi32v2). The playhead (pos) and acc are audio-task-owned; the UI reads
 * pos for display only. */
#include "roto_seq.h"
#include <string.h>

roto_seq_t roto;

/* ---------------------------------------------------------------- init */
void roto_seq_init(void)
{
    uint32_t t, s;
    memset(&roto, 0, sizeof roto);
    roto.bpm     = 120;
    roto.running = 0;
    roto.sel     = 0;
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        k->len = 16;
        k->div = 1;   /* 1/16 steps */
        k->pos = 0;
        k->acc = 0;
        for (s = 0; s < ROTO_STEPS; s++) {
            k->step[s].on   = 0;
            k->step[s].note = 0;
        }
    }
    /* seed a default pattern so the instrument is never silent at boot:
     * track 0 kick on the quarters, track 1 snare on 2 and 4,
     * track 2 hats on the off-beats, track 3 a simple bass line */
    for (s = 0; s < 16; s++) {
        roto.trk[0].step[s].on = (s % 4 == 0);
        roto.trk[1].step[s].on = (s % 8 == 4);
        roto.trk[2].step[s].on = (s % 2 == 1);
        roto.trk[3].step[s].on = (s % 3 == 0);
    }
    roto.trk[3].step[0].note  = 0;
    roto.trk[3].step[3].note  = 7;
    roto.trk[3].step[6].note  = 5;
    roto.trk[3].step[9].note  = 3;
    roto.trk[3].step[12].note = 7;
}

/* --------------------------------------------------------------- tick */
/* Samples per 1/16-note at this BPM */
static uint32_t sixteenth_samples(uint32_t bpm, uint32_t sr)
{
    /* (sr * 60) / (bpm * 4): one beat = 4 sixteenths */
    return (sr * 15u) / bpm;
}

void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate)
{
    uint32_t t, f;
    if (!roto.running) return;

    uint32_t sixteenth = sixteenth_samples((uint32_t)roto.bpm, sample_rate);

    for (f = 0; f < block_frames; f++) {
        for (t = 0; t < ROTO_TRACKS; t++) {
            roto_track_t *k = &roto.trk[t];
            uint32_t step_len = sixteenth * (uint32_t)k->div;
            k->acc++;
            if (k->acc >= step_len) {
                k->acc = 0;
                /* advance playhead */
                k->pos = (uint8_t)((k->pos + 1u) % (uint32_t)k->len);
                /* fire if the step is on */
                if (k->step[k->pos].on)
                    roto_note_on(t, k->step[k->pos].note);
            }
        }
    }
}

/* ------------------------------------------------------------- editing */
void roto_step_toggle(uint32_t trk, uint32_t step)
{
    if (trk >= ROTO_TRACKS || step >= ROTO_STEPS) return;
    roto.trk[trk].step[step].on ^= 1u;
}

void roto_len_set(uint32_t trk, uint32_t len)
{
    if (trk >= ROTO_TRACKS) return;
    if (len < 1)         len = 1;
    if (len > ROTO_STEPS) len = ROTO_STEPS;
    roto.trk[trk].len = (uint8_t)len;
    /* clamp playhead so it stays in range */
    if (roto.trk[trk].pos >= len)
        roto.trk[trk].pos = 0;
}

/* ---------------------------------------------------------- transport */
void roto_play(void)
{
    uint32_t t;
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto.trk[t].pos = 0;
        roto.trk[t].acc = 0;
    }
    roto.running = 1;
}

void roto_stop(void)
{
    roto.running = 0;
}
