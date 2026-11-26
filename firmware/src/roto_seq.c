/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto: ring sequencer.
 * Four circular sequencers, each looping on its own length (polymeter), all on
 * one 1/16 clock derived from the BPM; a track's DIV is how many 1/16s one of its
 * steps lasts. Each on-step plays its note (plus the track's chord notes) for
 * GATE percent of a step, then releases them.
 *
 * roto_seq_tick() runs in the audio interrupt; everything else in the UI task.
 * Shared fields are bytes (single writes). Notes are started and stopped only
 * by the audio side: roto_stop() just clears `running` and the next tick
 * releases whatever is sounding. */
#include "roto_seq.h"

roto_seq_t roto;

/* ------------------------------------------------------------- random */
static uint32_t rs_state = 1u;

static uint32_t rs_next(void)
{
    uint32_t x = rs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rs_state = x;
}
static uint32_t rnd(uint32_t n) { return n ? rs_next() % n : 0u; }
static int chance(uint32_t pct) { return rnd(100u) < pct; }

/* ------------------------------------------------------------- init */
void roto_seq_init(void)
{
    uint8_t *p = (uint8_t *)&roto;
    uint32_t n = (uint32_t)sizeof roto, t;
    while (n--) *p++ = 0;               /* (no string.h in this toolchain) */
    roto.bpm = 118;
    roto.swing = 50;
    roto.evolve = 50;
    roto.throw_trk = 0xFF;
    roto.ev_rs = 1u;
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto.trk[t].part = (uint8_t)(t < ROTO_PARTS ? t : t == 4u ? 0u : 3u);
        roto.trk[t].len = 16;
        roto.trk[t].div = 1;
        roto.trk[t].gate = 50;
        roto.trk[t].prob = 100;
    }
    roto.arp_speed = 1;                 /* ARP: off; 1/16, two octaves, short */
    roto.arp_range = 2;
    roto.arp_gate = 40;
    roto.arp_part = 3;
    roto.arp_cur = 0xFF;
    roto.arp_note = 0xFF;
}

/* ------------------------------------------------------------- generator */
static const uint8_t MODE_SC[3][7] = {
    {0, 2, 3, 5, 7, 8, 10},     /* aeolian */
    {0, 2, 3, 5, 7, 9, 10},     /* dorian */
    {0, 1, 3, 5, 7, 8, 10},     /* phrygian */
};

static void put(roto_track_t *k, uint32_t s, uint32_t note, uint32_t vel)
{
    k->step[s].on = 1;
    k->step[s].note = (uint8_t)(note > 127u ? 127u : note);
    k->step[s].vel = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
}

/* the sounds each role may get: engine, preset (engines.c ENGINES[] numbering), level.
 * Warm ones only (measured in their roles with tests/roto_render.c: under ~3 % of the energy
 * above 4 kHz; the bells, METAL, GLITCH, ARP 8BIT, RAVE, RESO, WOW BASS and the bright drum
 * kit are out). Levels (P_LEVEL: 112 = 0 dB, 0.5 dB a step) from the same renders: drums
 * loudest, bass about 3 dB under, chords and blips further back. */
static const uint8_t SND_DRUMS[][3] = {{10, 0, 113}, {10, 1, 113}, {10, 3, 113}};                 /* DRUM KIT, DEEP KIT, PERC KIT */
static const uint8_t SND_BASS[][3]  = {{0, 7, 111}};                                              /* SUB BASS (clean and round, up front) */
static const uint8_t SND_STAB[][3]  = {{12, 0, 100}, {0, 3, 101}};                               /* TINE EP, PWM STR (soft, held) */
static const uint8_t SND_BLIP[][3]  = {{0, 5, 100}, {7, 3, 108}};                               /* SINE KEY, SOFT FLUTE (the soft ones, set back) */
#define NSND(a) ((uint32_t)(sizeof a / sizeof a[0]))
#define ENG_DRUM 10u                                            /* (engines.c: DRUM) */

static void sound(roto_sound_t *s, const uint8_t es[3], uint32_t dly, uint32_t rev, uint32_t chor, int32_t pan)
{
    s->eng = es[0];
    s->preset = es[1];
    s->level = es[2];
    s->dly = (uint8_t)dly;
    s->rev = (uint8_t)rev;
    s->chor = (uint8_t)chor;
    s->pan = (int8_t)pan;
    s->name[0] = 0;
}

/* the pattern as set: EVOLVE wanders from it and comes back to it */
static void home_save(roto_track_t *k)
{
    uint32_t s;
    for (s = 0; s < ROTO_STEPS; s++) k->home[s] = k->step[s];
}

/* the grooves: written by hand, minimal and swung (after Baby Ford / Soul Capsule).
 * 16 steps of 1/16 each, one bar (the chords: 16 of 1/8, two bars). A power-on picks
 * one, a key, and a few small touches; EVOLVE only makes small touches too.
 *   drums  K kick  O open hat  o soft open hat
 *   shuf   h closed hat (a ghost)  s shaker (a ghost)  c clap  r rim
 *   perc   L low conga  M conga  m muted conga  b bongo  k clave  w woodblock
 *   bass   1..7 the mode's degrees, 8 the root an octave up
 *   chord  a the home chord  b another (both minor 7ths in the mode) */
typedef struct {
    const char *drums, *shuf, *perc, *bass, *chord, *chord_alt;
    uint8_t swing, bgate;
} roto_groove_t;

static const roto_groove_t GROOVES[] = {
    {"K.O.K.O.K.O.K.O.", ".h.hch.h.h.hch.h", "...m..L....m.L..", "..1...15..1..8.7", "...a............", "......a.........", 57, 55},
    {"K.o.K.O.K.o.K.O.", ".hrh.h.h.hrh.hsh", "..k.....k..k....", "...1..1...5..1..", "..........a.....", ".....a..........", 56, 45},
    {"K.O.K.O.K.O.K.Oo", ".h.h.h.h.h.h.h.h", "......L.....M...", "..1....1..1..5.8", "a.......b.......", "..a.......b.....", 55, 70},
    {"K.O.K.O.K.O.K.O.", ".s.sc..s.s.sc.ss", "m..L.m.....L.m..", "..1..1....1..7..", ".....a..........", ".............a..", 58, 45},
    {"K...K.O.K...K.O.", ".h.r.h.h.h.r.h.h", "................", "..1..1.1..1...5.", "...a.......a....", "......a.........", 57, 50},
    {"K.O.K.O.K.O.K.O.", "...hc..h...hc..h", "..b...b...b..b..", ".118.1.5.118.1.5", "........a.......", "a...............", 56, 35},
    {"K.O.K.OoK.O.K.O.", ".h.h.h.h.h.h.hrh", "....L.......L...", "...1...1..7...1.", "..a.............", "..........a.....", 58, 60},
    {"K.O.K.O.K.O.K.O.", ".s.s.s.s.s.s.s.s", "w.....w.....w...", "..1..5...1.5..8.", "....a...........", "............a...", 55, 55},
};
#define NGROOVE ((uint32_t)(sizeof GROOVES / sizeof GROOVES[0]))

/* a drum character's note and loudness; 0: a rest */
static uint32_t groove_drum(char c, uint32_t *vel, uint32_t *ghost)
{
    *ghost = 0;
    switch (c) {
    case 'K': *vel = 110u + rnd(8u); return 36;
    case 'O': *vel = 52u + rnd(14u); return 46;
    case 'o': *vel = 38u + rnd(10u); return 46;
    case 'h': *vel = 28u + rnd(16u); *ghost = 55u + rnd(36u); return 42;
    case 's': *vel = 30u + rnd(14u); *ghost = 60u + rnd(31u); return 70;
    case 'c': *vel = 50u + rnd(12u); return 39;
    case 'r': *vel = 44u + rnd(14u); return 37;
    case 'L': *vel = 62u + rnd(12u); return 64;
    case 'M': *vel = 54u + rnd(12u); return 63;
    case 'm': *vel = 48u + rnd(12u); return 62;
    case 'b': *vel = 48u + rnd(14u); return 60;
    case 'k': *vel = 42u + rnd(14u); return 75;
    case 'w': *vel = 42u + rnd(14u); return 76;
    }
    return 0;
}

static void groove_ring(roto_track_t *k, const char *p)
{
    uint32_t s, note, vel, gh;
    for (s = 0; s < 16u && p[s]; s++)
        if ((note = groove_drum(p[s], &vel, &gh)) != 0u) {
            put(k, s, note, vel);
            k->step[s].chance = (uint8_t)gh;
        }
}

static uint32_t ring_hits(const roto_track_t *k);

void roto_seq_generate(uint32_t seed)
{
    static const uint8_t ROOTS[6] = {2, 4, 9, 7, 0, 5};          /* D E A G C F */
    static const uint8_t PENTA[5] = {0, 3, 5, 7, 10};
    /* chord roots where a minor 7th chord stays in the mode (aeolian i iv v, dorian i ii v, phrygian i iv bvii) */
    static const uint8_t STAB_ROOT[3][3] = {{0, 5, 7}, {0, 2, 7}, {0, 5, 10}};
    const roto_groove_t *g;
    const uint8_t *sc;
    roto_track_t *k;
    uint32_t i, s, n, pc, base, t, first = 16;

    roto_seq_init();
    i = seed + 0x9E3779B9u;                                      /* (spread any seed: murmur3's finaliser) */
    i ^= i >> 16; i *= 0x85EBCA6Bu; i ^= i >> 13; i *= 0xC2B2AE35u; i ^= i >> 16;
    rs_state = i ? i : 1u;
    roto.seed = seed;

    g = &GROOVES[rnd(NGROOVE)];
    pc = ROOTS[rnd(6u)];
    roto.root = (uint8_t)pc;
    roto.mode = (uint8_t)rnd(3u);
    sc = MODE_SC[roto.mode];
    roto.bpm = 122;                                              /* (Soul Capsule's tempo) */
    roto.dtime = 10;                                             /* params.c N_DTIME: 1/8D */
    roto.dfdbk = (uint8_t)(55u + rnd(12u));                     /* a dubby tail */
    roto.swing = (uint8_t)(g->swing + rnd(2u));
    roto.evolve = 40;

    /* 1: kick and open hats; now and then one open hat closes */
    k = &roto.trk[0];
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = 42;
    groove_ring(k, g->drums);
    if (chance(30)) { s = 2u + 4u * rnd(4u); if (k->step[s].on && k->step[s].note == 46u) k->step[s].note = 42; }
    sound(&roto.snd[0], SND_DRUMS[0], 0, 18, 0, 0);             /* DRUM KIT (it has the congas and claves) */

    /* 2: the bass hook; maybe one note an octave over, maybe one a 1/16 later (never onto the kick) */
    k = &roto.trk[1];
    k->len = 16; k->div = 1; k->gate = g->bgate;
    base = 36u + pc;
    if (pc >= 7u) base -= 12u;
    k->fill_note = (uint8_t)base;
    for (s = 0; s < 16u; s++) {
        char c = g->bass[s];
        uint32_t note;
        if (c < '1' || c > '8') continue;
        note = c == '8' ? base + 12u : base + sc[c - '1'];
        put(k, s, note, 94u + rnd(16u));
        if (first == 16u) first = s;
    }
    if (first < 16u) k->step[first].vel = 118;                  /* (the first leans on) */
    if (chance(40)) {
        n = 0;
        for (s = first + 1u; s < 16u; s++) n += k->step[s].on;
        if (n) {
            uint32_t r = rnd(n);
            for (s = first + 1u; s < 16u; s++)
                if (k->step[s].on && !r--) {
                    k->step[s].note = (uint8_t)(k->step[s].note >= base + 12u ? k->step[s].note - 12u : k->step[s].note + 12u);
                    break;
                }
        }
    }
    if (chance(30)) {
        for (s = 15u; s > first; s--)
            if (k->step[s].on && s + 1u < 16u && !k->step[s + 1u].on && ((s + 1u) & 3u)) {
                k->step[s + 1u] = k->step[s];
                k->step[s].on = 0;
                break;
            }
    }
    sound(&roto.snd[1], SND_BASS[0], 0, 0, 0, 0);               /* SUB BASS: round and clean */

    /* 3: the dub chord, a two-bar loop; held, low, into the delay */
    k = &roto.trk[2];
    k->len = 16; k->div = 2; k->gate = 90;
    k->chord[0] = 3; k->chord[1] = 7; k->chord[2] = 10;
    k->fill_note = (uint8_t)(48u + pc);
    {
        const char *p = chance(50) ? g->chord_alt : g->chord;
        uint32_t home = chance(30) ? STAB_ROOT[roto.mode][1] : 0u;
        for (s = 0; s < 16u && p[s]; s++)
            if (p[s] == 'a' || p[s] == 'b')
                put(k, s, 48u + pc + (p[s] == 'a' ? home : STAB_ROOT[roto.mode][2]), 64u + rnd(16u));
    }
    sound(&roto.snd[2], SND_STAB[rnd(NSND(SND_STAB))], 82, 52, 20, -22);

    /* 4: a blip, on some power-ons only: one soft note in two bars */
    k = &roto.trk[3];
    k->len = 16; k->div = 2; k->gate = 25;
    k->fill_note = (uint8_t)(60u + pc);
    if (chance(35)) {
        s = 1u + 2u * rnd(8u);
        put(k, s, 60u + pc + PENTA[rnd(5u)] + (chance(25) ? 12u : 0u), 70u + rnd(20u));
    }
    sound(&roto.snd[3], SND_BLIP[rnd(NSND(SND_BLIP))], 76, 50, 0, 26);

    /* 5: the shuffle — hats or shaker on the in-between 1/16s, claps or rims; sometimes the clap is a rim */
    k = &roto.trk[4];
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = 42;
    groove_ring(k, g->shuf);
    if (chance(40))
        for (s = 0; s < 16u; s++) if (k->step[s].on && k->step[s].note == 39u) k->step[s].note = 37;

    /* 6: the percussion — congas, bongos, claves (on the drum kit); maybe one fewer, or one where there were none */
    k = &roto.trk[5];
    k->part = 0;
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = 63;
    groove_ring(k, g->perc);
    n = ring_hits(k);
    if (n > 2u && chance(40)) {
        uint32_t r = rnd(n);
        for (s = 0; s < 16u; s++) if (k->step[s].on && !r--) { k->step[s].on = 0; break; }
    } else if (!n && chance(50)) {
        put(k, chance(50) ? 6u : 14u, 64, 60u + rnd(12u));
    }

    for (t = 0; t < ROTO_TRACKS; t++) home_save(&roto.trk[t]);
    roto.ev_rs = rs_next() | 1u;
    roto.sel = 0;                                                /* (stopped: the caller sets the sounds, then PLAY) */
}

void roto_seq_blank(void)
{
    static const uint8_t FILLN[ROTO_TRACKS] = {36, 36, 48, 72, 42, 79};      /* kick, C2, C3, C5, closed hat, G5 */
    static const uint8_t GATE[ROTO_TRACKS] = {25, 55, 30, 25, 25, 25};
    uint32_t t;
    roto_seq_init();
    roto.bpm = 120;
    roto.swing = 50;
    roto.evolve = 0;
    roto.dtime = 10;                                             /* 1/8D */
    roto.dfdbk = 55;
    roto.root = 0;
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        k->len = 16; k->div = 1; k->gate = GATE[t]; k->prob = 100;
        k->fill_note = FILLN[t];
    }
    roto.trk[2].chord[0] = 3; roto.trk[2].chord[1] = 7; roto.trk[2].chord[2] = 10;
    sound(&roto.snd[0], SND_DRUMS[0], 0, 14, 0, 0);              /* DRUM KIT */
    sound(&roto.snd[1], SND_BASS[0], 0, 0, 0, 0);                /* SUB BASS */
    sound(&roto.snd[2], SND_STAB[0], 64, 40, 20, -22);           /* TINE EP */
    sound(&roto.snd[3], SND_BLIP[0], 72, 46, 0, 26);             /* SINE KEY */
    for (t = 0; t < ROTO_TRACKS; t++) home_save(&roto.trk[t]);
}

/* ------------------------------------------------------------- tick */
static void release(uint32_t t, roto_track_t *k)
{
    uint32_t i;
    for (i = 0; i < k->nheld; i++) roto_note_off(k->part, k->held[i]);
    (void)t;
    k->nheld = 0;
    k->gate_left = 0;
}

static uint32_t audio_rs = 0x2545F491u;     /* PROB's and the ghosts' dice (audio side only) */

static uint32_t audio_dice(uint32_t n)
{
    uint32_t x = audio_rs;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    audio_rs = x;
    return x % n;
}

static void fire(uint32_t t, roto_track_t *k, uint32_t step_len)
{
    const roto_step_t *st = &k->step[k->pos];
    uint32_t i, g;
    if (k->nheld) release(t, k);
    if (!st->on) return;
    if (k->prob < 100u && audio_dice(100u) >= k->prob) return;
    if (st->chance && audio_dice(100u) >= st->chance) return;
    k->held[k->nheld++] = st->note;
    roto_note_on(k->part, st->note, st->vel ? st->vel : 100u);
    for (i = 0; i < ROTO_CHORD; i++) {
        int32_t n = (int32_t)st->note + k->chord[i];
        if (!k->chord[i] || n < 0 || n > 127) continue;
        k->held[k->nheld++] = (uint8_t)n;
        roto_note_on(k->part, (uint8_t)n, st->vel ? st->vel : 100u);
    }
    g = (step_len * (k->gate ? k->gate : 50u)) / 100u;
    k->gate_left = g ? g : 1u;
}

/* -------- EVOLVE: every 4 bars, maybe one small change; sometimes back home */
static uint32_t ev_dice(uint32_t n)
{
    uint32_t x = roto.ev_rs;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    roto.ev_rs = x ? x : 1u;
    return x % n;
}

static void steps_rotate(roto_track_t *k, int32_t dir)
{
    roto_step_t keep;
    uint32_t s, len = k->len;
    if (len < 2u) return;
    if (dir > 0) {
        keep = k->step[len - 1u];
        for (s = len - 1u; s > 0u; s--) k->step[s] = k->step[s - 1u];
        k->step[0] = keep;
    } else {
        keep = k->step[0];
        for (s = 0; s + 1u < len; s++) k->step[s] = k->step[s + 1u];
        k->step[len - 1u] = keep;
    }
    k->rot = (uint8_t)((k->rot + (dir > 0 ? 1u : len - 1u)) % len);
    for (s = 0; s < k->fill_n; s++)                  /* (FILL's hits moved with it) */
        k->fill_at[s] = (uint8_t)((k->fill_at[s] + (dir > 0 ? 1u : len - 1u)) % len);
}

/* a random hit (ghosts == 1: only ghosts, 0: no ghosts) of k, or len if none */
static uint32_t ev_hit(const roto_track_t *k, int ghosts)
{
    uint32_t s, n = 0, r;
    for (s = 0; s < k->len; s++) n += k->step[s].on && (!k->step[s].chance) == !ghosts;
    if (!n) return k->len;
    r = ev_dice(n);
    for (s = 0; s < k->len; s++)
        if (k->step[s].on && (!k->step[s].chance) == !ghosts && !r--) return s;
    return k->len;
}

static uint32_t ring_hits(const roto_track_t *k);

static void evolve_once(void)
{
    static const uint8_t PENTA[5] = {0, 3, 5, 7, 10};
    uint32_t r = ev_dice(100u), s, t;
    roto_track_t *k;

    if (r < 30u) {                                           /* drift back: a ring that has wandered goes home */
        for (t = 0; t < ROTO_TRACKS; t++) {
            uint32_t u = (t + ev_dice(ROTO_TRACKS)) % ROTO_TRACKS, d = 0;
            k = &roto.trk[u];
            for (s = 0; s < k->len; s++)
                d |= k->step[s].on != k->home[s].on || k->step[s].note != k->home[s].note;
            if (d) {
                for (s = 0; s < ROTO_STEPS; s++) k->step[s] = k->home[s];
                k->rot = 0;
                k->fill_n = 0;
                return;
            }
        }
        return;
    }
    if (r < 45u) {                                           /* the bass: a note (not the first) jumps an octave */
        uint32_t first;
        k = &roto.trk[1];
        for (first = 0; first < k->len && !k->step[first].on; first++) ;
        if ((s = ev_hit(k, 0)) < k->len && s != first)
            k->step[s].note = (uint8_t)(k->step[s].note >= k->fill_note + 12u ? k->step[s].note - 12u : k->step[s].note + 12u);
        return;
    }
    if (r < 70u) {                                           /* the shuffle: a ghost hat in or out (on the in-between 1/16s) */
        k = &roto.trk[4];
        if (ev_dice(2u) && (s = ev_hit(k, 1)) < k->len) {
            k->step[s].on = 0;
            k->step[s].chance = 0;
        } else {
            s = 1u + 2u * ev_dice(k->len / 2u);
            if (s < k->len && !k->step[s].on) {
                k->step[s].on = 1;
                k->step[s].note = 42;
                k->step[s].vel = (uint8_t)(28u + ev_dice(14u));
                k->step[s].chance = (uint8_t)(55u + ev_dice(30u));
            }
        }
        return;
    }
    if (r < 85u) {                                           /* the percussion: a hit drops out (home brings it back) */
        k = &roto.trk[5];
        if (ring_hits(k) > 1u && (s = ev_hit(k, 0)) < k->len) k->step[s].on = 0;
        return;
    }
    if (r < 90u) {                                           /* a blip: another note of the pentatonic */
        k = &roto.trk[3];
        if ((s = ev_hit(k, 0)) < k->len)
            k->step[s].note = (uint8_t)(k->fill_note + PENTA[ev_dice(5u)] + (ev_dice(4u) ? 0u : 12u));
        return;
    }
    k = &roto.trk[0];                                        /* the drums: an off-beat hat opens or closes */
    for (s = 2; s < k->len; s += 4u)
        if (k->step[s].on && (k->step[s].note == 46u || k->step[s].note == 42u) && ev_dice(2u)) {
            k->step[s].note = k->step[s].note == 46u ? 42u : 46u;
            return;
        }
}

/* -------- dub throws: now and then a stab or blip ring is thrown into the delay for a bar */
static void throw_bar(void)
{
    if (roto.throw_trk < ROTO_PARTS) {
        return;                                              /* (one at a time) */
    }
    if (ev_dice(100u) < (uint32_t)roto.evolve / 6u) {        /* evolve 50: a throw in about 1 bar of 12 */
        uint32_t t = 2u + ev_dice(2u);
        uint32_t d = (uint32_t)roto.snd[t].dly + 50u, fb = (uint32_t)roto.dfdbk + 16u;
        roto.throw_trk = (uint8_t)t;
        roto.throw_left = 16;
        roto_fx_send(t, (uint8_t)(d > 127u ? 127u : d));
        roto_fx_feedback((uint8_t)(fb > 82u ? 82u : fb));
    }
}

static void throw_tick(void)
{
    if (roto.throw_trk >= ROTO_PARTS || !roto.throw_left) return;
    if (--roto.throw_left == 0u) {
        roto_fx_send(roto.throw_trk, roto.snd[roto.throw_trk].dly);
        roto_fx_feedback(roto.dfdbk);
        roto.throw_trk = 0xFF;
    }
}

/* -------- ARP: on the 1/32 grid (so it swings with the rings) */
static void rarp_off(void)
{
    if (roto.arp_note != 0xFFu) roto_note_off(roto.arp_npart, roto.arp_note);
    roto.arp_note = 0xFF;
    roto.arp_gate_left = 0;
}

static void rarp_32(uint32_t six)
{
    static const uint8_t SP[4] = {1, 2, 4, 8};          /* 1/32s per note */
    uint32_t sp = SP[roto.arp_speed & 3u], n = roto.arp_len, i;
    if ((roto.arp_n32++ % sp) != 0u) return;
    rarp_off();
    if (!roto.arp_chord || !n) { roto.arp_cur = 0xFF; return; }
    i = roto.arp_cur;
    if (i >= n) {                                        /* the first note */
        i = roto.arp_mode == ROTO_ARP_DOWN ? n - 1u : 0u;
        roto.arp_dir = 0; roto.arp_k = 0;
    } else switch (roto.arp_mode) {
    case ROTO_ARP_DOWN:   i = i ? i - 1u : n - 1u; break;
    case ROTO_ARP_UPDOWN:
        if (n < 2u) { i = 0; break; }
        if (!roto.arp_dir) { if (i + 1u >= n) { roto.arp_dir = 1; i = n - 2u; } else i++; }
        else               { if (!i) { roto.arp_dir = 0; i = 1; } else i--; }
        break;
    case ROTO_ARP_RANDOM: { uint32_t j = audio_dice(n); i = (j == i && n > 1u) ? (j + 1u) % n : j; } break;
    case ROTO_ARP_PEDAL:
        if (i || n < 2u) i = 0;
        else { roto.arp_k = (uint8_t)(roto.arp_k % (n - 1u) + 1u); i = roto.arp_k; }
        break;
    default:              i = (i + 1u) % n; break;
    }
    roto.arp_cur = (uint8_t)i;
    roto.arp_note = roto.arp_seq[i];
    roto.arp_npart = roto.arp_part;
    roto_note_on(roto.arp_part, roto.arp_note, (roto.arp_n32 - 1u) % 32u < sp ? 84u : 68u);   /* (behind the groove; the bar's first a little louder) */
    {
        uint32_t g = (six * sp / 2u * (roto.arp_gate ? roto.arp_gate : 40u)) / 100u;
        roto.arp_gate_left = g ? g : 1u;
    }
}

/* every ring on one 1/16 clock: a ring moves on every `div` 1/16s. Swing makes the
 * on-beat 1/16s longer and the off-beat ones shorter by the same amount. */
static void sixteenth(uint32_t six)
{
    uint32_t t;
    throw_tick();
    if ((roto.n16 & 15u) == 0u && roto.n16) {               /* a new bar */
        if (((roto.n16 >> 4) & 3u) == 0u && ev_dice(100u) < roto.evolve) evolve_once();
        throw_bar();
    }
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        uint32_t div = k->div ? k->div : 1u, len = k->len ? k->len : 1u;
        if (k->pos == ROTO_POS_START) {
            k->pos = 0;
            k->sub = 0;
        } else {
            if (++k->sub < div) continue;
            k->sub = 0;
            k->pos = (uint8_t)((k->pos + 1u) % len);
        }
        fire(t, k, six * div);
    }
    rarp_32(six);
}

void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate)
{
    uint32_t t, f, six, sw, len16;

    if (!roto.running) {
        for (t = 0; t < ROTO_TRACKS; t++)
            if (roto.trk[t].nheld) release(t, &roto.trk[t]);
        if (roto.throw_trk < ROTO_PARTS) { roto.throw_left = 1; throw_tick(); }
        rarp_off();
        return;
    }
    six = (sample_rate * 15u) / (roto.bpm ? roto.bpm : 120u);
    sw = roto.swing < 50u ? 50u : roto.swing > 75u ? 75u : roto.swing;
    len16 = (roto.n16 & 1u) ? (2u * six * (100u - sw)) / 100u : (2u * six * sw) / 100u;
    if (roto.trk[0].pos == ROTO_POS_START) {                 /* PLAY: everything from the top, now */
        roto.n16 = 0;
        roto.acc16 = 0;
        roto.arp_n32 = 0;
        roto.arp_cur = 0xFF;
        sixteenth(six);
        len16 = (2u * six * sw) / 100u;
    }

    for (f = 0; f < block_frames; f++) {
        for (t = 0; t < ROTO_TRACKS; t++) {
            roto_track_t *k = &roto.trk[t];
            if (k->gate_left && --k->gate_left == 0u) release(t, k);
        }
        if (roto.arp_gate_left && --roto.arp_gate_left == 0u) rarp_off();
        if (++roto.acc16 == len16 / 2u) rarp_32(six);        /* (the off 1/32) */
        if (roto.acc16 >= len16) {
            roto.acc16 = 0;
            roto.n16++;
            len16 = (roto.n16 & 1u) ? (2u * six * (100u - sw)) / 100u : (2u * six * sw) / 100u;
            sixteenth(six);
        }
    }
}

/* ------------------------------------------------------------- editing */
void roto_step_toggle(uint32_t trk, uint32_t step)
{
    roto_step_t *st;
    if (trk >= ROTO_TRACKS || step >= ROTO_STEPS) return;
    st = &roto.trk[trk].step[step];
    if (!st->on && !st->vel) {                      /* a new step: the track's first note, full velocity */
        uint32_t s;
        st->note = roto.trk[trk].fill_note ? roto.trk[trk].fill_note : 60u;
        for (s = 0; s < ROTO_STEPS; s++)
            if (roto.trk[trk].step[s].on) { st->note = roto.trk[trk].step[s].note; break; }
        st->vel = 100;
    }
    st->on ^= 1u;
    {                                               /* (a step set by hand belongs to the pattern) */
        roto_track_t *k = &roto.trk[trk];
        uint32_t i, j = 0;
        for (i = 0; i < k->fill_n; i++) if (k->fill_at[i] != step) k->fill_at[j++] = k->fill_at[i];
        k->fill_n = (uint8_t)j;
    }
    st->chance = 0;
    home_save(&roto.trk[trk]);
}

void roto_len_set(uint32_t trk, uint32_t len)
{
    if (trk >= ROTO_TRACKS) return;
    if (len < 1) len = 1;
    if (len > ROTO_STEPS) len = ROTO_STEPS;
    roto.trk[trk].len = (uint8_t)len;
    roto.trk[trk].rot = (uint8_t)(roto.trk[trk].rot % len);
    if (roto.trk[trk].pos != ROTO_POS_START && roto.trk[trk].pos >= len)
        roto.trk[trk].pos = 0;
}

/* FILL: one hit more goes in the middle of the biggest gap (the note of the hit
 * before it; a drum ring adds closed hats); one hit fewer first takes away what
 * FILL added, newest first (so turning back undoes it), then the most crowded
 * hit (the smallest gaps either side), so the rest stays even. */
static uint32_t ring_hits(const roto_track_t *k)
{
    uint32_t s, n = 0;
    for (s = 0; s < k->len; s++) n += k->step[s].on;
    return n;
}

/* steps from hit s to the next hit, going round (len if s is the only one) */
static uint32_t gap_after(const roto_track_t *k, uint32_t s)
{
    uint32_t d;
    for (d = 1; d < k->len; d++)
        if (k->step[(s + d) % k->len].on) return d;
    return k->len;
}

static uint32_t gap_before(const roto_track_t *k, uint32_t s)
{
    uint32_t d;
    for (d = 1; d < k->len; d++)
        if (k->step[(s + k->len - d) % k->len].on) return d;
    return k->len;
}

static void fill_once(uint32_t trk, int32_t dir)
{
    roto_track_t *k;
    uint32_t s, n, best = 0, bestv = 0;
    if (trk >= ROTO_TRACKS || !dir) return;
    k = &roto.trk[trk];
    n = ring_hits(k);
    if (dir > 0) {
        roto_step_t *st;
        if (n >= k->len) return;
        if (!n) {
            best = 0;
            bestv = 0;
        } else {
            for (s = 0; s < k->len; s++) {           /* the biggest gap (the first, on a tie) */
                uint32_t g;
                if (!k->step[s].on) continue;
                g = gap_after(k, s);
                if (g > bestv) { bestv = g; best = s; }
            }
        }
        st = &k->step[n ? (best + bestv / 2u) % k->len : 0u];
        if (!n || roto.snd[k->part].eng == 10u)      /* (engines.c: 10 = DRUM) */
            st->note = k->fill_note ? k->fill_note : 60u;
        else
            st->note = k->step[best].note;
        st->vel = n && roto.snd[k->part].eng != 10u ? k->step[best].vel : 64u;
        st->on = 1;
        if (k->fill_n >= sizeof k->fill_at) {        /* (remember the newest 8) */
            uint32_t i;
            for (i = 1; i < sizeof k->fill_at; i++) k->fill_at[i - 1] = k->fill_at[i];
            k->fill_n--;
        }
        k->fill_at[k->fill_n++] = (uint8_t)(st - k->step);
    } else {
        if (!n) return;
        while (k->fill_n) {                          /* what FILL added goes first, newest first */
            s = k->fill_at[--k->fill_n];
            if (s < k->len && k->step[s].on) { k->step[s].on = 0; return; }
        }
        bestv = 0xFFFFFFFFu;
        for (s = 0; s < k->len; s++) {               /* the most crowded hit (the last, on a tie) */
            uint32_t c;
            if (!k->step[s].on) continue;
            c = gap_before(k, s) + gap_after(k, s);
            if (c <= bestv) { bestv = c; best = s; }
        }
        k->step[best].on = 0;
    }
}

void roto_fill(uint32_t trk, int32_t dir)
{
    if (trk >= ROTO_TRACKS) return;
    fill_once(trk, dir);
    home_save(&roto.trk[trk]);
}

/* ROTATE: every step of the ring moves one step later (dir > 0) or earlier */
void roto_rotate(uint32_t trk, int32_t dir)
{
    if (trk >= ROTO_TRACKS || !dir) return;
    steps_rotate(&roto.trk[trk], dir);
    home_save(&roto.trk[trk]);
}

/* EUCLID: exactly `hits` hits as evenly as they go (step s is a hit when
 * (s * hits) mod len < hits), turned by the ring's rot. The ring's notes and
 * velocities are kept, in order, and dealt out again round the new hits. */
void roto_euclid(uint32_t trk, uint32_t hits)
{
    roto_track_t *k;
    uint8_t note[ROTO_STEPS], vel[ROTO_STEPS];
    uint32_t s, n = 0, j = 0, len;
    if (trk >= ROTO_TRACKS) return;
    k = &roto.trk[trk];
    len = k->len ? k->len : 1u;
    if (hits > len) hits = len;
    for (s = 0; s < len; s++)
        if (k->step[s].on) { note[n] = k->step[s].note; vel[n] = k->step[s].vel; n++; }
    if (!n) { note[0] = k->fill_note ? k->fill_note : 60u; vel[0] = 96u; n = 1; }
    for (s = 0; s < len; s++) {
        uint32_t u = (s + len - k->rot % len) % len;    /* the step before the turn */
        roto_step_t *st = &k->step[s];
        st->on = (uint8_t)((u * hits) % len < hits);
        if (st->on) { st->note = note[j % n]; st->vel = vel[j % n]; j++; }
    }
    k->fill_n = 0;
    home_save(k);
}

/* FILTER: per engine, the edit settings (0..7) that brighten, bite and dirty the sound.
 * From each engine's own list (ANALOG CUT RES DRV, FM6 MLVL FB MEG, DRUM TONE SNAP DRV ...). */
static const uint8_t TONE_MAP[14][4] = {
    /* bright edge drive  env? */
    {4, 5, 6, 1},          /*  0 ANALOG  CUT  RES  DRV  */
    {0xFF, 0xFF, 0xFF, 0}, /*  1 (none) */
    {2, 3, 7, 1},          /*  2 PHASE   DCW  ENV  FOLD */
    {7, 2, 3, 1},          /*  3 LOFI    TONE DUTY CRSH */
    {4, 2, 6, 1},          /*  4 SAMPLE  CUT  BITS DRV  */
    {4, 6, 5, 1},          /*  5 VOICE   BUZZ Q    BRTH */
    {5, 6, 7, 1},          /*  6 TRIO    CUT  RES  PW   */
    {3, 5, 6, 0},          /*  7 WHEEL   TOP  CLICK DRV */
    {7, 6, 3, 1},          /*  8 GRAIN   TONE RAND DENS */
    {2, 3, 5, 1},          /*  9 PHYS    BRIT DAMP ACC  */
    {2, 4, 7, 0},          /* 10 DRUM    TONE SNAP DRV  */
    {2, 3, 7, 1},          /* 11 NOISE   FREQ RES  CRSH */
    {2, 1, 4, 1},          /* 12 FM6     MLVL FB   MEG  */
    {7, 6, 0xFF, 1},       /* 13 SLICE   TONE DCAY -    */
};

void roto_tone_map(uint32_t eng, uint8_t out[3], int *has_env)
{
    const uint8_t *m = TONE_MAP[eng < 14u ? eng : 1u];
    out[0] = m[0]; out[1] = m[1]; out[2] = m[2];
    *has_env = m[3];
}

/* DELAY TIME, as params.c N_DTIME lists them */
static const char *const DT_NAME[13] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T", "1/2", "1/1", "2BAR", "4BAR", "1/8D", "1/16D", "1/4D"};
static const uint16_t DT_96[13] = {24, 12, 6, 3, 8, 4, 48, 96, 192, 384, 18, 9, 36};
static const uint8_t DT_ORDER[13] = {3, 5, 2, 4, 11, 1, 10, 0, 12, 6, 7, 8, 9};    /* shortest to longest */

const char *roto_dtime_name(uint32_t t) { return DT_NAME[t < 13u ? t : 1u]; }
uint32_t roto_dtime_96(uint32_t t) { return DT_96[t < 13u ? t : 1u]; }

uint32_t roto_dtime_step(uint32_t t, int32_t dir)
{
    int32_t i, at = 5;
    for (i = 0; i < 13; i++) if (DT_ORDER[i] == t) at = i;
    at += dir;
    if (at < 0) at = 0;
    if (at > 12) at = 12;
    return DT_ORDER[at];
}

/* ------------------------------------------------------------- SOUND (UI side) */
typedef struct { const char *name; uint8_t n; const uint8_t (*s)[2]; } roto_cat_t;
static const uint8_t RCAT_BASS[][2]  = {{0, 7}, {6, 0}, {0, 2}, {0, 4}, {12, 2}, {3, 1}, {5, 2}};
static const uint8_t RCAT_SYNTH[][2] = {{0, 0}, {6, 2}, {6, 1}, {0, 6}, {3, 0}, {3, 3}, {3, 4}, {3, 2}, {0, 9}, {2, 0}, {12, 3}, {5, 1}};
static const uint8_t RCAT_KEYS[][2]  = {{12, 0}, {0, 5}, {4, 0}, {7, 0}, {7, 1}, {7, 2}, {7, 4}, {2, 1}, {12, 6}};
static const uint8_t RCAT_PAD[][2]   = {{0, 1}, {0, 3}, {0, 11}, {2, 2}, {12, 4}, {5, 0}, {5, 3}, {6, 4}, {8, 0}, {8, 2}, {8, 3}, {9, 7}};
static const uint8_t RCAT_PLUCK[][2] = {{0, 8}, {12, 7}, {9, 2}, {9, 8}, {9, 4}, {9, 1}, {12, 5}, {12, 1}, {9, 0}, {6, 3}, {2, 4}, {2, 3}, {2, 5}, {9, 3}};
static const uint8_t RCAT_WIND[][2]  = {{7, 3}, {4, 2}, {4, 3}, {0, 10}};
static const uint8_t RCAT_PERC[][2]  = {{9, 5}, {9, 6}};
static const uint8_t RCAT_FX[][2]    = {{11, 0}, {11, 1}, {11, 2}, {11, 3}, {8, 1}, {13, 0}, {13, 1}};
#define RCAT(nm, a) {nm, (uint8_t)(sizeof a / sizeof a[0]), a}
static const roto_cat_t ROTO_CATS[] = {
    RCAT("BASS", RCAT_BASS), RCAT("SYNTH", RCAT_SYNTH), RCAT("KEYS", RCAT_KEYS), RCAT("PAD", RCAT_PAD),
    RCAT("PLUCK", RCAT_PLUCK), RCAT("WIND", RCAT_WIND), RCAT("PERC", RCAT_PERC), RCAT("FX", RCAT_FX),
};
#define RNCAT ((uint32_t)(sizeof ROTO_CATS / sizeof ROTO_CATS[0]))

uint32_t roto_cat_count(void) { return RNCAT; }
const char *roto_cat_name(uint32_t c) { return ROTO_CATS[c % RNCAT].name; }
uint32_t roto_cat_size(uint32_t c) { return ROTO_CATS[c % RNCAT].n; }
void roto_cat_sound(uint32_t c, uint32_t i, uint8_t *eng, uint8_t *preset)
{
    const roto_cat_t *k = &ROTO_CATS[c % RNCAT];
    *eng = k->s[i % k->n][0];
    *preset = k->s[i % k->n][1];
}
void roto_cat_find(uint32_t eng, uint32_t preset, uint32_t *c, uint32_t *i)
{
    uint32_t a, b;
    for (a = 0; a < RNCAT; a++)
        for (b = 0; b < ROTO_CATS[a].n; b++)
            if (ROTO_CATS[a].s[b][0] == eng && ROTO_CATS[a].s[b][1] == preset) { *c = a; *i = b; return; }
    *c = 0; *i = 0;
}

/* the kits: Felucca's STD, the drum-machine models (KIT 80 10 66 55 77), DEEP TIGHT PERC */
static const char *const KIT_NAME[9] = {"STD KIT", "80 KIT", "10 KIT", "66 KIT", "55 KIT", "77 KIT", "DEEP KIT", "TIGHT KIT", "PERC KIT"};
static const uint8_t KIT_PRESET[9] = {0, 0, 0, 0, 0, 0, 1, 2, 3};
static const int8_t KIT_VAL[9] = {-1, 4, 5, 6, 7, 8, -1, -1, -1};
uint32_t roto_kit_count(void) { return 9u; }
const char *roto_kit_name(uint32_t k) { return KIT_NAME[k % 9u]; }
void roto_kit_sound(uint32_t k, uint8_t *preset, int32_t *kitval) { *preset = KIT_PRESET[k % 9u]; *kitval = KIT_VAL[k % 9u]; }

/* the pieces a drum ring can play (General MIDI notes: the STD kit has them all; a model kit plays its own
 * piece in the same lane) */
static const char *const PIECE_NAME[14] = {"KICK", "SNARE", "CLAP", "CL HAT", "OP HAT", "RIM", "TOM", "CONGA",
                                           "LO CONGA", "BONGO", "CLAVE", "SHAKER", "COWBELL", "CYMBAL"};
static const uint8_t PIECE_NOTE[14] = {36, 38, 39, 42, 46, 37, 45, 63, 64, 60, 75, 70, 56, 49};
uint32_t roto_piece_count(void) { return 14u; }
const char *roto_piece_name(uint32_t p) { return PIECE_NAME[p % 14u]; }

static uint32_t ring_main_note(const roto_track_t *k)
{
    uint32_t s;
    for (s = 0; s < k->len; s++) if (k->step[s].on) return k->step[s].note;
    return k->fill_note;
}
uint32_t roto_piece_of(uint32_t ring)
{
    uint32_t n = ring_main_note(&roto.trk[ring % ROTO_TRACKS]), p;
    for (p = 0; p < 14u; p++) if (PIECE_NOTE[p] == n) return p;
    for (p = 0; p < 14u; p++) if (n == 62u && PIECE_NOTE[p] == 63u) return p;   /* (a muted conga: CONGA) */
    return 0;
}
void roto_piece_set(uint32_t ring, uint32_t p)
{
    roto_track_t *k = &roto.trk[ring % ROTO_TRACKS];
    uint32_t from = ring_main_note(k), to = PIECE_NOTE[p % 14u], s;
    for (s = 0; s < ROTO_STEPS; s++) {
        if (k->step[s].on && k->step[s].note == from) k->step[s].note = (uint8_t)to;
        if (k->home[s].on && k->home[s].note == from) k->home[s].note = (uint8_t)to;
    }
    if (k->fill_note == from || !ring_hits(k)) k->fill_note = (uint8_t)to;
}

/* ------------------------------------------------------------- ARP (UI side) */
static const uint8_t RARP_DEG[7] = {0, 3, 4, 5, 2, 6, 1};   /* i, iv, v, VI, III, VII, ii */

static uint32_t rarp_tone(uint32_t deg)                   /* semitones above the key's root */
{
    return MODE_SC[roto.mode < 3u ? roto.mode : 0u][deg % 7u] + 12u * (deg / 7u);
}

void roto_arp_build(void)
{
    uint8_t s[16];
    uint32_t n = 0, o, j, d, base, r = roto.arp_range < 1u ? 1u : roto.arp_range > 4u ? 4u : roto.arp_range;
    if (roto.arp_chord) {
        d = RARP_DEG[(roto.arp_chord - 1u) % 7u];
        base = (roto.arp_part <= 1u ? 36u : 48u) + roto.root % 12u;
        if (r > 2u && base >= 48u) base -= 12u;          /* (wide ranges start lower) */
        for (o = 0; o < r; o++)
            for (j = 0; j < 4u; j++) {
                uint32_t note = base + rarp_tone(d + 2u * j) + 12u * o;
                s[n++] = (uint8_t)(note > 120u ? 120u : note);
            }
    }
    roto.arp_len = 0;                                    /* (the audio side never sees half a list) */
    for (j = 0; j < n; j++) roto.arp_seq[j] = s[j];
    roto.arp_len = (uint8_t)n;
}

void roto_arp_name(char *out)
{
    static const char *const PC[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t d, r, t3, t7, i = 0;
    const char *q, *p;
    if (!roto.arp_chord) { out[0] = 'O'; out[1] = 'F'; out[2] = 'F'; out[3] = 0; return; }
    d = RARP_DEG[(roto.arp_chord - 1u) % 7u];
    r = rarp_tone(d);
    t3 = rarp_tone(d + 2u) - r;
    t7 = rarp_tone(d + 6u) - r;
    q = t3 == 4u ? (t7 == 11u ? "MAJ7" : "DOM7") : (rarp_tone(d + 4u) - r == 6u ? "HDIM" : "MIN7");
    for (p = PC[(roto.root + r) % 12u]; *p; p++) out[i++] = *p;
    out[i++] = ' ';
    for (p = q; *p; p++) out[i++] = *p;
    out[i] = 0;
}

/* ------------------------------------------------------------- transport */
void roto_play(void)
{
    uint32_t t;
    roto.running = 0;                               /* (the tick releases anything still held) */
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto.trk[t].sub = 0;
        roto.trk[t].pos = ROTO_POS_START;           /* step 1 plays at once */
    }
    roto.running = 1;
}

void roto_stop(void)
{
    roto.running = 0;
}
