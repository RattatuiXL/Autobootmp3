/*
 * AutoMusic - PSP kernel plugin (ARK-4, VSH/XMB)
 * Plays every .mp3 in ms0:/MUSIC automatically after the XMB loads.
 *   Note button, short press : next song
 *   Note button, hold ~1 sec : stop (hold again or press to resume)
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
#define HOLD_US      800000        /* hold time that counts as "stop" (us)    */
#define MAX_FILES    200
#define NAME_LEN     128
#define BUF_SZ       (16 * 1024)
#define OUT_FRAMES   1152

enum { CMD_NONE = 0, CMD_NEXT, CMD_TOGGLE };

static char names[MAX_FILES][NAME_LEN];
static int nfiles = 0;

static mp3dec_t dec;
static unsigned char buf[BUF_SZ] __attribute__((aligned(64)));
static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME] __attribute__((aligned(64)));
static short outbuf[OUT_FRAMES * 2] __attribute__((aligned(64)));
static int outfill = 0;

static int cmd = CMD_NONE;
static int was_down = 0, handled = 0;
static unsigned int t0 = 0;

/* ---- Note button: short press = NEXT, hold = TOGGLE ---- */
static void poll_button(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return;
    int down = (pad.Buttons & PSP_CTRL_NOTE) != 0;
    unsigned int now = sceKernelGetSystemTimeLow();

    if (down && !was_down) {
        t0 = now;
        handled = 0;
    } else if (down && !handled && (now - t0) >= HOLD_US) {
        handled = 1;
        cmd = CMD_TOGGLE;
    } else if (!down && was_down && !handled) {
        cmd = CMD_NEXT;
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

/* ---- audio output: returns 1 if a button command arrived ---- */
static int emit(const short *p, int frames, int ch)
{
    for (int i = 0; i < frames; i++) {
        short l = p[i * ch];
        short r = (ch == 2) ? p[i * ch + 1] : l;
        outbuf[outfill * 2]     = l;
        outbuf[outfill * 2 + 1] = r;
        if (++outfill == OUT_FRAMES) {
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, outbuf);
            outfill = 0;
            poll_button();
            if (cmd != CMD_NONE) return 1;
        }
    }
    return 0;
}

/* returns CMD_NONE (song ended / error), CMD_NEXT or CMD_TOGGLE */
static int play_file(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return CMD_NONE;

    mp3dec_init(&dec);
    int filled = 0, eof = 0, need_more = 0, hz = 0, result = CMD_NONE;
    outfill = 0;
    cmd = CMD_NONE;

    while (1) {
        if (!eof && (filled < BUF_SZ / 4 || need_more)) {
            int n = sceIoRead(fd, buf + filled, BUF_SZ - filled);
            if (n > 0) filled += n; else eof = 1;
            need_more = 0;
        }
        if (filled <= 0) break;

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, buf, filled, pcm, &info);

        if (info.frame_bytes > 0) {
            filled -= info.frame_bytes;
            memmove(buf, buf + info.frame_bytes, filled);
        } else if (eof) {
            break;
        } else if (filled >= BUF_SZ) {
            filled -= 1024;
            memmove(buf, buf + 1024, filled);
        } else {
            need_more = 1;
        }

        if (samples > 0) {
            if (info.hz != hz) {
                if (hz) sceAudioSRCChRelease();
                hz = 0;
                if (sceAudioSRCChReserve(OUT_FRAMES, info.hz, 2) < 0) {
                    sceKernelDelayThread(2000000);   /* audio busy, try next song later */
                    break;
                }
                hz = info.hz;
            }
            if (emit(pcm, samples, info.channels)) {
                result = cmd;
                cmd = CMD_NONE;
                break;
            }
        } else {
            poll_button();
            if (cmd != CMD_NONE) { result = cmd; cmd = CMD_NONE; break; }
        }
    }

    if (hz) sceAudioSRCChRelease();
    sceIoClose(fd);
    return result;
}

static int main_thread(SceSize args, void *argp)
{
    sceKernelDelayThread(START_DELAY);
    scan();
    if (nfiles == 0) return 0;

    scePowerSetClockFrequency(333, 333, 166);

    int idx = 0, stopped = 0;
    while (1) {
        if (stopped) {
            sceKernelDelayThread(50000);
            poll_button();
            if (cmd != CMD_NONE) { cmd = CMD_NONE; stopped = 0; }
            continue;
        }
        int r = play_file(names[idx]);
        if (r == CMD_TOGGLE) stopped = 1;
        else idx = (idx + 1) % nfiles;
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("AutoMusic", main_thread, 0x28, 0x10000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, args, argp);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    return 0;
}
