/*
 * playsound - play a 16-bit PCM .wav file on ccos
 *
 * Uses the kernel's simple OSS audio interface (/dev/dsp), so no audio
 * libraries are needed. Exits quietly if there is no sound card.
 *
 * Usage: playsound file.wav
 * Build: gcc -O2 -static -o playsound playsound.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: playsound file.wav\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "playsound: not a WAV file\n"); return 1;
    }
    int channels = 0, rate = 0, bits = 0;
    uint32_t data_len = 0;
    for (;;) {                                   /* walk the chunks: "fmt " then "data" */
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) { fprintf(stderr, "playsound: no audio data\n"); return 1; }
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[40] = { 0 };
            if (len > sizeof fmt || fread(fmt, 1, len, f) != len) return 1;
            if (le16(fmt) != 1) { fprintf(stderr, "playsound: only plain PCM is supported\n"); return 1; }
            channels = le16(fmt + 2); rate = (int)le32(fmt + 4); bits = le16(fmt + 14);
        } else if (!memcmp(ch, "data", 4)) { data_len = len; break; }
        else fseek(f, len + (len & 1), SEEK_CUR);
    }
    if (bits != 16 || channels < 1 || channels > 2) { fprintf(stderr, "playsound: need 16-bit mono/stereo\n"); return 1; }

    if (access("/run/user/0/pulse/native", F_OK) == 0) {   /* the sound server owns the card */
        fclose(f);
        setenv("PULSE_SERVER", "unix:/run/user/0/pulse/native", 1);
        execl("/usr/bin/paplay", "paplay", argv[1], (char *)NULL);
        f = fopen(argv[1], "rb");                    /* no paplay: try the card ourselves */
        if (!f) return 1;
        fseek(f, 12, SEEK_SET);
        for (;;) { unsigned char ch[8]; if (fread(ch, 1, 8, f) != 8) return 1;
                   uint32_t len = le32(ch + 4); if (!memcmp(ch, "data", 4)) break; fseek(f, len + (len & 1), SEEK_CUR); }
    }
    int dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) return 0;                       /* no sound card: stay silent */

    /* the volume is ccos's own setting (Settings > Sound, volume keys) */

    int fmt = AFMT_S16_LE, ch = channels, sp = rate;
    ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(dsp, SNDCTL_DSP_SPEED, &sp);

    char buf[8192];
    while (data_len > 0) {
        size_t want = data_len < sizeof buf ? data_len : sizeof buf;
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        if (write(dsp, buf, n) < 0) break;
        data_len -= (uint32_t)n;
    }
    ioctl(dsp, SNDCTL_DSP_SYNC, 0);              /* wait until it has finished playing */
    close(dsp);
    fclose(f);
    return 0;
}
