#include <psptypes.h>
#include <string.h>
#include "osd.h"
#include "muldiv.h"

#define SCREEN_W 480
#define SCREEN_H 272
#define OUT_HZ 44100

static const unsigned char font[45][7] = {
    {0,0,0,0,0,0,0},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {0,0,0,31,0,0,0}, {0,0,0,0,0,12,12},
    {0,4,4,0,4,4,0}, {1,2,4,8,16,0,0},
    {14,17,1,2,4,0,4},
    {0,0,0,0,0,0,31}, {2,4,8,0,0,0,0},
    {4,4,4,4,4,0,4}
};

static int inside(int x, int y)
{
    return x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H;
}

static u32 uncached_address(void *address)
{
    return ((u32)address & 0x1FFFFFFFu) | 0x40000000u;
}

static u16 pack16(int format, int r, int g, int b)
{
    if (format == 0)
        return (u16)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
    if (format == 1)
        return (u16)(0x8000 | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    return (u16)(0xF000 | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4));
}

static void pixel(u32 base, int stride, int format, int x, int y,
                  int r, int g, int b, int alpha)
{
    if (!inside(x, y)) return;
    if (format == 3) {
        u32 *p = (u32 *)(base + ((u32)y * stride + x) * 4u);
        u32 d = *p;
        int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
        int inverse = 255 - alpha;
        int rr = (r * alpha + dr * inverse) / 255;
        int gg = (g * alpha + dg * inverse) / 255;
        int bb = (b * alpha + db * inverse) / 255;
        *p = 0xFF000000u | ((u32)bb << 16) | ((u32)gg << 8) | (u32)rr;
    } else {
        u16 *p = (u16 *)(base + ((u32)y * stride + x) * 2u);
        u16 d = *p;
        int dr, dg, db, inverse;
        if (format == 0) {
            dr = (d & 31) << 3; dg = ((d >> 5) & 63) << 2; db = ((d >> 11) & 31) << 3;
        } else if (format == 1) {
            dr = (d & 31) << 3; dg = ((d >> 5) & 31) << 3; db = ((d >> 10) & 31) << 3;
        } else {
            dr = (d & 15) << 4; dg = ((d >> 4) & 15) << 4; db = ((d >> 8) & 15) << 4;
        }
        inverse = 255 - alpha;
        *p = pack16(format, (r * alpha + dr * inverse) / 255,
                    (g * alpha + dg * inverse) / 255,
                    (b * alpha + db * inverse) / 255);
    }
}

static void rect(u32 base, int stride, int format, int x, int y,
                 int w, int h, int r, int g, int b, int alpha)
{
    int xx, yy;
    for (yy = y; yy < y + h; yy++)
        for (xx = x; xx < x + w; xx++)
            pixel(base, stride, format, xx, yy, r, g, b, alpha);
}

static void line(u32 base, int stride, int format, int x0, int y0,
                 int x1, int y1, int r, int g, int b, int alpha)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        pixel(base, stride, format, x0, y0, r, g, b, alpha);
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static int glyph_index(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (ch >= '0' && ch <= '9') return 1 + ch - '0';
    if (ch >= 'A' && ch <= 'Z') return 11 + ch - 'A';
    switch (ch) {
    case ' ': return 0;
    case '-': return 37;
    case '.': return 38;
    case ':': return 39;
    case '/': return 40;
    case '?': return 41;
    case '_': return 42;
    case '\'': return 43;
    case '!': return 44;
    default: return 41;
    }
}

static int draw_text(u32 base, int stride, int format, int x, int y,
                     const char *text, int max_chars,
                     int r, int g, int b, int alpha)
{
    int n = 0;
    while (*text && n < max_chars) {
        const unsigned char *rows = font[glyph_index(*text++)];
        int row, col;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (rows[row] & (1 << (4 - col)))
                    pixel(base, stride, format, x + col, y + row,
                          r, g, b, alpha);
        x += 6;
        n++;
    }
    return n;
}

static void append_uint(char *out, int *at, unsigned int value)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < (int)sizeof(digits));
    while (n--) out[(*at)++] = digits[n];
}

static void append_two(char *out, int *at, unsigned int value)
{
    out[(*at)++] = (char)('0' + (value / 10) % 10);
    out[(*at)++] = (char)('0' + value % 10);
}

static void format_time(char *out, unsigned int elapsed, unsigned int total)
{
    unsigned int e = elapsed / OUT_HZ;
    unsigned int t = total / OUT_HZ;
    unsigned int elapsed_minutes = e / 60;
    int at = 0;
    if (elapsed_minutes < 10) out[at++] = '0';
    append_uint(out, &at, elapsed_minutes);
    out[at++] = ':';
    append_two(out, &at, e % 60);
    out[at++] = '/';
    if (total) append_uint(out, &at, t / 60);
    else { out[at++] = '-'; out[at++] = '-'; }
    out[at++] = ':';
    if (total) append_two(out, &at, t % 60);
    else { out[at++] = '-'; out[at++] = '-'; }
    out[at] = '\0';
}

static void draw_disc(u32 base, int stride, int format, int cx, int cy,
                      int playing, unsigned int phase)
{
    static const signed char dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const signed char dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    int x, y;
    for (y = -13; y <= 13; y++) {
        for (x = -13; x <= 13; x++) {
            int d = x * x + y * y;
            if (d >= 125 && d <= 169)
                pixel(base, stride, format, cx + x, cy + y, 205, 216, 232, 245);
            else if (d >= 72 && d <= 85)
                pixel(base, stride, format, cx + x, cy + y, 104, 119, 143, 210);
            else if (d <= 12)
                pixel(base, stride, format, cx + x, cy + y, 14, 20, 30, 255);
        }
    }
    line(base, stride, format, cx, cy, cx + dx[phase & 7], cy + dy[phase & 7],
         playing ? 98 : 139, playing ? 210 : 150, playing ? 255 : 162, 255);
}

void osd_draw(void *topaddr, int bufferwidth, int pixelformat,
              const char *title, unsigned int elapsed_frames,
              unsigned int total_frames, int playing,
              unsigned int animation_phase)
{
    const int x = 12, y = 216, w = 456, h = 46;
    const int progress_x = 66, progress_y = 250, progress_w = 386;
    u32 base;
    char time_text[24];
    int fill;

    if (!topaddr || bufferwidth < SCREEN_W || bufferwidth > 1024 ||
        pixelformat < 0 || pixelformat > 3)
        return;
    base = uncached_address(topaddr);

    rect(base, bufferwidth, pixelformat, x, y, w, h, 7, 12, 20, 202);
    rect(base, bufferwidth, pixelformat, x, y, w, 1, 83, 166, 220, 220);
    rect(base, bufferwidth, pixelformat, x, y + h - 1, w, 1, 83, 166, 220, 190);
    draw_disc(base, bufferwidth, pixelformat, 38, 239, playing,
              playing ? animation_phase : 0);

    draw_text(base, bufferwidth, pixelformat, 64, 222,
              title, 47, 243, 247, 255, 255);
    format_time(time_text, elapsed_frames, total_frames);
    draw_text(base, bufferwidth, pixelformat, 364, 222,
              time_text, 16, 203, 224, 244, 255);

    rect(base, bufferwidth, pixelformat, progress_x, progress_y,
         progress_w, 3, 67, 82, 101, 255);
    if (total_frames) {
        if (elapsed_frames > total_frames) elapsed_frames = total_frames;
        fill = (int)muldiv_u32(progress_w, elapsed_frames, total_frames);
        if (fill > 0)
            rect(base, bufferwidth, pixelformat, progress_x, progress_y,
                 fill, 3, 95, 198, 255, 255);
    }
}
