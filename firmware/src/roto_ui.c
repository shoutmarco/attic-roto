/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto ring screen.
 *
 * Our own drawing code. The only thing used from the firmware is
 * lcd_fill(x, y, w, h, colour), which writes a rectangle to the 240x240 LCD.
 *
 * No flicker: the screen is cleared and drawn in full only when something
 * structural changes (track, steps, length, or the firmware asks via `force`).
 * When just the playhead moves, two small boxes are repainted: the one it left
 * and the one it entered. Each box is cleared and the whole scene is drawn
 * again clipped to that box, so what ends up on the glass is exactly what a
 * full redraw would give, without ever blanking the screen.
 *
 * Look (from the user's seq-euclid / seq-step references, controls removed):
 *   header   "1 RING" in the track colour, rate in white, BPM in grey,
 *            the four track boxes at the right, a dark rule under it
 *   ring     a dim circle in the track colour; off steps a small grey dot,
 *            on steps a diamond outline in the track colour with their note
 *            name outside the ring, grey lines joining the on steps
 *   playhead filled diamond in a grey circle, three pink dots trailing it
 *   centre   the number of hits, "OF <len>" under it
 */

#include "roto_seq.h"
#include "roto_font.h"

/* ------------------------------------------------------------- colours */
#define RT_RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

#define RT_BG    RT_RGB(0, 0, 0)
#define RT_TEXT  RT_RGB(200, 204, 200)
#define RT_GREY  RT_RGB(112, 116, 112)
#define RT_LINE  RT_RGB(88, 92, 88)
#define RT_DOT   RT_RGB(72, 76, 72)
#define RT_RULE  RT_RGB(32, 32, 32)
#define RT_PINK  RT_RGB(164, 68, 123)

/* track colours, sampled from the reference images */
static const uint16_t RT_TRK_COL[ROTO_TRACKS] = {
    RT_RGB(172, 105, 49),   /* 1 orange */
    RT_RGB(82, 178, 65),    /* 2 green  */
    RT_RGB(164, 68, 123),   /* 3 pink   */
    RT_RGB(40, 140, 168),   /* 4 cyan   */
};
/* the ring itself: the track colour at about a fifth */
/* ghost notes: the track colour at about half */
static const uint16_t RT_TRK_HALF[ROTO_TRACKS] = {
    RT_RGB(96, 58, 27),
    RT_RGB(44, 98, 36),
    RT_RGB(90, 38, 68),
    RT_RGB(22, 78, 94),
};
static const uint16_t RT_TRK_RING[ROTO_TRACKS] = {
    RT_RGB(38, 23, 10),
    RT_RGB(16, 38, 12),
    RT_RGB(36, 14, 27),
    RT_RGB(8, 31, 37),
};

/* ------------------------------------------------------------- layout */
#define RT_W       240
#define RT_H       240
#define RT_CX      120
#define RT_CY      132
#define RT_R       80          /* ring radius */
#define RT_R_LABEL 95          /* note names sit on this radius */
#define RT_RULE_Y  19
#define RT_BOX_X   196         /* the four track boxes */
#define RT_BOX_W   10
#define RT_BPM_X1  190         /* BPM text ends here */

/* ------------------------------------------------------------- clip */
static int32_t rt_cx0, rt_cy0, rt_cx1, rt_cy1;      /* inclusive x0,y0 / exclusive x1,y1 */

static void rt_clip_all(void) { rt_cx0 = 0; rt_cy0 = 0; rt_cx1 = RT_W; rt_cy1 = RT_H; }

static void rt_clip_set(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    rt_cx0 = x0 < 0 ? 0 : x0;  rt_cy0 = y0 < 0 ? 0 : y0;
    rt_cx1 = x1 > RT_W ? RT_W : x1;  rt_cy1 = y1 > RT_H ? RT_H : y1;
}

/* a horizontal run of pixels, clipped: one lcd_fill */
static void rt_hspan(int32_t x0, int32_t x1, int32_t y, uint16_t c)
{
    if (y < rt_cy0 || y >= rt_cy1) return;
    if (x0 < rt_cx0) x0 = rt_cx0;
    if (x1 > rt_cx1) x1 = rt_cx1;
    if (x1 <= x0) return;
    lcd_fill((uint32_t)x0, (uint32_t)y, (uint32_t)(x1 - x0), 1u, c);
}

static void rt_px(int32_t x, int32_t y, uint16_t c) { rt_hspan(x, x + 1, y, c); }

static void rt_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    if (x0 < rt_cx0) x0 = rt_cx0;
    if (y0 < rt_cy0) y0 = rt_cy0;
    if (x1 > rt_cx1) x1 = rt_cx1;
    if (y1 > rt_cy1) y1 = rt_cy1;
    if (x1 <= x0 || y1 <= y0) return;
    lcd_fill((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), c);
}

/* ------------------------------------------------------------- geometry */
/* angle: 1024 units per turn. Returns Q14 (16384 = 1.0). */
static int32_t rt_sin(int32_t a)
{
    a &= 1023;
    if (a < 256)  return  (int32_t)ROTO_SIN[a];
    if (a < 512)  return  (int32_t)ROTO_SIN[512 - a];
    if (a < 768)  return -(int32_t)ROTO_SIN[a - 512];
    return -(int32_t)ROTO_SIN[1024 - a];
}
static int32_t rt_cos(int32_t a) { return rt_sin(a + 256); }

static int32_t rt_round14(int32_t v) { return v >= 0 ? (v + 8192) >> 14 : -((-v + 8192) >> 14); }

/* point at angle a on radius r around the ring centre */
static void rt_polar(int32_t a, int32_t r, int32_t *x, int32_t *y)
{
    *x = RT_CX + rt_round14(rt_cos(a) * r);
    *y = RT_CY + rt_round14(rt_sin(a) * r);
}

/* step s of n: step 0 at the top, clockwise */
static int32_t rt_step_angle(uint32_t s, uint32_t n)
{
    return (int32_t)((s * 1024u + n / 2u) / n) - 256;
}

/* ------------------------------------------------------------- shapes */
static void rt_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, e2;
    for (;;) {
        rt_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rt_circle(int32_t xc, int32_t yc, int32_t r, uint16_t c)
{
    int32_t x = r, y = 0, d = 1 - r;
    while (x >= y) {
        rt_px(xc + x, yc + y, c); rt_px(xc - x, yc + y, c);
        rt_px(xc + x, yc - y, c); rt_px(xc - x, yc - y, c);
        rt_px(xc + y, yc + x, c); rt_px(xc - y, yc + x, c);
        rt_px(xc + y, yc - x, c); rt_px(xc - y, yc - x, c);
        y++;
        if (d < 0) d += 2 * y + 1;
        else { x--; d += 2 * (y - x) + 1; }
    }
}

/* diamond of half-size r, filled: one span per row */
static void rt_diamond_fill(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t i;
    for (i = -r; i <= r; i++) {
        int32_t w = r - (i < 0 ? -i : i);
        rt_hspan(x - w, x + w + 1, y + i, c);
    }
}

static void rt_diamond_line(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t i;
    for (i = -r; i <= r; i++) {
        int32_t w = r - (i < 0 ? -i : i);
        rt_px(x - w, y + i, c);
        if (w) rt_px(x + w, y + i, c);
    }
}

/* ------------------------------------------------------------- text */
static int32_t rt_glyph(char ch)
{
    int32_t i;
    for (i = 0; ROTO_FONT_CHARS[i]; i++)
        if (ROTO_FONT_CHARS[i] == ch) return i;
    return 0;   /* space */
}

static uint32_t rt_strlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* one bitmap row: each run of set bits is one span (scale s: s x s pixels) */
static void rt_row_spans(int32_t x, int32_t y, uint32_t bits, int32_t s, uint16_t c)
{
    int32_t b = 0;
    while (b < 8) {
        if (bits & (0x80u >> b)) {
            int32_t e = b, k;
            while (e < 8 && (bits & (0x80u >> e))) e++;
            for (k = 0; k < s; k++)
                rt_hspan(x + b * s, x + e * s, y + k, c);
            b = e;
        } else b++;
    }
}

/* small 5x7, advance 6 */
static int32_t rt_text_s(int32_t x, int32_t y, const char *t, uint16_t c)
{
    for (; *t; t++, x += 6) {
        const unsigned char *g = ROTO_SMALL[rt_glyph(*t)];
        int32_t r;
        for (r = 0; r < ROTO_SMALL_H; r++) rt_row_spans(x, y + r, g[r], 1, c);
    }
    return x;
}
#define RT_TEXT_S_W(t) ((int32_t)rt_strlen(t) * 6 - 1)

/* big 8x11 (Unifont), scale s, advance 8*s */
static int32_t rt_text_b(int32_t x, int32_t y, const char *t, int32_t s, uint16_t c)
{
    for (; *t; t++, x += 8 * s) {
        const unsigned char *g = ROTO_BIG[rt_glyph(*t)];
        int32_t r;
        for (r = 0; r < ROTO_BIG_H; r++) rt_row_spans(x, y + r * s, g[r], s, c);
    }
    return x;
}

static void rt_fmt_u(char *out, uint32_t v)
{
    char tmp[6];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 5u);
    while (n) *out++ = tmp[--n];
    *out = 0;
}

static void rt_note_name(char *out, uint32_t midi)
{
    static const char RT_NM[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char *n = RT_NM[midi % 12u];
    int32_t oct = (int32_t)(midi / 12u) - 1;
    while (*n) *out++ = *n++;
    if (oct < 0) { *out++ = '-'; oct = -oct; }
    *out++ = (char)('0' + oct);
    *out = 0;
}

/* ------------------------------------------------------------- scene */

/* what this frame draws (snapshotted once; the audio side keeps moving) */
static struct {
    uint8_t sel, len, pos, div, bpm, hits, view, rot, prob, show;
    uint8_t on[ROTO_STEPS];
    uint8_t note[ROTO_STEPS];
    uint8_t ghost[ROTO_STEPS];
} rt_sn;

static void rt_snapshot(void)
{
    const roto_track_t *k = &roto.trk[roto.sel < ROTO_TRACKS ? roto.sel : 0];
    uint32_t s;
    rt_sn.sel = roto.sel < ROTO_TRACKS ? roto.sel : 0;
    rt_sn.len = k->len ? k->len : 1;
    rt_sn.pos = k->pos < rt_sn.len ? k->pos : 0;
    rt_sn.div = k->div;
    rt_sn.bpm = roto.bpm;
    rt_sn.view = roto.view;
    rt_sn.rot = k->rot;
    rt_sn.prob = k->prob;
    rt_sn.hits = 0;
    for (s = 0; s < ROTO_STEPS; s++) {
        rt_sn.on[s] = (uint8_t)(s < rt_sn.len && k->step[s].on);
        rt_sn.note[s] = k->step[s].note;
        rt_sn.ghost[s] = (uint8_t)(k->step[s].chance != 0);
        rt_sn.hits = (uint8_t)(rt_sn.hits + rt_sn.on[s]);
    }
}

static void rt_draw_header(void)
{
    uint16_t col = RT_TRK_COL[rt_sn.sel];
    char buf[16];
    int32_t x, t;
    static const char *RT_DIV[5] = {"1/16", "1/16", "1/8", "3/16", "1/4"};

    buf[0] = (char)('1' + rt_sn.sel); buf[1] = 0;
    x = rt_text_b(4, 4, buf, 1, col);
    x = rt_text_b(x + 6, 4, roto.snd[rt_sn.sel].name[0] ? roto.snd[rt_sn.sel].name : "RING", 1, col);
    if (rt_sn.view == ROTO_VIEW_EUCLID) {               /* "EUCLID 3/5", as the reference */
        char e[16] = "EUCLID ";
        uint32_t n;
        rt_fmt_u(e + 7, rt_sn.hits);
        n = rt_strlen(e); e[n] = '/';
        rt_fmt_u(e + n + 1, rt_sn.len);
        rt_text_s(x + 8, 8, e, RT_TEXT);
    } else {
        rt_text_s(x + 8, 8, RT_DIV[rt_sn.div <= 4 ? rt_sn.div : 1], RT_TEXT);
        rt_fmt_u(buf, rt_sn.bpm);
        { uint32_t n = rt_strlen(buf); buf[n] = ' '; buf[n+1] = 'B'; buf[n+2] = 'P'; buf[n+3] = 'M'; buf[n+4] = 0; }
        rt_text_s(RT_BPM_X1 - RT_TEXT_S_W(buf), 8, buf, RT_GREY);
    }

    for (t = 0; t < ROTO_TRACKS; t++) {
        int32_t bx = RT_BOX_X + t * RT_BOX_W;
        uint16_t tc = RT_TRK_COL[t];
        char d[2] = {(char)('1' + t), 0};
        if (t == (int32_t)rt_sn.sel) {
            rt_rect(bx, 5, bx + 9, 16, tc);
            rt_text_s(bx + 2, 7, d, RT_BG);
        } else {
            rt_hspan(bx, bx + 9, 5, tc);
            rt_hspan(bx, bx + 9, 15, tc);
            rt_rect(bx, 5, bx + 1, 16, tc);
            rt_rect(bx + 8, 5, bx + 9, 16, tc);
            rt_text_s(bx + 2, 7, d, tc);
        }
    }
    rt_rect(4, RT_RULE_Y, 236, RT_RULE_Y + 2, RT_RULE);
}

/* the centre: the hits and "OF n"; while a knob turns, its value and name
 * (the name in the track colour) instead; on EUCLID also "ROT r  p%" */
static void rt_draw_centre(void)
{
    static const char *const RT_DIVS[5] = {"1/16", "1/16", "1/8", "3/16", "1/4"};
    static const char *const RT_SHOWN[7] = {"", "LENGTH", "FILL", "ROTATE", "RATE", "PROB", "BPM"};
    char big[8], small[16];
    uint16_t small_col = RT_GREY;
    uint32_t w;

    switch (rt_sn.show) {
    case ROTO_SHOW_LENGTH: rt_fmt_u(big, rt_sn.len); break;
    case ROTO_SHOW_FILL:   rt_fmt_u(big, rt_sn.hits); break;
    case ROTO_SHOW_ROTATE: big[0] = '+'; rt_fmt_u(big + 1, rt_sn.rot); if (!rt_sn.rot) { big[0] = '0'; big[1] = 0; } break;
    case ROTO_SHOW_RATE:   { const char *d = RT_DIVS[rt_sn.div <= 4 ? rt_sn.div : 1]; uint32_t i; for (i = 0; d[i]; i++) big[i] = d[i]; big[i] = 0; } break;
    case ROTO_SHOW_PROB:   rt_fmt_u(big, rt_sn.prob); w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0; break;
    case ROTO_SHOW_BPM:    rt_fmt_u(big, rt_sn.bpm); break;
    default:               rt_fmt_u(big, rt_sn.hits); break;
    }
    if (rt_sn.show && rt_sn.show < 7u) {
        const char *l = RT_SHOWN[rt_sn.show];
        uint32_t i;
        for (i = 0; l[i]; i++) small[i] = l[i];
        small[i] = 0;
        small_col = RT_TRK_COL[rt_sn.sel];
    } else {
        small[0] = 'O'; small[1] = 'F'; small[2] = ' ';
        rt_fmt_u(small + 3, rt_sn.len);
    }
    rt_text_b(RT_CX - (int32_t)rt_strlen(big) * 8, RT_CY - 18, big, 2, RT_TEXT);
    rt_text_s(RT_CX - RT_TEXT_S_W(small) / 2, RT_CY + 10, small, small_col);

    if (rt_sn.view == ROTO_VIEW_EUCLID && !rt_sn.show) {
        char e[16] = "ROT ";
        uint32_t n;
        rt_fmt_u(e + 4, rt_sn.rot);
        n = rt_strlen(e); e[n] = ' '; e[n + 1] = ' ';
        rt_fmt_u(e + n + 2, rt_sn.prob);
        n = rt_strlen(e); e[n] = '%'; e[n + 1] = 0;
        rt_text_s(RT_CX - RT_TEXT_S_W(e) / 2, RT_CY + 21, e, RT_GREY);
    }
}

static void rt_draw_ring(void)
{
    uint16_t col = RT_TRK_COL[rt_sn.sel];
    uint32_t n = rt_sn.len, s;
    int32_t x, y, fx = 0, fy = 0, px = 0, py = 0, have = 0;
    char buf[8];

    rt_circle(RT_CX, RT_CY, RT_R, RT_TRK_RING[rt_sn.sel]);

    /* grey lines joining the on steps, in order, closed */
    for (s = 0; s < n; s++) {
        if (!rt_sn.on[s] || rt_sn.ghost[s]) continue;   /* (the lines join the real hits) */
        rt_polar(rt_step_angle(s, n), RT_R, &x, &y);
        if (have) rt_line(px, py, x, y, RT_LINE);
        else { fx = x; fy = y; }
        px = x; py = y; have++;
    }
    if (have > 2) rt_line(px, py, fx, fy, RT_LINE);

    rt_draw_centre();

    /* steps */
    for (s = 0; s < n; s++) {
        int32_t a = rt_step_angle(s, n);
        rt_polar(a, RT_R, &x, &y);
        if (rt_sn.on[s] && rt_sn.ghost[s]) {             /* a ghost note: small, half bright, no name */
            rt_diamond_fill(x, y, 2, RT_BG);
            rt_diamond_line(x, y, 3, RT_TRK_HALF[rt_sn.sel]);
        } else if (rt_sn.on[s]) {
            int32_t lx, ly;
            rt_diamond_fill(x, y, 3, RT_BG);
            rt_diamond_line(x, y, 4, col);
            rt_px(x, y, col);
            rt_note_name(buf, rt_sn.note[s]);
            rt_polar(a, RT_R_LABEL, &lx, &ly);
            rt_text_s(lx - RT_TEXT_S_W(buf) / 2, ly - 3, buf, RT_GREY);
        } else {
            rt_rect(x, y, x + 2, y + 2, RT_DOT);
        }
    }

    /* playhead: three pink dots trailing it inside the ring, then the head */
    {
        int32_t a = rt_step_angle(rt_sn.pos, n), k;
        for (k = 1; k <= 3; k++) {
            rt_polar(a - k * 9, RT_R - 9, &x, &y);
            rt_rect(x, y, x + 2, y + 2, RT_PINK);
        }
        rt_polar(a, RT_R, &x, &y);
        rt_circle(x, y, 7, RT_GREY);
        rt_diamond_fill(x, y, 4, col);
    }
}

static void rt_draw_scene(void)
{
    rt_draw_header();
    rt_draw_ring();
}

/* clear a box and draw the scene clipped to it */
static void rt_repaint(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    rt_clip_set(x0, y0, x1, y1);
    rt_rect(x0, y0, x1, y1, RT_BG);
    rt_draw_scene();
    rt_clip_all();
}

/* the box the playhead (and its trail) covers at step pos */
static void rt_playhead_box(uint32_t pos, uint32_t n, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int32_t a = rt_step_angle(pos, n), x, y, k;
    rt_polar(a, RT_R, &x, &y);
    *x0 = x - 9; *y0 = y - 9; *x1 = x + 10; *y1 = y + 10;
    for (k = 1; k <= 3; k++) {
        rt_polar(a - k * 9, RT_R - 9, &x, &y);
        if (x - 1 < *x0) *x0 = x - 1;
        if (y - 1 < *y0) *y0 = y - 1;
        if (x + 3 > *x1) *x1 = x + 3;
        if (y + 3 > *y1) *y1 = y + 3;
    }
}

/* ------------------------------------------------------------- state */
static uint8_t roto_in_editor = 0;
static uint8_t rt_shown = 0;               /* 0: nothing of ours on screen yet */
static uint32_t rt_shown_head, rt_shown_ring;   /* what the header / the ring showed */
static uint8_t rt_shown_pos, rt_shown_bpm, rt_shown_sel;
static uint8_t rt_show_what;                /* ROTO_SHOW_*: a knob's value in the centre ... */
static uint32_t rt_show_until;              /* ... until this time (ms) */
#define RT_SHOW_MS 900u

void roto_ui_show(uint32_t what, uint32_t now_ms)
{
    rt_show_what = (uint8_t)what;
    rt_show_until = now_ms + RT_SHOW_MS;
}

#define RT_BAND 8                           /* rows per strip when a big area changes */
#define RT_HEAD_Y1 (RT_RULE_Y + 2)          /* the header: rows 0 .. RT_HEAD_Y1 - 1 */

static uint32_t rt_fnv(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* the header: track, rate, the sound's name (the BPM has its own repaint) */
static uint32_t rt_head_sig(void)
{
    uint32_t h = 2166136261u, s;
    h = rt_fnv(h, rt_sn.sel);
    h = rt_fnv(h, rt_sn.div);
    h = rt_fnv(h, rt_sn.view);
    if (rt_sn.view == ROTO_VIEW_EUCLID) { h = rt_fnv(h, rt_sn.hits); h = rt_fnv(h, rt_sn.len); }
    for (s = 0; s < sizeof roto.snd[0].name && roto.snd[rt_sn.sel].name[s]; s++)
        h = rt_fnv(h, (uint8_t)roto.snd[rt_sn.sel].name[s]);
    return h;
}

/* the ring: track, length, the steps and their notes */
static uint32_t rt_ring_sig(void)
{
    uint32_t h = 2166136261u, s;
    h = rt_fnv(h, rt_sn.sel);
    h = rt_fnv(h, rt_sn.len);
    h = rt_fnv(h, rt_sn.view);
    h = rt_fnv(h, rt_sn.show);
    if (rt_sn.show) { h = rt_fnv(h, rt_sn.rot); h = rt_fnv(h, rt_sn.prob); h = rt_fnv(h, rt_sn.div); h = rt_fnv(h, rt_sn.bpm); }
    else if (rt_sn.view == ROTO_VIEW_EUCLID) { h = rt_fnv(h, rt_sn.rot); h = rt_fnv(h, rt_sn.prob); }
    for (s = 0; s < rt_sn.len; s++)
        h = rt_fnv(h, (uint32_t)(rt_sn.on[s] | (rt_sn.ghost[s] << 1) | (rt_sn.note[s] << 2)));
    return h;
}

/* repaint rows y0..y1 a strip at a time: each strip is blank only while it is
 * drawn, never the whole area at once (no flash when a knob changes the layout) */
static void rt_repaint_rows(int32_t y0, int32_t y1)
{
    int32_t y;
    for (y = y0; y < y1; y += RT_BAND)
        rt_repaint(0, y, RT_W, y + RT_BAND < y1 ? y + RT_BAND : y1);
}

int roto_ui_draw(int force, uint32_t now_ms)
{
    uint32_t head, ring;
    if (roto_in_editor) { rt_shown = 0; return 0; }

    rt_snapshot();
    if (rt_show_what && (int32_t)(rt_show_until - now_ms) <= 0) rt_show_what = 0;
    rt_sn.show = rt_show_what;
    head = rt_head_sig();
    ring = rt_ring_sig();
    rt_clip_all();

    if (force || !rt_shown || rt_sn.sel != rt_shown_sel) {
        rt_repaint_rows(0, RT_H);                       /* everything (another track, or the screen was someone else's) */
    } else {
        if (ring != rt_shown_ring)
            rt_repaint_rows(RT_HEAD_Y1, RT_H);          /* the ring only (K3, a step key) */
        if (head != rt_shown_head)
            rt_repaint_rows(0, RT_HEAD_Y1);             /* the header only (K4) */
        else if (rt_sn.bpm != rt_shown_bpm && rt_sn.view == ROTO_VIEW_RING)
            rt_repaint(RT_BPM_X1 - 50, 4, RT_BPM_X1 + 1, 17);
        if (ring == rt_shown_ring && rt_sn.pos != rt_shown_pos) {
            int32_t x0, y0, x1, y1;
            rt_playhead_box(rt_shown_pos < rt_sn.len ? rt_shown_pos : 0, rt_sn.len, &x0, &y0, &x1, &y1);
            rt_repaint(x0, y0, x1, y1);
            rt_playhead_box(rt_sn.pos, rt_sn.len, &x0, &y0, &x1, &y1);
            rt_repaint(x0, y0, x1, y1);
        }
    }
    rt_shown = 1;
    rt_shown_head = head;
    rt_shown_ring = ring;
    rt_shown_pos = rt_sn.pos;
    rt_shown_bpm = rt_sn.bpm;
    rt_shown_sel = rt_sn.sel;
    return 1;
}

/* ------------------------------------------------------------- input
 * (stage 3 will rework the controls; this keeps the step keys working) */
int roto_ui_input(uint32_t pressed, uint32_t notes)
{
    (void)pressed;
    if (roto_in_editor) return 0;
    if (notes) {
        uint32_t s, sel = (uint32_t)roto.sel;
        uint32_t maxs = (uint32_t)roto.trk[sel].len;
        if (maxs > 16u) maxs = 16u;
        for (s = 0; s < maxs; s++)
            if ((notes >> s) & 1u) roto_step_toggle(sel, s);
    }
    return 1;
}

void roto_ui_audio_level(uint8_t level)
{
    (void)level;
}
