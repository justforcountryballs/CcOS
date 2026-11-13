/* ccos-camera - the webcam, for the Camera app
 *
 * Shows the picture to the desktop through a memory file (fd 3: two
 * 1920x1080x4 slots), takes photos (JPEG) and videos (H.264 + the
 * microphone as AAC, in MP4).
 *   out (stdout):  S w h | F slot | P path | V path seconds | ERR text | NOCAM
 *   in  (stdin):   D slot (shown, free again) | PHOTO path | REC path | STOP | QUIT
 *
 * build: gcc -O2 -o ccos-camera cam.c -lavformat -lavcodec -lswscale -lavutil -lpulse-simple -lpulse -lpthread
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pulse/simple.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAXW 1920
#define MAXH 1080
static void say(const char *fmt, const char *a) { printf(fmt, a); putchar('\n'); fflush(stdout); }
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static int xioctl(int fd, unsigned long r, void *a) { int k; do k = ioctl(fd, r, a); while (k < 0 && errno == EINTR); return k; }

/* ---- the video file ---- */
static AVFormatContext *oc;
static AVStream *vst, *ast;
static AVCodecContext *venc, *aenc;
static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static volatile int rec_on, stop_audio;
static pa_simple *mic;
static pthread_t mic_th;
static long long rec_start, rec_last;
static int64_t mic_samples;
static char rec_path[512];

static void put_packets(AVCodecContext *c, AVStream *st) {
    AVPacket *p = av_packet_alloc();
    while (avcodec_receive_packet(c, p) == 0) {
        av_packet_rescale_ts(p, c->time_base, st->time_base);
        p->stream_index = st->index;
        pthread_mutex_lock(&mux); av_interleaved_write_frame(oc, p); pthread_mutex_unlock(&mux);
    }
    av_packet_free(&p);
}
static void *mic_thread(void *x) {
    (void)x;
    int fs = aenc->frame_size ? aenc->frame_size : 1024;
    AVFrame *f = av_frame_alloc();
    f->format = aenc->sample_fmt; f->nb_samples = fs; f->sample_rate = 48000;
    av_channel_layout_copy(&f->ch_layout, &aenc->ch_layout);
    av_frame_get_buffer(f, 0);
    int16_t *b = malloc((size_t)fs * 4);
    while (!stop_audio) {
        int e; if (pa_simple_read(mic, b, (size_t)fs * 4, &e) < 0) break;
        av_frame_make_writable(f);
        float *l = (float *)f->data[0], *r = (float *)f->data[1];
        for (int i = 0; i < fs; i++) { l[i] = b[2 * i] / 32768.0f; r[i] = b[2 * i + 1] / 32768.0f; }
        f->pts = mic_samples; mic_samples += fs;
        if (avcodec_send_frame(aenc, f) == 0) put_packets(aenc, ast);
    }
    free(b); av_frame_free(&f);
    return NULL;
}
static int rec_begin(const char *path, int w, int h) {
    snprintf(rec_path, sizeof rec_path, "%s", path);
    if (avformat_alloc_output_context2(&oc, NULL, "mp4", path) < 0) return -1;
    const AVCodec *vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) vc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    vst = avformat_new_stream(oc, NULL);
    venc = avcodec_alloc_context3(vc);
    venc->width = w & ~1; venc->height = h & ~1;
    venc->time_base = (AVRational){ 1, 1000 }; venc->framerate = (AVRational){ 30, 1 };
    venc->pix_fmt = AV_PIX_FMT_YUV420P; venc->gop_size = 60; venc->max_b_frames = 0; venc->thread_count = 0;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (!strcmp(vc->name, "libx264")) { av_opt_set(venc->priv_data, "preset", "veryfast", 0); av_opt_set(venc->priv_data, "crf", "21", 0); }
    else venc->bit_rate = 6000000;
    if (avcodec_open2(venc, vc, NULL) < 0) return -1;
    avcodec_parameters_from_context(vst->codecpar, venc); vst->time_base = venc->time_base;
    pa_sample_spec ss = { PA_SAMPLE_S16LE, 48000, 2 };
    mic = pa_simple_new(NULL, "ccos Camera", PA_STREAM_RECORD, NULL, "camera microphone", &ss, NULL, NULL, NULL);
    const AVCodec *ac = mic ? avcodec_find_encoder(AV_CODEC_ID_AAC) : NULL;
    if (ac) {
        ast = avformat_new_stream(oc, NULL);
        aenc = avcodec_alloc_context3(ac);
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP; aenc->sample_rate = 48000; av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->bit_rate = 128000; aenc->time_base = (AVRational){ 1, 48000 };
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aenc, ac, NULL) < 0) { avcodec_free_context(&aenc); ast = NULL; }
        else { avcodec_parameters_from_context(ast->codecpar, aenc); ast->time_base = aenc->time_base; }
    }
    if (!aenc && mic) { pa_simple_free(mic); mic = NULL; }
    if (avio_open(&oc->pb, path, AVIO_FLAG_WRITE) < 0 || avformat_write_header(oc, NULL) < 0) return -1;
    stop_audio = 0; mic_samples = 0;
    if (aenc) pthread_create(&mic_th, NULL, mic_thread, NULL);
    rec_start = now_ms(); rec_last = -1; rec_on = 1;
    return 0;
}
static void rec_frame(AVFrame *yuv) {
    long long t = now_ms() - rec_start;
    if (t <= rec_last) t = rec_last + 1;
    rec_last = t;
    yuv->pts = t;
    if (avcodec_send_frame(venc, yuv) == 0) put_packets(venc, vst);
}
static void rec_end(void) {
    if (!rec_on) return;
    rec_on = 0;
    if (aenc) { stop_audio = 1; pthread_join(mic_th, NULL); avcodec_send_frame(aenc, NULL); put_packets(aenc, ast); pa_simple_free(mic); mic = NULL; avcodec_free_context(&aenc); }
    avcodec_send_frame(venc, NULL); put_packets(venc, vst);
    av_write_trailer(oc); avio_closep(&oc->pb);
    avcodec_free_context(&venc); avformat_free_context(oc); oc = NULL; ast = vst = NULL;
    char b[40]; snprintf(b, sizeof b, " %lld", rec_last / 1000);
    printf("V %s%s\n", rec_path, b); fflush(stdout);
}

/* a photo: the picture as JPEG */
static int save_jpeg(const char *path, AVFrame *yuvj) {
    const AVCodec *jc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!jc) return -1;
    AVCodecContext *c = avcodec_alloc_context3(jc);
    c->width = yuvj->width; c->height = yuvj->height; c->pix_fmt = AV_PIX_FMT_YUVJ420P;
    c->time_base = (AVRational){ 1, 25 };
    c->flags |= AV_CODEC_FLAG_QSCALE; c->global_quality = FF_QP2LAMBDA * 2;
    if (avcodec_open2(c, jc, NULL) < 0) { avcodec_free_context(&c); return -1; }
    yuvj->quality = c->global_quality; yuvj->pts = 0;
    int ok = -1;
    AVPacket *p = av_packet_alloc();
    if (avcodec_send_frame(c, yuvj) == 0 && avcodec_receive_packet(c, p) == 0) {
        FILE *f = fopen(path, "wb");
        if (f) { ok = fwrite(p->data, 1, (size_t)p->size, f) == (size_t)p->size ? 0 : -1; if (fclose(f)) ok = -1; }
    }
    av_packet_free(&p); avcodec_free_context(&c);
    return ok;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    uint32_t *shm = mmap(NULL, (size_t)MAXW * MAXH * 4 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
    if (shm == MAP_FAILED) { say("ERR %s", "no screen memory"); return 1; }
    /* the first camera that gives pictures */
    int fd = -1;
    for (int i = 0; i < 8 && fd < 0; i++) {
        char p[32]; snprintf(p, sizeof p, "/dev/video%d", i);
        int f = open(p, O_RDWR | O_NONBLOCK);
        if (f < 0) continue;
        struct v4l2_capability cap;
        if (xioctl(f, VIDIOC_QUERYCAP, &cap) == 0 && (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) && (cap.device_caps & V4L2_CAP_STREAMING)) fd = f;
        else close(f);
    }
    if (fd < 0) { say("%s", "NOCAM"); return 0; }
    /* MJPEG at 1280x720 if it can (sharp and smooth), else YUYV */
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = 1280; fmt.fmt.pix.height = 720; fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fmt.fmt.pix.width = 640; fmt.fmt.pix.height = 480; fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { say("ERR %s", "The camera didn't accept a picture size."); return 1; }
    }
    int w = (int)fmt.fmt.pix.width, h = (int)fmt.fmt.pix.height, mjpeg = fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG && fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) { say("ERR %s", "This camera's picture format isn't supported."); return 1; }
    if (w > MAXW || h > MAXH) { say("ERR %s", "The camera's pictures are too big."); return 1; }
    struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    sp.parm.capture.timeperframe = (struct v4l2_fract){ 1, 30 };
    xioctl(fd, VIDIOC_S_PARM, &sp);
    struct v4l2_requestbuffers rb = { .count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { say("ERR %s", "The camera is busy (another app is using it?)."); return 1; }
    void *bufs[8]; size_t lens[8];
    for (unsigned i = 0; i < rb.count && i < 8; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        xioctl(fd, VIDIOC_QUERYBUF, &b);
        lens[i] = b.length; bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        xioctl(fd, VIDIOC_QBUF, &b);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { say("ERR %s", "The camera didn't start."); return 1; }

    const AVCodec *dc = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *dec = NULL;
    if (mjpeg) { dec = avcodec_alloc_context3(dc); if (avcodec_open2(dec, dc, NULL) < 0) { say("ERR %s", "Couldn't read the camera's pictures."); return 1; } }
    AVFrame *df = av_frame_alloc(), *yuv = av_frame_alloc(), *yuvj = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV420P; yuv->width = w & ~1; yuv->height = h & ~1; av_frame_get_buffer(yuv, 0);
    yuvj->format = AV_PIX_FMT_YUVJ420P; yuvj->width = w; yuvj->height = h; av_frame_get_buffer(yuvj, 0);
    struct SwsContext *to_rgb = NULL, *to_yuv = NULL, *to_yuvj = NULL;
    int busy[2] = { 0, 0 };
    char want_photo[512] = "";
    printf("S %d %d\n", w, h); fflush(stdout);
    fcntl(0, F_SETFL, O_NONBLOCK);
    char in[1024]; int inl = 0;

    for (;;) {
        struct pollfd pf[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        if (poll(pf, 2, 1000) < 0 && errno != EINTR) break;
        if (pf[1].revents & (POLLIN | POLLHUP)) {                    /* commands */
            ssize_t k = read(0, in + inl, sizeof in - 1 - (size_t)inl);
            if (k == 0) break;
            if (k > 0) inl += (int)k;
            in[inl] = 0;
            char *l = in, *nl;
            while ((nl = strchr(l, '\n'))) {
                *nl = 0;
                int s;
                if (sscanf(l, "D %d", &s) == 1 && s >= 0 && s < 2) busy[s] = 0;
                else if (!strncmp(l, "PHOTO ", 6)) snprintf(want_photo, sizeof want_photo, "%s", l + 6);
                else if (!strncmp(l, "REC ", 4)) { if (!rec_on && rec_begin(l + 4, w, h)) say("ERR %s", "Couldn't start the video (is D: full?)"); }
                else if (!strcmp(l, "STOP")) rec_end();
                else if (!strcmp(l, "QUIT")) { rec_end(); return 0; }
                l = nl + 1;
            }
            inl = (int)strlen(l); memmove(in, l, (size_t)inl + 1);
        }
        if (!(pf[0].revents & POLLIN)) {
            if (pf[0].revents & (POLLERR | POLLHUP)) { rec_end(); say("ERR %s", "The camera was unplugged."); return 1; }
            continue;
        }
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { if (errno == EAGAIN) continue; rec_end(); say("ERR %s", "The camera stopped."); return 1; }
        AVFrame *src = NULL;
        static AVFrame *yuyv;
        if (mjpeg) {
            AVPacket *p = av_packet_alloc();
            p->data = bufs[b.index]; p->size = (int)b.bytesused;
            if (avcodec_send_packet(dec, p) == 0 && avcodec_receive_frame(dec, df) == 0) src = df;
            av_packet_free(&p);
        } else {
            if (!yuyv) { yuyv = av_frame_alloc(); yuyv->format = AV_PIX_FMT_YUYV422; yuyv->width = w; yuyv->height = h; av_frame_get_buffer(yuyv, 0); }
            av_image_copy_plane(yuyv->data[0], yuyv->linesize[0], bufs[b.index], w * 2, w * 2, h);
            src = yuyv;
        }
        xioctl(fd, VIDIOC_QBUF, &b);
        if (!src) continue;
        /* to the screen */
        int slot = !busy[0] ? 0 : !busy[1] ? 1 : -1;
        if (slot >= 0) {
            to_rgb = sws_getCachedContext(to_rgb, w, h, src->format, w, h, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
            uint8_t *dst[1] = { (uint8_t *)(shm + (size_t)slot * MAXW * MAXH) }; int ds[1] = { w * 4 };
            sws_scale(to_rgb, (const uint8_t *const *)src->data, src->linesize, 0, h, dst, ds);
            busy[slot] = 1;
            printf("F %d\n", slot); fflush(stdout);
        }
        if (want_photo[0]) {
            to_yuvj = sws_getCachedContext(to_yuvj, w, h, src->format, w, h, AV_PIX_FMT_YUVJ420P, SWS_BICUBIC, NULL, NULL, NULL);
            av_frame_make_writable(yuvj);
            sws_scale(to_yuvj, (const uint8_t *const *)src->data, src->linesize, 0, h, yuvj->data, yuvj->linesize);
            if (save_jpeg(want_photo, yuvj)) say("ERR %s", "Couldn't save the photo (is D: full?)");
            else say("P %s", want_photo);
            want_photo[0] = 0;
        }
        if (rec_on) {
            to_yuv = sws_getCachedContext(to_yuv, w, h, src->format, w & ~1, h & ~1, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
            av_frame_make_writable(yuv);
            sws_scale(to_yuv, (const uint8_t *const *)src->data, src->linesize, 0, h, yuv->data, yuv->linesize);
            rec_frame(yuv);
        }
    }
    rec_end();
    return 0;
}
