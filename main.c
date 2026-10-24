/*
 * AutoMusic - PSP kernel plugin (ARK-4, VSH/XMB)
 * Plays every .mp3 in ms0:/MUSIC automatically after the XMB loads.
 *   Note button, single press : stop / resume
 *   Note button, double press : next song
 *   Hold Note + L trigger     : plugin volume up
 *   Hold Note + R trigger     : plugin volume down
 * The plugin volume is separate from the PSP volume keys and is remembered.
 *
 * When another sound starts (video, music player, ...) the music fades out and
 * pauses, then resumes with a fade in when the other sound ends. Short menu
 * sound effects are ignored.
 *
 * Decode once: a song that plays through to the end is saved as raw sound in
 * ms0:/SEPLUGINS/AMCache/ ; the next time it plays straight from that file with no
 * MP3 decoding at all (about 10 MB per minute of music).
 *
 * ms0:/SEPLUGINS/AutoMusic.cfg (created on first run) holds three numbers:
 *   line 1: CPU MHz used only while music plays (33..333), 0 = never touch the clock
 *   line 2: 1 = fade out when other sound starts, 0 = off
 *   line 3: maximum size of the sound cache in MB, 0 = no cache (default 1024)
 * Two threads: a decoder thread fills a ring buffer, an output thread plays it.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <psptypes.h>
#include <string.h>

#if __has_include(<systemctrl.h>)
#include <systemctrl.h>
#else
u32 sctrlHENFindFunction(const char *szMod, const char *szLib, u32 nid);
void sctrlHENPatchSyscall(void *addr, void *newaddr);
#endif

#include "osd.h"

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
#define DISPLAY_NAME_LEN 64
#define BUF_SZ       (32 * 1024)
#define OUT_FRAMES   1152
#define RING_BLOCKS  20            /* about 0.5 s of audio buffered           */
#define PREBUFFER    6             /* blocks to collect before (re)starting   */
#define RING_SPARE   4             /* blocks the decoder never writes into, so it
                                      cannot overwrite what the hardware is playing */
#define OUT_HZ       44100         /* normal PSP audio channel rate           */
#define FADE_MAX     256
#define FADE_OUT_STEP 6            /* per 26 ms block: about 1.1 s fade out   */
#define FADE_IN_STEP  12           /* about 0.6 s fade in                     */
#define BUSY_TRIGGER 800000        /* other sound must last this long (us)    */
#define BUSY_CAP     2000000
#define CFG_FILE     "ms0:/SEPLUGINS/AutoMusic.cfg"
#define CACHE_DIR    "ms0:/SEPLUGINS/AMCache"
#define BLOCK_BYTES  (OUT_FRAMES * 4)          /* one 44100 Hz stereo block */
#define CW_SZ        (8 * BLOCK_BYTES)         /* cache write buffer        */
#define MAGIC_OK     0x31434d41u               /* "AMC1" complete cache file */
#define MAGIC_PART   0x30434d41u               /* "AMC0" being written       */
#define CACHE_BATCH_MIN 8                      /* read cache in bursts of 8..12 blocks */
#define CACHE_BATCH_MAX 12
#define FREE_FLOOR_MB 256                      /* never fill the stick below this */

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

static volatile int fade = FADE_MAX;          /* current fade gain 0..256  */
static volatile int fade_target = FADE_MAX;
static volatile int is_stopped = 0;           /* music stopped (set by decoder) */
static volatile int autofade_on = 1;
static volatile int detect_ready = 0;
static volatile int auto_pause_pending = 0;
static volatile int auto_stopped = 0;            /* paused by other-audio detection */
static unsigned int ignore_mask = 0;          /* channels busy while the XMB was idle */

/* OSD state is shared with the display-flip hook. */
static char display_name[DISPLAY_NAME_LEN];
static volatile unsigned int display_elapsed_frames = 0;
static volatile unsigned int display_total_frames = 0;
static volatile int display_track_ready = 0;

#define NID_SCE_DISPLAY_SET_FRAME_BUF 0x289D82FE
static int (*display_set_frame_buf_orig)(void *topaddr, int bufferwidth,
                                         int pixelformat, int sync) = NULL;
static volatile int display_hook_inflight = 0;
static volatile int display_hook_enabled = 0;

static void display_track_begin(const char *path)
{
    char title[DISPLAY_NAME_LEN];
    const char *base = path;
    const char *end;
    int i = 0, intr;
    memset(title, 0, sizeof(title));
    while (*path) {
        if (*path == '/' || *path == '\\') base = path + 1;
        path++;
    }
    end = base + strlen(base);
    if (end - base >= 4 && end[-4] == '.') end -= 4;
    while (base + i < end && i < DISPLAY_NAME_LEN - 1) {
        title[i] = base[i];
        i++;
    }
    title[i] = '\0';

    intr = sceKernelCpuSuspendIntr();
    memcpy(display_name, title, sizeof(display_name));
    display_elapsed_frames = 0;
    display_total_frames = 0;
    display_track_ready = 1;
    sceKernelCpuResumeIntr(intr);
}

static void display_set_total(unsigned int frames)
{
    int intr = sceKernelCpuSuspendIntr();
    display_total_frames = frames;
    sceKernelCpuResumeIntr(intr);
}

static void display_set_estimated_total(unsigned int file_bytes,
                                        unsigned int bytes_seen,
                                        unsigned int source_samples,
                                        int source_hz)
{
    unsigned long long total;
    if (!bytes_seen || !source_samples || source_hz <= 0) return;
    total = (unsigned long long)file_bytes * source_samples / bytes_seen;
    total = total * OUT_HZ / (unsigned int)source_hz;
    if (total > 0xFFFFFFFFu) total = 0xFFFFFFFFu;
    display_set_total((unsigned int)total);
}

static int display_set_frame_buf_hook(void *topaddr, int bufferwidth,
                                      int pixelformat, int sync)
{
    char title[DISPLAY_NAME_LEN];
    unsigned int elapsed, total, phase;
    int ready, playing, intr;

    intr = sceKernelCpuSuspendIntr();
    display_hook_inflight++;
    ready = display_track_ready;
    if (ready) {
        memcpy(title, display_name, sizeof(title));
        elapsed = display_elapsed_frames;
        total = display_total_frames;
    }
    playing = !is_stopped && !auto_stopped;
    sceKernelCpuResumeIntr(intr);

    if (ready) {
        phase = (unsigned int)(sceKernelGetSystemTimeLow() / 100000u);
        osd_draw(topaddr, bufferwidth, pixelformat, title, elapsed, total,
                 playing, phase);
    }

    int result = display_set_frame_buf_orig(topaddr, bufferwidth, pixelformat, sync);
    intr = sceKernelCpuSuspendIntr();
    display_hook_inflight--;
    sceKernelCpuResumeIntr(intr);
    return result;
}

static void install_display_hook(void)
{
    u32 address = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay",
                                       NID_SCE_DISPLAY_SET_FRAME_BUF);
    if (!address) return;
    display_set_frame_buf_orig = (void *)address;
    display_hook_enabled = 1;
    sctrlHENPatchSyscall((void *)address, display_set_frame_buf_hook);
}

static void remove_display_hook(void)
{
    int intr;
    if (!display_hook_enabled || !display_set_frame_buf_orig) return;
    display_hook_enabled = 0;
    sctrlHENPatchSyscall((void *)display_set_frame_buf_orig,
                         (void *)display_set_frame_buf_orig);
    do {
        sceKernelDelayThread(1000);
        intr = sceKernelCpuSuspendIntr();
        int inflight = display_hook_inflight;
        sceKernelCpuResumeIntr(intr);
        if (!inflight) break;
    } while (1);
    display_set_frame_buf_orig = NULL;
}

/* ---- sound cache ---- */
static int cache_limit_mb = 1024;                    /* 0 = off */
static unsigned int cache_used = 0;                  /* bytes in the cache folder */
static SceUID cache_fd = -1;                         /* file being written, or -1 */
static int cache_bad = 0;
static unsigned int cache_blocks = 0;
static unsigned int cw_fill = 0;
static unsigned char cw[CW_SZ] __attribute__((aligned(64)));

/* ---- CPU clock: lowered only while music plays, original restored after ---- */
static int clock_saved = 0, orig_cpu = 0, orig_bus = 0;
static int cpu_target = 0;                           /* MHz, 0 = do not touch */

static int next_number(const char *t, int *i, int *out)
{
    while (t[*i] && (t[*i] < '0' || t[*i] > '9')) (*i)++;
    if (!t[*i]) return 0;
    int v = 0;
    while (t[*i] >= '0' && t[*i] <= '9') {
        if (v < 1000) v = v * 10 + (t[*i] - '0');
        (*i)++;
    }
    *out = v;
    return 1;
}

static void load_config(void)
{
    char t[32];
    SceUID f = sceIoOpen(CFG_FILE, PSP_O_RDONLY, 0);
    if (f < 0) {                                     /* first run: write the default */
        f = sceIoOpen(CFG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (f >= 0) { sceIoWrite(f, "0\r\n1\r\n1024\r\n", 12); sceIoClose(f); }
        return;
    }
    int n = sceIoRead(f, t, sizeof(t) - 1);
    sceIoClose(f);
    if (n <= 0) return;
    t[n] = 0;

    int i = 0, v = 0;
    if (next_number(t, &i, &v)) {
        if (v == 0) cpu_target = 0;
        else if (v < 33) cpu_target = 33;
        else if (v > 333) cpu_target = 333;
        else cpu_target = v;
    }
    if (next_number(t, &i, &v)) autofade_on = (v != 0);
    if (next_number(t, &i, &v)) cache_limit_mb = (v > 4000) ? 4000 : v;
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
 * single press = TOGGLE, double press = NEXT,
 * Note held + L = louder, Note held + R = quieter                       */
static int was_down = 0, pending = 0, swallow = 0, combo = 0;
static unsigned int rel_t = 0, vol_t = 0;

static void poll_button(void)
{
    SceCtrlData pad;
    unsigned int now = sceKernelGetSystemTimeLow();

    if (pending && (now - rel_t) >= DOUBLE_US) {
        pending = 0;
        cmd = CMD_TOGGLE;
    }

    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return;
    int down = (pad.Buttons & PSP_CTRL_NOTE) != 0;

    if (down && !was_down) {
        combo = 0;
        if (pending) { pending = 0; swallow = 1; cmd = CMD_NEXT; }
    } else if (!down && was_down) {
        if (swallow) swallow = 0;
        else if (combo) combo = 0;
        else { pending = 1; rel_t = now; }
    }

    if (down && !swallow) {
        int step = 0;
        if (pad.Buttons & PSP_CTRL_LTRIGGER) step = 1;        /* Note + L = louder  */
        else if (pad.Buttons & PSP_CTRL_RTRIGGER) step = -1;  /* Note + R = quieter */
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

/* ---- sound cache helpers ---- */
typedef struct { unsigned int maxClusters, freeClusters; int unk1; unsigned int sectorSize, sectorCount; } MsInfo;

static unsigned int free_mb(void)
{
    MsInfo info;
    MsInfo *p = &info;
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ms0:", 0x02425818, &p, sizeof(p), NULL, 0) < 0) return 0;
    unsigned int cluster_kb = (info.sectorSize * info.sectorCount) / 1024;
    return (info.freeClusters * cluster_kb) / 1024;
}

static void cache_init(void)
{
    if (cache_limit_mb <= 0) return;
    sceIoMkdir(CACHE_DIR, 0777);
    SceUID d = sceIoDopen(CACHE_DIR);
    if (d < 0) { cache_limit_mb = 0; return; }       /* cannot use a cache folder */
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        cache_used += (unsigned int)e.d_stat.st_size;
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static void cache_path(char *out, const char *path, unsigned int size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned int h = 2166136261u;
    while (*path) { h ^= (unsigned char)*path++; h *= 16777619u; }
    h ^= size;
    h *= 16777619u;
    int n = 0;
    const char *d = CACHE_DIR "/";
    while (*d) out[n++] = *d++;
    for (int i = 7; i >= 0; i--) out[n++] = hex[(h >> (i * 4)) & 15];
    out[n++] = '.'; out[n++] = 'p'; out[n++] = 'c'; out[n++] = 'm'; out[n] = 0;
}

static void cache_flush(void)
{
    if (cw_fill && cache_fd >= 0 && !cache_bad) {
        if (sceIoWrite(cache_fd, cw, cw_fill) != (int)cw_fill) cache_bad = 1;
    }
    cw_fill = 0;
}

static void cache_add(const void *block)             /* one finished 44100 Hz stereo block */
{
    if (cache_bad) return;
    memcpy(cw + cw_fill, block, BLOCK_BYTES);
    cw_fill += BLOCK_BYTES;
    cache_blocks++;
    if (cw_fill + BLOCK_BYTES > CW_SZ) cache_flush();
}

static void cache_begin(const char *cpath, unsigned int mp3_size)
{
    cache_fd = -1;
    cache_bad = 0;
    cache_blocks = 0;
    cw_fill = 0;
    if (cache_limit_mb <= 0) return;
    if (cache_used >= (unsigned int)cache_limit_mb * 1048576u) return;   /* cache is full */
    if (free_mb() < FREE_FLOOR_MB) return;                              /* stick nearly full */
    SceUID f = sceIoOpen(cpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0) return;
    unsigned int hdr[4] = { MAGIC_PART, mp3_size, 0, 0 };
    if (sceIoWrite(f, hdr, 16) != 16) { sceIoClose(f); sceIoRemove(cpath); return; }
    cache_fd = f;
}

static void cache_end(const char *cpath, unsigned int mp3_size, int complete)
{
    if (cache_fd < 0) return;
    if (complete && !cache_bad) {
        cache_flush();
        if (!cache_bad && cache_blocks > 0) {
            unsigned int hdr[4] = { MAGIC_OK, mp3_size, cache_blocks, 0 };
            sceIoLseek32(cache_fd, 0, PSP_SEEK_SET);
            if (sceIoWrite(cache_fd, hdr, 16) != 16) cache_bad = 1;
        } else {
            cache_bad = 1;
        }
    }
    sceIoClose(cache_fd);
    if (!complete || cache_bad) sceIoRemove(cpath);                      /* unfinished: delete */
    else cache_used += 16 + cache_blocks * BLOCK_BYTES;
    cache_fd = -1;
    cw_fill = 0;
}

/* ---- detects other sound (video, music player ...) on the PSP audio channels ---- */
static unsigned int busy_mask(int own)
{
    unsigned int m = 0;
    for (int c = 0; c <= 8; c++) {
        if (c == own) continue;
        if (sceAudioGetChannelRestLen(c) > 0) m |= 1u << c;
    }
    return m;
}

static void calibrate_other_audio(void)    /* remembers channels that are always busy */
{
    int counts[9] = { 0 };
    const int samples = 30;
    for (int n = 0; n < samples; n++) {
        unsigned int m = busy_mask(-1);
        for (int c = 0; c <= 8; c++) if (m & (1u << c)) counts[c]++;
        sceKernelDelayThread(50000);
    }
    for (int c = 0; c <= 8; c++) if (counts[c] > samples / 2) ignore_mask |= 1u << c;
}

static unsigned int busy_us = 0, last_det = 0;
static int other_playing = 0;

static void poll_other_audio(int own)
{
    if (!detect_ready || !autofade_on) return;
    unsigned int now = sceKernelGetSystemTimeLow();
    unsigned int dt = now - last_det;
    if (dt < 50000) return;
    last_det = now;
    if (dt > 200000) dt = 200000;

    int busy = (busy_mask(own) & ~ignore_mask) != 0;
    if (busy) {
        busy_us += dt;
        if (busy_us > BUSY_CAP) busy_us = BUSY_CAP;
    } else {
        unsigned int d = dt * 2;
        busy_us = (busy_us > d) ? busy_us - d : 0;
    }

    if (!other_playing && busy_us >= BUSY_TRIGGER) {
        other_playing = 1;
        if (!is_stopped) { fade_target = 0; auto_pause_pending = 1; }
    } else if (other_playing && busy_us == 0) {
        other_playing = 0;
        if (auto_pause_pending && !is_stopped) {
            /* The competing sound ended before the fade-out completed. */
            auto_pause_pending = 0;
            fade_target = FADE_MAX;
        }
    }
}

/* Wait in the current player's read loop, preserving its position and decoder state. */
static int resume_after_auto_pause(int command)
{
    if (command != CMD_TOGGLE || !auto_stopped) return 0;

    while (other_playing) sceKernelDelayThread(40000);
    fade = 0;
    fade_target = FADE_MAX;
    auto_stopped = 0;
    return 1;
}

/* ---- output thread: plays ring buffer blocks, reads the buttons ---- */
static int output_thread(SceSize args, void *argp)
{
    int ch = -1, buffering = 1;

    while (1) {
        poll_button();
        poll_other_audio(ch);

        /* Hold the queued audio until the decoder resumes after the other sound ends. */
        if (auto_stopped) {
            sceKernelDelayThread(8000);
            continue;
        }

        if (flush_req) {
            ring_tail = ring_head;
            buffering = 1;
            flush_req = 0;
            continue;
        }

        unsigned int avail = ring_head - ring_tail;
        if (avail == 0) {
            buffering = 1;
            sceKernelDelayThread(8000);
            continue;
        }
        if (buffering) {
            if (avail < PREBUFFER) { sceKernelDelayThread(4000); continue; }
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
        int base = lv * lv * 128;                 /* 0 .. 0x8000, finer at low levels */

        int f = fade;                             /* fade in / out, one step per block */
        if (f > fade_target) { f -= FADE_OUT_STEP; if (f < fade_target) f = fade_target; }
        else if (f < fade_target) { f += FADE_IN_STEP; if (f > fade_target) f = fade_target; }
        fade = f;
        int volume = (base * f) / FADE_MAX;
        if (f == 0 && auto_pause_pending) {       /* faded out: stop the music */
            auto_pause_pending = 0;
            if (!is_stopped) {
                auto_stopped = 1;
                cmd = CMD_TOGGLE;
            }
        }
        sceKernelDcacheWritebackRange(ring[slot], OUT_FRAMES * 2 * sizeof(short));
        sceAudioOutputBlocking(ch, volume, ring[slot]);
        if (display_track_ready) {
            int intr = sceKernelCpuSuspendIntr();
            display_elapsed_frames += OUT_FRAMES;
            sceKernelCpuResumeIntr(intr);
        }
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
        sceKernelDelayThread(8000);
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
        if (cache_fd >= 0) cache_add(dst);
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

/* plays a song from its cache file (no decoding). Returns 1 if the cache file was usable. */
static int play_cached(const char *cpath, unsigned int mp3_size, int *result)
{
    SceUID cf = sceIoOpen(cpath, PSP_O_RDONLY, 0);
    if (cf < 0) return 0;

    unsigned int hdr[4];
    int fsize = sceIoLseek32(cf, 0, PSP_SEEK_END);
    sceIoLseek32(cf, 0, PSP_SEEK_SET);
    if (sceIoRead(cf, hdr, 16) != 16 || hdr[0] != MAGIC_OK || hdr[1] != mp3_size ||
        hdr[2] == 0 || fsize != (int)(16 + hdr[2] * BLOCK_BYTES)) {
        sceIoClose(cf);
        sceIoRemove(cpath);                           /* unfinished or outdated: rebuild */
        return 0;
    }

    unsigned int blocks_left = hdr[2];
    int tick = 0, res = CMD_NONE;
    blk_fill = 0;
    display_set_total(blocks_left * OUT_FRAMES);

    while (blocks_left > 0) {
        if (cmd != CMD_NONE) {
            res = cmd;
            cmd = CMD_NONE;
            if (resume_after_auto_pause(res)) { res = CMD_NONE; continue; }
            break;
        }

        int freeb = (RING_BLOCKS - RING_SPARE) - (int)(ring_head - ring_tail);
        if (freeb >= CACHE_BATCH_MIN) {
            int slot = ring_head % RING_BLOCKS;
            int k = freeb;
            if (k > RING_BLOCKS - slot) k = RING_BLOCKS - slot;
            if (k > CACHE_BATCH_MAX) k = CACHE_BATCH_MAX;
            if ((unsigned int)k > blocks_left) k = (int)blocks_left;
            sceKernelDcacheWritebackInvalidateRange(ring[slot], k * BLOCK_BYTES);
            int n = sceIoRead(cf, ring[slot], k * BLOCK_BYTES);
            int got = (n > 0) ? n / BLOCK_BYTES : 0;
            if (got == 0) break;
            __sync_synchronize();
            ring_head += got;
            blocks_left -= got;
        } else {
            sceKernelDelayThread(15000);
        }

        if ((++tick & 15) == 0) clock_low();
        save_volume_if_changed();
    }

    sceIoClose(cf);
    *result = res;
    return 1;
}

/* returns CMD_NONE (song ended / error), CMD_NEXT or CMD_TOGGLE */
static int play_file(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { sceKernelDelayThread(500000); return CMD_NONE; }

    display_track_begin(path);

    unsigned int mp3_size = (unsigned int)sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char cpath[64];
    cache_path(cpath, path, mp3_size);
    if (cache_limit_mb > 0) {
        int cres = CMD_NONE;
        if (play_cached(cpath, mp3_size, &cres)) { sceIoClose(fd); return cres; }
    }
    cache_begin(cpath, mp3_size);

    mp3dec_init(&dec);
    unsigned int consumed = 0, source_samples = 0;
    int pos = 0, filled = 0, eof = 0, need_more = 0, result = CMD_NONE, tick = 0;
    blk_fill = 0;
    rs_phase = 0;
    prev_l = prev_r = 0;

    while (1) {
        if (cmd != CMD_NONE) {
            result = cmd;
            cmd = CMD_NONE;
            if (resume_after_auto_pause(result)) { result = CMD_NONE; continue; }
            break;
        }

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

        if (info.frame_bytes > 0)  { pos += info.frame_bytes; consumed += info.frame_bytes; }
        else if (eof)              break;
        else if (avail >= BUF_SZ)  { pos += 1024; consumed += 1024; }
        else                       need_more = 1;

        if (samples > 0 && info.frame_bytes > 0) {
            source_samples += samples;
            if ((tick & 31) == 0 || display_total_frames == 0)
                display_set_estimated_total(mp3_size, consumed, source_samples, info.hz);
        }

        if (samples > 0 && push_frames(pcm, samples, info.channels, info.hz)) {
            result = cmd;
            cmd = CMD_NONE;
            if (resume_after_auto_pause(result)) { result = CMD_NONE; continue; }
            break;
        }

        /* keep the CPU clock low while playing, in case something raised it */
        if ((++tick & 255) == 0) clock_low();
        save_volume_if_changed();
    }

    cache_end(cpath, mp3_size, result == CMD_NONE && consumed >= (mp3_size / 10) * 9);
    sceIoClose(fd);
    return result;
}

static int decoder_thread(SceSize args, void *argp)
{
    sceKernelDelayThread(START_DELAY);
    load_config();
    load_volume();
    cache_init();
    if (autofade_on) calibrate_other_audio();
    detect_ready = 1;
    scan();
    if (nfiles == 0) return 0;

    int idx = 0, stopped = 0;
    while (1) {
        if (stopped) {
            sceKernelDelayThread(40000);
            save_volume_if_changed();
            if (cmd != CMD_NONE || (auto_stopped && !other_playing)) {
                int resume_cmd = cmd;
                cmd = CMD_NONE;
                if (resume_cmd == CMD_NEXT) idx = (idx + 1) % nfiles;
                stopped = 0;
                is_stopped = 0;
                auto_stopped = 0;
                fade = 0;                      /* fade the music back in */
                fade_target = FADE_MAX;
            }
            continue;
        }

        clock_low();
        int r = play_file(names[idx]);
        if (r == CMD_TOGGLE) {
            flush_ring();
            clock_restore();
            stopped = 1;
            is_stopped = 1;
            /* auto_stopped is set by the output thread only for auto-pauses. */
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
    install_display_hook();
    SceUID t1 = sceKernelCreateThread("AutoMusicOut", output_thread, 0x18, 0x4000, 0, NULL);
    if (t1 >= 0) sceKernelStartThread(t1, 0, NULL);

    SceUID t2 = sceKernelCreateThread("AutoMusicDec", decoder_thread, 0x22, 0x10000, 0, NULL);
    if (t2 >= 0) sceKernelStartThread(t2, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    if (!is_stopped && ring_head != ring_tail) {      /* fade out before unloading */
        fade_target = 0;
        for (int i = 0; i < 80 && fade > 0; i++) sceKernelDelayThread(10000);
    }
    remove_display_hook();
    clock_restore();
    return 0;
}
