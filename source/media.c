/*
 * ccos-media - plays a video or a sound file for the Videos and Music apps
 *
 *   ccos-media FILE MAXW MAXH SHMFILE
 *
 * Decodes with FFmpeg. Sound goes to PulseAudio. Video frames (scaled to fit
 * MAXW x MAXH) are written into SHMFILE (two frame slots) and announced on
 * stdout. The desktop sends commands on stdin.
 *
 *   stdout: INFO w h duration has_video has_audio      (once, at the start)
 *           META title|artist|album                     (tags, if any)
 *           COVER w h                                   (album art in slot 0)
 *           FRAME slot                                   (a new picture)
 *           POS seconds                                  (a few times a second)
 *           END                                          (finished)
 *           ERR message
 *   stdin:  p (pause/resume)   s SECONDS (jump)   v 0-100 (volume)   q (quit)
 *
 * Build: gcc -O2 -o ccos-media media.c -lavformat -lavcodec -lswscale -lswresample -lavutil -lpulse-simple -lpulse -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <pulse/simple.h>
#include <pulse/error.h>

static AVFormatContext *fmt;
static AVCodecContext *vdec, *adec;
static int vs = -1, as = -1, cover = -1;
static int out_w, out_h;
static uint32_t *shm;                 /* 2 slots of out_w * out_h pixels */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int paused, quit, eof_demux, seek_req; static double seek_to;
static int volume = 100;
static double clock_base; static double clock_wall; /* audio clock: media time at a wall time */
static int clock_set;
static int gen;                                      /* bumps on every seek */
static int a_done, v_done;                           /* the sound / picture reached the end */

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void say(const char *f, ...) {
    va_list ap; va_start(ap, f);
    pthread_mutex_lock(&lk);
    vprintf(f, ap); fflush(stdout);
    pthread_mutex_unlock(&lk);
    va_end(ap);
}

/* ---- packet queues ----------------------------------------------------- */
typedef struct { AVPacket *p[256]; int r, w, n; } PQ;
static PQ vq, aq;
static int pq_put(PQ *q, AVPacket *p) {             /* called with lk held */
    if (q->n >= 256) return 0;
    q->p[q->w] = p; q->w = (q->w + 1) % 256; q->n++;
    return 1;
}
static AVPacket *pq_get(PQ *q) {
    if (!q->n) return NULL;
    AVPacket *p = q->p[q->r]; q->r = (q->r + 1) % 256; q->n--;
    return p;
}
static void pq_flush(PQ *q) { AVPacket *p; while ((p = pq_get(q))) av_packet_free(&p); }

/* media time now (sound's clock, else the wall clock) */
static double media_clock(void) {
    if (!clock_set) return -1;
    if (paused) return clock_base;
    return clock_base + (now() - clock_wall);
}

/* ---- demuxer: reads the file, hands packets to the decoders ------------ */
static void *demux_thread(void *x) {
    (void)x;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!quit && !seek_req && (vq.n >= 200 || aq.n >= 200 || eof_demux)) pthread_cond_wait(&cv, &lk);
        if (quit) { pthread_mutex_unlock(&lk); break; }
        if (seek_req) {
            int64_t ts = (int64_t)(seek_to * AV_TIME_BASE);
            avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0);
            pq_flush(&vq); pq_flush(&aq);
            seek_req = 0; eof_demux = 0; gen++;
            clock_base = seek_to; clock_wall = now(); clock_set = 1;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&lk);
            continue;
        }
        pthread_mutex_unlock(&lk);
        AVPacket *p = av_packet_alloc();
        int r = av_read_frame(fmt, p);
        pthread_mutex_lock(&lk);
        if (r < 0) { av_packet_free(&p); eof_demux = 1; }
        else if (p->stream_index == vs) { if (!pq_put(&vq, p)) av_packet_free(&p); }
        else if (p->stream_index == as) { if (!pq_put(&aq, p)) av_packet_free(&p); }
        else av_packet_free(&p);
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

/* ---- sound -------------------------------------------------------------- */
static pa_simple *pa;
static void *audio_thread(void *x) {
    (void)x;
    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };
    pa_buffer_attr ba = { .maxlength = (uint32_t)-1, .tlength = 48000 * 4 / 10, .prebuf = (uint32_t)-1, .minreq = (uint32_t)-1, .fragsize = (uint32_t)-1 };
    int err;
    pa = pa_simple_new(NULL, "ccos", PA_STREAM_PLAYBACK, NULL, "media", &ss, NULL, &ba, &err);
    if (!pa) say("ERR no sound (%s)\n", pa_strerror(err));
    SwrContext *swr = NULL;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL);
    swr_init(swr);
    AVFrame *f = av_frame_alloc();
    int16_t *buf = NULL; int bufcap = 0;
    int mygen = -1;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!quit && (paused || (!aq.n && !eof_demux))) pthread_cond_wait(&cv, &lk);
        if (quit) { pthread_mutex_unlock(&lk); break; }
        if (mygen != gen) { mygen = gen; avcodec_flush_buffers(adec); swr_init(swr); }
        AVPacket *p = pq_get(&aq);
        int done = !p && eof_demux;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lk);
        if (done) { usleep(20000); pthread_mutex_lock(&lk); int stop = eof_demux && !aq.n && !seek_req; pthread_mutex_unlock(&lk); if (stop) break; continue; }
        if (!p) continue;
        if (avcodec_send_packet(adec, p) < 0) { av_packet_free(&p); continue; }
        av_packet_free(&p);
        while (avcodec_receive_frame(adec, f) == 0) {
            pthread_mutex_lock(&lk); int stale = mygen != gen; pthread_mutex_unlock(&lk);
            if (stale) break;
            int n = swr_get_out_samples(swr, f->nb_samples);
            if (n * 2 > bufcap) { bufcap = n * 2 + 1024; buf = realloc(buf, (size_t)bufcap * 2); }
            uint8_t *outp[1] = { (uint8_t *)buf };
            int got = swr_convert(swr, outp, n, (const uint8_t **)f->extended_data, f->nb_samples);
            if (got <= 0) continue;
            double pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp * av_q2d(fmt->streams[as]->time_base) : -1;
            for (int i = 0; i < got * 2; i++) buf[i] = (int16_t)(buf[i] * volume / 100);
            if (pa) pa_simple_write(pa, buf, (size_t)got * 4, &err);
            else usleep((useconds_t)(got * 1000000.0 / 48000));
            if (pts >= 0) {                           /* the clock: what's coming out of the speaker now */
                pa_usec_t lat = pa ? pa_simple_get_latency(pa, &err) : 0;
                pthread_mutex_lock(&lk);
                if (!paused && mygen == gen) { clock_base = pts + got / 48000.0 - lat / 1e6; clock_wall = now(); clock_set = 1; }
                pthread_mutex_unlock(&lk);
            }
        }
    }
    if (pa) { pa_simple_drain(pa, &err); }
    pthread_mutex_lock(&lk); a_done = 1; pthread_mutex_unlock(&lk);
    av_frame_free(&f); free(buf); swr_free(&swr);
    return NULL;
}

/* ---- pictures ----------------------------------------------------------- */
static void *video_thread(void *x) {
    (void)x;
    AVFrame *f = av_frame_alloc();
    struct SwsContext *sws = NULL; int sw = 0, sh = 0, sf = -1;
    int slot = 0, mygen = -1;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!quit && !vq.n && !eof_demux) pthread_cond_wait(&cv, &lk);
        if (quit) { pthread_mutex_unlock(&lk); break; }
        if (mygen != gen) { mygen = gen; avcodec_flush_buffers(vdec); }
        AVPacket *p = pq_get(&vq);
        int done = !p && eof_demux;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&lk);
        if (done) { usleep(20000); pthread_mutex_lock(&lk); int stop = eof_demux && !vq.n && !seek_req; pthread_mutex_unlock(&lk); if (stop) break; continue; }
        if (!p) continue;
        if (avcodec_send_packet(vdec, p) < 0) { av_packet_free(&p); continue; }
        av_packet_free(&p);
        while (avcodec_receive_frame(vdec, f) == 0) {
            double pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp * av_q2d(fmt->streams[vs]->time_base) : 0;
            /* wait for its moment */
            for (;;) {
                pthread_mutex_lock(&lk);
                int stale = mygen != gen, q = quit, pz = paused;
                if (!clock_set && as < 0) { clock_base = pts; clock_wall = now(); clock_set = 1; }
                double c = media_clock();
                pthread_mutex_unlock(&lk);
                if (stale || q) goto next;
                if (pz) { usleep(20000); continue; }
                if (c < 0) { usleep(5000); continue; }          /* sound not started yet */
                double d = pts - c;
                if (d < -0.15) goto next;                       /* too late: drop it */
                if (d <= 0.005) break;
                usleep((useconds_t)(d > 0.05 ? 50000 : d * 1e6));
            }
            if (!sws || sw != f->width || sh != f->height || sf != f->format) {
                sws_freeContext(sws);
                sws = sws_getContext(f->width, f->height, f->format, out_w, out_h, AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
                sw = f->width; sh = f->height; sf = f->format;
            }
            uint8_t *dst[1] = { (uint8_t *)(shm + (size_t)slot * out_w * out_h) };
            int ls[1] = { out_w * 4 };
            sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, ls);
            say("FRAME %d\n", slot);
            slot ^= 1;
        next:;
        }
    }
    pthread_mutex_lock(&lk); v_done = 1; pthread_mutex_unlock(&lk);
    av_frame_free(&f); sws_freeContext(sws);
    return NULL;
}

/* album art: decode the attached picture once into slot 0 */
static void show_cover(void) {
    AVStream *st = fmt->streams[cover];
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) return;
    AVCodecContext *cc = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(cc, st->codecpar);
    if (avcodec_open2(cc, c, NULL) < 0) { avcodec_free_context(&cc); return; }
    AVFrame *f = av_frame_alloc();
    if (avcodec_send_packet(cc, &st->attached_pic) >= 0 && avcodec_receive_frame(cc, f) == 0) {
        struct SwsContext *s = sws_getContext(f->width, f->height, f->format, out_w, out_h, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
        uint8_t *dst[1] = { (uint8_t *)shm }; int ls[1] = { out_w * 4 };
        if (s) { sws_scale(s, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, ls); sws_freeContext(s); say("COVER %d %d\n", out_w, out_h); }
    }
    av_frame_free(&f); avcodec_free_context(&cc);
}

static void meta(void) {
    const char *keys[3] = { "title", "artist", "album" };
    char v[3][200] = { "", "", "" };
    for (int i = 0; i < 3; i++) {
        AVDictionaryEntry *e = av_dict_get(fmt->metadata, keys[i], NULL, 0);
        if (!e && as >= 0) e = av_dict_get(fmt->streams[as]->metadata, keys[i], NULL, 0);
        if (e) { snprintf(v[i], sizeof v[i], "%s", e->value); for (char *p = v[i]; *p; p++) if (*p == '|' || *p == '\n') *p = ' '; }
    }
    say("META %s|%s|%s\n", v[0], v[1], v[2]);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: ccos-media FILE MAXW MAXH SHMFILE\n"); return 1; }
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0) { say("ERR can't open this file\n"); return 1; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { say("ERR can't read this file\n"); return 1; }
    int maxw = atoi(argv[2]), maxh = atoi(argv[3]);
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) cover = (int)i;
    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs >= 0 && (fmt->streams[vs]->disposition & AV_DISPOSITION_ATTACHED_PIC)) vs = -1;
    as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vs < 0) vs = -1;
    if (as < 0) as = -1;
    if (vs >= 0) {
        AVStream *st = fmt->streams[vs];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (c) {
            vdec = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(vdec, st->codecpar);
            vdec->thread_count = 0;                          /* use all the processor's cores */
            if (avcodec_open2(vdec, c, NULL) < 0) { avcodec_free_context(&vdec); vs = -1; }
        } else vs = -1;
    }
    if (as >= 0) {
        AVStream *st = fmt->streams[as];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (c) {
            adec = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(adec, st->codecpar);
            if (avcodec_open2(adec, c, NULL) < 0) { avcodec_free_context(&adec); as = -1; }
        } else as = -1;
    }
    if (vs < 0 && as < 0) { say("ERR no picture or sound this computer can play\n"); return 1; }
    /* the picture size: the video's, shrunk to fit (album art: square) */
    int srcw = vs >= 0 ? vdec->width : 1, srch = vs >= 0 ? vdec->height : 1;
    if (vs >= 0) {
        AVRational sar = vdec->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0) srcw = srcw * sar.num / sar.den;
    }
    double sc = fmin((double)maxw / srcw, (double)maxh / srch);
    if (vs >= 0 && sc > 1) sc = 1;
    out_w = vs >= 0 ? ((int)(srcw * sc) & ~1) : maxw; out_h = vs >= 0 ? ((int)(srch * sc) & ~1) : maxh;
    if (vs < 0) { int s = maxw < maxh ? maxw : maxh; out_w = out_h = s; }
    if (out_w < 2) out_w = 2;
    if (out_h < 2) out_h = 2;
    int fd = open(argv[4], O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)out_w * out_h * 8) < 0) { say("ERR no memory\n"); return 1; }
    shm = mmap(NULL, (size_t)out_w * out_h * 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { say("ERR no memory\n"); return 1; }
    double dur = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
    say("INFO %d %d %.3f %d %d\n", out_w, out_h, dur, vs >= 0, as >= 0);
    meta();
    if (vs < 0 && cover >= 0) show_cover();

    pthread_t td, ta, tv;
    pthread_create(&td, NULL, demux_thread, NULL);
    if (as >= 0) pthread_create(&ta, NULL, audio_thread, NULL);
    if (vs >= 0) pthread_create(&tv, NULL, video_thread, NULL);
    if (as < 0) { clock_base = 0; clock_wall = now(); clock_set = vs < 0; }

    /* commands from the desktop; the position a few times a second */
    char line[128]; int ll = 0;
    double last_pos = -1;
    fcntl(0, F_SETFL, O_NONBLOCK);
    for (;;) {
        struct pollfd p = { 0, POLLIN, 0 };
        poll(&p, 1, 200);
        char ch;
        while (read(0, &ch, 1) == 1) {
            if (ch != '\n') { if (ll < 127) line[ll++] = ch; continue; }
            line[ll] = 0; ll = 0;
            pthread_mutex_lock(&lk);
            if (line[0] == 'p') {
                if (!paused) { clock_base = media_clock(); paused = 1; }
                else { paused = 0; clock_wall = now(); }
            } else if (line[0] == 's') {
                seek_to = atof(line + 1); if (seek_to < 0) seek_to = 0;
                if (dur > 0 && seek_to > dur - 0.5) seek_to = dur - 0.5;
                seek_req = 1; clock_set = 0;
            } else if (line[0] == 'v') volume = atoi(line + 1);
            else if (line[0] == 'q') quit = 1;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&lk);
        }
        if (p.revents & (POLLHUP | POLLERR)) { pthread_mutex_lock(&lk); quit = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk); }
        pthread_mutex_lock(&lk);
        double c = media_clock(); int q = quit;
        int finished = (as < 0 || a_done) && (vs < 0 || v_done);
        pthread_mutex_unlock(&lk);
        if (q) break;
        if (c >= 0 && (c - last_pos > 0.25 || c < last_pos)) { say("POS %.2f\n", c); last_pos = c; }
        if (finished) { say("END\n"); break; }
    }
    pthread_mutex_lock(&lk); quit = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
    _exit(0);                              /* threads may be blocked in the sound server: just go */
}
