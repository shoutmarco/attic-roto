/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto: render the power-on pattern to a WAV on the host, with the same
 * DSP sources as the firmware (as tests/hostsim.c does) plus roto_seq.c.
 *
 *   build/host/roto_render SEED SECONDS OUT.wav [SOLO]
 *
 * SOLO 1..4 renders that ring only. Prints the pattern, the sounds, the peak
 * level, samples at full scale, and voices left sounding after a stop (must be 0). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/bsp/chip/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/mod.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/slicer.c"
#include "../firmware/src/fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/bsp/chip/usb.c"
#include "../firmware/src/midi_uart.c"
#include "../firmware/src/song_chain.c"
#include "../firmware/src/seq.c"
#include "../firmware/src/roto_seq.c"

/* as hostsim.c host_tracks_init / host_preset (= ui.c set_engine_of + apply_preset_to) */
static void host_init(void)
{
    uint32_t i, k;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < P_E0; i++)
            trk[k].p[i] = TP[i].def;
        for (i = 0; i < NSTEP; i++)
            trk[k].step[i].time = ST_REST;
    }
    song.master_q12 = 2048;                 /* main.c felucca_init */
}

static void host_preset(track_t *t, uint32_t e, uint32_t pi)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const preset_t *p;
    uint32_t i;
    t->eng_req = (uint8_t)e;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = ENGINES[e]->edit[i].def;
    if (ENGINES[e]->npresets) {
        pi %= ENGINES[e]->npresets;
        p = &ENGINES[e]->presets[pi];
        t->preset = (uint8_t)pi;
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = p->e[i];
        t->p[P_ATK] = p->env[0];
        t->p[P_DEC] = p->env[1];
        t->p[P_SUS] = p->env[2];
        t->p[P_REL] = p->env[3];
        t->p[P_ED_FLT] = p->fenv;
        t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
        for (i = 0; i < 4u; i++)
            t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
        fm6_track_loaded(t);
    }
    t->engine = t->eng_req;
    eng_mem_claim(t);
}

/* main.c roto_boot, on the host */
static void host_roto_boot(uint32_t seed)
{
    uint32_t t, i;
    roto_seq_generate(seed);
    for (t = 0; t < ROTO_TRACKS; t++) {
        track_t *tk = &trk[t];
        roto_sound_t *sd = &roto.snd[t];
        const engine_t *e;
        const char *nm;
        host_preset(tk, sd->eng, sd->preset);
        tk->p[P_LEVEL] = sd->level;
        tk->p[P_MUTE] = 0;
        tk->p[P_PAN] = sd->pan;
        tk->p[P_DLY] = sd->dly;
        tk->p[P_REV] = sd->rev;
        tk->p[P_CHOR] = sd->chor;
        e = ENGINES[tk->eng_req % NENGINES];
        nm = e->npresets ? e->presets[tk->preset % e->npresets].name : e->name;
        for (i = 0; i + 1u < sizeof sd->name && nm[i]; i++)
            sd->name[i] = nm[i];
        sd->name[i] = 0;
    }
    song.g[G_BPM] = roto.bpm;
    song.g[G_DTIME] = roto.dtime;
    song.g[G_DFDBK] = roto.dfdbk;
    roto_play();
}

static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t v;
    uint16_t a = 1, ch = 2, ba = 4, bits = 16;
    uint32_t sr = FS, br = FS * 4;
    fwrite("RIFF", 1, 4, f); v = 36 + frames * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4; fwrite(&v, 4, 1, f);
}

static const char *const MODE[3] = {"aeolian", "dorian", "phrygian"};
static const char *const PC[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

int main(int argc, char **argv)
{
    uint32_t seed, secs, solo, frames, f, i, t, clips = 0, peak = 0, left = 0, stop_at;
    FILE *w;
    if (argc < 4) {
        fprintf(stderr, "usage: %s SEED SECONDS OUT.wav [SOLO]\n", argv[0]);
        return 2;
    }
    seed = (uint32_t)strtoul(argv[1], 0, 0);
    secs = (uint32_t)atoi(argv[2]);
    solo = argc > 4 ? (uint32_t)atoi(argv[4]) : 0u;
    host_init();
    roto_seq_init();
    host_roto_boot(seed);
    if (getenv("SND")) {                        /* SND=eng,preset: that sound on the SOLO ring (calibration) */
        uint32_t e = 0, pr = 0;
        sscanf(getenv("SND"), "%u,%u", &e, &pr);
        host_preset(&trk[solo ? solo - 1u : 0u], e, pr);
        trk[solo ? solo - 1u : 0u].p[P_LEVEL] = 112;           /* 0 dB */
        trk[solo ? solo - 1u : 0u].p[P_MUTE] = 0;
    }
    if (solo)
        for (t = 0; t < NTRK; t++)
            if (t + 1u != solo) trk[t].p[P_LEVEL] = 0;

    printf("seed %u: %s %s, %u BPM, delay 1/8D feedback %u\n", seed, PC[roto.root], MODE[roto.mode], roto.bpm, roto.dfdbk);
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        printf("  ring %u %-11s len %2u div %u gate %2u%%:", t + 1u, roto.snd[t].name, k->len, k->div, k->gate);
        for (i = 0; i < k->len; i++)
            if (k->step[i].on) printf(" %u:%s%d", i + 1u, PC[k->step[i].note % 12u], k->step[i].note / 12 - 1);
        printf("\n");
    }

    if (!(w = fopen(argv[3], "wb"))) return 1;
    frames = secs * FS;
    stop_at = frames - 3u * FS;                 /* stop 3 s before the end: every note must be released */
    wav_hdr(w, frames);
    for (f = 0; f < frames; f += CTL) {
        int32_t o[2 * CTL];
        if (f >= stop_at && roto.running) roto_stop();
        mix_block(o, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            int32_t v = o[i], a = v < 0 ? -v : v;
            int16_t s16 = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            if ((uint32_t)a > peak) peak = (uint32_t)a;
            if (a >= 32700) clips++;
            fwrite(&s16, 2, 1, w);
        }
    }
    fclose(w);
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < NVOICE; i++)
            left += trk[t].v[i].active && trk[t].v[i].gate;
    {
        uint32_t held = 0;
        for (t = 0; t < ROTO_TRACKS; t++) held += roto.trk[t].nheld;
        printf("peak %.1f dBFS, %u samples at full scale, notes still held after stop: %u, voices still gated: %u\n",
               peak ? 20.0 * log10((double)peak / 32768.0) : -99.0, clips, held, left);
    }
    return 0;
}
