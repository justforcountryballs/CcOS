/* ccos-record - turns the screen into a video file (for the Record app)
 *
 *   ccos-record OUT.mp4 W H SOUND           (stdin/stdout talk to the desktop)
 *
 * The desktop shares its screen pictures through a memory file (fd 3):
 * NSLOT pictures of W x H, 4 bytes a pixel. It writes "F slot ms\n" on
 * stdin when slot holds a new picture taken ms after the start, and "E\n"
 * to finish. We answer "D slot\n" on stdout when the slot can be reused,
 * then "OK seconds" or "ERR message".
 * SOUND=1: what ccos plays (PulseAudio's monitor of the speakers) is
 * recorded too.
 * H.264 (libx264, fast settings) + AAC in MP4.
 *
 * build: gcc -O2 -o ccos-record rec.c -lavformat -lavcodec -lswscale -lavutil -lpulse-simple -lpulse -lpthread
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <pulse/simple.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NSLOT 3
static AVFormatContext *oc;
static AVStream *vst, *ast;
static AVCodecContext *venc, *aenc;
static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;   /* both write into the file */
static volatile int stop_audio;
static int64_t audio_samples;

static void out(const char *s) { fputs(s, stdout); fputc('\n', stdout); fflush(stdout); }
static void fail(const char *m) { printf("ERR %s\n", m); fflush(stdout); exit(1); }

static void write_packets(AVCodecContext *c, AVStream *st) {
    AVPacket *p = av_packet_alloc();
    while (avcodec_receive_packet(c, p) == 0) {
        av_packet_rescale_ts(p, c->time_base, st->time_base);
        p->stream_index = st->index;
        pthread_mutex_lock(&mux);
        av_interleaved_write_frame(oc, p);
        pthread_mutex_unlock(&mux);
    }
    av_packet_free(&p);
}

/* ---- sound: from the speakers' monitor, AAC ---- */
static pa_simple *pa;
static void *audio_thread(void *arg) {
    (void)arg;
    int fs = aenc->frame_size ? aenc->frame_size : 1024;
    AVFrame *f = av_frame_alloc();
    f->format = aenc->sample_fmt; f->nb_samples = fs; f->sample_rate = 48000;
    av_channel_layout_copy(&f->ch_layout, &aenc->ch_layout);
    av_frame_get_buffer(f, 0);
    int16_t *buf = malloc((size_t)fs * 2 * 2);
    while (!stop_audio) {
        int err;
        if (pa_simple_read(pa, buf, (size_t)fs * 2 * 2, &err) < 0) break;
        av_frame_make_writable(f);
        float *l = (float *)f->data[0], *r = (float *)f->data[1];       /* AAC takes float planar */
        for (int i = 0; i < fs; i++) { l[i] = buf[2 * i] / 32768.0f; r[i] = buf[2 * i + 1] / 32768.0f; }
        f->pts = audio_samples; audio_samples += fs;
        if (avcodec_send_frame(aenc, f) == 0) write_packets(aenc, ast);
    }
    free(buf);
    av_frame_free(&f);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: ccos-record OUT.mp4 W H SOUND\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    const char *path = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]), sound = atoi(argv[4]);
    size_t one = (size_t)W * H * 4;
    uint32_t *shm = mmap(NULL, one * NSLOT, PROT_READ, MAP_SHARED, 3, 0);
    if (shm == MAP_FAILED) fail("Couldn't see the screen");

    /* the video size: even numbers, at most 1920 wide */
    int ow = W, oh = H;
    if (ow > 1920) { oh = (int)((long)oh * 1920 / ow); ow = 1920; }
    ow &= ~1; oh &= ~1;

    if (avformat_alloc_output_context2(&oc, NULL, "mp4", path) < 0) fail("Couldn't start the file");
    const AVCodec *vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) vc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vc) fail("No video encoder");
    vst = avformat_new_stream(oc, NULL);
    venc = avcodec_alloc_context3(vc);
    venc->width = ow; venc->height = oh;
    venc->time_base = (AVRational){ 1, 1000 };             /* milliseconds: frames come when the screen changes */
    venc->framerate = (AVRational){ 30, 1 };
    venc->pix_fmt = AV_PIX_FMT_YUV420P;
    venc->gop_size = 60;
    venc->max_b_frames = 0;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (!strcmp(vc->name, "libx264")) {
        av_opt_set(venc->priv_data, "preset", "veryfast", 0);
        av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
        av_opt_set(venc->priv_data, "crf", "23", 0);
    } else venc->bit_rate = 8000000;
    venc->thread_count = 0;
    if (avcodec_open2(venc, vc, NULL) < 0) fail("Couldn't start the video encoder");
    avcodec_parameters_from_context(vst->codecpar, venc);
    vst->time_base = venc->time_base;

    if (sound) {
        pa_sample_spec ss = { PA_SAMPLE_S16LE, 48000, 2 };
        pa_buffer_attr ba = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, 48000 * 4 / 25 };
        pa = pa_simple_new(NULL, "ccos Record", PA_STREAM_RECORD, "@DEFAULT_MONITOR@", "screen recording", &ss, NULL, &ba, NULL);
        const AVCodec *ac = pa ? avcodec_find_encoder(AV_CODEC_ID_AAC) : NULL;
        if (ac) {
            ast = avformat_new_stream(oc, NULL);
            aenc = avcodec_alloc_context3(ac);
            aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
            aenc->sample_rate = 48000;
            av_channel_layout_default(&aenc->ch_layout, 2);
            aenc->bit_rate = 160000;
            aenc->time_base = (AVRational){ 1, 48000 };
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(aenc, ac, NULL) < 0) { avcodec_free_context(&aenc); ast = NULL; }
            else { avcodec_parameters_from_context(ast->codecpar, aenc); ast->time_base = aenc->time_base; }
        }
        if (!aenc && pa) { pa_simple_free(pa); pa = NULL; }
    }
    if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0) fail("Couldn't write the file (is D: full?)");
    if (avformat_write_header(oc, NULL) < 0) fail("Couldn't write the file");

    pthread_t at;
    if (aenc) pthread_create(&at, NULL, audio_thread, NULL);

    struct SwsContext *sws = sws_getContext(W, H, AV_PIX_FMT_BGR0, ow, oh, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P; f->width = ow; f->height = oh;
    av_frame_get_buffer(f, 0);
    out("READY");
    char l[64]; long long last_ms = -1, end_ms = 0;
    while (fgets(l, sizeof l, stdin)) {
        int slot; long long ms;
        if (l[0] == 'E') break;
        if (sscanf(l, "F %d %lld", &slot, &ms) != 2 || slot < 0 || slot >= NSLOT) continue;
        if (ms <= last_ms) ms = last_ms + 1;                   /* times must go up */
        av_frame_make_writable(f);
        const uint8_t *src[1] = { (const uint8_t *)(shm + (size_t)slot * W * H) };
        int sstride[1] = { W * 4 };
        sws_scale(sws, src, sstride, 0, H, f->data, f->linesize);
        printf("D %d\n", slot); fflush(stdout);                 /* the desktop may reuse it now */
        f->pts = ms; last_ms = ms; end_ms = ms;
        if (avcodec_send_frame(venc, f) == 0) write_packets(venc, vst);
    }
    if (aenc) { stop_audio = 1; pthread_join(at, NULL); avcodec_send_frame(aenc, NULL); write_packets(aenc, ast); pa_simple_free(pa); }
    avcodec_send_frame(venc, NULL);
    write_packets(venc, vst);
    av_write_trailer(oc);
    avio_closep(&oc->pb);
    printf("OK %lld\n", end_ms / 1000); fflush(stdout);
    return 0;
}
