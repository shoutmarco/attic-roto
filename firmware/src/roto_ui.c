/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto UI (2026-11-24).
 *
 * Two screens:
 *   ROTO_SCR_HOME  — rotating wireframe icosahedron, audio-reactive (home / visualiser)
 *   ROTO_SCR_RING  — full-screen ring sequencer for the selected track
 *
 * Navigation:
 *   EN_SELECT (main dial) turned  → cycle selected track 0..3
 *   EN_SELECT pressed             → toggle between RING and voice/engine editor
 *   B_HOME pressed                → return to HOME screen
 *   B_PLAY pressed                → start / stop transport
 *   white keys                    → toggle steps on the active ring
 *
 * Colours (RGB565):
 *   Track 0  white       0xFFFF
 *   Track 1  light blue  0x867F   (approx #80C8FF)
 *   Track 2  pale amber  0xFD40   (approx #FFA800)
 *   Track 3  soft coral  0xF8A4   (approx #F04040)
 *   Dimmed: each colour at ~40% brightness for inactive tracks.
 *
 * The drawing uses only the primitives already in Felucca (gfx.c / lcd.c):
 *   lcd_fill, draw_line, draw_circle_px, draw_text_line.
 * No new drawing dependencies are introduced. */

#include "core.h"
#include "roto_seq.h"
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- palette */
/* RGB565 helpers */
#define RGB565(r,g,b) ((uint16_t)(((r)&0xF8)<<8|(((g)&0xFC)<<3)|((b)>>3)))

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

#define COL_BG   RGB565(0, 0, 0)
#define COL_WHITE RGB565(255,255,255)

/* ---------------------------------------------------------------- screen state */
enum { ROTO_SCR_HOME = 0, ROTO_SCR_RING };
static uint8_t roto_screen = ROTO_SCR_HOME;
static uint8_t roto_in_editor = 0;   /* 1 = Felucca's voice editor is showing */

/* ---------------------------------------------------------------- geometry */
#define SCR_W 240
#define SCR_H 240
#define CX    120
#define CY    120

/* Ring radii for each track (track 0 = smallest inner ring) */
static const uint8_t RING_R[ROTO_TRACKS] = { 50, 72, 94, 108 };

/* -------------------------------------------------------- integer maths */
/* Fixed-point sine / cosine: enough for dot placement on a circle.
 * Returns value * 256. angle in 0..255 (full circle). */
static int32_t isin(int32_t a)
{
    /* quarter-wave table, 64 entries */
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
static void ring_dot(uint8_t r, uint32_t step, uint32_t nsteps, int32_t *ox, int32_t *oy)
{
    /* angle: step 0 at top (angle 192 = -90 deg in 0..255 space) */
    int32_t a = (int32_t)((step * 256u) / nsteps + 192u) & 255;
    *ox = CX + (icos(a) * (int32_t)r) / 256;
    *oy = CY + (isin(a) * (int32_t)r) / 256;
}

/* ---------------------------------------------------------- draw primitives */
static void draw_pixel(int32_t x, int32_t y, uint16_t c)
{
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    lcd_fill((uint32_t)x, (uint32_t)y, 1, 1, c);
}

/* Bresenham line */
static void draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        draw_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* filled circle, radius 2 */
static void draw_dot(int32_t x, int32_t y, uint16_t c)
{
    int32_t i, j;
    for (j = -2; j <= 2; j++)
        for (i = -2; i <= 2; i++)
            if (i*i + j*j <= 5)
                draw_pixel(x + i, y + j, c);
}

/* thin ring (hollow circle) */
static void draw_ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x = r, y = 0, err = 0;
    while (x >= y) {
        draw_pixel(cx+x, cy+y, c); draw_pixel(cx+y, cy+x, c);
        draw_pixel(cx-y, cy+x, c); draw_pixel(cx-x, cy+y, c);
        draw_pixel(cx-x, cy-y, c); draw_pixel(cx-y, cy-x, c);
        draw_pixel(cx+y, cy-x, c); draw_pixel(cx+x, cy-y, c);
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

    lcd_fill(0, 0, SCR_W, SCR_H, COL_BG);

    uint32_t r = RING_R[sel] + 12u;   /* ring drawn slightly larger when full-screen */

    /* the ring itself */
    draw_ring(CX, CY, (int32_t)r, col);

    /* playhead line from centre to ring edge */
    if (roto.running) {
        int32_t px, py;
        ring_dot((uint8_t)r, (uint32_t)k->pos, (uint32_t)k->len, &px, &py);
        draw_line(CX, CY, px, py, col);
        /* bright dot at the tip */
        draw_dot(px, py, COL_WHITE);
    }

    /* step dots */
    for (s = 0; s < (uint32_t)k->len; s++) {
        int32_t dx, dy;
        ring_dot((uint8_t)r, s, (uint32_t)k->len, &dx, &dy);
        uint16_t dc = k->step[s].on ? col : TRK_DIM[sel];
        draw_dot(dx, dy, dc);
    }

    /* track number and step count — top left, monospace */
    {
        char buf[16];
        buf[0] = 'T'; buf[1] = (char)('1' + sel);
        buf[2] = ' '; buf[3] = ' ';
        /* step count as two digits */
        uint8_t l = k->len;
        buf[4] = (char)('0' + l / 10);
        buf[5] = (char)('0' + l % 10);
        buf[6] = 'S'; buf[7] = 'T'; buf[8] = 'P'; buf[9] = 0;
        draw_text_line(4, 4, 120, &AF_S, buf, col, COL_BG, 0);
    }

    /* small track indicators bottom right: 4 squares */
    {
        uint32_t t;
        for (t = 0; t < ROTO_TRACKS; t++) {
            uint16_t ic = (t == sel) ? TRK_COL[t] : TRK_DIM[t];
            uint32_t bx = SCR_W - 28u + t * 7u;
            lcd_fill(bx, SCR_H - 10u, 5u, 5u, ic);
        }
    }
}

/* ---------------------------------------------------------- home / visualiser */
/* Icosahedron wireframe: 12 vertices projected, 30 edges.
 * Rotates slowly; pulses on audio level. */

/* Icosahedron vertices (unit sphere * 100, integer) */
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

static uint8_t ico_angle = 0;   /* rotation angle 0..255, incremented each frame */
static uint8_t ico_pulse = 80;  /* scale factor (normal ~80, grows on loud) */

static void draw_home_screen(void)
{
    uint32_t i;
    int32_t px[12], py[12];

    lcd_fill(0, 0, SCR_W, SCR_H, COL_BG);

    int32_t ca = icos((int32_t)ico_angle);
    int32_t sa = isin((int32_t)ico_angle);
    int32_t sc = (int32_t)ico_pulse;

    /* project each vertex: rotate around Y axis, then orthographic */
    for (i = 0; i < 12u; i++) {
        int32_t x = ICO_V[i][0], y = ICO_V[i][1], z = ICO_V[i][2];
        /* rotate Y: x' = x*cos - z*sin, z' = x*sin + z*cos */
        int32_t xr = (x * ca - z * sa) / 256;
        int32_t yr = y;
        /* scale and project */
        px[i] = CX + (xr * sc) / 256;
        py[i] = CY - (yr * sc) / 256;
    }

    /* draw edges in the selected track's colour */
    uint16_t col = TRK_COL[(uint32_t)roto.sel];
    for (i = 0; i < 30u; i++) {
        uint32_t a = ICO_E[i][0], b = ICO_E[i][1];
        draw_line(px[a], py[a], px[b], py[b], col);
    }

    /* title */
    draw_text_line(0, 6, SCR_W, &AF_S, "ATTIC ROTO", COL_WHITE, COL_BG, 1);

    /* BPM bottom centre */
    {
        char buf[8];
        uint8_t bpm = roto.bpm;
        buf[0] = (char)('0' + bpm / 100);
        buf[1] = (char)('0' + (bpm / 10) % 10);
        buf[2] = (char)('0' + bpm % 10);
        buf[3] = ' '; buf[4] = 'B'; buf[5] = 'P'; buf[6] = 'M'; buf[7] = 0;
        draw_text_line(0, SCR_H - 20, SCR_W, &AF_S, buf, TRK_DIM[0], COL_BG, 1);
    }

    /* track indicators bottom */
    {
        uint32_t t;
        for (t = 0; t < ROTO_TRACKS; t++) {
            uint16_t ic = ((uint32_t)roto.sel == t) ? TRK_COL[t] : TRK_DIM[t];
            uint32_t bx = SCR_W/2u - 14u + t * 9u;
            lcd_fill(bx, SCR_H - 32u, 7u, 7u, ic);
        }
    }

    /* advance rotation */
    ico_angle = (uint8_t)((ico_angle + 1u) & 255u);
}

/* ---------------------------------------------------------- audio pulse hook */
/* Called from the audio / metering path with a 0..255 level reading.
 * Grows the icosahedron on loud passages. */
void roto_ui_audio_level(uint8_t level)
{
    int32_t target = 80 + (int32_t)level / 4;   /* 80..143 */
    int32_t cur    = (int32_t)ico_pulse;
    cur += (target - cur) / 8;
    if (cur < 60) cur = 60;
    if (cur > 150) cur = 150;
    ico_pulse = (uint8_t)cur;
}

/* ---------------------------------------------------------- input */
/* Called from the UI task (ui_input.c equivalent), once per frame.
 * Returns 1 if we handled the input and Felucca should NOT process it,
 * 0 to pass through. */
int roto_ui_input(void)
{
    int32_t sel_delta = panel_enc(EN_SELECT);

    /* SELECT dial: cycle track on ring screen, ignored on home */
    if (sel_delta && roto_screen == ROTO_SCR_RING && !roto_in_editor) {
        int32_t s = (int32_t)roto.sel + sel_delta;
        s = ((s % ROTO_TRACKS) + ROTO_TRACKS) % ROTO_TRACKS;
        roto.sel = (uint8_t)s;
        return 1;
    }

    /* HOME button → go to home screen */
    if (fm1_btn_fell(panel.btn[B_HOME])) {
        roto_screen = ROTO_SCR_HOME;
        roto_in_editor = 0;
        return 1;
    }

    /* PLAY button → toggle transport */
    if (fm1_btn_fell(panel.btn[B_PLAY])) {
        if (roto.running) roto_stop(); else roto_play();
        return 1;
    }

    /* SELECT press: on home → go to ring; on ring → toggle editor */
    if (fm1_btn_fell(panel.btn[B_SEQ])) {
        if (roto_screen == ROTO_SCR_HOME) {
            roto_screen = ROTO_SCR_RING;
            roto_in_editor = 0;
        } else if (!roto_in_editor) {
            roto_in_editor = 1;   /* drop into Felucca's voice editor */
        } else {
            roto_in_editor = 0;   /* back to ring */
        }
        return 1;
    }

    /* White keys toggle steps on the active ring (ring screen only) */
    if (roto_screen == ROTO_SCR_RING && !roto_in_editor) {
        /* Keys 0..15 map to steps 0..15 on the active track.
         * fm1_key_fell(k) is true the frame a white key k is pressed. */
        uint32_t s;
        for (s = 0; s < 16u && s < (uint32_t)roto.trk[(uint32_t)roto.sel].len; s++) {
            if (fm1_key_fell((uint32_t)s)) {
                roto_step_toggle((uint32_t)roto.sel, s);
                return 1;
            }
        }
    }

    return roto_in_editor ? 0 : 0;   /* pass through to Felucca in editor mode */
}

/* ---------------------------------------------------------- draw dispatch */
/* Called from ui_draw() once per frame. */
void roto_ui_draw(void)
{
    if (roto_in_editor) return;   /* Felucca draws its own editor pages */
    if (roto_screen == ROTO_SCR_HOME)
        draw_home_screen();
    else
        draw_ring_screen();
}
