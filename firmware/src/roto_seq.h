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
#define ROTO_GROOVES 4u      /* PRESETS dials through Marco's four reference grooves (ref_grooves.h): 1 BABY FORD, 2 BIRDS, 3 ORIN,
                                4 FACTORY. (roto_seq_generate's other seeds still make the old generated grooves.) */
#define ROTO_STEPS  32    /* max steps per ring (32: memory; the patterns use up to 16) */
#define ROTO_CHORD   3    /* extra chord notes per step */
#define ROTO_POS_START 0xFFu   /* pos before the first step after PLAY */

typedef struct {    /* 3 bytes */
    uint8_t note : 7;   /* MIDI note 0..127 */
    uint8_t on : 1;
    uint8_t vel : 7;    /* 1..127 */
    uint8_t len_lo : 1; /* LENGTH (roto_step_len): its low bit ... */
    uint8_t chance : 7; /* 0: always plays; 1..99: a ghost note, plays that percent of the time */
    uint8_t len_hi : 1; /* ... and its high bit: 0 the ring's own, 1 SHORT, 2 LONG, 3 FULL (older saves: 0) */
} roto_step_t;
typedef char roto_step_is_3_bytes[sizeof(roto_step_t) == 3u ? 1 : -1];

/* the white keys: a tap adds or takes away a hit; held on the ring screen, the knobs edit that step (K1 NOTE in
 * the key, K2 ACCENT, K3 LENGTH, K4 OCTAVE). ROTO_SIMPLE 1: one screen only (K1 SOUND, K2 TONE, K3 SPACE,
 * K4 SWING; SEQ held + K1 the length; the other screens off). Marco: all the screens, full sound editing (0) */
#define ROTO_HOLD_EDIT 1
/* the second core renders the sound (Felucca 1.6.1's, built in, OFF by default there): ON at every power-on in
 * attic-roto (its MENU is not reachable here). Felucca's own guards stay: a crash in the first 30 s of the last boot,
 * or core 1 misreading / not answering, and it is one core again (main.c). 0: one core, as before */
#define ROTO_CPU2_DEFAULT 1
#define ROTO_SIMPLE 0

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
    uint8_t  lag;        /* ms each hit plays behind the beat (0: on it): the lazy feel */
    int8_t   transp;     /* GLO PITCH: semitones on this ring's notes (not on a drum kit's: its pieces would change) */
    uint8_t  arp;        /* ARP: 0 off, ROTO_ARP_UP .. ROTO_ARP_CHORD */
    uint8_t  arp_rate;   /* 0 1/32, 1 1/16, 2 1/8, 3 1/16T, 4 1/8T, 5 1/8D, 6 1/4 */
    uint8_t  arp_range;  /* octaves 1..4; a drum ring: ROLL FADE 1 flat, 2..4 fading, 5..7 swelling */
    uint8_t  arp_len;    /* each arp note's length, percent of the rate (10..100) */
    uint8_t  arp_notes;  /* NOTES: 0 the key's 7th (or the ring's own chord), 1 triad, 2 9th, 3 sus, 4 octaves, 5 1-5-8;
                          * a drum ring: ROLL PITCH 0 off, 1 2 up, 3 4 5 down (semitones a repeat) */
    uint8_t  arp_rhythm; /* RHYTHM: ROTO_ARP_RHYTHMS patterns of 16 (0: every step) */
    uint8_t  arp_accent; /* ACCENT: 0 the beat, 1 flat, 2 the off-beat */
    uint8_t  arp_restart;/* HOLD: 0 carries on across hits, 1 starts again on each */
    uint8_t  a_ch, a_chn[4], a_sub, a_fresh;   /* audio side: CHORD's notes sounding; ticks left in this cycle; start */
    uint8_t  a_roll;     /* audio side, a drum ring's arp: repeats since the hit (ROLL FADE, ROLL PITCH) */
    uint8_t  dly_bus;    /* FX: the ring's delay, 0 none, 1 A, 2 B */
    uint8_t  dly_send;   /* FX: how much of the ring goes into it (0..127) */
    uint8_t  choke;      /* audio side, a drum ring: the hit sounding has a LENGTH of its own (cut at its end) */
    uint16_t a_pos;      /* arp steps played (RHYTHM's place) */
    uint32_t a_last16;   /* the 1/16 (+1) the arp last stepped on */
    /* audio side: the arp of the last hit */
    uint8_t  a_n, a_idx, a_dir, a_k, a_on, a_vel;
    uint8_t  a_notes[16];
    uint32_t a_left, a_int, a_gate_left;
    uint8_t  lag_step;   /* audio side: the step waiting out its lag ... */
    uint32_t lag_left;   /* ... samples to go (0: none) */
    uint32_t lag_len;    /* ... and its length */
    roto_step_t home[ROTO_STEPS];   /* the pattern as set (generator or your edits): EVOLVE drifts away and back to it */
} roto_track_t;

/* the sound a track gets at power-on (applied by main.c roto_boot_sounds) */
typedef struct {
    uint8_t eng, preset;
    uint8_t level, dly, rev, chor;
    int8_t  pan;
    int8_t  drift;       /* the slow filter movement: the LFO (a slow sine, about ten bars) to the filter, -64..63 (0: none) */
    uint8_t kit;         /* DRUM: ROTO_KITS index (the kit's KIT setting over its preset) */
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
    uint8_t  btime, bfdbk, bcolor;   /* delay B: TIME, FEEDBACK, COLOUR (delay A: dtime, dfdbk, dcolor) */
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
    uint8_t  spage;      /* SOUND screen: 0 choose, 1 shape 1-4, 2 shape 5-8 */
    uint8_t  apage;      /* ARP screen: 0 MODE RATE RANGE LENGTH, 1 NOTES RHYTHM ACCENT HOLD */
    int8_t   ed[8];      /* SHAPE: the sound's eight settings 0..100 (-128: none), for the screen (ui_input.c fills it) */
    char     ed_lbl[8][6];   /* their names */
    char     ed_txt[8][7];   /* their values as text ("64%", "SAW") */
    uint8_t  mute;       /* bit r: ring r muted (OP1..OP6, the first six black keys) */
    uint8_t  kpage;      /* the step keys show steps 1-16 (0) or 17-32 (1): OCT- / OCT+ */
    uint8_t  saves_used; /* SAVE: bit n, save n holds a groove (main.c reads them at power-on) */
    char     save_name[8][9];   /* their names */
    uint8_t  save_cur;   /* the save last loaded or kept (0xFF: none) */
    uint8_t  naming;     /* SAVE pressed: the name is being set (K1 moves, K2 the letter; SAVE keeps, HOME cancels) */
    uint8_t  name_pos;   /* the letter K1 is on */
    uint8_t  name_slot;  /* K3: 0 a new save (or over the oldest), 1..ROTO_SAVES over that save */
    char     name_buf[9];
    uint16_t gnum;       /* PRESETS: the groove number being dialled (shown while turning; 0: none) */
    uint8_t  grand;      /* GROOVE RANDOM: 0..100, each hit a little softer or later at random (up to -40 % velocity, +15 ms) */
    uint8_t  gprob;      /* GROOVE PROB: 0..100, the chance every hit plays (with each ring's own PROB) */
    uint8_t  dub;        /* GROOVE: 0..100, how often a sound is thrown into the echo (a bar or so) */
} roto_seq_t;

#define ROTO_VIEW_RING   0
#define ROTO_VIEW_EUCLID 1
#define ROTO_VIEW_ENV    2
#define ROTO_VIEW_FLT    3
#define ROTO_VIEW_DLY    4
#define ROTO_VIEW_GRV    5   /* GROOVE (SCL): swing, random, prob, evolve; the needle the tempo */
#define ROTO_VIEW_ARP    7   /* ARP: the selected ring's arp */
#define ROTO_VIEW_GLO    8   /* GLO: the selected ring's sound, live: pitch, filter, echo, length */
#define ROTO_ARP_UP     1
#define ROTO_ARP_DOWN   2
#define ROTO_ARP_UPDOWN 3
#define ROTO_ARP_RANDOM 4
#define ROTO_ARP_PEDAL  5   /* the lowest note between each of the others */
#define ROTO_ARP_DOWNUP 6
#define ROTO_ARP_CONVERGE 7 /* outside in: lowest, highest, next lowest ... */
#define ROTO_ARP_CHORD  8   /* the notes together, again and again */
#define ROTO_ARP_MODES  9   /* (with OFF) */
#define ROTO_ARP_RATES  7
#define ROTO_ARP_SHAPES 6
#define ROTO_ARP_RHYTHMS 10
const char *roto_arp_rhythm_name(uint32_t r);
#define ROTO_VIEW_SND    6

/* what a knob just changed, for roto_ui_show() (the centre shows it for a moment) */
enum { ROTO_SHOW_NONE, ROTO_SHOW_LENGTH, ROTO_SHOW_FILL, ROTO_SHOW_ROTATE, ROTO_SHOW_RATE, ROTO_SHOW_PROB, ROTO_SHOW_BPM,
       ROTO_SHOW_ATK, ROTO_SHOW_DEC, ROTO_SHOW_SUS, ROTO_SHOW_REL,
       ROTO_SHOW_BRIGHT, ROTO_SHOW_EDGE, ROTO_SHOW_FENV, ROTO_SHOW_DRIVE,
       ROTO_SHOW_DTIME, ROTO_SHOW_DFDBK, ROTO_SHOW_DSEND, ROTO_SHOW_DCOLOR,
       ROTO_SHOW_HOLD, ROTO_SHOW_NEW,
       ROTO_SHOW_G1, ROTO_SHOW_G2, ROTO_SHOW_G3, ROTO_SHOW_G4,   /* GROOVE: K1..K4 */
       ROTO_SHOW_GNUM,   /* PRESETS: a groove number (roto.gnum), or a save's name (roto.gnum >= 10000: save gnum - 10000) */
       ROTO_SHOW_SAVED, ROTO_SHOW_NOTSAVED,
       ROTO_SHOW_A1, ROTO_SHOW_A2, ROTO_SHOW_A3, ROTO_SHOW_A4,   /* ARP: K1..K4 */
       ROTO_SHOW_O1, ROTO_SHOW_O2, ROTO_SHOW_O3, ROTO_SHOW_O4,   /* GLO: K1..K4 */
       ROTO_SHOW_S1, ROTO_SHOW_S2, ROTO_SHOW_S3, ROTO_SHOW_S4,   /* SOUND: K1..K4 (the page says which) */
       ROTO_SHOW_TEXT };  /* roto_ui_say's words */

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
void roto_step_add(uint32_t trk, uint32_t step);   /* switch a step on with a note that carries the line on, an accent by where it falls */
void roto_step_edit(uint32_t trk, uint32_t step, int32_t notes_in_key, int32_t accent, int32_t length, int32_t octaves);
uint32_t roto_step_len(const roto_step_t *st);   /* 0 the ring's own, 1 SHORT, 2 LONG, 3 FULL */
void roto_len_set(uint32_t trk, uint32_t len);
void roto_fill(uint32_t trk, int32_t dir);     /* one hit more (dir > 0) or fewer, spread evenly, notes kept */
void roto_rotate(uint32_t trk, int32_t dir);   /* spin the pattern one step later (dir > 0) or earlier */
void roto_euclid(uint32_t trk, uint32_t hits); /* exactly `hits` hits, spread evenly (Euclidean), turned by rot; notes kept */

/* FILTER: for a sound engine (engines.c numbering), which of its 8 edit settings is BRIGHT, EDGE
 * and DRIVE (0xFF: none), and whether it follows the envelope-to-filter amount (P_ED_FLT) */
void roto_tone_map(uint32_t eng, uint8_t out[3], int *has_env);
uint32_t roto_tone_k3(uint32_t eng);   /* FILTER K3 where the sound has no filter envelope: its setting, or 0xFF */

/* DELAY TIME (params.c N_DTIME numbering): its name, its length in 1/96 of a bar, and the next one
 * longer (dir > 0) or shorter, so K1 runs through them in order of length */
const char *roto_dtime_name(uint32_t t);
uint32_t roto_dtime_96(uint32_t t);
uint32_t roto_dtime_step(uint32_t t, int32_t dir);
void roto_delay_from_sounds(void);   /* every ring into delay A at its sound's send, B as A (a groove, an older save) */
void roto_delay_apply(void);         /* (UI side, every pass) the rings' delays to the audio side */

/* SOUND: the sounds by kind. Pitched: categories of (engine, preset); drums: kits and pieces. */
#define ROTO_ENG_DRUM 10u
uint32_t roto_cat_count(void);
const char *roto_cat_name(uint32_t c);
uint32_t roto_cat_size(uint32_t c);
void roto_cat_sound(uint32_t c, uint32_t i, uint8_t *eng, uint8_t *preset);
void roto_cat_find(uint32_t eng, uint32_t preset, uint32_t *c, uint32_t *i);   /* (not listed: 0, 0) */
uint32_t roto_kit_count(void);
int32_t roto_kit_tweak(uint32_t k, uint32_t e);   /* a kit's own value for its setting E(e+1) over the preset's, or -1 */
const char *roto_kit_name(uint32_t k);
void roto_kit_sound(uint32_t k, uint8_t *preset, int32_t *kitval);   /* kitval: the KIT setting (-1: the preset's own) */
uint32_t roto_piece_count(void);
const char *roto_piece_name(uint32_t p);
uint32_t roto_piece_of(uint32_t ring);       /* the ring's main piece (its first hit's), index */
void roto_piece_set(uint32_t ring, uint32_t p);   /* the ring's main piece becomes p (its hits, FILL's note, home) */
/* SAVE: a groove kept by name: the rings, tempo, key, feel, and the four sounds as chosen and shaped */
#define ROTO_SAVES 8u
#define ROTO_SAVE_MAGIC 0x33534F52u   /* "ROS3" (with the arp and pitch) */
#define ROTO_NAME_CHARS " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"
typedef struct {
    roto_step_t step[ROTO_STEPS];
    uint8_t len, div, gate, part, prob, fill_note, rot, lag;
    int8_t chord[ROTO_CHORD];
    int8_t transp;
    uint8_t arp, arp_rate, arp_range, arp_len;
} roto_save_ring_t;
typedef struct {
    uint32_t magic, order;       /* order: higher = saved later (power-on brings back the latest) */
    uint16_t gnum;
    char name[10];
    uint8_t bpm, swing, root, mode, dtime, dfdbk, dcolor, evolve, dub, rsv[3];
    roto_save_ring_t ring[ROTO_TRACKS];
    roto_sound_t snd[ROTO_PARTS];
    int16_t shape[ROTO_PARTS][13];   /* each sound as shaped: ATK DEC SUS REL, ENV>FILTER, its eight own settings (main.c) */
} roto_save_t;
void roto_save_take(roto_save_t *s);          /* roto -> s (not the shape: main.c) */
void roto_save_give(const roto_save_t *s);    /* s -> roto (not the sounds: main.c) */
int roto_saves_read(void);                    /* (main.c) the saves' names -> roto.saves_used, save_name; the latest, or -1 */
int roto_save_keep(const char *name);         /* (main.c) a new save (or over the oldest when all 8 are used); 0: saved */
int roto_save_load(uint32_t n);               /* (main.c) 0: loaded */

/* (main.c) give part t the sound in roto.snd[t] */
void roto_apply_part(uint32_t t);

/* transport (UI side; the audio side releases notes after a stop) */
void roto_play(void);
void roto_stop(void);

/* the voice side (seq.c) */
void roto_note_on(uint32_t trk, uint8_t note, uint8_t vel);
void roto_note_off(uint32_t trk, uint8_t note);
void roto_drum_note_on(uint32_t trk, uint8_t note, uint8_t vel, int32_t st);   /* (a drum, st semitones off) */
void roto_drum_cut(uint32_t trk, uint8_t note);   /* (a drum hit's own LENGTH is over) */
void roto_fx_send(uint32_t trk, uint8_t dly);   /* a dub throw: the ring's delay send */
void roto_fx_feedback(uint8_t fb);              /* ... and the delay feedback */
void roto_fx_route(uint32_t trk, uint32_t bus);  /* a part's delay: 0 A, 1 B, 2 none */
void roto_fx_delay_b(uint8_t time, uint8_t fb, uint8_t col);   /* delay B's TIME, FEEDBACK, COLOUR */
void roto_fx_drum_lanes(uint32_t trk, int on, const uint8_t *ls, uint8_t lb);   /* a drum part's lanes' own sends */
uint32_t roto_drum_lane(uint32_t note);         /* the drum lane a note strikes (0..7) */

/* UI hooks (roto_ui.c), called from ui_input.c / ui_draw.c */
int  roto_ui_input(uint32_t pressed, uint32_t notes, uint32_t held);   /* notes: keys just pressed; held: keys down now */
int  roto_ui_draw(int force, uint32_t now_ms);   /* returns 1 if the screen is ours (caller skips Felucca drawing); force: redraw everything */
void roto_ui_show(uint32_t what, uint32_t now_ms);   /* a knob turned: show its value in the centre for a moment */
void roto_ui_audio_level(uint8_t level);
void roto_ui_say(const char *big, const char *small, uint32_t now_ms);   /* the centre: these words for a moment */
int roto_ui_held_step(void);                  /* the step whose white key is held (the selected ring), or -1 */
void roto_ui_hold_used(void);                 /* the held step was edited: letting go keeps it */
void roto_ui_say_step(uint32_t now_ms);       /* the centre: the held step's note, accent, length */
void roto_ui_splash(void);                       /* the power-on screen */
int roto_ui_splash_frame(int32_t prev, int32_t t);   /* the logo raining in, t ms on (prev: the last frame's t, or -1); 1: done */
uint32_t roto_key_leds(void);                    /* the keys lit: the ring's steps, the playhead's key flipped (bit k: key k of 27) */

#endif /* ROTO_SEQ_H */
