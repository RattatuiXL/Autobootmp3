/*
 * AutoMusic - PSP kernel plugin (ARK-4, VSH/XMB)
 * Plays every .mp3 in ms0:/MUSIC automatically after the XMB loads.
 *   Note button, single press : next song
 *   Note button, double press : stop (press once to resume)
 *
 * Two threads: a decoder thread fills a ring buffer, an output thread
 * plays it, so memory-stick stalls and busy moments do not cause lag.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

PSP_MODULE_INFO("AutoMusic", PSP_MODULE_KERNEL, 1, 0);

#define MUSIC_DIR    "ms0:/MUSIC/"
#define DIR_LEN      11
#define START_DELAY  12000000      /* wait for the XMB to finish loading (us) */
#define DOUBLE_US    350000        /* max gap between the two presses (us)    */
#define MAX_FILES    200
#define NAME_LEN     128
#define BUF_SZ       (32 * 1024)
#define OUT_FRAMES   1152
#define RING_BLOCKS  20            /* about 0.5 s of audio buffered           */
#define PREBUFFER    6             /* blocks to collect before (re)starting   */

enum { CMD_NONE = 0, CMD_NEXT, CMD_TOGGLE };

static char names[MAX_FILES][NAME_LEN];
static int nfiles = 0;

static mp3dec_t dec;
static unsigned char buf[BUF_SZ] __attribute__((aligned(64)));
static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME] __attribute__((aligned(64)));

static short ring[RING_BLOCKS][OUT_FRAMES * 2] __attribute__((aligned(64)));
static int ring_hz[RING_BLOCKS];
static volatile unsigned int ring_head = 0;   /* written by decoder thread */
static volatile unsigned int ring_tail = 0;   /* written by output thread  */
static volatile int flush_req = 0;
static volatile int cmd = CMD_NONE;
static int blk_fill = 0;


/* ---- diagnostics: writes ms0:/AutoMusic.log every few seconds ---- */
static volatile unsigned int st_out_cnt = 0, st_out_sum = 0, st_out_max = 0;
static volatile unsigned int st_ring_min = 99, st_underrun = 0, st_resfail = 0;
static unsigned int st_dec_cnt = 0, st_dec_sum = 0, st_dec_max = 0, st_read_max = 0;
static unsigned int st_hz = 0, st_ch = 0, st_kbps = 0, st_layer = 0;

static char lg[1024];
static int lg_n = 0;

static void put_s(const char *s)
{
    while (*s && lg_n < (int)sizeof(lg) - 1) lg[lg_n++] = *s++;
}

static void put_n(unsigned int v)
{
    char t[12];
    int i = 0;
    if (v == 0) t[i++] = '0';
    while (v) { t[i++] = '0' + (v % 10); v /= 10; }
    while (i && lg_n < (int)sizeof(lg) - 1) lg[lg_n++] = t[--i];
}

static void write_log(void)
{
    lg_n = 0;
    put_s("AutoMusic stats (about the last 6 seconds)\r\n");
    put_s("cpu MHz: "); put_n(scePowerGetCpuClockFrequency());
    put_s("   bus MHz: "); put_n(scePowerGetBusClockFrequency()); put_s("\r\n");
    put_s("song: "); put_n(st_hz); put_s(" Hz, channels "); put_n(st_ch);
    put_s(", "); put_n(st_kbps); put_s(" kbps, layer "); put_n(st_layer); put_s("\r\n");
    put_s("decode time per frame (us): avg "); put_n(st_dec_cnt ? st_dec_sum / st_dec_cnt : 0);
    put_s(" max "); put_n(st_dec_max); put_s("   (must stay under about 26000)\r\n");
    put_s("file read time max (us): "); put_n(st_read_max); put_s("\r\n");
    put_s("audio block time (us): avg "); put_n(st_out_cnt ? st_out_sum / st_out_cnt : 0);
    put_s(" max "); put_n(st_out_max); put_s("   (about 26000 expected at 44100 Hz)\r\n");
    put_s("buffered blocks min: "); put_n(st_ring_min); put_s(" of 20\r\n");
    put_s("underruns so far: "); put_n(st_underrun); put_s("\r\n");
    put_s("audio channel failures: "); put_n(st_resfail); put_s("\r\n");

    SceUID f = sceIoOpen("ms0:/AutoMusic.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f >= 0) { sceIoWrite(f, lg, lg_n); sceIoClose(f); }

    st_dec_cnt = st_dec_sum = st_dec_max = st_read_max = 0;
    st_out_cnt = st_out_sum = st_out_max = 0;
    st_ring_min = 99;
}

/* make the FPU flush tiny (denormal) numbers to zero instead of trapping */
static void flush_to_zero(void)
{
    unsigned int fcsr;
    asm volatile("cfc1 %0, $31" : "=r"(fcsr));
    fcsr |= (1u << 24);
    asm volatile("ctc1 %0, $31" : : "r"(fcsr));
}

/* ---- Note button (output thread only): single = NEXT, double = TOGGLE ---- */
static int was_down = 0, pending = 0, swallow = 0;
static unsigned int rel_t = 0;

static void poll_button(void)
{
    SceCtrlData pad;
    unsigned int now = sceKernelGetSystemTimeLow();

    if (pending && (now - rel_t) >= DOUBLE_US) {
        pending = 0;
        cmd = CMD_NEXT;
    }

    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return;
    int down = (pad.Buttons & PSP_CTRL_NOTE) != 0;

    if (down && !was_down) {
        if (pending) { pending = 0; swallow = 1; cmd = CMD_TOGGLE; }
    } else if (!down && was_down) {
        if (swallow) swallow = 0;
        else { pending = 1; rel_t = now; }
    }
    was_down = down;
}

/* ---- file list ---- */
static void str_copy(char *dst, const char *src)
{
    while ((*dst++ = *src++) != 0) { }
}

static int ends_mp3(const char *s)
{
    int n = strlen(s);
    if (n < 5) return 0;
    const char *e = s + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'm' && (e[2] | 32) == 'p' && e[3] == '3';
}

static void scan(void)
{
    SceUID d = sceIoDopen("ms0:/MUSIC");
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (nfiles < MAX_FILES && sceIoDread(d, &e) > 0) {
        if (ends_mp3(e.d_name) && strlen(e.d_name) < NAME_LEN - DIR_LEN - 1) {
            str_copy(names[nfiles], MUSIC_DIR);
            str_copy(names[nfiles] + DIR_LEN, e.d_name);
            nfiles++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);

    /* sort alphabetically */
    for (int i = 0; i < nfiles - 1; i++)
        for (int j = 0; j < nfiles - 1 - i; j++)
            if (strcmp(names[j], names[j + 1]) > 0) {
                static char tmp[NAME_LEN];
                str_copy(tmp, names[j]);
                str_copy(names[j], names[j + 1]);
                str_copy(names[j + 1], tmp);
            }
}

/* ---- output thread: plays ring buffer blocks, reads the Note button ---- */
static int output_thread(SceSize args, void *argp)
{
    int ch_hz = 0, buffering = 1;

    while (1) {
        poll_button();

        if (flush_req) {
            ring_tail = ring_head;
            buffering = 1;
            flush_req = 0;
            continue;
        }

        unsigned int avail = ring_head - ring_tail;
        if (avail == 0) {
            if (!buffering) st_underrun++;
            buffering = 1;
            sceKernelDelayThread(2000);
            continue;
        }
        if (buffering) {
            if (avail < PREBUFFER) { sceKernelDelayThread(2000); continue; }
            buffering = 0;
        }

        int slot = ring_tail % RING_BLOCKS;
        int hz = ring_hz[slot];
        if (hz != ch_hz) {
            if (ch_hz) sceAudioSRCChRelease();
            ch_hz = 0;
            if (sceAudioSRCChReserve(OUT_FRAMES, hz, 2) < 0) {
                st_resfail++;
                sceKernelDelayThread(500000);
