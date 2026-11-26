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
    RT_RGB(190, 160, 64),   /* 5 sand   (more drums) */
    RT_RGB(128, 106, 196),  /* 6 violet (a counter-line on the blips) */
};
/* the ring itself: the track colour at about a fifth */
/* ghost notes: the track colour at about half */
static const uint16_t RT_TRK_HALF[ROTO_TRACKS] = {
    RT_RGB(96, 58, 27),
    RT_RGB(44, 98, 36),
    RT_RGB(90, 38, 68),
    RT_RGB(22, 78, 94),
    RT_RGB(104, 88, 35),
    RT_RGB(70, 58, 108),
};
static const uint16_t RT_TRK_RING[ROTO_TRACKS] = {
    RT_RGB(38, 23, 10),
    RT_RGB(16, 38, 12),
    RT_RGB(36, 14, 27),
    RT_RGB(8, 31, 37),
    RT_RGB(42, 35, 14),
    RT_RGB(28, 23, 43),
};

/* ------------------------------------------------------------- layout */
#define RT_W       240
#define RT_H       240
#define RT_CX      120
#define RT_CY      132
#define RT_R       80          /* ring radius */
#define RT_R_LABEL 95          /* note names sit on this radius */
#define RT_RULE_Y  19
#define RT_BOX_X   182         /* the six ring boxes */
#define RT_BOX_W   9
#define RT_BPM_X1  176         /* BPM text ends here */

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
    uint8_t env[4];
    int8_t flt[4];
    uint8_t dtime, dfdbk, dcolor, dsend, part;
    char flt_lbl[4][6];
    uint8_t on[ROTO_STEPS];
    uint8_t note[ROTO_STEPS];
    uint8_t ghost[ROTO_STEPS];
    uint8_t a_chord, a_mode, a_speed, a_range, a_gate, a_len, a_cur;
    uint8_t a_seq[16];
    char a_name[12];
    uint8_t spage, s_drum, s_cat, s_idx, s_kit, s_piece, s_level;
    int8_t s_pan;
    int8_t ed[8];
    char ed_lbl[8][6];
    char ed_txt[8][7];
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
    rt_sn.part = (uint8_t)(k->part < ROTO_PARTS ? k->part : 0u);
    { uint32_t e; for (e = 0; e < 4u; e++) rt_sn.env[e] = (uint8_t)(roto.env[rt_sn.part][e] & 127u); }
    rt_sn.dtime = roto.dtime; rt_sn.dfdbk = roto.dfdbk; rt_sn.dcolor = roto.dcolor; rt_sn.dsend = roto.snd[rt_sn.part].dly;
    {
        uint32_t e, c;
        for (e = 0; e < 4u; e++) {
            rt_sn.flt[e] = roto.flt[rt_sn.part][e];
            for (c = 0; c < 5u && roto.flt_lbl[rt_sn.part][e][c]; c++) rt_sn.flt_lbl[e][c] = roto.flt_lbl[rt_sn.part][e][c];
            rt_sn.flt_lbl[e][c] = 0;
        }
    }
    rt_sn.a_chord = roto.arp_chord; rt_sn.a_mode = roto.arp_mode; rt_sn.a_speed = roto.arp_speed;
    rt_sn.a_range = roto.arp_range; rt_sn.a_gate = roto.arp_gate;
    rt_sn.a_len = roto.arp_len <= 16u ? roto.arp_len : 16u;
    for (s = 0; s < rt_sn.a_len; s++) rt_sn.a_seq[s] = roto.arp_seq[s];
    rt_sn.a_cur = roto.arp_cur < rt_sn.a_len && roto.arp_chord ? roto.arp_cur : 0xFFu;
    roto_arp_name(rt_sn.a_name);
    {
        const roto_sound_t *sd = &roto.snd[rt_sn.part];
        uint32_t c, i, e;
        rt_sn.spage = roto.spage <= 2u ? roto.spage : 0u;
        rt_sn.s_drum = sd->eng == ROTO_ENG_DRUM;
        roto_cat_find(sd->eng, sd->preset, &c, &i);
        rt_sn.s_cat = (uint8_t)c; rt_sn.s_idx = (uint8_t)i;
        rt_sn.s_kit = sd->kit; rt_sn.s_piece = (uint8_t)roto_piece_of(rt_sn.sel);
        rt_sn.s_level = sd->level; rt_sn.s_pan = sd->pan;
        for (e = 0; e < 8u; e++) {
            rt_sn.ed[e] = roto.ed[e];
            for (c = 0; c < 5u && roto.ed_lbl[e][c]; c++) rt_sn.ed_lbl[e][c] = roto.ed_lbl[e][c];
            rt_sn.ed_lbl[e][c] = 0;
            for (c = 0; c < 6u && roto.ed_txt[e][c]; c++) rt_sn.ed_txt[e][c] = roto.ed_txt[e][c];
            rt_sn.ed_txt[e][c] = 0;
        }
    }
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
    x = rt_text_b(x + 6, 4, roto.snd[rt_sn.part].name[0] ? roto.snd[rt_sn.part].name : "RING", 1, col);
    if (rt_sn.view == ROTO_VIEW_EUCLID) {               /* "EUCLID 3/5", as the reference */
        char e[16] = "EUCLID ";
        uint32_t n;
        rt_fmt_u(e + 7, rt_sn.hits);
        n = rt_strlen(e); e[n] = '/';
        rt_fmt_u(e + n + 1, rt_sn.len);
        rt_text_s(x + 8, 8, e, RT_TEXT);
    } else if (rt_sn.view == ROTO_VIEW_ENV) {
        rt_text_s(x + 8, 8, "ENVELOPE", RT_TEXT);
    } else if (rt_sn.view == ROTO_VIEW_FLT) {
        rt_text_s(x + 8, 8, "FILTER", RT_TEXT);
    } else if (rt_sn.view == ROTO_VIEW_DLY) {
        rt_text_s(x + 8, 8, "DELAY", RT_TEXT);
    } else if (rt_sn.view == ROTO_VIEW_ARP) {
        rt_text_s(x + 8, 8, rt_sn.a_chord ? "ARP" : "ARP OFF", RT_TEXT);
    } else if (rt_sn.view == ROTO_VIEW_SND) {
        rt_text_s(x + 8, 8, rt_sn.spage == 0u ? "SOUND" : rt_sn.spage == 1u ? "SHAPE 1/2" : "SHAPE 2/2", RT_TEXT);
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
            rt_rect(bx, 5, bx + 8, 16, tc);
            rt_text_s(bx + 2, 7, d, RT_BG);
        } else {
            rt_hspan(bx, bx + 8, 5, tc);
            rt_hspan(bx, bx + 8, 15, tc);
            rt_rect(bx, 5, bx + 1, 16, tc);
            rt_rect(bx + 7, 5, bx + 8, 16, tc);
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
    static const char *const RT_SHOWN[11] = {"", "LENGTH", "FILL", "ROTATE", "RATE", "PROB", "BPM", "ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
    static const char *const RT_DSHOWN[4] = {"TIME", "FEEDBACK", "SEND", "COLOUR"};
    int dly_show = rt_sn.show >= ROTO_SHOW_DTIME && rt_sn.show <= ROTO_SHOW_DCOLOR;
    int flt_show = rt_sn.show >= ROTO_SHOW_BRIGHT && rt_sn.show <= ROTO_SHOW_DRIVE;
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
    case ROTO_SHOW_ATK: case ROTO_SHOW_DEC: case ROTO_SHOW_SUS: case ROTO_SHOW_REL:
        rt_fmt_u(big, (rt_sn.env[rt_sn.show - ROTO_SHOW_ATK] * 100u + 63u) / 127u);
        w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0;
        break;
    case ROTO_SHOW_HOLD: case ROTO_SHOW_NEW: big[0] = 'N'; big[1] = 'E'; big[2] = 'W'; big[3] = 0; break;
    case ROTO_SHOW_DTIME: { const char *d = roto_dtime_name(rt_sn.dtime); uint32_t i; for (i = 0; d[i] && i < 6u; i++) big[i] = d[i]; big[i] = 0; } break;
    case ROTO_SHOW_DFDBK:  rt_fmt_u(big, rt_sn.dfdbk); w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0; break;
    case ROTO_SHOW_DSEND:  rt_fmt_u(big, (rt_sn.dsend * 100u + 63u) / 127u); w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0; break;
    case ROTO_SHOW_DCOLOR: rt_fmt_u(big, (rt_sn.dcolor * 100u + 63u) / 127u); w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0; break;
    case ROTO_SHOW_BRIGHT: case ROTO_SHOW_EDGE: case ROTO_SHOW_FENV: case ROTO_SHOW_DRIVE: {
        int32_t v = rt_sn.flt[rt_sn.show - ROTO_SHOW_BRIGHT];
        if (v == -128) { big[0] = '-'; big[1] = 0; break; }
        w = 0;
        if (rt_sn.show == ROTO_SHOW_FENV) { if (v > 0) big[w++] = '+'; if (v < 0) { big[w++] = '-'; v = -v; } }
        rt_fmt_u(big + w, (uint32_t)v);
        w = rt_strlen(big); big[w] = '%'; big[w + 1] = 0;
        break;
    }
    default:               rt_fmt_u(big, rt_sn.hits); break;
    }
    if (rt_sn.show == ROTO_SHOW_HOLD || rt_sn.show == ROTO_SHOW_NEW) {
        const char *l = rt_sn.show == ROTO_SHOW_HOLD ? "KEEP HOLDING" : "ALL CLEAR";
        uint32_t i;
        for (i = 0; l[i]; i++) small[i] = l[i];
        small[i] = 0;
        small_col = rt_sn.show == ROTO_SHOW_HOLD ? RT_PINK : RT_TRK_COL[rt_sn.sel];
    } else if (dly_show) {
        const char *l = RT_DSHOWN[rt_sn.show - ROTO_SHOW_DTIME];
        uint32_t i;
        for (i = 0; l[i]; i++) small[i] = l[i];
        small[i] = 0;
        small_col = RT_TRK_COL[rt_sn.sel];
    } else if (flt_show) {                                   /* the sound's own name for it */
        const char *l = rt_sn.flt[rt_sn.show - ROTO_SHOW_BRIGHT] == -128
                      ? (rt_sn.show == ROTO_SHOW_FENV ? "NO ENV" : "NONE") : rt_sn.flt_lbl[rt_sn.show - ROTO_SHOW_BRIGHT];
        uint32_t i;
        for (i = 0; l[i] && i < 15u; i++) small[i] = l[i];
        small[i] = 0;
        small_col = RT_TRK_COL[rt_sn.sel];
    } else if (rt_sn.show && rt_sn.show < 11u) {
        const char *l = RT_SHOWN[rt_sn.show];
        uint32_t i;
        for (i = 0; l[i]; i++) small[i] = l[i];
        small[i] = 0;
        small_col = RT_TRK_COL[rt_sn.sel];
    } else {
        small[0] = 'O'; small[1] = 'F'; small[2] = ' ';
        rt_fmt_u(small + 3, rt_sn.len);
    }
    if (rt_strlen(big) > 4u)                                 /* (5 wide, e.g. 1/16D: half size) */
        rt_text_b(RT_CX - (int32_t)rt_strlen(big) * 4, RT_CY - 10, big, 1, RT_TEXT);
    else
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

/* ------------------------------------------------------------- envelope
 * The ring's envelope as an orbit: going round from the top is time, the
 * distance from the centre is level (the dim inner circle is silence, the dim
 * outer one full). Attack climbs out, decay falls to the sustain level, sustain
 * holds for a fixed stretch, release falls back in. The joins get a small
 * circled plus, the stretches their letter outside. */
#define RT_ENV_R0  42                       /* level 0 */
#define RT_ENV_R1  90                       /* level 127 */
#define RT_ENV_S   160                      /* the sustain stretch (1024 a turn) */

static int32_t rt_env_span(uint32_t v) { return 24 + (int32_t)((v * 256u) / 127u); }   /* 24..280 */

static int32_t rt_env_r(int32_t level) { return RT_ENV_R0 + (level * (RT_ENV_R1 - RT_ENV_R0)) / 127; }

/* one stretch from level l0 to l1 over angles a0..a1; curved: 0 straight, 1 falls fast then eases */
static void rt_env_seg(int32_t a0, int32_t a1, int32_t l0, int32_t l1, int curved, uint16_t c)
{
    int32_t px = 0, py = 0, x, y, i, n = (a1 - a0) / 6 + 1;
    for (i = 0; i <= n; i++) {
        int32_t a = a0 + ((a1 - a0) * i) / n, l;
        if (curved) {                                    /* l1 + (l0 - l1) * (1 - t)^2 */
            int32_t u = n - i;
            l = l1 + ((l0 - l1) * u * u) / (n * n);
        } else {
            l = l0 + ((l1 - l0) * i) / n;
        }
        rt_polar(a, rt_env_r(l), &x, &y);
        if (i) rt_line(px, py, x, y, c);
        px = x; py = y;
    }
}

static void rt_env_join(int32_t a, int32_t level)
{
    int32_t x, y;
    rt_polar(a, rt_env_r(level), &x, &y);
    rt_diamond_fill(x, y, 3, RT_BG);
    rt_circle(x, y, 4, RT_GREY);
    rt_hspan(x - 2, x + 3, y, RT_TEXT);
    rt_rect(x, y - 2, x + 1, y + 3, RT_TEXT);
}

static void rt_env_label(int32_t a, const char *t)
{
    int32_t x, y;
    rt_polar(a, RT_ENV_R1 + 13, &x, &y);
    rt_text_s(x - 2, y - 3, t, RT_GREY);
}

static void rt_draw_env(void)
{
    uint16_t col = RT_TRK_COL[rt_sn.sel];
    int32_t sus = rt_sn.env[2];
    int32_t a0 = -256, a1, a2, a3, a4;
    a1 = a0 + rt_env_span(rt_sn.env[0]);
    a2 = a1 + rt_env_span(rt_sn.env[1]);
    a3 = a2 + RT_ENV_S;
    a4 = a3 + rt_env_span(rt_sn.env[3]);

    rt_circle(RT_CX, RT_CY, RT_ENV_R0, RT_TRK_RING[rt_sn.sel]);
    rt_circle(RT_CX, RT_CY, RT_ENV_R1, RT_TRK_RING[rt_sn.sel]);
    {                                                    /* where it starts: a spoke at the top */
        int32_t x0, y0, x1, y1;
        rt_polar(a0, RT_ENV_R0 - 6, &x0, &y0);
        rt_polar(a0, RT_ENV_R1 + 6, &x1, &y1);
        rt_line(x0, y0, x1, y1, RT_LINE);
    }

    rt_env_seg(a0, a1, 0, 127, 0, col);                  /* attack */
    rt_env_seg(a1, a2, 127, sus, 1, col);                /* decay */
    rt_env_seg(a2, a3, sus, sus, 0, col);                /* sustain */
    rt_env_seg(a3, a4, sus, 0, 1, col);                  /* release */

    rt_env_join(a1, 127);
    rt_env_join(a2, sus);
    rt_env_join(a3, sus);
    rt_env_label((a0 + a1) / 2, "A");
    rt_env_label((a1 + a2) / 2, "D");
    rt_env_label((a2 + a3) / 2, "S");
    rt_env_label((a3 + a4) / 2, "R");
    {                                                    /* the end of the release: a pink dot on silence */
        int32_t x, y;
        rt_polar(a4, RT_ENV_R0, &x, &y);
        rt_rect(x - 1, y - 1, x + 2, y + 2, RT_PINK);
    }

    if (rt_sn.show) {
        rt_draw_centre();
    } else {                                             /* at rest: the four, small */
        char l1[16], l2[16];
        uint32_t n;
        l1[0] = 'A'; l1[1] = ' '; rt_fmt_u(l1 + 2, (rt_sn.env[0] * 100u + 63u) / 127u);
        n = rt_strlen(l1); l1[n] = ' '; l1[n + 1] = ' '; l1[n + 2] = 'D'; l1[n + 3] = ' ';
        rt_fmt_u(l1 + n + 4, (rt_sn.env[1] * 100u + 63u) / 127u);
        l2[0] = 'S'; l2[1] = ' '; rt_fmt_u(l2 + 2, (rt_sn.env[2] * 100u + 63u) / 127u);
        n = rt_strlen(l2); l2[n] = ' '; l2[n + 1] = ' '; l2[n + 2] = 'R'; l2[n + 3] = ' ';
        rt_fmt_u(l2 + n + 4, (rt_sn.env[3] * 100u + 63u) / 127u);
        rt_text_s(RT_CX - RT_TEXT_S_W(l1) / 2, RT_CY - 9, l1, RT_GREY);
        rt_text_s(RT_CX - RT_TEXT_S_W(l2) / 2, RT_CY + 3, l2, RT_GREY);
    }
}

/* ------------------------------------------------------------- filter
 * After the "lens" reference: white lines and green on black.
 *   K1 BRIGHT  nested circles, all touching at one point on the right; they
 *              spread out as the filter opens and huddle in as it closes
 *   K2 EDGE    the green cone; more edge, a longer, narrower spike
 *   K3 ENV     the small circle riding the big one: it runs round on each hit
 *              (as far as the envelope takes the tone) and falls back
 *   K4 DRIVE   the long lines through the focus; more drive, more and steeper
 * Each control has a box with its letter on its line; turning it lights that
 * line green, and its full name and value show along the bottom. */
#define RT_F_CX   108          /* the big circle */
#define RT_F_CY   122
#define RT_F_R    86
#define RT_F_PX   194          /* the focus: where the nested circles touch */
#define RT_F_N    6
#define RT_F_Y0   24           /* the scene's rows (lines are clipped to these) */
#define RT_F_Y1   214
#define RT_F_SR   9            /* the small circle */
#define RT_FW     RT_RGB(176, 180, 176)
#define RT_FDIM   RT_RGB(56, 60, 56)
#define RT_FGRN   RT_RGB(44, 150, 110)
#define RT_FHOT   RT_RGB(96, 236, 170)

static int32_t rt_isqrt(int32_t v)
{
    int32_t r = 0, b = 1 << 30;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* a value 0..100 (or the middle when the sound has none) */
static int32_t rt_fv(uint32_t k, int32_t none)
{
    int32_t v = rt_sn.flt[k];
    return v == -128 ? none : v;
}

/* the small circle's centre: at rest top left; a hit sends it round by the
 * envelope amount, and it falls back over the next three steps */
static void rt_flt_orb(int32_t *x, int32_t *y)
{
    int32_t a = -330, back = 0, k = rt_sn.flt[2];
    uint32_t n = rt_sn.len ? rt_sn.len : 1u;
    if (k != -128 && k) {
        while (back < 3 && !rt_sn.on[(rt_sn.pos + n - (uint32_t)back) % n]) back++;
        a += (k * 170 * (3 - back)) / 300;
    }
    *x = RT_F_CX + rt_round14(rt_cos(a) * (RT_F_R - RT_F_SR));
    *y = RT_F_CY + rt_round14(rt_sin(a) * (RT_F_R - RT_F_SR));
}

/* a line clipped to the scene's rows */
static void rt_fline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t a = rt_cy0, b = rt_cy1;
    if (rt_cy0 < RT_F_Y0) rt_cy0 = RT_F_Y0;
    if (rt_cy1 > RT_F_Y1) rt_cy1 = RT_F_Y1;
    if (rt_cy1 > rt_cy0) rt_line(x0, y0, x1, y1, c);
    rt_cy0 = a; rt_cy1 = b;
}

/* the letter box: outlined on black, or filled green while its knob turns */
static void rt_fbox(int32_t x, int32_t y, char ch, uint16_t c, int hot)
{
    char t[2] = {ch, 0};
    rt_rect(x - 5, y - 6, x + 6, y + 7, hot ? RT_FHOT : RT_BG);
    if (!hot) {
        rt_hspan(x - 5, x + 6, y - 6, c); rt_hspan(x - 5, x + 6, y + 6, c);
        rt_rect(x - 5, y - 6, x - 4, y + 7, c); rt_rect(x + 5, y - 6, x + 6, y + 7, c);
    }
    rt_text_s(x - 2, y - 3, t, hot ? RT_BG : c);
}

/* the four letters: each name's first, or its next one if that is taken */
static void rt_flt_letters(char out[4])
{
    uint32_t k, j, i;
    for (k = 0; k < 4u; k++) {
        const char *l = k == 2u ? "ENV" : rt_sn.flt_lbl[k];
        out[k] = l[0] ? l[0] : (char)"BEED"[k];
        for (i = 0; l[i]; i++) {
            int taken = 0;
            for (j = 0; j < k; j++) taken |= out[j] == l[i];
            if (!taken && l[i] != ' ') { out[k] = l[i]; break; }
        }
    }
}

static void rt_draw_flt(void)
{
    int32_t b = rt_fv(0, 50), e = rt_fv(1, 0), d = rt_fv(3, 0);
    int hot = rt_sn.show >= ROTO_SHOW_BRIGHT && rt_sn.show <= ROTO_SHOW_DRIVE ? (int)(rt_sn.show - ROTO_SHOW_BRIGHT) + 1 : 0;
    int32_t sp = 4 + (b * 10) / 100, i, gr, gx, ax, L, n, spread;
    int32_t kx[4], ky[4];
    char let[4];

    /* K4: the long lines through just past the focus */
    n = 2 + (d * 3 + 50) / 100;
    spread = 24 + (d * 96) / 100;
    for (i = 0; i < n; i++) {
        int32_t a = 512 - spread + (2 * spread * i) / (n - 1), qx = RT_F_PX + 12;
        int32_t c = rt_cos(a), s = rt_sin(a);
        rt_fline(qx - rt_round14(c * 40), RT_F_CY - rt_round14(s * 40),
                 qx + rt_round14(c * 260), RT_F_CY + rt_round14(s * 260),
                 hot == 4 ? RT_FHOT : rt_sn.flt[3] == -128 ? RT_FDIM : RT_FW);
        if (i == n - 1) {                       /* its box: on the top line at the left edge, kept below the header */
            int32_t dist = ((qx - 14) << 14) / (c < -1 ? -c : 1), sy = -s;
            if (sy > 0 && sy * dist > (RT_F_CY - 36) << 14) dist = ((RT_F_CY - 36) << 14) / sy;
            kx[3] = qx + rt_round14(c * dist); ky[3] = RT_F_CY + rt_round14(s * dist);
        }
    }

    rt_circle(RT_F_CX, RT_F_CY, RT_F_R, RT_FW);

    /* K1: the nested circles, all touching the focus; the fourth is the lens; the box at the bottom */
    for (i = 0; i < RT_F_N; i++) {
        int32_t r = 10 + i * sp;
        rt_circle(RT_F_PX - r, RT_F_CY, r, hot == 1 ? RT_FHOT : i == 3 ? RT_FGRN : RT_FW);
    }
    { int32_t r = 10 + (RT_F_N - 1) * sp; kx[0] = RT_F_PX - r; ky[0] = RT_F_CY + r; }

    /* K2: the cone from its tip, touching the lens top and bottom, on past it */
    gr = 10 + 3 * sp; gx = RT_F_PX - gr;
    L = gr + 16 + (e * (gx - 6 - gr - 16)) / 100;
    ax = gx - L;
    {
        uint16_t c = hot == 2 ? RT_FHOT : rt_sn.flt[1] == -128 ? RT_FDIM : RT_FGRN;
        int32_t tx = gx - (gr * gr) / L, ty = (gr * rt_isqrt(L * L - gr * gr)) / L, k;
        for (k = -1; k <= 1; k += 2) {
            int32_t ex = RT_F_PX + 20, ey = RT_F_CY + k * ((ty * (ex - ax)) / (tx - ax));
            rt_fline(ax, RT_F_CY, ex, ey, c);
        }
        rt_hspan(ax, gx, RT_F_CY, c);                       /* the axis to the cutoff ... */
        rt_rect(gx, RT_F_CY - 4, gx + 1, RT_F_CY + 5, c);   /* ... and its tick */
        kx[1] = ax; ky[1] = RT_F_CY;
    }

    /* K3: the small circle */
    rt_flt_orb(&kx[2], &ky[2]);
    rt_circle(kx[2], ky[2], RT_F_SR, hot == 3 ? RT_FHOT : rt_sn.flt[2] == -128 ? RT_FDIM : RT_FW);

    rt_flt_letters(let);
    for (i = 0; i < 4; i++)
        rt_fbox(kx[i], ky[i], let[i], rt_sn.flt[i] == -128 ? RT_FDIM : i == 1 ? RT_FGRN : RT_FW, hot == i + 1);

    if (hot) {                                              /* the full name and value, big */
        char line[16];
        uint32_t k = (uint32_t)hot - 1u, c, w = 0;
        int32_t v = rt_sn.flt[k];
        const char *l = v == -128 ? (k == 2u ? "NO ENV" : "NONE") : k == 2u ? "ENV" : rt_sn.flt_lbl[k];
        for (c = 0; l[c] && c < 6u; c++) line[w++] = l[c];
        if (v != -128) {
            line[w++] = ' ';
            if (k == 2u && v > 0) line[w++] = '+';
            if (v < 0) { line[w++] = '-'; v = -v; }
            rt_fmt_u(line + w, (uint32_t)v);
            w = rt_strlen(line);
            line[w++] = '%';
        }
        line[w] = 0;
        rt_text_b(RT_CX - (int32_t)w * 4, RT_H - 20, line, 1, RT_FHOT);
    } else {                                                /* the four, small */
        char line[48];
        uint32_t k, n2 = 0;
        for (k = 0; k < 4u; k++) {
            const char *l = k == 2u ? "ENV" : rt_sn.flt_lbl[k];
            int32_t v = rt_sn.flt[k];
            uint32_t c;
            if (k) { line[n2++] = ' '; line[n2++] = ' '; }
            for (c = 0; l[c] && c < 5u; c++) line[n2++] = l[c];
            line[n2++] = ' ';
            if (v == -128) { line[n2++] = '-'; continue; }
            if (k == 2u && v > 0) line[n2++] = '+';
            if (v < 0) { line[n2++] = '-'; v = -v; }
            line[n2] = 0;
            rt_fmt_u(line + n2, (uint32_t)v);
            n2 = rt_strlen(line);
        }
        line[n2] = 0;
        rt_text_s(RT_CX - RT_TEXT_S_W(line) / 2, RT_H - 15, line, RT_GREY);
    }
}

/* ------------------------------------------------------------- arp
 * After the gauge reference: warm yellow on black, a dome over a crosshair.
 * The dome is the keyboard, low on the left, high on the right; the chord's
 * notes sit on the dashed arcs, one arc per octave (RANGE lights more). The
 * needle swings from the centre to the note playing now. The ticks round the
 * dome are the SPEED, the ellipse is the NOTE LENGTH, the icon is the MODE.
 * Along the bottom: the chord and mode; while a dial turns, its name and value. */
#define RT_A_X    120          /* the pivot */
#define RT_A_Y    166
#define RT_A_R    104          /* the dome */
#define RT_A_BASE 182          /* its base line */
#define RT_A_Y1   194          /* the drawing stops above the readout */
#define RT_AY     RT_RGB(240, 230, 148)
#define RT_AM     RT_RGB(176, 168, 104)
#define RT_AD     RT_RGB(88, 84, 52)

static void rt_apol(int32_t a, int32_t r, int32_t *x, int32_t *y)
{
    *x = RT_A_X + rt_round14(rt_cos(a) * r);
    *y = RT_A_Y + rt_round14(rt_sin(a) * r);
}

/* a line kept above the readout and below the header */
static void rt_aline(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t a = rt_cy0, b = rt_cy1;
    if (rt_cy0 < RT_RULE_Y + 4) rt_cy0 = RT_RULE_Y + 4;
    if (rt_cy1 > RT_A_Y1) rt_cy1 = RT_A_Y1;
    if (rt_cy1 > rt_cy0) rt_line(x0, y0, x1, y1, c);
    rt_cy0 = a; rt_cy1 = b;
}

/* the octave arcs: the top octave outermost */
static int32_t rt_arc_r(uint32_t o, uint32_t range) { return 94 - (int32_t)(range - 1u - o) * 19; }

/* note i of the chord: where it sits */
static void rt_anote(uint32_t i, int32_t *x, int32_t *y)
{
    uint32_t n = rt_sn.a_len, range = rt_sn.a_range ? rt_sn.a_range : 1u;
    int32_t lo = rt_sn.a_seq[0], hi = rt_sn.a_seq[n ? n - 1u : 0u], a;
    a = 512 + 34 + (hi > lo ? ((rt_sn.a_seq[i] - lo) * (512 - 68)) / (hi - lo) : 222);
    rt_apol(a, rt_arc_r(i / 4u, range), x, y);
}

static void rt_dash_arc(int32_t r, uint16_t c)
{
    int32_t a, x, y, px = 0, py = 0, keep = rt_cy1;
    if (rt_cy1 > RT_A_BASE) rt_cy1 = RT_A_BASE;           /* (inside the dome only) */
    for (a = 512 - 48; a <= 1024 + 48; a += 3) {
        rt_apol(a, r, &x, &y);
        if (((a + 600) / 14) & 1) rt_aline(px, py, x, y, c);
        px = x; py = y;
    }
    rt_cy1 = keep;
}

static void rt_ring_at(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t a = rt_cy1;
    if (rt_cy1 > RT_A_Y1) rt_cy1 = RT_A_Y1;
    rt_circle(x, y, r, c);
    rt_cy1 = a;
}

/* the needle: a long thin triangle from the pivot to note i */
static void rt_aneedle(uint32_t i, uint16_t c)
{
    int32_t nx, ny, x0, y0, x1, y1, dx, dy, len, bx, by, px, py;
    rt_anote(i, &nx, &ny);
    dx = nx - RT_A_X; dy = ny - RT_A_Y;
    len = rt_isqrt(dx * dx + dy * dy);
    if (len < 24) return;
    bx = RT_A_X + (dx * (len - 9)) / len; by = RT_A_Y + (dy * (len - 9)) / len;   /* the wide end, short of the note */
    px = (-dy * 5) / len; py = (dx * 5) / len;
    x0 = RT_A_X + (dx * 12) / len; y0 = RT_A_Y + (dy * 12) / len;                 /* the tip, at the pivot's ring */
    x1 = bx + px; y1 = by + py;
    rt_aline(x0, y0, x1, y1, c);
    rt_aline(x0, y0, bx - px, by - py, c);
    rt_aline(x1, y1, bx - px, by - py, c);
}

/* the box the needle and its note cover (for the repaint when it moves) */
static void rt_aneedle_box(uint32_t i, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
    int32_t nx, ny;
    rt_anote(i, &nx, &ny);
    *x0 = (nx < RT_A_X ? nx : RT_A_X) - 9; *x1 = (nx > RT_A_X ? nx : RT_A_X) + 10;
    *y0 = (ny < RT_A_Y ? ny : RT_A_Y) - 9; *y1 = (ny > RT_A_Y ? ny : RT_A_Y) + 10;
}

static void rt_tri_up(int32_t x, int32_t y, int32_t h, int up, uint16_t c)   /* filled, apex at (x, y) */
{
    int32_t i;
    for (i = 0; i < h; i++) rt_hspan(x - i, x + i + 1, up ? y + i : y - i, c);
}

static void rt_amode_icon(int32_t x, int32_t y, uint32_t m, uint16_t c)
{
    switch (m) {
    case ROTO_ARP_UP:     rt_tri_up(x, y - 6, 6, 1, c); rt_rect(x - 1, y, x + 2, y + 7, c); break;
    case ROTO_ARP_DOWN:   rt_tri_up(x, y + 6, 6, 0, c); rt_rect(x - 1, y - 7, x + 2, y, c); break;
    case ROTO_ARP_UPDOWN: rt_tri_up(x - 4, y - 7, 5, 1, c); rt_rect(x - 5, y - 2, x - 3, y + 7, c);
                          rt_tri_up(x + 4, y + 7, 5, 0, c); rt_rect(x + 3, y - 7, x + 5, y + 2, c);
                          rt_rect(x - 8, y, x + 9, y + 1, c); break;
    case ROTO_ARP_RANDOM: rt_rect(x - 7, y - 6, x - 3, y - 2, c); rt_rect(x + 2, y - 2, x + 6, y + 2, c);
                          rt_rect(x - 4, y + 3, x, y + 7, c); rt_rect(x + 5, y - 8, x + 8, y - 5, c); break;
    default:              rt_tri_up(x, y - 6, 6, 1, c); rt_rect(x - 1, y, x + 2, y + 4, c);
                          rt_rect(x - 7, y + 5, x + 8, y + 7, c); break;    /* PEDAL: up from a floor */
    }
}

static void rt_draw_arp(void)
{
    static const char *const MODE_N[ROTO_ARP_MODES] = {"UP", "DOWN", "UP-DOWN", "RANDOM", "PEDAL"};
    static const char *const SPD_N[4] = {"1/32", "1/16", "1/8", "1/4"};
    static const uint8_t TICKS[4] = {32, 16, 8, 4};
    uint32_t show = rt_sn.show >= ROTO_SHOW_ACHORD && rt_sn.show <= ROTO_SHOW_ALEN ? rt_sn.show : 0u;
    uint32_t range = rt_sn.a_range ? rt_sn.a_range : 1u, n = rt_sn.a_len, i, o;
    int32_t x, y, x1, y1, a;
    char l[16], r[16];

    /* the frame: crosshair, the two diagonals and their end marks */
    rt_rect(RT_A_X, RT_RULE_Y + 6, RT_A_X + 1, RT_H - 6, RT_AM);
    rt_hspan(6, RT_W - 6, RT_A_Y, RT_AM);
    for (a = 640; a <= 896; a += 256) {
        rt_apol(a, 112, &x, &y); rt_apol(a + 512, 44, &x1, &y1);
        rt_aline(x, y, x1, y1, RT_AD);
        rt_apol(a, 118, &x, &y); rt_apol(a, 134, &x1, &y1);
        rt_aline(x, y, x1, y1, RT_AM);
    }

    /* the dome, its base, and the SPEED ticks inside it */
    for (a = 512; a < 1024; a += 2) {
        rt_apol(a, RT_A_R, &x, &y); rt_apol(a + 2, RT_A_R, &x1, &y1);
        rt_aline(x, y, x1, y1, RT_AY);
    }
    rt_rect(RT_A_X - RT_A_R, RT_A_Y, RT_A_X - RT_A_R + 1, RT_A_BASE, RT_AY);
    rt_rect(RT_A_X + RT_A_R, RT_A_Y, RT_A_X + RT_A_R + 1, RT_A_BASE, RT_AY);
    rt_hspan(RT_A_X - RT_A_R, RT_A_X + RT_A_R + 1, RT_A_BASE, RT_AY);
    {
        uint32_t t = TICKS[rt_sn.a_speed & 3u];
        uint16_t c = show == ROTO_SHOW_ASPEED ? RT_AY : RT_AM;
        for (i = 1; i < t; i++) {
            a = 512 + (int32_t)((i * 512u) / t);
            rt_apol(a, RT_A_R - 2, &x, &y); rt_apol(a, RT_A_R - (i % 4u ? 5 : 8), &x1, &y1);
            rt_aline(x, y, x1, y1, c);
        }
    }

    /* RANGE: the octave arcs, lit as far as it reaches */
    for (o = 0; o < 4u; o++) {
        int lit = o >= 4u - range;
        rt_dash_arc(94 - (int32_t)(3u - o) * 19, lit ? (show == ROTO_SHOW_ARANGE ? RT_AY : RT_AM) : RT_AD);
    }

    /* NOTE LENGTH: the ellipse, fatter for longer notes */
    {
        int32_t b = 4 + (rt_sn.a_gate * 30) / 100, t, px = 0, py = 0, ca = rt_cos(-52), sa = rt_sin(-52);
        uint16_t c = show == ROTO_SHOW_ALEN ? RT_AY : RT_AM;
        for (t = 0; t <= 1024; t += 8) {
            int32_t ex = rt_round14(rt_cos(t) * 80), ey = rt_round14(rt_sin(t) * b);
            x = 150 + rt_round14(ex * ca - ey * sa);
            y = 136 + rt_round14(ex * sa + ey * ca);
            if (t) rt_aline(px, py, x, y, c);
            px = x; py = y;
        }
    }

    rt_ring_at(RT_A_X, RT_A_Y, 11, RT_AY);                  /* the pivot */
    rt_amode_icon(78, 92, rt_sn.a_mode, show == ROTO_SHOW_AMODE ? RT_AY : RT_AM);

    /* CHORD: its notes on their arcs, the one playing lit, the needle to it */
    if (rt_sn.a_chord && n) {
        for (i = 0; i < n; i++) {
            rt_anote(i, &x, &y);
            if (i == rt_sn.a_cur) continue;
            rt_rect(x - 2, y - 2, x + 3, y + 3, RT_BG);
            rt_ring_at(x, y, 2, show == ROTO_SHOW_ACHORD ? RT_AY : RT_AM);
        }
        if (rt_sn.a_cur < n) {
            rt_aneedle(rt_sn.a_cur, RT_AY);
            rt_anote(rt_sn.a_cur, &x, &y);
            rt_rect(x - 6, y - 6, x + 7, y + 7, RT_BG);
            rt_ring_at(x, y, 6, RT_AY);
            rt_rect(x - 2, y - 2, x + 3, y + 3, RT_AY);
        }
    }

    /* the bars: the dial's setting on the left, the octaves on the right */
    {
        uint32_t f = 0;
        switch (show) {
        case ROTO_SHOW_ACHORD: f = rt_sn.a_chord * 100u / 7u; break;
        case ROTO_SHOW_AMODE:  f = (rt_sn.a_mode + 1u) * 100u / ROTO_ARP_MODES; break;
        case ROTO_SHOW_ASPEED: f = (4u - (rt_sn.a_speed & 3u)) * 25u; break;
        case ROTO_SHOW_ARANGE: f = range * 25u; break;
        default:               f = rt_sn.a_gate; break;
        }
        rt_rect(16, 198, 16 + 80, 199, RT_AD);
        rt_rect(16, 196, 16 + (int32_t)(f * 80u / 100u), 201, show ? RT_AY : RT_AM);
        for (o = 0; o < 4u; o++)
            rt_rect(152 + (int32_t)o * 19, 196, 152 + (int32_t)o * 19 + 14, 201, o < range ? (show == ROTO_SHOW_ARANGE ? RT_AY : RT_AM) : RT_AD);
    }

    /* the readout */
    {
        const char *v = 0;
        uint32_t k = 0;
        l[0] = r[0] = 0;
        switch (show) {
        case ROTO_SHOW_ACHORD: v = "CHORD"; break;
        case ROTO_SHOW_AMODE:  v = "MODE"; break;
        case ROTO_SHOW_ASPEED: v = "SPEED"; break;
        case ROTO_SHOW_ARANGE: v = "RANGE"; break;
        case ROTO_SHOW_ALEN:   v = "LENGTH"; break;
        }
        if (v) { for (k = 0; v[k]; k++) l[k] = v[k]; l[k] = 0; }
        if (show == ROTO_SHOW_ACHORD || !show) {
            char nm[12];
            for (k = 0; k < 11u && rt_sn.a_name[k]; k++) nm[k] = rt_sn.a_name[k];
            nm[k] = 0;
            if (!show) { for (k = 0; nm[k]; k++) l[k] = nm[k]; l[k] = 0; }
            else { for (k = 0; nm[k]; k++) r[k] = nm[k]; r[k] = 0; }
        }
        if (!show) {
            if (!rt_sn.a_chord) { const char *t = "TURN K1"; for (k = 0; t[k]; k++) r[k] = t[k]; r[k] = 0; }
            else {
                const char *m = MODE_N[rt_sn.a_mode % ROTO_ARP_MODES], *s = SPD_N[rt_sn.a_speed & 3u];
                for (k = 0; *m; m++) r[k++] = *m;
                r[k++] = ' ';
                for (; *s; s++) r[k++] = *s;
                r[k] = 0;
            }
        } else if (show == ROTO_SHOW_AMODE) { const char *m = MODE_N[rt_sn.a_mode % ROTO_ARP_MODES]; for (k = 0; m[k]; k++) r[k] = m[k]; r[k] = 0; }
        else if (show == ROTO_SHOW_ASPEED) { const char *s = SPD_N[rt_sn.a_speed & 3u]; for (k = 0; s[k]; k++) r[k] = s[k]; r[k] = 0; }
        else if (show == ROTO_SHOW_ARANGE) { rt_fmt_u(r, range); k = rt_strlen(r); r[k] = ' '; r[k+1] = 'O'; r[k+2] = 'C'; r[k+3] = 'T'; r[k+4] = 0; }
        else if (show == ROTO_SHOW_ALEN) { rt_fmt_u(r, rt_sn.a_gate); k = rt_strlen(r); r[k] = '%'; r[k+1] = 0; }
        rt_text_b(16, 212, l, 1, show ? RT_AY : RT_AM);
        rt_text_b(224 - (int32_t)rt_strlen(r) * 8, 212, r, 1, show ? RT_AY : RT_AM);
    }
}

/* ------------------------------------------------------------- sound
 * CHOOSE (page 0): the kinds as arcs round the outside (the one you're in lit), the
 * sounds of that kind as dots on the ring inside (the one playing filled), its name big
 * in the middle. Drums: the kits outside, the pieces inside, this ring's piece in the middle.
 * SHAPE (pages 1, 2): four spokes, one per setting, the value as a point along each;
 * the points joined make the sound's shape. A letter box at each spoke's end. */
static void rt_arc(int32_t a0, int32_t a1, int32_t r, uint16_t c)
{
    int32_t a, x, y, px = 0, py = 0;
    for (a = a0; a <= a1; a += 4) {
        rt_polar(a, r, &x, &y);
        if (a > a0) rt_line(px, py, x, y, c);
        px = x; py = y;
    }
}

static void rt_centre_text(int32_t y, const char *t, uint16_t c, int big)
{
    if (big) rt_text_b(RT_CX - (int32_t)rt_strlen(t) * 4, y, t, 1, c);
    else rt_text_s(RT_CX - RT_TEXT_S_W(t) / 2, y, t, c);
}

/* the bottom: a dial's name and value while it turns (big), else the hint (small, grey) */
static void rt_snd_bottom(const char *name, const char *val, const char *hint)
{
    char line[24];
    uint32_t i = 0, k;
    if (name) {
        for (k = 0; name[k] && i < 10u; k++) line[i++] = name[k];
        if (i) line[i++] = ' ';
        for (k = 0; val[k] && i < 22u; k++) line[i++] = val[k];
        line[i] = 0;
        rt_centre_text(RT_H - 20, line, RT_TRK_COL[rt_sn.sel], 1);
    } else {
        rt_centre_text(RT_H - 13, hint, RT_GREY, 0);
    }
}

static void rt_draw_snd_choose(void)
{
    uint16_t col = RT_TRK_COL[rt_sn.sel], half = RT_TRK_HALF[rt_sn.sel];
    uint32_t nout = rt_sn.s_drum ? roto_kit_count() : roto_cat_count();
    uint32_t cur_out = rt_sn.s_drum ? rt_sn.s_kit : rt_sn.s_cat;
    uint32_t nin = rt_sn.s_drum ? roto_piece_count() : roto_cat_size(rt_sn.s_cat);
    uint32_t cur_in = rt_sn.s_drum ? rt_sn.s_piece : rt_sn.s_idx, i;
    uint32_t show = rt_sn.show >= ROTO_SHOW_S1 && rt_sn.show <= ROTO_SHOW_S4 ? rt_sn.show - ROTO_SHOW_S1 + 1u : 0u;
    int32_t x, y;
    char val[16];

    for (i = 0; i < nout; i++) {                                 /* the kinds (kits) */
        int32_t a0 = -256 + (int32_t)((i * 1024u) / nout) + 10, a1 = -256 + (int32_t)(((i + 1u) * 1024u) / nout) - 10;
        rt_arc(a0, a1, 90, i == cur_out ? col : half);
        if (i == cur_out) rt_arc(a0, a1, 87, col);
    }
    for (i = 0; i < nin; i++) {                                  /* the sounds (pieces) */
        rt_polar(-256 + (int32_t)((i * 1024u) / nin), 68, &x, &y);
        if (i == cur_in) rt_diamond_fill(x, y, 4, col);
        else rt_diamond_line(x, y, 2, RT_LINE);
    }
    rt_circle(RT_CX, RT_CY, 68, RT_TRK_RING[rt_sn.sel]);

    rt_centre_text(RT_CY - 26, rt_sn.s_drum ? roto_kit_name(rt_sn.s_kit) : roto_cat_name(rt_sn.s_cat), col, 0);
    rt_centre_text(RT_CY - 10, rt_sn.s_drum ? roto_piece_name(rt_sn.s_piece) : roto.snd[rt_sn.part].name, RT_TEXT, 1);
    rt_fmt_u(val, cur_in + 1u);
    i = rt_strlen(val); val[i++] = ' '; val[i++] = '/'; val[i++] = ' ';
    rt_fmt_u(val + i, nin);
    rt_centre_text(RT_CY + 12, val, RT_GREY, 0);
    if (rt_sn.s_drum && rt_sn.sel != 0u) rt_centre_text(RT_CY + 24, "KIT SHARED", RT_PINK, 0);

    switch (show) {
    case 1: rt_snd_bottom(rt_sn.s_drum ? "" : "KIND", rt_sn.s_drum ? roto_kit_name(rt_sn.s_kit) : roto_cat_name(rt_sn.s_cat), 0); break;
    case 2: rt_snd_bottom(rt_sn.s_drum ? "PIECE" : "SOUND", rt_sn.s_drum ? roto_piece_name(rt_sn.s_piece) : roto.snd[rt_sn.part].name, 0); break;
    case 3: rt_fmt_u(val, (rt_sn.s_level * 100u + 63u) / 127u); i = rt_strlen(val); val[i] = '%'; val[i + 1] = 0; rt_snd_bottom("LEVEL", val, 0); break;
    case 4: {
        int32_t p = rt_sn.s_pan;
        val[0] = p < 0 ? 'L' : p > 0 ? 'R' : 'C';
        if (p) rt_fmt_u(val + 1, (uint32_t)(p < 0 ? -p : p)); else val[1] = 0;
        rt_snd_bottom("PAN", val, 0);
        break;
    }
    default: rt_snd_bottom(0, 0, rt_sn.s_drum ? "K1 KIT  K2 PIECE  K3 LEVEL  K4 PAN" : "K1 KIND  K2 SOUND  K3 LEVEL  K4 PAN"); break;
    }
}

static void rt_draw_snd_shape(void)
{
    static const int32_t ANG[4] = {640, 896, 128, 384};          /* top left, top right, bottom right, bottom left */
    uint16_t col = RT_TRK_COL[rt_sn.sel];
    uint32_t base = (rt_sn.spage - 1u) * 4u, k, j, i;
    uint32_t show = rt_sn.show >= ROTO_SHOW_S1 && rt_sn.show <= ROTO_SHOW_S4 ? rt_sn.show - ROTO_SHOW_S1 + 1u : 0u;
    int32_t px[4], py[4], x, y;
    char let[4];

    rt_circle(RT_CX, RT_CY, 30, RT_TRK_RING[rt_sn.sel]);
    rt_circle(RT_CX, RT_CY, 56, RT_TRK_RING[rt_sn.sel]);
    rt_circle(RT_CX, RT_CY, 82, RT_TRK_RING[rt_sn.sel]);
    for (k = 0; k < 4u; k++) {
        int32_t v = rt_sn.ed[base + k], r = v == -128 ? 6 : 6 + (v * 76) / 100;
        rt_polar(ANG[k], 6, &x, &y);
        rt_polar(ANG[k], 82, &px[k], &py[k]);
        rt_line(x, y, px[k], py[k], show == k + 1u ? col : RT_LINE);
        rt_polar(ANG[k], r, &px[k], &py[k]);
    }
    for (k = 0; k < 4u; k++) {                                   /* the shape */
        uint32_t n = (k + 1u) % 4u;
        if (rt_sn.ed[base + k] == -128 || rt_sn.ed[base + n] == -128) continue;
        rt_line(px[k], py[k], px[n], py[n], col);
    }
    for (k = 0; k < 4u; k++)
        if (rt_sn.ed[base + k] != -128) rt_diamond_fill(px[k], py[k], show == k + 1u ? 4 : 3, show == k + 1u ? RT_TEXT : col);

    for (k = 0; k < 4u; k++) {                                   /* the letters: first, or next free */
        const char *l = rt_sn.ed_lbl[base + k];
        let[k] = l[0] ? l[0] : '-';
        for (i = 0; l[i]; i++) {
            int taken = 0;
            for (j = 0; j < k; j++) taken |= let[j] == l[i];
            if (!taken) { let[k] = l[i]; break; }
        }
        rt_polar(ANG[k], 94, &x, &y);
        {
            char t[2] = {let[k], 0};
            int hot = show == k + 1u;
            uint16_t c = rt_sn.ed[base + k] == -128 ? RT_DOT : hot ? col : RT_GREY;
            rt_rect(x - 5, y - 6, x + 6, y + 7, hot ? col : RT_BG);
            if (!hot) {
                rt_hspan(x - 5, x + 6, y - 6, c); rt_hspan(x - 5, x + 6, y + 6, c);
                rt_rect(x - 5, y - 6, x - 4, y + 7, c); rt_rect(x + 5, y - 6, x + 6, y + 7, c);
            }
            rt_text_s(x - 2, y - 3, t, hot ? RT_BG : c);
        }
    }
    if (show) rt_snd_bottom(rt_sn.ed_lbl[base + show - 1u], rt_sn.ed_txt[base + show - 1u], 0);
    else {
        char line[40];
        uint32_t n = 0, c;
        for (k = 0; k < 4u; k++) {
            if (k) line[n++] = ' ';
            for (c = 0; rt_sn.ed_lbl[base + k][c] && c < 5u; c++) line[n++] = rt_sn.ed_lbl[base + k][c];
            if (!c) line[n++] = '-';
        }
        line[n] = 0;
        rt_snd_bottom(0, 0, line);
    }
}

static void rt_draw_snd(void)
{
    if (rt_sn.spage) rt_draw_snd_shape();
    else rt_draw_snd_choose();
}

/* ------------------------------------------------------------- delay
 * A clock face: one turn is a bar, 16 ticks for its 1/16s. The pink dot at the
 * top is a hit; the white dots after it are its echoes, as far apart as the
 * delay TIME, fading and spiralling in with the FEEDBACK. The ring's SEND is an
 * arc in the track colour round the middle. Terminal lines along the bottom. */
#define RT_DLY_R 84

static void rt_draw_dly(void)
{
    int32_t i, x, y, x1, y1, step = (int32_t)((roto_dtime_96(rt_sn.dtime) * 1024u) / 96u);
    uint32_t amp = 256u, fb = rt_sn.dfdbk > 118u ? 118u : rt_sn.dfdbk;

    rt_circle(RT_CX, RT_CY, RT_DLY_R, RT_DOT);
    for (i = 0; i < 16; i++) {                               /* the 1/16s; the beats longer */
        rt_polar(i * 64 - 256, RT_DLY_R, &x, &y);
        rt_polar(i * 64 - 256, RT_DLY_R - (i % 4 ? 4 : 9), &x1, &y1);
        rt_line(x, y, x1, y1, i % 4 ? RT_DOT : RT_GREY);
    }

    for (i = 1; i <= 24; i++) {                              /* the echoes */
        uint16_t c;
        int32_t r = RT_DLY_R - 12 - i * 2;
        amp = (amp * fb) / 100u;
        if (amp < 10u || r < 50) break;
        c = amp > 150u ? RT_TEXT : amp > 70u ? RT_GREY : RT_DOT;
        rt_polar(-256 + i * step, r, &x, &y);
        rt_rect(x - 1, y - 1, x + 2, y + 2, c);
    }
    rt_polar(-256, RT_DLY_R - 12, &x, &y);                   /* the hit */
    rt_rect(x - 2, y - 2, x + 3, y + 3, RT_PINK);

    {                                                        /* the ring's SEND */
        int32_t end = (int32_t)((rt_sn.dsend * 1024u) / 127u), a;
        for (a = 0; end > 0 && a <= end; a += 6) {
            rt_polar(-256 + a, 44, &x, &y);
            rt_px(x, y, RT_TRK_COL[rt_sn.sel]);
        }
    }

    if (rt_sn.show) {
        rt_rect(RT_CX - 36, RT_CY - 22, RT_CX + 37, RT_CY + 20, RT_BG);
        rt_draw_centre();
    } else {                                                 /* at rest: the time, big; the feedback under it */
        const char *t = roto_dtime_name(rt_sn.dtime);
        char fbs[12] = "FDBK ";
        uint32_t n = rt_strlen(t);
        if (n > 4u) rt_text_b(RT_CX - (int32_t)n * 4, RT_CY - 10, t, 1, RT_TEXT);
        else rt_text_b(RT_CX - (int32_t)n * 8, RT_CY - 18, t, 2, RT_TEXT);
        rt_fmt_u(fbs + 5, rt_sn.dfdbk);
        n = rt_strlen(fbs); fbs[n] = '%'; fbs[n + 1] = 0;
        rt_text_s(RT_CX - RT_TEXT_S_W(fbs) / 2, RT_CY + 10, fbs, RT_GREY);
    }

    {                                                        /* > terminal lines */
        char a[32] = "> ", b[32] = "> SEND ";
        uint32_t n;
        const char *t = roto_dtime_name(rt_sn.dtime);
        for (n = 0; t[n]; n++) a[2 + n] = t[n];
        a[2 + n] = 0;
        n = rt_strlen(a); a[n] = ' '; a[n + 1] = ' '; a[n + 2] = 'F'; a[n + 3] = 'D'; a[n + 4] = 'B'; a[n + 5] = 'K'; a[n + 6] = ' ';
        rt_fmt_u(a + n + 7, rt_sn.dfdbk);
        n = rt_strlen(a); a[n] = '%'; a[n + 1] = 0;
        rt_fmt_u(b + 7, (rt_sn.dsend * 100u + 63u) / 127u);
        n = rt_strlen(b); b[n] = '%'; b[n + 1] = ' '; b[n + 2] = ' '; b[n + 3] = 'C'; b[n + 4] = 'O'; b[n + 5] = 'L'; b[n + 6] = 'R'; b[n + 7] = ' ';
        rt_fmt_u(b + n + 8, (rt_sn.dcolor * 100u + 63u) / 127u);
        n = rt_strlen(b); b[n] = '%'; b[n + 1] = 0;
        rt_text_s(6, RT_H - 21, a, RT_GREY);
        rt_text_s(6, RT_H - 11, b, RT_GREY);
    }
}

static void rt_draw_scene(void)
{
    rt_draw_header();
    if (rt_sn.view == ROTO_VIEW_ENV) rt_draw_env();
    else if (rt_sn.view == ROTO_VIEW_FLT) rt_draw_flt();
    else if (rt_sn.view == ROTO_VIEW_DLY) rt_draw_dly();
    else if (rt_sn.view == ROTO_VIEW_ARP) rt_draw_arp();
    else if (rt_sn.view == ROTO_VIEW_SND) rt_draw_snd();
    else rt_draw_ring();
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

/* ------------------------------------------------------------- splash
 * The power-on screen: six rings in the rings' colours, each with a few hits spread
 * evenly round it (as EUCLID spreads them), and the name big in the middle. */
void roto_ui_splash(void)
{
    static const uint8_t HITS[ROTO_TRACKS] = {4, 3, 5, 2, 7, 1};
    int32_t i, x, y;
    uint32_t s, n;
    rt_clip_all();
    rt_rect(0, 0, RT_W, RT_H, RT_BG);
    for (i = 0; i < ROTO_TRACKS; i++) {
        int32_t r = 102 - i * 9;
        rt_circle(RT_CX, 112, r, RT_TRK_HALF[i]);
        n = HITS[i];
        for (s = 0; s < n; s++) {
            int32_t a = (int32_t)((s * 1024u) / n) - 256 + i * 37;
            x = RT_CX + rt_round14(rt_cos(a) * r);
            y = 112 + rt_round14(rt_sin(a) * r);
            rt_diamond_fill(x, y, 3, RT_TRK_COL[i]);
        }
    }
    rt_text_b(RT_CX - 5 * 8, 112 - 26, "ATTIC", 2, RT_TEXT);
    rt_text_b(RT_CX - 4 * 8, 112 + 2, "ROTO", 2, RT_TEXT);
    {
        char v[12];                                          /* (the small font is capitals only) */
        for (s = 0; s < 11u && ROTO_VERSION[s]; s++) v[s] = (char)(ROTO_VERSION[s] >= 'a' && ROTO_VERSION[s] <= 'z' ? ROTO_VERSION[s] - 32 : ROTO_VERSION[s]);
        v[s] = 0;
        rt_text_s(RT_CX - RT_TEXT_S_W(v) / 2, 226, v, RT_GREY);
    }
}

/* ------------------------------------------------------------- state */
static uint8_t roto_in_editor = 0;
static uint8_t rt_shown = 0;               /* 0: nothing of ours on screen yet */
static uint32_t rt_shown_head, rt_shown_ring;   /* what the header / the ring showed */
static uint8_t rt_shown_pos, rt_shown_bpm, rt_shown_sel;
static uint8_t rt_arp_cur = 0xFF;          /* the note the arp's needle pointed at */
static int32_t rt_orb_x, rt_orb_y;       /* where the filter's small circle was drawn */
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
    if (rt_sn.view == ROTO_VIEW_ARP) h = rt_fnv(h, rt_sn.a_chord != 0u);
    if (rt_sn.view == ROTO_VIEW_SND) h = rt_fnv(h, rt_sn.spage);
    for (s = 0; s < sizeof roto.snd[0].name && roto.snd[rt_sn.part].name[s]; s++)
        h = rt_fnv(h, (uint8_t)roto.snd[rt_sn.part].name[s]);
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
    if (rt_sn.view == ROTO_VIEW_ENV) { uint32_t e; for (e = 0; e < 4u; e++) h = rt_fnv(h, rt_sn.env[e]); }
    if (rt_sn.view == ROTO_VIEW_DLY) { h = rt_fnv(h, rt_sn.dtime); h = rt_fnv(h, rt_sn.dfdbk); h = rt_fnv(h, rt_sn.dcolor); h = rt_fnv(h, rt_sn.dsend); }
    if (rt_sn.view == ROTO_VIEW_SND) {
        uint32_t e, c;
        h = rt_fnv(h, rt_sn.spage); h = rt_fnv(h, rt_sn.s_cat); h = rt_fnv(h, rt_sn.s_idx); h = rt_fnv(h, rt_sn.s_kit);
        h = rt_fnv(h, rt_sn.s_piece); h = rt_fnv(h, rt_sn.s_level); h = rt_fnv(h, (uint8_t)rt_sn.s_pan); h = rt_fnv(h, rt_sn.s_drum);
        for (c = 0; roto.snd[rt_sn.part].name[c] && c < sizeof roto.snd[0].name; c++) h = rt_fnv(h, (uint8_t)roto.snd[rt_sn.part].name[c]);
        for (e = 0; e < 8u; e++) {
            h = rt_fnv(h, (uint8_t)rt_sn.ed[e]);
            for (c = 0; rt_sn.ed_lbl[e][c]; c++) h = rt_fnv(h, (uint8_t)rt_sn.ed_lbl[e][c]);
            for (c = 0; rt_sn.ed_txt[e][c]; c++) h = rt_fnv(h, (uint8_t)rt_sn.ed_txt[e][c]);
        }
    }
    if (rt_sn.view == ROTO_VIEW_ARP) {
        h = rt_fnv(h, rt_sn.a_chord); h = rt_fnv(h, rt_sn.a_mode); h = rt_fnv(h, rt_sn.a_speed);
        h = rt_fnv(h, rt_sn.a_range); h = rt_fnv(h, rt_sn.a_gate); h = rt_fnv(h, rt_sn.a_len);
        for (s = 0; s < rt_sn.a_len; s++) h = rt_fnv(h, rt_sn.a_seq[s]);
    }
    if (rt_sn.view == ROTO_VIEW_FLT) { uint32_t e, c; for (e = 0; e < 4u; e++) { h = rt_fnv(h, (uint8_t)rt_sn.flt[e]); for (c = 0; rt_sn.flt_lbl[e][c]; c++) h = rt_fnv(h, (uint8_t)rt_sn.flt_lbl[e][c]); } }
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
        if (ring == rt_shown_ring && rt_sn.pos != rt_shown_pos && rt_sn.view != ROTO_VIEW_ENV && rt_sn.view != ROTO_VIEW_FLT && rt_sn.view != ROTO_VIEW_DLY && rt_sn.view != ROTO_VIEW_ARP && rt_sn.view != ROTO_VIEW_SND) {
            int32_t x0, y0, x1, y1;
            rt_playhead_box(rt_shown_pos < rt_sn.len ? rt_shown_pos : 0, rt_sn.len, &x0, &y0, &x1, &y1);
            rt_repaint(x0, y0, x1, y1);
            rt_playhead_box(rt_sn.pos, rt_sn.len, &x0, &y0, &x1, &y1);
            rt_repaint(x0, y0, x1, y1);
        }
    }
    if (rt_sn.view == ROTO_VIEW_ARP && rt_shown && ring == rt_shown_ring && rt_sn.a_cur != rt_arp_cur) {
        int32_t x0, y0, x1, y1, y;                       /* the needle moved: its old and new places, a strip at a time */
        uint32_t k;
        for (k = 0; k < 2u; k++) {
            uint32_t c = k ? rt_sn.a_cur : rt_arp_cur;
            if (c >= rt_sn.a_len) continue;
            rt_aneedle_box(c, &x0, &y0, &x1, &y1);
            for (y = y0; y < y1; y += RT_BAND) rt_repaint(x0, y, x1, y + RT_BAND < y1 ? y + RT_BAND : y1);
        }
    }
    rt_arp_cur = rt_sn.a_cur;
    if (rt_sn.view == ROTO_VIEW_FLT) {                  /* the small circle on the filter screen */
        int32_t ox, oy;
        rt_flt_orb(&ox, &oy);
        if (rt_shown && ring == rt_shown_ring && (ox != rt_orb_x || oy != rt_orb_y)) {
            rt_repaint(rt_orb_x - RT_F_SR - 2, rt_orb_y - RT_F_SR - 2, rt_orb_x + RT_F_SR + 3, rt_orb_y + RT_F_SR + 3);
            rt_repaint(ox - RT_F_SR - 2, oy - RT_F_SR - 2, ox + RT_F_SR + 3, oy + RT_F_SR + 3);
        }
        rt_orb_x = ox; rt_orb_y = oy;
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
/* the 27 keys from F: black or white, and a white key's place (0..15) */
static int rt_key_black(uint32_t k) { return (int)((0x54Au >> ((k + 5u) % 12u)) & 1u); }
static uint32_t rt_white_place(uint32_t k)
{
    uint32_t i, n = 0;
    for (i = 0; i < k; i++) n += (uint32_t)!rt_key_black(i);
    return n;
}

/* the white keys are the steps of the ring: 1-16, or 17-32 with OCT+ (roto.kpage). Silent (seq.c). */
int roto_ui_input(uint32_t pressed, uint32_t notes)
{
    (void)pressed;
    if (roto_in_editor) return 0;
    if (notes && roto.view != ROTO_VIEW_ARP) {
        uint32_t k, sel = (uint32_t)roto.sel, len = roto.trk[sel].len, base = roto.kpage ? 16u : 0u;
        for (k = 0; k < 27u; k++) {
            uint32_t s;
            if (!((notes >> k) & 1u) || rt_key_black(k)) continue;
            s = base + rt_white_place(k);
            if (s < len) roto_step_toggle(sel, s);
        }
    }
    return 1;
}

uint32_t roto_key_leds(void)
{
    const roto_track_t *k = &roto.trk[roto.sel < ROTO_TRACKS ? roto.sel : 0];
    uint32_t key, m = 0, base = roto.kpage ? 16u : 0u, len = k->len;
    if (roto_in_editor) return 0;
    for (key = 0; key < 27u; key++) {
        uint32_t s, on;
        if (rt_key_black(key)) continue;
        s = base + rt_white_place(key);
        if (s >= len) continue;
        on = k->step[s].on;
        if (roto.running && k->pos == s) on = !on;           /* (the running light) */
        if (on) m |= 1u << key;
    }
    return m;
}

void roto_ui_audio_level(uint8_t level)
{
    (void)level;
}
