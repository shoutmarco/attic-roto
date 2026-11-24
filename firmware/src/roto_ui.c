/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto UI (2026-11-24).
 *
 * Two screens:
 *   ROTO_SCR_HOME  — rotating wireframe icosahedron, audio-reactive (home / visualiser)
 *   ROTO_SCR_RING  — full-screen ring sequencer for the selected track
 *
 * Navigation:
 *   EN_SELECT turned   → cycle selected track 0..3
 *   B_SEQ pressed      → HOME→RING; RING→toggle editor pass-through
 *   B_HOME pressed     → return to HOME screen
 *   B_PLAY pressed     → start / stop transport
 *   white keys 0..15   → toggle steps on the active ring
 *
 * Colours (RGB565):
 *   Track 0  white       0xFFFF
 *   Track 1  light blue  RGB565(128,200,255)
 *   Track 2  pale amber  RGB565(255,168,0)
 *   Track 3  soft coral  RGB565(240,64,64)
 *   Dimmed: each colour at ~40% brightness for inactive tracks.
 *
 * Uses only Felucca primitives already present in gfx.c / lcd.c:
 *   lcd_fill(x,y,w,h,col), draw_line in gfx.c, cv_text, AF_S.
 * No new drawing dependencies introduced. */

#include "roto_seq.h"   /* roto_seq_t, roto, ROTO_TRACKS, ROTO_STEPS */
/* core.h and stdint.h are already in scope via the unity build in felucca.c */

/* ---------------------------------------------------------------- palette */
#define RGB565(r,g,b) ((uint16_t)(((r)&0xF8u)<<8u|(((g)&0xFCu)<<3u)|((b)>>3u)))

static const uint16_t TRK_COL[ROTO_TRACKS] = {
    RGB565(255, 255, 255),   /* 0 white  */
    RGB565(128, 200, 255),   /* 1 blue   */
    RGB565(255, 168,   0),   /* 2 amber  */
    RGB565(240,  64,  64),   /* 3 coral  */
};
/* dimmed versions (~40%) for inactive tracks */
static const uint16_t TRK_DIM[ROTO_TRACKS] = {
    RGB565(100, 100, 100),
    RGB565( 51,  80, 102),
    RGB565(102,  67,   0),
    RGB565( 96,  26,  26),
};

#define COL_BG    RGB565(  0,   0,   0)
#define COL_WHITE RGB565(255, 255, 255)

/* ---------------------------------------------------------------- screen state */
enum { ROTO_SCR_HOME = 0, ROTO_SCR_RING };
static uint8_t roto_screen    = ROTO_SCR_HOME;
static uint8_t roto_in_editor = 0;   /* 1 = pass input to Felucca's voice editor */

/* ---------------------------------------------------------------- geometry */
/* FM-1 LCD is 240 × 240 px (square).  CV_MAX = 240x124, so we draw
 * content in the top 124 rows and lcd_fill covers the rest. */
#define SCR_W 240
#define SCR_H 240
#define CX    (SCR_W / 2)   /* 120 */
#define CY    (SCR_H / 2)   /* 120 */

/* Ring display radius for the full-screen ring view */
#define RING_R_FULL 90

/* -------------------------------------------------------- integer maths */
/* Fixed-point sine / cosine: enough for dot placement on a circle.
 * Returns value * 256.  angle in 0..255 (full circle). */
static int32_t isin(int32_t a)
{
    static const int8_t T[65] = {
        0,  6, 13, 19, 25, 31, 37, 44, 50, 56, 62, 68, 74, 80, 86, 92,
       97,103,109,114,120,125,130,135,141,146,151,155,160,165,169,174,
      178,182,186,190,194,198,202,205,209,212,215,218,221,224,226,229,
      231,234,236,238,240,241,243,244,245,246,247,248,249,249,250,250,
      250
    };
    int32_t q = (a >> 6) & 3, i = a & 63;
    int32_t v = (q == 1 || q == 3) ? T[63 - i] : T[i];
    return (q >= 2) ? -v : v;
}
static int32_t icos(int32_t a) { return isin((a + 64) & 255); }

/* dot position on a ring */
static void ring_dot(int32_t r, uint32_t step, uint32_t nsteps, int32_t *ox, int32_t *oy)
{
    /* step 0 at top (angle offset 192 = top in 0..255 space) */
    int32_t a = (int32_t)((step * 256u) / nsteps + 192u) & 255;
    *ox = CX + (icos(a) * r) / 256;
    *oy = CY + (isin(a) * r) / 256;
}

/* ---------------------------------------------------------- draw primitives */
/* All drawing goes through cv_px[] (gfx.c's framebuffer) so we share the
 * same blit path as Felucca. cv_begin / cv_blit are called in roto_ui_draw(). */
static void roto_pixel(int32_t x, int32_t y, uint16_t c)
{
    cv_pset(x, y, c);   /* bounds-checked inside cv_pset */
}

/* Bresenham line */
static void roto_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        roto_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* filled 3×3 dot */
static void roto_dot(int32_t x, int32_t y, uint16_t c)
{
    int32_t i, j;
    for (j = -2; j <= 2; j++)
        for (i = -2; i <= 2; i++)
            if (i*i + j*j <= 5)
                roto_pixel(x + i, y + j, c);
}

/* thin ring (hollow circle, midpoint algorithm) */
static void roto_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x = r, y = 0, err = 0;
    while (x >= y) {
        roto_pixel(cx+x, cy+y, c); roto_pixel(cx+y, cy+x, c);
        roto_pixel(cx-y, cy+x, c); roto_pixel(cx-x, cy+y, c);
        roto_pixel(cx-x, cy-y, c); roto_pixel(cx-y, cy-x, c);
        roto_pixel(cx+y, cy-x, c); roto_pixel(cx+x, cy-y, c);
        y++; err += 1 + 2*y;
        if (2*(err-x) + 1 > 0) { x--; err += 1 - 2*x; }
    }
}

/* ---------------------------------------------------------- ring screen */
static void draw_ring_screen(void)
{
    uint32_t sel = (uint32_t)roto.sel;
    roto_track_t *k = &roto.trk[sel];
    uint16_t col = TRK_COL[sel];
    uint32_t s;
    int32_t r = RING_R_FULL;

    /* background cleared by cv_begin() in roto_ui_draw() */

    /* the ring */
    roto_ring(CX, CY, r, col);

    /* playhead line + tip dot */
    if (roto.running) {
        int32_t px, py;
        ring_dot(r, (uint32_t)k->pos, (uint32_t)k->len, &px, &py);
        roto_line(CX, CY, px, py, col);
        roto_dot(px, py, COL_WHITE);
    }

    /* step dots */
    for (s = 0; s < (uint32_t)k->len; s++) {
        int32_t dx2, dy2;
        ring_dot(r, s, (uint32_t)k->len, &dx2, &dy2);
        roto_dot(dx2, dy2, k->step[s].on ? col : TRK_DIM[sel]);
    }

    /* track label + step count — top left in AF_S */
    {
        char buf[10];
        uint8_t l = k->len;
        buf[0] = 'T'; buf[1] = (char)('1' + (char)sel);
        buf[2] = ' ';
        buf[3] = (char)('0' + l / 10);
        buf[4] = (char)('0' + l % 10);
        buf[5] = 'S'; buf[6] = 0;
        cv_text(4, 4, &AF_S, buf, col);
    }

    /* 4 small track-indicator squares, bottom right */
    {
        uint32_t t;
        for (t = 0; t < ROTO_TRACKS; t++) {
            uint16_t ic = (t == sel) ? TRK_COL[t] : TRK_DIM[t];
            int32_t bx = (int32_t)((uint32_t)SCR_W - 28u + t * 7u);
            cv_rect(bx, (int32_t)SCR_H - 10, 5, 5, ic);
        }
    }
}

/* ---------------------------------------------------------- home / visualiser */
/* Icosahedron wireframe: 12 vertices, 30 edges. Rotates around Y; pulses on audio. */

/* Vertices scaled to unit sphere * 100 (integer coords) */
static const int8_t ICO_V[12][3] = {
    {  0,  45,  72 }, {  0,  45, -72 }, {  0, -45,  72 }, {  0, -45, -72 },
    { 72,   0,  45 }, { 72,   0, -45 }, {-72,   0,  45 }, {-72,   0, -45 },
    { 45,  72,   0 }, {-45,  72,   0 }, { 45, -72,   0 }, {-45, -72,   0 },
};
/* 30 edges as pairs of vertex indices */
static const uint8_t ICO_E[30][2] = {
    {0,2},{0,4},{0,6},{0,8},{0,9},
    {1,3},{1,5},{1,7},{1,8},{1,9},
    {2,4},{2,6},{2,10},{2,11},
    {3,5},{3,7},{3,10},{3,11},
    {4,5},{4,8},{4,10},
    {5,8},{5,10},
    {6,7},{6,9},{6,11},
    {7,9},{7,11},
    {8,9},{10,11},
};

static uint8_t ico_angle = 0;   /* 0..255, incremented each frame */
static uint8_t ico_pulse = 80;  /* effective scale factor (60..150) */

static void draw_home_screen(void)
{
    uint32_t i;
    int32_t px[12], py[12];

    /* background cleared by cv_begin() in roto_ui_draw() */

    int32_t ca = icos((int32_t)ico_angle);
    int32_t sa = isin((int32_t)ico_angle);
    int32_t sc = (int32_t)ico_pulse;

    /* project each vertex: rotate around Y, orthographic */
    for (i = 0; i < 12u; i++) {
        int32_t x = ICO_V[i][0], y = ICO_V[i][1], z = ICO_V[i][2];
        int32_t xr = (x * ca - z * sa) / 256;
        int32_t yr = y;
        px[i] = CX + (xr * sc) / 256;
        py[i] = CY - (yr * sc) / 256;
    }

    /* draw edges in the selected track's colour */
    uint16_t col = TRK_COL[(uint32_t)roto.sel];
    for (i = 0; i < 30u; i++) {
        uint32_t a = ICO_E[i][0], b = ICO_E[i][1];
        roto_line(px[a], py[a], px[b], py[b], col);
    }

    /* title centred */
    cv_text(CX - 36, 4, &AF_S, "ATTIC ROTO", COL_WHITE);

    /* BPM bottom centre */
    {
        char buf[8];
        uint8_t bpm = roto.bpm;
        buf[0] = (char)('0' + bpm / 100);
        buf[1] = (char)('0' + (bpm / 10) % 10);
        buf[2] = (char)('0' + bpm % 10);
        buf[3] = ' '; buf[4] = 'B'; buf[5] = 'P'; buf[6] = 'M'; buf[7] = 0;
        cv_text(CX - 20, SCR_H - 14, &AF_S, buf, TRK_DIM[0]);
    }

    /* 4 track-indicator squares bottom centre */
    {
        uint32_t t;
        for (t = 0; t < ROTO_TRACKS; t++) {
            uint16_t ic = ((uint32_t)roto.sel == t) ? TRK_COL[t] : TRK_DIM[t];
            int32_t bx = (int32_t)((uint32_t)CX - 14u + t * 9u);
            cv_rect(bx, (int32_t)SCR_H - 24, 7, 7, ic);
        }
    }

    /* advance rotation one step per frame */
    ico_angle = (uint8_t)((ico_angle + 1u) & 255u);
}

/* ---------------------------------------------------------- audio pulse hook */
/* Called from mix_block / metering path with 0..255 level. */
void roto_ui_audio_level(uint8_t level)
{
    int32_t target = 80 + (int32_t)level / 4;   /* 80..143 */
    int32_t cur    = (int32_t)ico_pulse;
    cur += (target - cur) / 8;
    if (cur < 60)  cur = 60;
    if (cur > 150) cur = 150;
    ico_pulse = (uint8_t)cur;
}

/* ---------------------------------------------------------- input */
/* Called from ui_input() with the already-consumed edge bitmasks.
 * pressed: button edges (1u << panel.btn[Bxxx]).
 * notes:   key-press edges (bit k = key k).
 * Returns 1 if roto consumed the input (Felucca should ignore this frame). */
int roto_ui_input(uint32_t pressed, uint32_t notes)
{

    /* EN_SELECT turned → cycle track (ring screen, not in editor) */
    {
        int32_t delta = panel_enc(EN_SELECT);
        if (delta && roto_screen == ROTO_SCR_RING && !roto_in_editor) {
            int32_t s = (int32_t)roto.sel + delta;
            s = ((s % (int32_t)ROTO_TRACKS) + (int32_t)ROTO_TRACKS) % (int32_t)ROTO_TRACKS;
            roto.sel = (uint8_t)s;
            return 1;
        }
    }

    /* B_HOME → go to home screen */
    if (pressed & (1u << panel.btn[B_HOME])) {
        roto_screen    = ROTO_SCR_HOME;
        roto_in_editor = 0;
        return 1;
    }

    /* B_PLAY → toggle transport */
    if (pressed & (1u << panel.btn[B_PLAY])) {
        if (roto.running) roto_stop(); else roto_play();
        return 1;
    }

    /* B_SEQ: HOME→RING; RING→toggle editor */
    if (pressed & (1u << panel.btn[B_SEQ])) {
        if (roto_screen == ROTO_SCR_HOME) {
            roto_screen    = ROTO_SCR_RING;
            roto_in_editor = 0;
        } else {
            roto_in_editor ^= 1u;
        }
        return 1;
    }

    /* White keys 0..15 toggle steps on the ring (ring screen, not editor) */
    if (roto_screen == ROTO_SCR_RING && !roto_in_editor) {
        uint32_t s;
        uint32_t maxs = (uint32_t)roto.trk[(uint32_t)roto.sel].len;
        if (maxs > 16u) maxs = 16u;
        for (s = 0; s < maxs; s++) {
            if ((notes >> s) & 1u) {
                roto_step_toggle((uint32_t)roto.sel, s);
                return 1;
            }
        }
    }

    /* in editor mode pass everything through */
    return 0;
}

/* ---------------------------------------------------------- draw dispatch */
/* Called from ui_draw_page() once per frame. */
/* LCD is 240x240. CV_MAX = 240x124, so cv_begin clamps at 124 rows.
 * We cover the full screen: lcd_fill clears all 240 rows first, then
 * cv_begin/cv_blit handles the top 124 rows with our content. */
#define ROTO_LCD_W  240u
#define ROTO_LCD_H  240u
#define ROTO_CV_H   124u   /* CV_MAX / 240 */

int roto_ui_draw(void)
{
    if (roto_in_editor) return 0;   /* let Felucca draw its own editor pages */
    /* 1. Black out the entire 240x240 LCD */
    lcd_fill(0, 0, ROTO_LCD_W, ROTO_LCD_H, COL_BG);
    /* 2. Draw our content into the cv_px[] framebuffer (top 124 rows) */
    cv_begin(ROTO_LCD_W, ROTO_CV_H, COL_BG);
    if (roto_screen == ROTO_SCR_HOME)
        draw_home_screen();
    else
        draw_ring_screen();
    cv_blit(0, 0);   /* push 240x124 canvas to LCD at y=0 */
    return 1;   /* we drew the full screen; caller skips Felucca drawing */
}
