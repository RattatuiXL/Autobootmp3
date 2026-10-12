/*
 * AutoMusic - PSP kernel plugin (ARK-4, VSH/XMB)
 * Plays every .mp3 in ms0:/MUSIC automatically after the XMB loads.
 *   Note button, single press : next song
 *   Note button, double press : stop (press once to resume)
 *   Hold Note + R trigger     : plugin volume up
 *   Hold Note + L trigger     : plugin volume down
 * The plugin volume is separate from the PSP volume keys and is remembered.
 *
 * The CPU clock is NOT touched by default. Optionally, ms0:/SEPLUGINS/AutoMusic.cfg
 * can hold a number (33..333 = MHz used only while music plays; 0 = never touch).
 * The original clock is saved first and restored when the music is stopped.
 * Two threads: a decoder thread fills a ring buffer, an output thread plays it.
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
#define VOL_FILE     "ms0:/SEPLUGINS/AutoMusic.vol"
#define START_DELAY  12000000      /* wait for the XMB to finish loading (us) */
#define DOUBLE_US    350000        /* max gap between the two presses (us)    */
#define VOL_REPEAT   180000        /* volume step repeat while held (us)      */
#define VOL_MAX      16
#define MAX_FILES    200
#define NAME_LEN     128
#define BUF_SZ       (32 * 1024)
#define OUT_FRAMES   1152
#define RING_BLOCKS  20            /* about 0.5 s of audio buffered           */
#define PREBUFFER    6             /* blocks to collect before (re)starting   */
#define RING_SPARE   4             /* blocks the decoder never writes into, so it
                                      cannot overwrite what the hardware is playing */
#define OUT_HZ       44100         /* normal PSP audio channel rate           */
#define CFG_FILE     "ms0:/SEPLUGINS/AutoMusic.cfg"

enum { CMD_NONE = 0, CMD_NEXT, CMD_TOGGLE };

static char names[MAX_FILES][NAME_LEN];
static int nfiles = 0;

static mp3dec_t dec;
static unsigned char buf[BUF_SZ] __attribute__((aligned(64)));
static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME] __attribute__((aligned(64)));

static short ring[RING_BLOCKS][OUT_FRAMES * 2] __attribute__((aligned(64)));
static volatile unsigned int ring_head = 0;   /* written by decoder thread */
static volatile unsigned int ring_tail = 0;   /* written by output thread  */
static volatile int flush_req = 0;
static volatile int cmd = CMD_NONE;
static int blk_fill = 0;

static volatile int vol_level = VOL_MAX;      /* 0..16, plugin volume      */
static volatile int vol_dirty = 0;

/* ---- CPU clock: lowered only while music plays, original restored after ---- */
static int clock_saved = 0, orig_cpu = 0, orig_bus = 0;
static int cpu_target = 0;                           /* MHz, 0 = do not touch */

static void load_config(void)
{
    char t[32];
    SceUID f = sceIoOpen(CFG_FILE, PSP_O_RDONLY, 0);
    if (f < 0) {                                     /* first run: write the default */
        f = sceIoOpen(CFG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (f >= 0) { sceIoWrite(f, "0\r\n", 3); sceIoClose(f); }
        return;
    }
    int n = sceIoRead(f, t, sizeof(t) - 1);
    sceIoClose(f);
    if (n <= 0) return;
    t[n] = 0;

    int i = 0, v = 0, have = 0;
    while (t[i] && (t[i] < '0' || t[i] > '9')) i++;
    while (t[i] >= '0' && t[i] <= '9' && v < 1000) { v = v * 10 + (t[i] - '0'); i++; have = 1; }
    if (!have) return;
    if (v == 0) cpu_target = 0;
    else if (v < 33) cpu_target = 33;
    else if (v > 333) cpu_target = 333;
    else cpu_target = v;
}

static void clock_low(void)
{
    if (cpu_target == 0) return;                     /* user asked: never touch the clock */
    int cpu = scePowerGetCpuClockFrequency();
    if (cpu <= cpu_target + 10) return;              /* already low enough */
    if (!clock_saved) {
        orig_cpu = cpu;
        orig_bus = scePowerGetBusClockFrequency();
        clock_saved = 1;
    }
    scePowerSetClockFrequency(cpu_target, cpu_target, cpu_target / 2);
}

static void clock_restore(void)
{
    if (!clock_saved) return;
    scePowerSetClockFrequency(orig_cpu, orig_cpu, orig_bus);
    clock_saved = 0;
}

/* ---- remembered volume ---- */
static void load_volume(void)
{
    SceUID f = sceIoOpen(VOL_FILE, PSP_O_RDONLY, 0);
    if (f < 0) return;
    unsigned char v = 0;
    if (sceIoRead(f, &v, 1) == 1 && v <= VOL_MAX) vol_level = v;
    sceIoClose(f);
}

static void save_volume_if_changed(void)
{
    if (!vol_dirty) return;
    vol_dirty = 0;
    SceUID f = sceIoOpen(VOL_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0) return;
    unsigned char v = (unsigned char)vol_level;
    sceIoWrite(f, &v, 1);
    sceIoClose(f);
}

/* ---- Note button (output thread only) ----
 * single press = NEXT, double press = TOGGLE,
 * Note held + R = louder, Note held + L = quieter                       */
static int was_down = 0, pending = 0, swallow = 0, combo = 0;
static unsigned int rel_t = 0, vol_t = 0;

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
        combo = 0;
        if (pending) { pending = 0; swallow = 1; cmd = CMD_TOGGLE; }
    } else if (!down && was_down) {
        if (swallow) swallow = 0;
        else if (combo) combo = 0;
        else { pending = 1; rel_t = now; }
    }

    if (down && !swallow) {
        int step = 0;
        if (pad.Buttons & PSP_CTRL_RTRIGGER) step = 1;
        else if (pad.Buttons & PSP_CTRL_LTRIGGER) step = -1;
        if (step) {
            combo = 1;
            if ((now - vol_t) >= VOL_REPEAT) {
                vol_t = now;
                int v = vol_level + step;
                if (v < 0) v = 0;
                if (v > VOL_MAX) v = VOL_MAX;
                if (v != vol_level) { vol_level = v; vol_dirty = 1; }
            }
        }
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

/* ---- output thread: plays ring buffer blocks, reads the buttons ---- */
static int output_thread(SceSize args, void *argp)
{
    int ch = -1, buffering = 1;

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
            buffering = 1;
            sceKernelDelayThread(2000);
            continue;
        }
        if (buffering) {
            if (avail < PREBUFFER) { sceKernelDelayThread(2000); continue; }
            buffering = 0;
        }

        int slot = ring_tail % RING_BLOCKS;
        if (ch < 0) {
            ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_FRAMES, PSP_AUDIO_FORMAT_STEREO);
            if (ch < 0) {
                sceKernelDelayThread(500000);
                continue;
            }
        }
        int lv = vol_level;
        int volume = lv * lv * 128;               /* 0 .. 0x8000, finer at low levels */
        sceKernelDcacheWritebackRange(ring[slot], OUT_FRAMES * 2 * sizeof(short));
        sceAudioOutputBlocking(ch, volume, ring[slot]);
        ring_tail++;
    }
    return 0;
}

/* ---- decoder side ---- */
static void flush_ring(void)
{
    flush_req = 1;
    for (int i = 0; i < 500 && flush_req; i++) sceKernelDelayThread(1000);
    if (flush_req) { ring_tail = ring_head; flush_req = 0; }
    blk_fill = 0;
}

static int wait_space(void)   /* returns 1 if a button command arrived */
{
    while ((ring_head - ring_tail) >= RING_BLOCKS - RING_SPARE) {
        if (cmd != CMD_NONE) return 1;
        sceKernelDelayThread(3000);
    }
    return 0;
}

static int rs_phase = 0;                 /* 16.16 fixed point          */
static int prev_l = 0, prev_r = 0;

static int emit_out(short l, short r)    /* returns 1 if a command arrived */
{
    if (blk_fill == 0 && wait_space()) return 1;
    short *dst = ring[ring_head % RING_BLOCKS];
    dst[blk_fill * 2]     = l;
    dst[blk_fill * 2 + 1] = r;
    if (++blk_fill == OUT_FRAMES) {
        __sync_synchronize();
        ring_head++;
        blk_fill = 0;
    }
    return 0;
}

/* converts any MP3 sample rate to 44100 Hz (linear interpolation) */
static int push_frames(const short *p, int frames, int ch, int hz)
{
    int step = (int)((((unsigned int)hz) << 16) / OUT_HZ);
    for (int i = 0; i < frames; i++) {
        int l = p[i * ch];
        int r = (ch == 2) ? p[i * ch + 1] : l;
        while (rs_phase < 65536) {
            int f = rs_phase >> 1;
            short ol = (short)(prev_l + (((l - prev_l) * f) >> 15));
            short orr = (short)(prev_r + (((r - prev_r) * f) >> 15));
            if (emit_out(ol, orr)) return 1;
            rs_phase += step;
        }
        rs_phase -= 65536;
        prev_l = l;
        prev_r = r;
    }
    return 0;
}

/* returns CMD_NONE (song ended / error), CMD_NEXT or CMD_TOGGLE */
static int play_file(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { sceKernelDelayThread(500000); return CMD_NONE; }

    mp3dec_init(&dec);
    int pos = 0, filled = 0, eof = 0, need_more = 0, result = CMD_NONE, tick = 0;
    blk_fill = 0;
    rs_phase = 0;
    prev_l = prev_r = 0;

    while (1) {
        if (cmd != CMD_NONE) { result = cmd; cmd = CMD_NONE; break; }

        if (!eof && ((filled - pos) < BUF_SZ / 2 || need_more)) {
            if (pos > 0) {
                memmove(buf, buf + pos, filled - pos);
                filled -= pos;
                pos = 0;
            }
            int n = sceIoRead(fd, buf + filled, BUF_SZ - filled);
            if (n > 0) filled += n; else eof = 1;
            need_more = 0;
        }

        int avail = filled - pos;
        if (avail <= 0) break;

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, buf + pos, avail, pcm, &info);

        if (info.frame_bytes > 0)  pos += info.frame_bytes;
        else if (eof)              break;
        else if (avail >= BUF_SZ)  pos += 1024;
        else                       need_more = 1;

        if (samples > 0 && push_frames(pcm, samples, info.channels, info.hz)) {
            result = cmd;
            cmd = CMD_NONE;
            break;
        }

        /* keep the CPU clock low while playing, in case something raised it */
        if ((++tick & 255) == 0) clock_low();
        save_volume_if_changed();
    }

    sceIoClose(fd);
    return result;
}

static int decoder_thread(SceSize args, void *argp)
{
    sceKernelDelayThread(START_DELAY);
    load_config();
    load_volume();
    scan();
    if (nfiles == 0) return 0;

    int idx = 0, stopped = 0;
    while (1) {
        if (stopped) {
            sceKernelDelayThread(20000);
            save_volume_if_changed();
            if (cmd != CMD_NONE) { cmd = CMD_NONE; stopped = 0; }
            continue;
        }

        clock_low();
        int r = play_file(names[idx]);
        if (r == CMD_TOGGLE) {
            flush_ring();
            clock_restore();
            stopped = 1;
        } else if (r == CMD_NEXT) {
            flush_ring();
            idx = (idx + 1) % nfiles;
        } else {
            idx = (idx + 1) % nfiles;     /* song ended: let the buffer drain */
        }
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID t1 = sceKernelCreateThread("AutoMusicOut", output_thread, 0x18, 0x4000, 0, NULL);
    if (t1 >= 0) sceKernelStartThread(t1, 0, NULL);

    SceUID t2 = sceKernelCreateThread("AutoMusicDec", decoder_thread, 0x22, 0x10000, 0, NULL);
    if (t2 >= 0) sceKernelStartThread(t2, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    clock_restore();
    return 0;
}
