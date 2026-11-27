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
        roto.trk[t].a_on = 0xFF;
        roto.trk[t].arp_rate = 1;
        roto.trk[t].arp_range = 1;
        roto.trk[t].arp_len = 50;
    }
    roto.dub = 50;
    roto.gprob = 100;
    roto.save_cur = 0xFF;
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
static const uint8_t SND_DRUMS[][3] = {{10, 0, 116}, {10, 1, 116}, {10, 3, 116}};                 /* DRUM KIT, DEEP KIT, PERC KIT */
static const uint8_t SND_BASS[][3]  = {{0, 7, 126}, {0, 2, 127}};                                /* SUB BASS, SQR BASS (Juno-like; round, up front) */
static const uint8_t SND_STAB[][3]  = {{12, 0, 121}, {0, 3, 122}};                               /* TINE EP, PWM STR (soft, held) */
static const uint8_t SND_BLIP[][3]  = {{0, 5, 111}, {7, 3, 119}};                               /* SINE KEY, SOFT FLUTE (the soft ones, set back) */
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

static uint32_t rt_strlen0(const char *p) { uint32_t n = 0; while (p[n]) n++; return n; }

/* the grooves: written by hand, one sound to a ring, after three producers:
 *   Andrew Weatherall  the "drug-chug" (his club night: "never knowingly exceeding 120 BPMs"; "spacy dubby
 *                      disco"): a lazy, strolling bass behind the beat, claps on 2 and 4, dub echo
 *   Baby Ford          "rolling drum programming", "bubbling" basslines, gated stabs, less is more
 *   Ricardo Villalobos "static rhythmic construction you can dance to" (median 126 BPM, minor keys): a soft
 *                      four-on-the-floor, sparse percussion on loops of odd lengths that drift against the
 *                      bar, a one-note bass on the off-beats, a few melody notes into a dotted-eighth echo
 * Rings: 1 kick, 2 bass, 3 chord, 4 melody, 5 one hat, 6 one percussion. A power-on picks a groove, a key
 * and a few small touches; EVOLVE only makes small touches too.
 *   drums  X loud  x medium  g a ghost (quiet, only sometimes there)   . nothing
 *   bass, melody  1..7 the mode's degrees, 8 the root an octave up
 *   chord  a the home chord  b another (minor 7ths in the mode); 16 steps of 1/8, two bars */
typedef struct {
    const char *kick, *hat, *perc, *bass, *chord, *mel;
    uint8_t hat_note, perc_note, perc_len, mel_div;
    uint8_t bpm, swing, bgate, cgate, blag, plag, rnd, style;   /* style: 0 Weatherall (deep, dub), 1 Baby Ford, 2 Villalobos */
} roto_groove_t;

static const roto_groove_t GROOVES[] = {
    /* -- Weatherall -- */
    /* drug chug */      {"X...X...X...X...", "..X...x...X...x.", "....X.......X..g", "..1..18...1..1.7", "...a............", "......................5.........",
                          42, 39, 16, 1, 105, 53, 45, 25, 14, 6, 8, 0},
    /* Sabres dub */     {"X...X...X...X.x.", ".gX..gXg.gX..gXg", "....X..g....X...", "1.....1...5.....1.....1...8...7.", "....a...........", "..............................3.",
                          42, 38, 16, 1, 108, 53, 70, 25, 12, 4, 8, 0},
    /* lazy stroll */    {"X...X...X...X...", "gxXggxXggxXggxXg", "......X.......X.", "..1..3.5.........5.3..1.....8...", "..a.............", "........8.......................",
                          70, 37, 16, 1, 103, 54, 55, 60, 16, 0, 10, 0},
    /* spacy disco */    {"X...X...X...X...", "..X...X...X...X.", "..x..x....x.", "..8..18...8..18.", "a.......b.......", "1.......5...........3...........",
                          42, 56, 12, 1, 110, 53, 35, 90, 10, 0, 6, 0},
    /* -- Baby Ford -- */
    /* rolling */        {"X...X...X...X...", "..X.g.X.g.X.g.Xg", ".g..x..g.g..x", "1.11..1.1.11..8.", "..a.......a.....", "",
                          42, 37, 13, 1, 126, 52, 40, 20, 6, 3, 8, 1},
    /* deep minimal */   {"X...X..xX...X...", "..X...x...X...x.", "....X.......X...", "..1...1.....5...", "a...............", "...........8.................5..",
                          42, 39, 16, 1, 124, 51, 50, 90, 6, 8, 8, 1},
    /* bubbling */       {"X...X...X...X...", "gxXggxXggxXggxXg", "x..g..x.g...", "1.8.1.8.1.8.1.88", "......a.........", "",
                          70, 63, 12, 1, 128, 52, 30, 25, 4, 0, 10, 1},
    /* marble */         {"X...X...X...X...", "..X...X...X...x.", "..g...........x...........g.", "..1.....1.1.....5..1.....8.1....", "...a........b...", "",
                          42, 37, 28, 1, 125, 51, 45, 25, 6, 0, 8, 1},
    /* -- Villalobos -- */
    /* electro 8ths */   {"X...X...X...X...", "x.X.x.X.x.X.x.X.", "......x.....x..x......x....x....", "..1...1...1...1.", "", "1.......5.......3.......2.......",
                          42, 45, 32, 2, 130, 51, 25, 25, 4, 5, 12, 2},
    /* tambours */       {"X...X...X...X...", "ggXgggXgggXgggXg", "x..g.x.", "..1.....1..1....", "a...............", "",
                          70, 63, 7, 1, 134, 52, 45, 90, 4, 0, 15, 2},
    /* easy */           {"X...X...X...X...", "..X...X...X...X.", "x...g..x...", "..1..5.....3.1....1..5.....7.8..", "", "5.......3.......1.......2.......",
                          42, 75, 11, 2, 124, 52, 45, 25, 4, 3, 12, 2},
    /* ostinato */       {"X...X...X...X...", "..x...x...x...x.", "x..g...x....g..x...", "1..1..1...1..1..", "..........a.....", "8.6.5...8.6.5...8.6.5.3.........",
                          42, 37, 19, 1, 128, 51, 35, 25, 4, 2, 12, 2},
};
#define NGROOVE ((uint32_t)(sizeof GROOVES / sizeof GROOVES[0]))

/* a drum ring: every hit the same note, loud / medium / a ghost */
static void groove_ring(roto_track_t *k, const char *p, uint32_t note, uint32_t len, uint32_t level)
{
    uint32_t s;                                               /* (level: percent; the hats and percussion sit back, soft) */
    for (s = 0; s < len && s < ROTO_STEPS && p[s]; s++) {
        char c = p[s];
        if (c == 'X') put(k, s, note, ((108u + rnd(12u)) * level) / 100u);
        else if (c == 'x') put(k, s, note, ((66u + rnd(18u)) * level) / 100u);
        else if (c == 'g') { put(k, s, note, ((34u + rnd(14u)) * level) / 100u); k->step[s].chance = (uint8_t)(55u + rnd(36u)); }
    }
}

static uint32_t ring_hits(const roto_track_t *k);

void roto_seq_generate(uint32_t seed)
{
    static const uint8_t ROOTS[6] = {2, 4, 9, 7, 0, 5};          /* D E A G C F */
    /* chord roots where a minor 7th chord stays in the mode (aeolian i iv v, dorian i ii v, phrygian i iv bvii) */
    static const uint8_t STAB_ROOT[3][3] = {{0, 5, 7}, {0, 2, 7}, {0, 5, 10}};
    const roto_groove_t *g;
    const uint8_t *sc;
    roto_track_t *k;
    uint32_t i, s, n, pc, base, t, first = ROTO_STEPS, blen;

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
    roto.bpm = (uint8_t)(g->bpm - 1u + rnd(3u));
    roto.dtime = chance(75) ? 10u : 12u;                         /* params.c N_DTIME: 1/8D (the dotted-eighth echo), now and then 1/4D */
    roto.dfdbk = (uint8_t)(g->style == 0u ? 64u + rnd(10u) : 55u + rnd(14u));   /* (Weatherall: a longer dub tail) */
    if (g->style == 0u) roto.dub = 70;                           /* (and more throws into it) */
    roto.swing = (uint8_t)(g->swing + rnd(2u));
    roto.grand = g->rnd;                                         /* (a little looseness: "organic, human") */
    roto.evolve = 40;

    /* 1: the kick, alone */
    k = &roto.trk[0];
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = 36;
    groove_ring(k, g->kick, 36, 16, 84);                          /* (the bass as loud as the kick, as on the records) */
    if (g->style == 0u) { sound(&roto.snd[0], SND_DRUMS[1], 0, 28, 0, 0); roto.snd[0].kit = 10; }   /* Weatherall: SUB KIT, more room */
    else { sound(&roto.snd[0], SND_DRUMS[0], 0, 18, 0, 0); roto.snd[0].kit = 9; }               /* ROUND KIT (every piece the grooves use) */

    /* 2: the bass (one or two bars); maybe one note an octave over, maybe one a 1/16 later (never onto a beat) */
    k = &roto.trk[1];
    blen = (uint32_t)rt_strlen0(g->bass);
    k->len = (uint8_t)(blen > 16u ? 32u : 16u); k->div = 1; k->gate = g->bgate; k->lag = g->blag;
    base = 26u + (pc + 10u) % 12u;                               /* (D1 .. C#2, 37 .. 69 Hz: where the records' sub sits) */
    k->fill_note = (uint8_t)base;
    for (s = 0; s < blen && s < ROTO_STEPS; s++) {
        char c = g->bass[s];
        if (c < '1' || c > '8') continue;
        put(k, s, c == '8' ? base + 12u : base + sc[c - '1'], 92u + rnd(18u));
        if (first == ROTO_STEPS) first = s;
    }
    if (first < ROTO_STEPS) k->step[first].vel = 118;           /* (the first leans on) */
    if (chance(40)) {
        n = 0;
        for (s = first + 1u; s < k->len; s++) n += k->step[s].on;
        if (n) {
            uint32_t r = rnd(n);
            for (s = first + 1u; s < k->len; s++)
                if (k->step[s].on && !r--) {
                    k->step[s].note = (uint8_t)(k->step[s].note >= base + 12u ? k->step[s].note - 12u : k->step[s].note + 12u);
                    break;
                }
        }
    }
    if (chance(25)) {
        for (s = (uint32_t)k->len - 1u; s > first; s--)
            if (k->step[s].on && s + 1u < k->len && !k->step[s + 1u].on && ((s + 1u) & 3u)) {
                k->step[s + 1u] = k->step[s];
                k->step[s].on = 0;
                break;
            }
    }
    sound(&roto.snd[1], SND_BASS[g->style == 0u ? 0u : rnd(NSND(SND_BASS))], 0, 0, 0, 0);   /* (Weatherall: the round SUB) */

    /* 3: the chord, a two-bar loop into the echo; it opens and closes slowly */
    k = &roto.trk[2];
    k->len = 16; k->div = 2; k->gate = g->cgate; k->lag = 6;
    k->chord[0] = 3; k->chord[1] = 7; k->chord[2] = 10;
    n = 55u + (pc + 5u) % 12u;                                   /* (G3 .. F#4: over the box at 150..500 Hz the records keep clear) */
    k->fill_note = (uint8_t)n;
    {
        uint32_t home = chance(30) ? STAB_ROOT[roto.mode][1] : 0u;
        for (s = 0; s < 16u && g->chord[s]; s++)
            if (g->chord[s] == 'a' || g->chord[s] == 'b')
                put(k, s, n + (g->chord[s] == 'a' ? home : STAB_ROOT[roto.mode][2]) - (n + STAB_ROOT[roto.mode][2] > 70u && g->chord[s] == 'b' ? 12u : 0u), 64u + rnd(16u));
    }
    sound(&roto.snd[2], SND_STAB[rnd(NSND(SND_STAB))], g->style == 0u ? 104u : 82u, g->style == 0u ? 62u : 52u, 20, -22);   /* (Weatherall: deeper into the echo) */
    roto.snd[2].drift = (int8_t)(44 + (int32_t)rnd(12u));

    /* 4: the melody, a few notes into the echo */
    k = &roto.trk[3];
    blen = (uint32_t)rt_strlen0(g->mel);
    k->len = (uint8_t)(blen ? (blen > 16u ? 32u : 16u) : 16u); k->div = g->mel_div ? g->mel_div : 1u; k->gate = 30;
    k->fill_note = (uint8_t)(60u + pc);
    for (s = 0; s < blen && s < ROTO_STEPS; s++) {
        char c = g->mel[s];
        if (c < '1' || c > '8') continue;
        put(k, s, 60u + pc + (c == '8' ? 12u : sc[c - '1']), 78u + rnd(20u));
    }
    sound(&roto.snd[3], SND_BLIP[rnd(NSND(SND_BLIP))], g->style == 0u ? 110u : 92u, 50, 0, 26);   /* (a note fired into the echo) */

    /* 5: one hat (loud on the off-beats, ghosts between) */
    k = &roto.trk[4];
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = g->hat_note;
    groove_ring(k, g->hat, g->hat_note, 16, 92);                 /* (the records' hats: clear, up front) */

    /* 6: one percussion, often on a loop of its own length (it drifts against the bar); maybe one hit fewer */
    k = &roto.trk[5];
    k->part = 0;
    k->len = g->perc_len; k->div = 1; k->gate = 25; k->fill_note = g->perc_note; k->lag = g->plag;
    groove_ring(k, g->perc, g->perc_note, g->perc_len,           /* (low drums back: they fill 150..500 Hz) */
                g->perc_note == 45u || g->perc_note == 60u || g->perc_note == 63u || g->perc_note == 64u ? 60u : 86u);
    n = ring_hits(k);
    if (n > 3u && chance(30)) {
        uint32_t r = rnd(n);
        for (s = 0; s < k->len; s++) if (k->step[s].on && !r--) { k->step[s].on = 0; break; }
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

static uint32_t roto_sr = 44100u;           /* (the tick's sample rate, for the lag) */

static uint32_t roto_six = 5512u;           /* (a 1/16 in samples, for the arp) */

/* -------- ARP. A hit sets the notes (its chord: NOTES, over RANGE octaves) and sounds the first at once; then
 * the arp steps through them on the groove's own grid (the swung 1/16s; 1/32 and the triplets between them) until
 * the next hit, where it carries on from where it was (HOLD) or starts again (RESTART). RHYTHM rests some of its
 * steps; ACCENT leans on the beat or the off-beat. */
static const uint8_t ARP_C[ROTO_ARP_RATES] = {1, 1, 2, 4, 4, 3, 4};   /* 1/16s in a cycle ... */
static const uint8_t ARP_N[ROTO_ARP_RATES] = {2, 1, 1, 6, 3, 1, 1};   /* ... notes in it: 1/32 1/16 1/8 1/16T 1/8T 1/8D 1/4 */
static const uint16_t ARP_RHY[ROTO_ARP_RHYTHMS] = {               /* 16 steps, the first the top bit */
    0xFFFF, 0xDDDD, 0xBBBB, 0xAAAA, 0x9292, 0x6666, 0x8889, 0x9249, 0x9228, 0xFEFC};
static const char *const ARP_RHY_N[ROTO_ARP_RHYTHMS] = {"ALL", "GALLOP", "SKIP", "HALF", "TRESILLO", "PUSH", "SPARSE", "THREES", "CLAVE", "BREATH"};
const char *roto_arp_rhythm_name(uint32_t r) { return ARP_RHY_N[r % ROTO_ARP_RHYTHMS]; }

static void rarp_off(roto_track_t *k)
{
    uint32_t i;
    if (k->a_on != 0xFFu) roto_note_off(k->part, k->a_on);
    for (i = 0; i < k->a_ch && i < 4u; i++) roto_note_off(k->part, k->a_chn[i]);
    k->a_on = 0xFF;
    k->a_ch = 0;
    k->a_gate_left = 0;
}

static void rarp_stop(roto_track_t *k)
{
    rarp_off(k);
    k->a_n = 0;
    k->a_sub = 0;
}

static void rarp_step(roto_track_t *k)             /* the next note, by MODE */
{
    uint32_t n = k->a_n, i = k->a_idx;
    if (!n) return;
    switch (k->arp) {
    case ROTO_ARP_DOWN:   i = i ? i - 1u : n - 1u; break;
    case ROTO_ARP_UPDOWN: case ROTO_ARP_DOWNUP:
        if (n < 2u) { i = 0; break; }
        if (!k->a_dir) { if (i + 1u >= n) { k->a_dir = 1; i = n - 2u; } else i++; }
        else           { if (!i) { k->a_dir = 0; i = 1; } else i--; }
        break;
    case ROTO_ARP_RANDOM: { uint32_t j = audio_dice(n); i = (j == i && n > 1u) ? (j + 1u) % n : j; } break;
    case ROTO_ARP_PEDAL:
        if (i || n < 2u) i = 0;
        else { k->a_k = (uint8_t)(k->a_k % (n - 1u) + 1u); i = k->a_k; }
        break;
    case ROTO_ARP_CONVERGE:
        k->a_k = (uint8_t)((k->a_k + 1u) % n);
        i = (k->a_k & 1u) ? n - 1u - (k->a_k >> 1) : (uint32_t)(k->a_k >> 1);
        break;
    case ROTO_ARP_CHORD:  i = 0; break;
    default:              i = (i + 1u) % n; break;
    }
    k->a_idx = (uint8_t)(i < n ? i : 0u);
}

/* one step of the arp: a rest (RHYTHM) or a note; phase: its 1/16 in the beat (0..3), 4 between 1/16s */
static void rarp_tick(roto_track_t *k, uint32_t phase)
{
    uint32_t n = k->a_n, r = k->arp_rate % ROTO_ARP_RATES, vel, g, len;
    if (!n || !k->arp) return;
    rarp_off(k);
    k->a_last16 = roto.n16 + 1u;
    if (!((ARP_RHY[k->arp_rhythm % ROTO_ARP_RHYTHMS] >> (15u - (k->a_pos & 15u))) & 1u)) { k->a_pos++; return; }
    if (k->a_fresh) k->a_fresh = 0; else rarp_step(k);
    vel = k->a_vel;
    if (k->arp_accent == 0u && phase != 0u) vel = (vel * 3u) / 4u;          /* the beat */
    if (k->arp_accent == 2u && phase != 2u) vel = (vel * 3u) / 4u;          /* the off-beat */
    if (vel < 20u) vel = 20u;
    if (k->arp == ROTO_ARP_CHORD) {
        uint32_t i;
        for (i = 0; i < n && i < 4u; i++) { k->a_chn[i] = k->a_notes[i]; roto_note_on(k->part, k->a_notes[i], vel); }
        k->a_ch = (uint8_t)i;
    } else {
        k->a_on = k->a_notes[k->a_idx < n ? k->a_idx : 0u];
        roto_note_on(k->part, k->a_on, vel);
    }
    len = (ARP_C[r] * roto_six) / ARP_N[r];
    g = (len * (k->arp_len ? k->arp_len : 50u)) / 100u;
    k->a_gate_left = g ? g : 1u;
    k->a_pos++;
}

/* the 1/16 clock: each arp steps on its rate's cycle, and sets up the steps between 1/16s (1/32, triplets) */
static void rarp_sixteenth(uint32_t six)
{
    uint32_t t;
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        uint32_t r = k->arp_rate % ROTO_ARP_RATES;
        if (!k->arp || !k->a_n || roto.n16 % ARP_C[r]) continue;
        if (k->a_last16 != roto.n16 + 1u) rarp_tick(k, roto.n16 & 3u);    /* (not twice: a hit just sounded) */
        if (ARP_N[r] > 1u) {
            k->a_int = (ARP_C[r] * six) / ARP_N[r];
            k->a_left = k->a_int ? k->a_int : 1u;
            k->a_sub = (uint8_t)(ARP_N[r] - 1u);
        } else k->a_sub = 0;
    }
}

/* a hit: its notes (NOTES: the ring's own chord, or the key's chord on it, over RANGE octaves; a drum: itself, a
 * roll), and the first of them now */
static void rarp_start(roto_track_t *k, uint32_t note, uint32_t vel)
{
    static const uint8_t DEG[ROTO_ARP_SHAPES][6] = {     /* the key's degrees above the note (0xFF: end) */
        {0, 2, 4, 6, 0xFF}, {0, 2, 4, 0xFF}, {0, 2, 4, 6, 8, 0xFF}, {0, 3, 4, 0xFF}, {0, 0xFF}, {0, 4, 7, 0xFF}};
    static const uint8_t SEMI[ROTO_ARP_SHAPES][6] = {    /* the same in semitones, for a note out of the key */
        {0, 3, 7, 10, 0xFF}, {0, 3, 7, 0xFF}, {0, 3, 7, 10, 14, 0xFF}, {0, 5, 7, 0xFF}, {0, 0xFF}, {0, 7, 12, 0xFF}};
    uint8_t s[16];
    uint32_t n = 0, o, j, sh = k->arp_notes % ROTO_ARP_SHAPES, was = k->a_n;
    uint32_t r = k->arp_range < 1u ? 1u : k->arp_range > 4u ? 4u : k->arp_range;
    if (roto.snd[k->part].eng == ROTO_ENG_DRUM) {
        s[n++] = (uint8_t)note;
    } else {
        int32_t iv[6];
        uint32_t m = 1;
        iv[0] = 0;
        if (sh == 0u) for (j = 0; j < ROTO_CHORD; j++) if (k->chord[j] > 0) iv[m++] = k->chord[j];
        if (m == 1u) {                                   /* the key's chord on this note */
            const uint8_t *sc = MODE_SC[roto.mode % 3u];
            uint32_t pc = (note + 120u - roto.root) % 12u, d = 0;
            for (j = 0; j < 7u; j++) if (sc[j] == pc) d = j + 1u;
            m = 0;
            for (j = 0; j < 6u && DEG[sh][j] != 0xFFu; j++) {
                if (d) { uint32_t g = d - 1u + DEG[sh][j]; iv[m++] = (int32_t)(sc[g % 7u] + 12u * (g / 7u)) - (int32_t)sc[d - 1u]; }
                else iv[m++] = SEMI[sh][j];
            }
        }
        for (o = 0; o < r; o++)
            for (j = 0; j < m && n < 16u; j++) {
                int32_t x = (int32_t)note + iv[j] + 12 * (int32_t)o;
                uint32_t q, dup = 0;
                if (x > 120) x = 120;
                for (q = 0; q < n; q++) dup |= s[q] == (uint8_t)x;   /* (each note once: 1-5-8 over octaves meets itself) */
                if (!dup) s[n++] = (uint8_t)x;
            }
    }
    rarp_off(k);
    for (j = 0; j < n; j++) k->a_notes[j] = s[j];
    k->a_n = (uint8_t)n;
    k->a_vel = (uint8_t)(vel ? vel : 100u);
    if (k->arp_restart || !was) {                         /* from the start */
        k->a_dir = (uint8_t)(k->arp == ROTO_ARP_DOWNUP);
        k->a_k = 0;
        k->a_idx = (uint8_t)(k->arp == ROTO_ARP_DOWN || k->arp == ROTO_ARP_DOWNUP ? n - 1u : 0u);
        k->a_pos = 0;
        k->a_fresh = 1;
    } else if (k->a_idx >= n) k->a_idx = 0;
    {   /* the hit sounds now if it falls on the arp's own grid (and the grid has not just stepped); else at its next step */
        uint32_t rr = k->arp_rate % ROTO_ARP_RATES;
        if (roto.n16 % ARP_C[rr] == 0u && k->a_last16 != roto.n16 + 1u) rarp_tick(k, roto.n16 & 3u);
    }
}

static uint32_t ring_note(const roto_track_t *k, uint32_t note)   /* GLO PITCH (not on a drum kit) */
{
    int32_t n = (int32_t)note;
    if (k->transp && roto.snd[k->part].eng != ROTO_ENG_DRUM) n += k->transp;
    return (uint32_t)(n < 0 ? 0 : n > 127 ? 127 : n);
}

static void fire_now(uint32_t t, roto_track_t *k, uint32_t step_len, uint32_t pos)
{
    const roto_step_t *st = &k->step[pos < ROTO_STEPS ? pos : 0u];
    uint32_t i, g, note, vel;
    if (k->nheld) release(t, k);
    if ((roto.mute >> t) & 1u) { if (k->a_n) rarp_stop(k); return; }   /* (muted: OP1..OP6) */
    if (!st->on) return;
    if (k->prob < 100u && audio_dice(100u) >= k->prob) { if (k->arp) rarp_stop(k); return; }
    if (roto.gprob < 100u && audio_dice(100u) >= roto.gprob) { if (k->arp) rarp_stop(k); return; }   /* (GROOVE PROB) */
    if (st->chance && audio_dice(100u) >= st->chance) { if (k->arp) rarp_stop(k); return; }
    note = ring_note(k, st->note);
    vel = st->vel ? st->vel : 100u;
    if (roto.grand) vel -= (vel * audio_dice(1u + (roto.grand * 40u) / 100u)) / 100u;   /* (RANDOM: a little softer) */
    if (vel < 1u) vel = 1u;
    if (k->arp) { rarp_start(k, note, vel); return; }
    k->held[k->nheld++] = (uint8_t)note;
    roto_note_on(k->part, (uint8_t)note, vel);
    for (i = 0; i < ROTO_CHORD; i++) {
        int32_t n = (int32_t)note + k->chord[i];
        if (!k->chord[i] || n < 0 || n > 127) continue;
        k->held[k->nheld++] = (uint8_t)n;
        roto_note_on(k->part, (uint8_t)n, vel);
    }
    {
        static const uint8_t LEN_PCT[4] = {0, 15, 70, 100};   /* LENGTH: the ring's own, SHORT, LONG, FULL */
        uint32_t L = roto_step_len(st);
        g = (step_len * (L ? LEN_PCT[L] : (k->gate ? k->gate : 50u))) / 100u;
    }
    k->gate_left = g ? g : 1u;
}

/* a step starts: now, or (a lazy ring) a few ms behind the beat */
static void fire(uint32_t t, roto_track_t *k, uint32_t step_len)
{
    uint32_t ms = k->lag + (roto.grand ? audio_dice(1u + (roto.grand * 15u) / 100u) : 0u);   /* (RANDOM: a little late) */
    uint32_t d = ms ? (roto_sr * ms) / 1000u : 0u;
    if (k->nheld) release(t, k);
    if (!d) { fire_now(t, k, step_len, k->pos); return; }
    k->lag_step = k->pos;
    k->lag_len = step_len > d ? step_len - d : 1u;
    k->lag_left = d;
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
                k->step[s].note = k->fill_note;               /* (the ring's own hat) */
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
    (void)s;                                                 /* (the rest: nothing this time) */
}

/* -------- dub throws: now and then a stab or blip ring is thrown into the delay for a bar */
static void throw_bar(void)
{
    if (roto.throw_trk < ROTO_PARTS) {
        return;                                              /* (one at a time) */
    }
    if (ev_dice(100u) < (uint32_t)roto.dub / 4u) {           /* DUB 50: a throw in about 1 bar of 8 (fired off now and then) */
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
    rarp_sixteenth(six);
}

void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate)
{
    uint32_t t, f, six, sw, len16;
    roto_sr = sample_rate ? sample_rate : 44100u;

    if (!roto.running) {
        for (t = 0; t < ROTO_TRACKS; t++) {
            roto.trk[t].lag_left = 0;
            if (roto.trk[t].nheld) release(t, &roto.trk[t]);
            if (roto.trk[t].a_n || roto.trk[t].a_on != 0xFFu) rarp_stop(&roto.trk[t]);
        }
        if (roto.throw_trk < ROTO_PARTS) { roto.throw_left = 1; throw_tick(); }
        return;
    }
    six = (sample_rate * 15u) / (roto.bpm ? roto.bpm : 120u);
    roto_six = six;
    sw = roto.swing < 50u ? 50u : roto.swing > 75u ? 75u : roto.swing;
    len16 = (roto.n16 & 1u) ? (2u * six * (100u - sw)) / 100u : (2u * six * sw) / 100u;
    if (roto.trk[0].pos == ROTO_POS_START) {                 /* PLAY: everything from the top, now */
        roto.n16 = 0;
        roto.acc16 = 0;
        sixteenth(six);
        len16 = (2u * six * sw) / 100u;
    }

    for (f = 0; f < block_frames; f++) {
        for (t = 0; t < ROTO_TRACKS; t++) {
            roto_track_t *k = &roto.trk[t];
            if (k->gate_left && --k->gate_left == 0u) release(t, k);
            if (k->lag_left && --k->lag_left == 0u) fire_now(t, k, k->lag_len, k->lag_step);
            if (k->a_n) {
                if (!k->arp) rarp_stop(k);
                else {
                    if (k->a_gate_left && --k->a_gate_left == 0u) rarp_off(k);
                    if (k->a_sub && k->a_left && --k->a_left == 0u) {   /* (a step between 1/16s) */
                        rarp_tick(k, 4u);
                        if (--k->a_sub) k->a_left = k->a_int;
                    }
                }
            }
        }
        if (++roto.acc16 >= len16) {
            roto.acc16 = 0;
            roto.n16++;
            len16 = (roto.n16 & 1u) ? (2u * six * (100u - sw)) / 100u : (2u * six * sw) / 100u;
            sixteenth(six);
        }
    }
}

/* ------------------------------------------------------------- editing */
uint32_t roto_step_len(const roto_step_t *st) { return (uint32_t)st->len_lo | ((uint32_t)st->len_hi << 1); }

static int roto_in_key(int32_t n)                    /* n is in the key (roto.root, roto.mode) */
{
    const uint8_t *sc = MODE_SC[roto.mode % 3u];
    uint32_t pc = (uint32_t)(n + 120 - (int32_t)roto.root) % 12u, j;
    for (j = 0; j < 7u; j++) if (sc[j] == pc) return 1;
    return 0;
}

static void step_belongs(roto_track_t *k, uint32_t step)   /* a step set by hand: not FILL's, and the pattern's own */
{
    uint32_t i, j = 0;
    for (i = 0; i < k->fill_n; i++) if (k->fill_at[i] != step) k->fill_at[j++] = k->fill_at[i];
    k->fill_n = (uint8_t)j;
    home_save(k);
}

void roto_step_add(uint32_t trk, uint32_t step)
{
    roto_track_t *k;
    roto_step_t *st;
    uint32_t s, d, at;
    int32_t note;
    if (trk >= ROTO_TRACKS || step >= ROTO_STEPS) return;
    k = &roto.trk[trk];
    st = &k->step[step];
    if (st->on) return;
    note = k->fill_note ? (int32_t)k->fill_note : 60;
    if (roto.snd[k->part].eng == ROTO_ENG_DRUM) {           /* a drum ring: its one drum */
        for (s = 0; s < k->len; s++) if (k->step[s].on) { note = k->step[s].note; break; }
    } else {                                                 /* the line carries on: the nearest note before it */
        for (d = 1; d < (uint32_t)k->len; d++) {
            s = (step + k->len - d) % k->len;
            if (k->step[s].on) { note = k->step[s].note; break; }
        }
        while (note > 0 && !roto_in_key(note)) note--;
    }
    at = (step * (k->div ? k->div : 1u)) % 4u;               /* the accent: on the beat, the off-beat, between */
    st->note = (uint8_t)note;
    st->vel = (uint8_t)(at == 0u ? 112u : at == 2u ? 92u : 76u);
    st->chance = 0; st->len_lo = 0; st->len_hi = 0;
    st->on = 1;
    step_belongs(k, step);
}

void roto_step_edit(uint32_t trk, uint32_t step, int32_t notes_in_key, int32_t accent, int32_t length, int32_t octaves)
{
    roto_track_t *k;
    roto_step_t *st;
    int32_t n, v, l;
    if (trk >= ROTO_TRACKS || step >= ROTO_STEPS) return;
    k = &roto.trk[trk];
    st = &k->step[step];
    if (!st->on) return;
    if (roto.snd[k->part].eng != ROTO_ENG_DRUM) {            /* (a drum ring keeps its drum) */
        n = st->note;
        while (notes_in_key > 0) { do n++; while (n < 96 && !roto_in_key(n)); notes_in_key--; }
        while (notes_in_key < 0) { do n--; while (n > 24 && !roto_in_key(n)); notes_in_key++; }
        n += 12 * octaves;
        while (n > 96) n -= 12;
        while (n < 24) n += 12;
        st->note = (uint8_t)n;
    }
    v = (int32_t)st->vel + 6 * accent;
    st->vel = (uint8_t)(v < 20 ? 20 : v > 127 ? 127 : v);
    l = (int32_t)roto_step_len(st) + length;
    l = l < 0 ? 0 : l > 3 ? 3 : l;
    st->len_lo = (uint8_t)(l & 1); st->len_hi = (uint8_t)(l >> 1);
    st->chance = 0;                                          /* (edited by hand: a real note, not a ghost) */
    step_belongs(k, step);
}

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

/* FILTER K3 on a sound with no filter envelope: another of its settings that moves the sound (DRUM TUNE, WHEEL BODY) */
uint32_t roto_tone_k3(uint32_t eng) { return eng == 10u ? 1u : eng == 7u ? 2u : 0xFFu; }

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

/* ------------------------------------------------------------- SAVE (UI side) */
void roto_save_take(roto_save_t *s)
{
    uint32_t t, i;
    uint8_t *p = (uint8_t *)s;
    for (i = 0; i < sizeof *s; i++) p[i] = 0;
    s->magic = ROTO_SAVE_MAGIC;
    s->gnum = (uint16_t)(roto.seed <= ROTO_GROOVES ? roto.seed : 0u);
    s->bpm = roto.bpm; s->swing = roto.swing; s->root = roto.root; s->mode = roto.mode;
    s->dtime = roto.dtime; s->dfdbk = roto.dfdbk; s->dcolor = roto.dcolor; s->evolve = roto.evolve; s->dub = roto.dub;
    s->rsv[0] = roto.grand; s->rsv[1] = (uint8_t)(100u - roto.gprob);   /* (stored as the drop chance: an older save's 0 is 100 %) */
    for (t = 0; t < ROTO_TRACKS; t++) {
        const roto_track_t *k = &roto.trk[t];
        roto_save_ring_t *r = &s->ring[t];
        for (i = 0; i < ROTO_STEPS; i++) r->step[i] = k->step[i];
        r->len = k->len; r->div = k->div; r->gate = k->gate; r->part = k->part;
        r->prob = k->prob; r->fill_note = k->fill_note; r->rot = k->rot; r->lag = k->lag;
        r->transp = k->transp; r->arp = (uint8_t)(k->arp | (k->arp_accent << 4) | (k->arp_restart << 6));
        r->arp_rate = (uint8_t)(k->arp_rate | (k->arp_notes << 3)); r->arp_range = (uint8_t)(k->arp_range | (k->arp_rhythm << 3)); r->arp_len = k->arp_len;
        for (i = 0; i < ROTO_CHORD; i++) r->chord[i] = k->chord[i];
    }
    for (t = 0; t < ROTO_PARTS; t++) s->snd[t] = roto.snd[t];
}

void roto_save_give(const roto_save_t *s)
{
    uint32_t t, i;
    roto.seed = s->gnum;
    roto.bpm = s->bpm < 40u ? 40u : s->bpm > 240u ? 240u : s->bpm;
    roto.swing = s->swing < 50u || s->swing > 75u ? 50u : s->swing;
    roto.root = s->root % 12u; roto.mode = s->mode % 3u;
    roto.dtime = s->dtime; roto.dfdbk = s->dfdbk; roto.dcolor = s->dcolor;
    roto.evolve = s->evolve <= 100u ? s->evolve : 50u; roto.dub = s->dub <= 100u ? s->dub : 50u;
    roto.grand = s->rsv[0] <= 100u ? s->rsv[0] : 0u; roto.gprob = (uint8_t)(s->rsv[1] <= 100u ? 100u - s->rsv[1] : 100u);
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto_track_t *k = &roto.trk[t];
        const roto_save_ring_t *r = &s->ring[t];
        k->len = r->len >= 1u && r->len <= ROTO_STEPS ? r->len : 16u;
        k->div = r->div >= 1u && r->div <= 4u ? r->div : 1u;
        k->gate = r->gate ? r->gate : 50u; k->part = r->part % ROTO_PARTS;
        k->prob = r->prob <= 100u ? r->prob : 100u; k->fill_note = r->fill_note; k->rot = r->rot;
        k->lag = r->lag <= 40u ? r->lag : 0u;
        k->transp = r->transp >= -12 && r->transp <= 12 ? r->transp : 0;
        k->arp = (r->arp & 15u) < ROTO_ARP_MODES ? (uint8_t)(r->arp & 15u) : 0u;          /* (older saves: the upper bits 0) */
        k->arp_accent = (uint8_t)((r->arp >> 4) & 3u) % 3u; k->arp_restart = (uint8_t)((r->arp >> 6) & 1u);
        k->arp_rate = (r->arp_rate & 7u) < ROTO_ARP_RATES ? (uint8_t)(r->arp_rate & 7u) : 1u;
        k->arp_notes = (uint8_t)(((r->arp_rate >> 3) & 7u) % ROTO_ARP_SHAPES);
        k->arp_range = (r->arp_range & 7u) >= 1u && (r->arp_range & 7u) <= 4u ? (uint8_t)(r->arp_range & 7u) : 1u;
        k->arp_rhythm = (uint8_t)(((r->arp_range >> 3) & 15u) % ROTO_ARP_RHYTHMS); k->arp_len = r->arp_len >= 10u && r->arp_len <= 100u ? r->arp_len : 50u;
        for (i = 0; i < ROTO_CHORD; i++) k->chord[i] = r->chord[i];
        for (i = 0; i < ROTO_STEPS; i++) { k->step[i] = r->step[i]; k->home[i] = r->step[i]; }
        k->fill_n = 0;
        if (k->pos != ROTO_POS_START && k->pos >= k->len) k->pos = 0;
    }
    for (t = 0; t < ROTO_PARTS; t++) roto.snd[t] = s->snd[t];
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

/* the kits: the STD kit, the drum-machine models (KIT 80 10 66 55 77), DEEP TIGHT PERC, and two with the kick
 * where the reference records have it (measured with tests/roto_render.c against Baby Ford, birds and Villalobos:
 * their kicks peak at 45..55 Hz, most of the energy under 60 Hz, a long tail):
 *   ROUND KIT  the STD kit (every GM piece), KICK ROUND, TUNE 61, DECY 88, TONE 82: the kick's weight at 45..60 Hz
 *   SUB KIT    the DEEP kit, TUNE 60, DECY 90, TONE 82: deeper still (the Weatherall grooves)
 * (TUNE and DECY move every piece a little: the hats come down a touch, out of the fizz over 8 kHz) */
#define NKIT 11u
static const char *const KIT_NAME[NKIT] = {"STD KIT", "80 KIT", "10 KIT", "66 KIT", "55 KIT", "77 KIT", "DEEP KIT", "TIGHT KIT", "PERC KIT",
                                           "ROUND KIT", "SUB KIT"};
static const uint8_t KIT_PRESET[NKIT] = {0, 0, 0, 0, 0, 0, 1, 2, 3, 0, 1};
static const int8_t KIT_VAL[NKIT] = {-1, 4, 5, 6, 7, 8, -1, -1, -1, -1, -1};
static const int8_t KIT_TWEAK[NKIT][4] = {   /* TUNE, DECY, KICK, TONE over the preset's (-1: as it is) */
    {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1},
    {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1},
    {61, 88, 1, 82}, {60, 90, -1, 82},
};
uint32_t roto_kit_count(void) { return NKIT; }
const char *roto_kit_name(uint32_t k) { return KIT_NAME[k % NKIT]; }
void roto_kit_sound(uint32_t k, uint8_t *preset, int32_t *kitval) { *preset = KIT_PRESET[k % NKIT]; *kitval = KIT_VAL[k % NKIT]; }
int32_t roto_kit_tweak(uint32_t k, uint32_t e)   /* the kit's own value for setting E(e+1), or -1 */
{
    const int8_t *t = KIT_TWEAK[k % NKIT];
    return e == 1u ? t[0] : e == 3u ? t[1] : e == 6u ? t[2] : e == 2u ? t[3] : -1;
}

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
