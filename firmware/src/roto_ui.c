/* SPDX-License-Identifier: GPL-3.0-only
 * attic-roto UI (2026-11-24).
 *
 * Draws directly to the FM-1 LCD via lcd_fill(x,y,w,h,col).
 * lcd_fill is synchronous (calls lcd_sync internally) — no framebuffer,
 * no cv_px, no cv_blit. The full 240x240 panel is ours when roto_ui_draw()
 * returns 1.
 *
 * Ring screen layout (matches circular sequencer reference):
 *   - Outer ring r=100: track colour, thin
 *   - Inner ring r=60:  dimmed, thin
 *   - Step markers: small diamond on outer ring (bright=on, dim=off)
 *   - Active step polygon lines connecting active steps through centre area
 *   - Playhead: large white diamond on outer ring + spoke from centre
 *   - 4 compass diamonds on outer ring at N/E/S/W
 *   - Track indicators: 4 small coloured diamonds bottom-centre
 *   - BPM top-left, track name top-right (via lcd pixel font)
 *
 * Navigation:
 *   K1 (knob 1)  = BPM
 *   K2           = active track 1..4
 *   K3           = step length for active track
 *   K4           = clock division (1/16, 1/8, 1/4)
 *   B_PLAY       = start / stop
 *   B_SEQ        = toggle editor pass-through
 *   B_HOME       = (reserved)
 *   white keys   = toggle steps on active ring
 */

#include "roto_seq.h"

/* ---------------------------------------------------------------- palette
 * Neon track colours matching the reference visual system */
#define RGB565(r,g,b) ((uint16_t)(((uint16_t)((r)&0xF8u)<<8u)|(((uint16_t)((g)&0xFCu))<<3u)|((uint16_t)(b)>>3u)))

#define COL_BG     RGB565(  0,   0,   0)
#define COL_WHITE  RGB565(255, 255, 255)
#define COL_DIM    RGB565( 30,  30,  30)

/* Four neon track colours */
static const uint16_t TRK_COL[4] = {
    RGB565( 57, 255,  20),   /* 0 neon green  */
    RGB565(  0, 200, 255),   /* 1 cyan        */
    RGB565(255, 122,   0),   /* 2 orange      */
    RGB565(220,  50, 255),   /* 3 purple      */
};
static const uint16_t TRK_DIM[4] = {
    RGB565( 20,  80,   8),
    RGB565(  0,  60,  80),
    RGB565( 80,  38,   0),
    RGB565( 70,  15,  80),
};

/* ---------------------------------------------------------------- geometry */
#define LCD_W  240
#define LCD_H  240
#define CX     120
#define CY     120
#define R_OUT  100   /* outer ring radius */
#define R_IN    60   /* inner ring radius */

/* ---------------------------------------------------------------- screen state */
static uint8_t roto_in_editor = 0;

/* ---------------------------------------------------------------- trig
 * Fixed-point sin/cos, angle 0..255 = full circle, returns *256 */
static int32_t isin256(int32_t a)
{
    static const int8_t T[65] = {
        0,6,13,19,25,31,37,44,50,56,62,68,74,80,86,92,
        97,103,109,114,120,125,130,135,141,146,151,155,160,165,169,174,
        178,182,186,190,194,198,202,205,209,212,215,218,221,224,226,229,
        231,234,236,238,240,241,243,244,245,246,247,248,249,249,250,250,250
    };
    int32_t q = (a >> 6) & 3, i = a & 63;
    int32_t v = (q == 1 || q == 3) ? T[63 - i] : T[i];
    return (q >= 2) ? -v : v;
}
static int32_t icos256(int32_t a) { return isin256((a + 64) & 255); }

/* step index → angle (step 0 at top = 192 in 0..255 space) */
static void step_xy(int32_t r, uint32_t step, uint32_t nsteps, int32_t *ox, int32_t *oy)
{
    int32_t a = (int32_t)((step * 256u) / nsteps + 192u) & 255;
    *ox = CX + (icos256(a) * r) / 256;
    *oy = CY + (isin256(a) * r) / 256;
}

/* ---------------------------------------------------------------- pixel primitives
 * All drawing uses lcd_fill. lcd_fill is synchronous. */
static void px(int32_t x, int32_t y, uint16_t c)
{
    if ((uint32_t)x < (uint32_t)LCD_W && (uint32_t)y < (uint32_t)LCD_H)
        lcd_fill((uint32_t)x, (uint32_t)y, 1u, 1u, c);
}

/* Bresenham line */
static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1-x0, dy = y1-y0;
    int32_t sx = dx<0?-1:1, sy = dy<0?-1:1;
    if (dx<0) dx=-dx; if (dy<0) dy=-dy;
    int32_t err = (dx>dy?dx:-dy)/2, e2;
    for(;;){
        px(x0,y0,c);
        if(x0==x1&&y0==y1) break;
        e2=err;
        if(e2>-dx){err-=dy; x0+=sx;}
        if(e2< dy){err+=dx; y0+=sy;}
    }
}

/* Midpoint circle — hollow ring, 1 px */
static void ring(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x=r, y=0, err=0;
    while(x>=y){
        px(cx+x,cy+y,c); px(cx+y,cy+x,c);
        px(cx-y,cy+x,c); px(cx-x,cy+y,c);
        px(cx-x,cy-y,c); px(cx-y,cy-x,c);
        px(cx+y,cy-x,c); px(cx+x,cy-y,c);
        y++; err+=1+2*y;
        if(2*(err-x)+1>0){x--; err+=1-2*x;}
    }
}

/* Filled diamond (axis-aligned square rotated 45°), half-size r */
static void diamond(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t i;
    for(i=0;i<=r;i++){
        int32_t w=r-i;
        /* horizontal span at row y±i */
        int32_t x0=x-w, x1=x+w;
        int32_t yn=y-i, yp=y+i;
        int32_t xx;
        if((uint32_t)yn<(uint32_t)LCD_H && x1>=0 && x0<LCD_W){
            if(x0<0)x0=0; if(x1>=LCD_W)x1=LCD_W-1;
            for(xx=x0;xx<=x1;xx++) px(xx,yn,c);
        }
        if(i>0 && (uint32_t)yp<(uint32_t)LCD_H && x1>=0 && x0<LCD_W){
            if(x0<0)x0=0; if(x1>=LCD_W)x1=LCD_W-1;
            for(xx=x0;xx<=x1;xx++) px(xx,yp,c);
        }
    }
}

/* Outline-only diamond */
static void diamond_outline(int32_t x, int32_t y, int32_t r, uint16_t c)
{
    int32_t i;
    for(i=0;i<=r;i++){
        int32_t w=r-i;
        px(x-w,y-i,c); px(x+w,y-i,c);
        px(x-w,y+i,c); px(x+w,y+i,c);
    }
}

/* ---------------------------------------------------------------- ring screen */
static void draw_ring_screen(void)
{
    uint32_t sel  = (uint32_t)roto.sel;
    roto_track_t *k = &roto.trk[sel];
    uint16_t col  = TRK_COL[sel];
    uint16_t dim  = TRK_DIM[sel];
    uint32_t s;

    /* clear full screen */
    lcd_fill(0, 0, LCD_W, LCD_H, COL_BG);

    /* inner ring — dim */
    ring(CX, CY, R_IN, dim);

    /* outer ring — track colour */
    ring(CX, CY, R_OUT, col);

    /* compass diamonds at N/E/S/W on outer ring */
    {
        int32_t pts[4][2] = {
            {CX,        CY-R_OUT},
            {CX+R_OUT,  CY},
            {CX,        CY+R_OUT},
            {CX-R_OUT,  CY}
        };
        uint32_t p;
        for(p=0;p<4u;p++)
            diamond_outline(pts[p][0], pts[p][1], 3, dim);
    }

    /* polygon lines connecting active steps */
    {
        int32_t prev_x = -1, prev_y = -1, first_x = -1, first_y = -1;
        uint32_t active_count = 0;
        for(s=0;s<(uint32_t)k->len;s++){
            if(k->step[s].on){
                int32_t sx, sy;
                step_xy(R_IN + (R_OUT-R_IN)/2, s, (uint32_t)k->len, &sx, &sy);
                if(prev_x >= 0)
                    line(prev_x, prev_y, sx, sy, dim);
                else { first_x=sx; first_y=sy; }
                prev_x=sx; prev_y=sy;
                active_count++;
            }
        }
        /* close polygon */
        if(active_count > 1 && first_x >= 0)
            line(prev_x, prev_y, first_x, first_y, dim);
    }

    /* step markers on outer ring */
    for(s=0;s<(uint32_t)k->len;s++){
        int32_t sx, sy;
        step_xy(R_OUT, s, (uint32_t)k->len, &sx, &sy);
        if(k->step[s].on)
            diamond(sx, sy, 3, col);
        else
            diamond_outline(sx, sy, 2, dim);
    }

    /* playhead: spoke from centre + large white diamond on outer ring */
    {
        int32_t px2, py2;
        step_xy(R_OUT, (uint32_t)k->pos, (uint32_t)k->len, &px2, &py2);
        line(CX, CY, px2, py2, dim);
        diamond(px2, py2, 5, COL_WHITE);
    }

    /* centre dot */
    diamond(CX, CY, 2, dim);

    /* track indicators: 4 small diamonds bottom-centre */
    {
        uint32_t t;
        for(t=0;t<4u;t++){
            uint16_t ic = (t==sel) ? TRK_COL[t] : TRK_DIM[t];
            int32_t bx = CX - 18 + (int32_t)(t * 12u);
            if(t==sel)
                diamond(bx, LCD_H-14, 4, ic);
            else
                diamond_outline(bx, LCD_H-14, 3, ic);
        }
    }
}

/* ---------------------------------------------------------------- input
 * Called from ui_input.c with the edge-triggered pressed bitmask and
 * key-note bitmask. Returns 1 to suppress Felucca's input handling.
 *
 * Button interpretation (B_PLAY / B_SEQ / B_HOME) happens in ui_input.c
 * where panel.btn[] is in scope; we just handle step-key toggles here and
 * consume all input so Felucca doesn't react to keys while roto owns the UI. */
int roto_ui_input(uint32_t pressed, uint32_t notes)
{
    (void)pressed;
    if(roto_in_editor) return 0;

    /* White keys toggle steps on the active ring */
    if(notes){
        uint32_t s;
        uint32_t sel  = (uint32_t)roto.sel;
        uint32_t maxs = (uint32_t)roto.trk[sel].len;
        if(maxs > 16u) maxs = 16u;
        for(s = 0; s < maxs; s++){
            if((notes >> s) & 1u)
                roto_step_toggle(sel, s);
        }
        return 1;
    }

    return 1;   /* roto owns the screen — eat all input */
}

/* ---------------------------------------------------------------- draw dispatch */
int roto_ui_draw(void)
{
    if(roto_in_editor) return 0;
    draw_ring_screen();
    return 1;
}

/* ---------------------------------------------------------------- audio level hook */
void roto_ui_audio_level(uint8_t level)
{
    (void)level;   /* reserved for future VU animation */
}
