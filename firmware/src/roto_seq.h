/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto: ring sequencer for four independent tracks.
 * Each track is a circular sequence of 1..64 steps. Each on-step plays a note
 * (or a chord) on its track for a set gate length. The four tracks share one
 * transport (BPM, running) but each loops on its own length (polymeter).
 *
 * At power-on roto_seq_generate() makes a new random pattern in the style of
 * Andrew Weatherall (drums, a drifting bassline, dub chord stabs, blips) and
 * chooses a sound for each track (roto.snd[], applied by main.c). */
#ifndef ROTO_SEQ_H
#define ROTO_SEQ_H
#include <stdint.h>

#define ROTO_VERSION "v0.2"   /* attic-roto's own version (the splash, MENU > ABOUT) */

#define ROTO_TRACKS  6    /* rings */
#define ROTO_PARTS   4    /* the synth's sounds: rings 1-4 play their own, ring 5 the drums, ring 6 the blips */
#define ROTO_STEPS  32    /* max steps per ring (32: memory; the patterns use up to 16) */
#define ROTO_CHORD   3    /* extra chord notes per step */
#define ROTO_POS_START 0xFFu   /* pos before the first step after PLAY */

typedef struct {    /* 3 bytes */
    uint8_t note : 7;   /* MIDI note 0..127 */
    uint8_t on : 1;
    uint8_t vel;        /* 1..127 */
    uint8_t chance;     /* 0: always plays; 1..99: a ghost note, plays that percent of the time */
} roto_step_t;

typedef struct {
    roto_step_t step[ROTO_STEPS];
    uint8_t  len;        /* active steps: 1..ROTO_STEPS */
    uint8_t  pos;        /* playhead 0..len-1 (ROTO_POS_START before the first step) */
    uint8_t  div;        /* sixteenths per step: 1 = 1/16, 2 = 1/8, 4 = 1/4 */
    uint8_t  gate;       /* note length, percent of a step (1..100) */
    int8_t   chord[ROTO_CHORD];   /* semitones above the step's note, 0 = none */
    uint8_t  nheld;      /* audio side: notes sounding now */
    uint8_t  held[1 + ROTO_CHORD];
    uint8_t  sub;        /* audio side: sixteenths into the current step (0..div-1) */
    uint32_t gate_left;  /* samples until the held notes are released */
    uint8_t  fill_note;  /* the note FILL adds when the ring has no hits to copy */
    uint8_t  part;       /* the sound (synth part 0..3) this ring plays */
    uint8_t  fill_n;     /* the hits FILL added, newest last (taken away first, newest first) */
    uint8_t  fill_at[8];
    uint8_t  rot;        /* how far ROTATE has turned the pattern (0..len-1, for display) */
    uint8_t  prob;       /* PROB: the chance each hit plays, percent (100 = always) */
    roto_step_t home[ROTO_STEPS];   /* the pattern as set (generator or your edits): EVOLVE drifts away and back to it */
} roto_track_t;

/* the sound a track gets at power-on (applied by main.c roto_boot_sounds) */
typedef struct {
    uint8_t eng, preset;
    uint8_t level, dly, rev, chor;
    int8_t  pan;
    char    name[12];    /* the preset's name, filled in when applied */
} roto_sound_t;

typedef struct {
    roto_track_t trk[ROTO_TRACKS];
    roto_sound_t snd[ROTO_PARTS];
    uint8_t  running;
    uint8_t  bpm;        /* 40..240 */
    uint8_t  sel;        /* selected track 0..3 */
    uint8_t  root;       /* pitch class 0..11 of the generated key */
    uint8_t  mode;       /* 0 aeolian, 1 dorian, 2 phrygian */
    uint8_t  dtime, dfdbk;   /* the delay (song.g G_DTIME / G_DFDBK): what dub throws come back to; DELAY edits it */
    uint8_t  dcolor;     /* the delay's colour (song.g G_DCOLOR), for the screen */
    uint32_t seed;
    uint8_t  view;       /* the screen: ROTO_VIEW_RING / ROTO_VIEW_EUCLID (UI side) */
    uint8_t  swing;      /* 50 = straight; 50..75: the off 1/16s later */
    uint8_t  evolve;     /* 0..100: how often the pattern changes by itself (and dub throws happen) */
    /* audio side */
    uint32_t acc16;      /* samples into the current 1/16 */
    uint32_t n16;        /* 1/16s since PLAY */
    uint8_t  throw_trk;  /* the sound (part) in a dub throw (0xFF: none) */
    uint8_t  throw_left; /* 1/16s until it ends */
    uint32_t ev_rs;      /* EVOLVE's dice */
    uint8_t  env[ROTO_PARTS][4];   /* each envelope, A D S R 0..127 (a copy of the track's P_ATK..P_REL, for the screen) */
    int8_t   flt[ROTO_PARTS][4];   /* FILTER, for the screen, K1..K4: bright, edge 0..100, env -100..100, drive 0..100 (-128: the sound has none) */
    char     flt_lbl[ROTO_PARTS][4][6];   /* the sound's own names for them ("CUT", "RES", ...) */
    /* ARP: a chord from the pattern's key, played one note at a time on a ring's sound.
     * UI side sets these (then roto_arp_build); it keeps playing when you leave the screen. */
    uint8_t  arp_chord;  /* 0: off; 1..7: the key's seventh chords (i, iv, v, VI, III, VII, ii) */
    uint8_t  arp_mode;   /* ROTO_ARP_UP .. ROTO_ARP_PEDAL */
    uint8_t  arp_speed;  /* 0 1/32, 1 1/16, 2 1/8, 3 1/4 */
    uint8_t  arp_range;  /* octaves 1..4 */
    uint8_t  arp_gate;   /* note length, percent of a step (5..100) */
    uint8_t  arp_part;   /* the sound it plays (the ring's, when the screen opened) */
    uint8_t  arp_len;    /* notes in arp_seq (0: nothing to play) */
    uint8_t  arp_seq[16];   /* the chord over the range, low to high */
    /* audio side */
    uint8_t  arp_cur;    /* index into arp_seq of the note playing (0xFF: none yet), for the screen */
    uint8_t  arp_note;   /* the note sounding (0xFF: none) ... */
    uint8_t  arp_npart;  /* ... on this sound */
    uint8_t  arp_dir;    /* UP-DOWN: 0 going up, 1 down */
    uint8_t  arp_k;      /* PEDAL: which upper note comes next */
    uint32_t arp_n32;    /* 1/32s since PLAY */
    uint32_t arp_gate_left;
} roto_seq_t;

#define ROTO_ARP_UP     0
#define ROTO_ARP_DOWN   1
#define ROTO_ARP_UPDOWN 2
#define ROTO_ARP_RANDOM 3
#define ROTO_ARP_PEDAL  4   /* the lowest note between each of the others */
#define ROTO_ARP_MODES  5

#define ROTO_VIEW_RING   0
#define ROTO_VIEW_EUCLID 1
#define ROTO_VIEW_ENV    2
#define ROTO_VIEW_FLT    3
#define ROTO_VIEW_DLY    4
#define ROTO_VIEW_ARP    5

/* what a knob just changed, for roto_ui_show() (the centre shows it for a moment) */
enum { ROTO_SHOW_NONE, ROTO_SHOW_LENGTH, ROTO_SHOW_FILL, ROTO_SHOW_ROTATE, ROTO_SHOW_RATE, ROTO_SHOW_PROB, ROTO_SHOW_BPM,
       ROTO_SHOW_ATK, ROTO_SHOW_DEC, ROTO_SHOW_SUS, ROTO_SHOW_REL,
       ROTO_SHOW_BRIGHT, ROTO_SHOW_EDGE, ROTO_SHOW_FENV, ROTO_SHOW_DRIVE,
       ROTO_SHOW_DTIME, ROTO_SHOW_DFDBK, ROTO_SHOW_DSEND, ROTO_SHOW_DCOLOR,
       ROTO_SHOW_HOLD, ROTO_SHOW_NEW,
       ROTO_SHOW_ACHORD, ROTO_SHOW_AMODE, ROTO_SHOW_ASPEED, ROTO_SHOW_ARANGE, ROTO_SHOW_ALEN };

extern roto_seq_t roto;

/* boot: everything empty and stopped */
void roto_seq_init(void);
/* a new random Weatherall-style pattern and sound plan from seed (stopped: set the sounds, then roto_play) */
void roto_seq_generate(uint32_t seed);
/* NEW: everything empty and plain — rings of 16 at 1/16, DRUM KIT / SUB BASS / TINE EP / SINE KEY,
 * 120 BPM, straight, EVOLVE off, delay 1/8D (stopped: set the sounds with roto_apply_sounds) */
void roto_seq_blank(void);
/* (main.c) give the tracks roto.snd[] and the song roto's tempo and delay */
void roto_apply_sounds(void);

/* audio side, once per sample block */
void roto_seq_tick(uint32_t block_frames, uint32_t sample_rate);

/* editing (UI side) */
void roto_step_toggle(uint32_t trk, uint32_t step);
void roto_len_set(uint32_t trk, uint32_t len);
void roto_fill(uint32_t trk, int32_t dir);     /* one hit more (dir > 0) or fewer, spread evenly, notes kept */
void roto_rotate(uint32_t trk, int32_t dir);   /* spin the pattern one step later (dir > 0) or earlier */
void roto_euclid(uint32_t trk, uint32_t hits); /* exactly `hits` hits, spread evenly (Euclidean), turned by rot; notes kept */

/* FILTER: for a sound engine (engines.c numbering), which of its 8 edit settings is BRIGHT, EDGE
 * and DRIVE (0xFF: none), and whether it follows the envelope-to-filter amount (P_ED_FLT) */
void roto_tone_map(uint32_t eng, uint8_t out[3], int *has_env);

/* DELAY TIME (params.c N_DTIME numbering): its name, its length in 1/96 of a bar, and the next one
 * longer (dir > 0) or shorter, so K1 runs through them in order of length */
const char *roto_dtime_name(uint32_t t);
uint32_t roto_dtime_96(uint32_t t);
uint32_t roto_dtime_step(uint32_t t, int32_t dir);

/* ARP (UI side): arp_seq from the chord, the key and the range; and the chord's name ("A MIN7") */
void roto_arp_build(void);
void roto_arp_name(char *out);

/* transport (UI side; the audio side releases notes after a stop) */
void roto_play(void);
void roto_stop(void);

/* the voice side (seq.c) */
void roto_note_on(uint32_t trk, uint8_t note, uint8_t vel);
void roto_note_off(uint32_t trk, uint8_t note);
void roto_fx_send(uint32_t trk, uint8_t dly);   /* a dub throw: the ring's delay send */
void roto_fx_feedback(uint8_t fb);              /* ... and the delay feedback */

/* UI hooks (roto_ui.c), called from ui_input.c / ui_draw.c */
int  roto_ui_input(uint32_t pressed, uint32_t notes);
int  roto_ui_draw(int force, uint32_t now_ms);   /* returns 1 if the screen is ours (caller skips Felucca drawing); force: redraw everything */
void roto_ui_show(uint32_t what, uint32_t now_ms);   /* a knob turned: show its value in the centre for a moment */
void roto_ui_audio_level(uint8_t level);
void roto_ui_splash(void);                       /* the power-on screen */

#endif /* ROTO_SEQ_H */
