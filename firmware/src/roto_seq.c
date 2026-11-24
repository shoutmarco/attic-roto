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
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto.trk[t].len = 16;
        roto.trk[t].div = 1;
        roto.trk[t].gate = 50;
    }
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

/* a free step of k, chosen with weights by s % 4 (w[0..3]); len if none */
static uint32_t pick_step(const roto_track_t *k, const uint8_t w[4])
{
    uint32_t s, total = 0, r;
    for (s = 0; s < k->len; s++)
        if (!k->step[s].on) total += w[s % 4u];
    if (!total) return k->len;
    r = rnd(total);
    for (s = 0; s < k->len; s++)
        if (!k->step[s].on) {
            if (r < w[s % 4u]) return s;
            r -= w[s % 4u];
        }
    return k->len;
}

/* the sounds each role may get: engine, preset (engines.c ENGINES[] numbering), level.
 * Levels (P_LEVEL: 112 = 0 dB, 0.5 dB a step) from rendering each one in its role
 * (tests/roto_render.c): drums loudest, bass about 3 dB under, stabs and blips further back. */
static const uint8_t SND_DRUMS[3][3] = {{10, 0, 111}, {10, 1, 111}, {10, 2, 114}};            /* DRUM KIT, DEEP KIT, TIGHT KIT */
static const uint8_t SND_BASS[4][3]  = {{0, 7, 106}, {0, 4, 116}, {12, 2, 122}, {6, 0, 121}}; /* SUB BASS, ACID, FM BASS, FAT BASS */
static const uint8_t SND_STAB[4][3]  = {{12, 0, 104}, {2, 1, 104}, {7, 1, 110}, {0, 3, 105}}; /* TINE EP, ORGAN, JAZZ PERC, PWM STR */
static const uint8_t SND_BLIP[4][3]  = {{9, 4, 127}, {9, 1, 127}, {12, 1, 103}, {9, 0, 121}}; /* KALIMBA, MARIMBA, BELL, BELL TREE */

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

void roto_seq_generate(uint32_t seed)
{
    static const uint8_t ROOTS[6] = {2, 4, 9, 7, 0, 5};          /* D E A G C F */
    static const uint8_t W_BASS[4] = {1, 3, 4, 3};               /* off the kick */
    static const uint8_t W_ODD[4] = {0, 3, 0, 3};
    static const uint8_t W_ANY[4] = {3, 2, 3, 2};
    static const uint8_t L_BASS[4] = {16, 15, 13, 14};
    static const uint8_t L_STAB[5] = {7, 9, 10, 11, 12};
    static const uint8_t L_BLIP[4] = {5, 7, 11, 13};
    static const uint8_t PENTA[5] = {0, 3, 5, 7, 10};
    /* stab roots where a minor 7th chord stays in the mode (aeolian i iv v, dorian i ii v, phrygian i iv bvii) */
    static const uint8_t STAB_ROOT[3][3] = {{0, 5, 7}, {0, 2, 7}, {0, 5, 10}};
    const uint8_t *sc;
    roto_track_t *k;
    uint32_t i, s, n, pc, base;

    roto_seq_init();
    i = seed + 0x9E3779B9u;                                      /* (spread any seed: murmur3's finaliser) */
    i ^= i >> 16; i *= 0x85EBCA6Bu; i ^= i >> 13; i *= 0xC2B2AE35u; i ^= i >> 16;
    rs_state = i ? i : 1u;
    roto.seed = seed;

    pc = ROOTS[rnd(6u)];
    roto.root = (uint8_t)pc;
    roto.mode = (uint8_t)rnd(3u);
    sc = MODE_SC[roto.mode];
    roto.bpm = (uint8_t)(112u + rnd(13u));
    roto.dtime = 10;                                             /* params.c N_DTIME: 1/8D */
    roto.dfdbk = (uint8_t)(55u + rnd(15u));

    /* 1: drums, 16 steps. Kick on the beats (sometimes the last one pushed),
     * off-beat open hats, a stray rim or clap, a few closed hats. */
    k = &roto.trk[0];
    k->len = 16; k->div = 1; k->gate = 25; k->fill_note = 42;   /* (FILL adds closed hats) */
    for (s = 0; s < 16u; s += 4u) put(k, s, 36, s ? 104u + rnd(12u) : 118u);
    if (chance(30)) { k->step[12].on = 0; put(k, chance(50) ? 11u : 14u, 36, 100); }
    for (s = 2; s < 16u; s += 4u) if (!k->step[s].on && chance(65)) put(k, s, 46, 62u + rnd(28u));
    n = chance(30) ? 2u : 1u;
    for (i = 0; i < n; i++) {
        uint32_t note, vel;
        s = pick_step(k, W_ODD);
        note = chance(50) ? 37u : 39u;                           /* rim or clap */
        vel = 78u + rnd(20u);
        if (s < k->len) put(k, s, note, vel);
    }
    n = rnd(3u);
    for (i = 0; i < n; i++) {
        uint32_t vel;
        s = pick_step(k, W_ANY);
        vel = 44u + rnd(24u);
        if (s < k->len) put(k, s, 42, vel);
    }
    sound(&roto.snd[0], SND_DRUMS[rnd(3u)], 0, 14, 0, 0);

    /* 2: bass, a length that drifts against the drums, mostly the root. */
    k = &roto.trk[1];
    k->len = L_BASS[rnd(4u)]; k->div = 1; k->gate = 55;
    base = 36u + pc;
    if (pc >= 7u) base -= 12u;
    k->fill_note = (uint8_t)base;
    n = 5u + rnd(3u);
    for (i = 0; i < n; i++) {
        uint32_t r = rnd(100u), note;
        s = pick_step(k, W_BASS);
        if (s >= k->len) break;
        note = r < 50u ? base : r < 65u ? base + 12u : r < 80u ? base + sc[4] : r < 90u ? base + sc[6] : base + sc[2];
        put(k, s, note, 92u + rnd(24u));
    }
    sound(&roto.snd[1], SND_BASS[rnd(4u)], 0, 0, 0, 0);

    /* 3: dub stabs, minor 7th chords that fit the mode, few and far apart. */
    k = &roto.trk[2];
    k->len = L_STAB[rnd(5u)]; k->div = (uint8_t)(chance(40) ? 2u : 1u); k->gate = 30;
    k->chord[0] = 3; k->chord[1] = 7; k->chord[2] = 10;
    k->fill_note = (uint8_t)(48u + pc);
    n = 2u + rnd(2u);
    base = rnd(k->len);                                          /* spread round the ring, a little uneven */
    for (i = 0; i < n; i++) {
        s = (base + (k->len * i) / n + (i ? rnd(2u) : 0u)) % k->len;
        {
            uint32_t root = i ? STAB_ROOT[roto.mode][rnd(3u)] : 0u, vel = 70u + rnd(24u);
            if (!k->step[s].on) put(k, s, 48u + pc + root, vel);
        }
    }
    sound(&roto.snd[2], SND_STAB[rnd(4u)], 64, 40, 24, -22);

    /* 4: blips, minor pentatonic up high, sparse. */
    k = &roto.trk[3];
    k->len = L_BLIP[rnd(4u)]; k->div = (uint8_t)(chance(50) ? 2u : 1u); k->gate = 25;
    k->fill_note = (uint8_t)(60u + pc);
    n = 2u + rnd(2u);
    base = rnd(k->len);
    for (i = 0; i < n; i++) {
        s = (base + (k->len * i) / n + (i ? rnd(2u) : 0u)) % k->len;
        {
            uint32_t deg = PENTA[rnd(5u)], up = chance(25) ? 12u : 0u, vel = 96u + rnd(28u);
            if (!k->step[s].on) put(k, s, 60u + pc + deg + up, vel);
        }
    }
    sound(&roto.snd[3], SND_BLIP[rnd(4u)], 72, 46, 0, 26);

    roto.sel = 0;                                                /* (stopped: the caller sets the sounds, then PLAY) */
}

/* ------------------------------------------------------------- tick */
static void release(uint32_t t, roto_track_t *k)
{
    uint32_t i;
    for (i = 0; i < k->nheld; i++) roto_note_off(t, k->held[i]);
    k->nheld = 0;
    k->gate_left = 0;
}

static void fire(uint32_t t, roto_track_t *k, uint32_t step_len)
{
    const roto_step_t *st = &k->step[k->pos];
    uint32_t i, g;
    if (k->nheld) release(t, k);
    if (!st->on) return;
    k->held[k->nheld++] = st->note;
    roto_note_on(t, st->note, st->vel ? st->vel : 100u);
    for (i = 0; i < ROTO_CHORD; i++) {
        int32_t n = (int32_t)st->note + k->chord[i];
        if (!k->chord[i] || n < 0 || n > 127) continue;
        k->held[k->nheld++] = (uint8_t)n;
        roto_note_on(t, (uint8_t)n, st->vel ? st->vel : 100u);
    }
    g = (step_len * (k->gate ? k->gate : 50u)) / 100u;
    k->gate_left = g ? g : 1u;
}

void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate)
{
    uint32_t t, f, sixteenth;

    if (!roto.running) {
        for (t = 0; t < ROTO_TRACKS; t++)
            if (roto.trk[t].nheld) release(t, &roto.trk[t]);
        return;
    }
    sixteenth = (sample_rate * 15u) / (roto.bpm ? roto.bpm : 120u);

    for (f = 0; f < block_frames; f++) {
        for (t = 0; t < ROTO_TRACKS; t++) {
            roto_track_t *k = &roto.trk[t];
            uint32_t step_len = sixteenth * (k->div ? k->div : 1u);
            uint32_t len = k->len ? k->len : 1u;
            if (k->gate_left && --k->gate_left == 0u) release(t, k);
            if (k->pos == ROTO_POS_START || ++k->acc >= step_len) {
                k->acc = 0;
                k->pos = (uint8_t)(k->pos == ROTO_POS_START ? 0u : (k->pos + 1u) % len);
                fire(t, k, step_len);
            }
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
        st->note = 60;
        for (s = 0; s < ROTO_STEPS; s++)
            if (roto.trk[trk].step[s].on) { st->note = roto.trk[trk].step[s].note; break; }
        st->vel = 100;
    }
    st->on ^= 1u;
    st->fill = 0;                                   /* (a step set by hand belongs to the pattern) */
}

void roto_len_set(uint32_t trk, uint32_t len)
{
    if (trk >= ROTO_TRACKS) return;
    if (len < 1) len = 1;
    if (len > ROTO_STEPS) len = ROTO_STEPS;
    roto.trk[trk].len = (uint8_t)len;
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

void roto_fill(uint32_t trk, int32_t dir)
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
        if (!n || roto.snd[trk].eng == 10u)          /* (engines.c: 10 = DRUM) */
            st->note = k->fill_note ? k->fill_note : 60u;
        else
            st->note = k->step[best].note;
        st->vel = n && roto.snd[trk].eng != 10u ? k->step[best].vel : 64u;
        if (k->fill_seq < 255u) k->fill_seq++;
        st->fill = k->fill_seq;
        st->on = 1;
    } else {
        uint32_t newest = 0;
        if (!n) return;
        for (s = 0; s < k->len; s++)                 /* what FILL added goes first, newest first */
            if (k->step[s].on && k->step[s].fill > newest) { newest = k->step[s].fill; best = s; }
        if (newest) {
            k->step[best].on = 0;
            k->step[best].fill = 0;
            if (newest == k->fill_seq) k->fill_seq--;
            return;
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

/* ROTATE: every step of the ring moves one step later (dir > 0) or earlier */
void roto_rotate(uint32_t trk, int32_t dir)
{
    roto_track_t *k;
    roto_step_t keep;
    uint32_t s, len;
    if (trk >= ROTO_TRACKS || !dir) return;
    k = &roto.trk[trk];
    len = k->len;
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
}

/* ------------------------------------------------------------- transport */
void roto_play(void)
{
    uint32_t t;
    roto.running = 0;                               /* (the tick releases anything still held) */
    for (t = 0; t < ROTO_TRACKS; t++) {
        roto.trk[t].acc = 0;
        roto.trk[t].pos = ROTO_POS_START;           /* step 1 plays at once */
    }
    roto.running = 1;
}

void roto_stop(void)
{
    roto.running = 0;
}
