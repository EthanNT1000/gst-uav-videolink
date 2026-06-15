/*
 * UAV video sender — encodes camera frames as H.264 or H.265 and transmits
 * them over RTP/UDP.  Designed for low-latency air-to-ground video links.
 *
 * Default port 5600 matches QGroundControl's expected MAVLink video stream.
 * Pass -g/-G to simultaneously announce the stream over MAVLink so QGC
 * auto-discovers and opens the feed.
 *
 * Usage: sender [-h HOST] [-p PORT] [-b KBPS] [-c CODEC 0:h264 1:h265]
 *               [-W WIDTH] [-H HEIGHT] [-f FPS]
 *               [-P PRESET 0:ultrafast..8:veryslow]
 *               [-g GCS_HOST] [-G GCS_PORT]
 *               [-s SOURCE 0:videotestsrc 1:libcamerasrc 2:v4l2src]
 *               [-F INPUT_FORMAT 0:raw(YUYV) 1:mjpeg  (v4l2src only)]
 *               [-T THREADS  encoder thread count (0=auto)]
 *               [-d  show local FPS overlay via fpsdisplaysink]
 */

#include <gst/gst.h>
#include <glib.h>
#include "mavlink_announce.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static GMainLoop *g_loop = NULL;

static void on_sigint(int sig)
{
    (void)sig;
    if (g_loop)
        g_main_loop_quit(g_loop);
}

static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    GMainLoop *loop = (GMainLoop *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("[sender] Error: %s\n", err->message);
        if (dbg) g_printerr("[sender] Debug: %s\n", dbg);
        g_clear_error(&err);
        g_free(dbg);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("[sender] EOS\n");
        g_main_loop_quit(loop);
        break;
    default:
        break;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    const char *host      = "127.0.0.1";
    int         port      = 5600;   /* QGroundControl default video port */
    int         bitrate   = 2000;   /* kbps */
    int         codec_id  = 0;
    int         width     = 1280;
    int         height    = 720;
    int         fps       = 30;
    const char *gcs_host  = NULL;   /* NULL → skip MAVLink announce */
    int         gcs_port  = 14550;
    int         source_id  = 0;
    int         preset_id  = 0;
    int         input_fmt  = 0;  /* 0=raw(video/x-raw)  1=mjpeg(image/jpeg) */
    int         display    = 0;
    int         threads    = 0;  /* 0 = auto */

    const char *codec_enc[]    = {"x264enc",    "x265enc"};
    const char *codec_pay[]    = {"rtph264pay", "rtph265pay"};
    const char *codec_name[]   = {"h264",       "h265"};
    const char *speed_preset[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                   "medium", "slow", "slower", "veryslow"};
    const char *source[]       = { "videotestsrc pattern=ball is-live=true",
                                   "libcamerasrc ! queue max-size-buffers=3 max-size-bytes=0 max-size-time=0",
                                   "v4l2src"};

    int opt;
    while ((opt = getopt(argc, argv, "h:p:b:c:W:H:f:g:G:s:P:F:T:d")) != -1) {
        switch (opt) {
        case 'h': host      = optarg;        break;
        case 'p': port      = atoi(optarg);  break;
        case 'b': bitrate   = atoi(optarg);  break;
        case 'c': codec_id  = atoi(optarg);  break;
        case 'W': width     = atoi(optarg);  break;
        case 'H': height    = atoi(optarg);  break;
        case 'f': fps       = atoi(optarg);  break;
        case 'g': gcs_host  = optarg;        break;
        case 'G': gcs_port  = atoi(optarg);  break;
        case 's': source_id = atoi(optarg);  break;
        case 'P': preset_id = atoi(optarg);  break;
        case 'F': input_fmt = atoi(optarg);  break;
        case 'T': threads   = atoi(optarg);  break;
        case 'd': display   = 1;             break;
        default:
            fprintf(stderr,
                "Usage: %s [-h HOST] [-p PORT] [-b KBPS]"
                " [-c CODEC 0:h264 1:h265]"
                " [-W WIDTH] [-H HEIGHT] [-f FPS]"
                " [-P PRESET 0:ultrafast 1:superfast 2:veryfast 3:faster 4:fast"
                " 5:medium 6:slow 7:slower 8:veryslow]"
                " [-g GCS_HOST] [-G GCS_PORT]"
                " [-s SOURCE 0:videotestsrc 1:libcamerasrc 2:v4l2src]"
                " [-F INPUT_FORMAT 0:raw 1:mjpeg (v4l2src only)]"
                " [-T THREADS (0=auto)]"
                " [-d]\n", argv[0]);
            return 1;
        }
    }

    gst_init(NULL, NULL);
    signal(SIGINT, on_sigint);

    /*
     * tune=zerolatency  — disables B-frames and look-ahead so every frame
     *                     is encoded and emitted immediately; critical for
     *                     real-time UAV control-loop latency.
     * key-int-max=30    — forces an IDR every ~1 s at 30 fps so a receiver
     *                     that joins mid-stream can recover quickly.
     * config-interval=1 — repeats SPS/PPS inline every IDR so the receiver
     *                     does not need out-of-band SDP negotiation.
     */
    int src_idx = source_id < (int)(sizeof(source)       / sizeof(source[0]))       ? source_id : 0;
    int cid     = codec_id  < (int)(sizeof(codec_enc)    / sizeof(codec_enc[0]))    ? codec_id  : 0;
    int pid     = preset_id < (int)(sizeof(speed_preset) / sizeof(speed_preset[0])) ? preset_id : 0;

    /* libcamerasrc outputs YUYV; both x264enc and x265enc need I420.
     * Make the conversion target explicit so videoconvert never settles on
     * a packed format regardless of codec. */
    const char *i420 = (src_idx == 1) ? "! video/x-raw,format=I420 " : "";

    /* Build source fragment.
     * -F 1 (mjpeg): v4l2src outputs MJPEG — negotiate image/jpeg caps then
     *               decode with jpegdec before the rest of the pipeline.
     * -F 0 (raw, default): all other sources output video/x-raw directly. */
    gchar *src_frag;
    if (src_idx == 2 && input_fmt == 1) {
        src_frag = g_strdup_printf(
            "v4l2src ! image/jpeg,width=%d,height=%d,framerate=%d/1 ! jpegdec",
            width, height, fps);
    } else {
        src_frag = g_strdup_printf(
            "%s ! video/x-raw,width=%d,height=%d,framerate=%d/1",
            source[src_idx], width, height, fps);
    }

    gchar *thr     = (cid == 0) ? g_strdup_printf("threads=%d ", threads) : g_strdup("");
    gchar *enc_pay = g_strdup_printf(
        "%s tune=zerolatency bitrate=%d key-int-max=30 speed-preset=%s %s"
        "! %s config-interval=1 pt=96",
        codec_enc[cid], bitrate, speed_preset[pid], thr, codec_pay[cid]);
    g_free(thr);

    gchar *desc;
    if (display) {
        desc = g_strdup_printf(
            "%s ! videoconvert %s! tee name=t "
            "t. ! queue ! %s ! udpsink host=%s port=%d sync=false async=false "
            "t. ! queue ! fpsdisplaysink video-sink=autovideosink sync=false",
            src_frag, i420, enc_pay, host, port);
    } else {
        desc = g_strdup_printf(
            "%s ! videoconvert %s! %s "
            "! udpsink host=%s port=%d sync=false async=false",
            src_frag, i420, enc_pay, host, port);
    }
    g_free(src_frag);

    g_print("[sender] Pipeline: %s\n\n", desc);

    GError     *err      = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    g_free(enc_pay);

    if (!pipeline) {
        g_printerr("[sender] Failed to build pipeline: %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    g_loop = g_main_loop_new(NULL, FALSE);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_cb, g_loop);
    gst_object_unref(bus);

    g_print("[sender] Streaming %s @ %dx%d %dfps → udp://%s:%d\n",
            codec_name[cid], width, height, fps, host, port);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    if (gcs_host) {
        /* Announce stream URI as "udp://0.0.0.0:PORT" — QGC listens on that port */
        gchar *uri = g_strdup_printf("udp://0.0.0.0:%d", port);
        MavlinkVideoConfig mav_cfg = {
            .gcs_host         = gcs_host,
            .gcs_port         = (guint16)gcs_port,
            .stream_uri       = uri,
            .stream_type      = VIDEO_STREAM_TYPE_RTPUDP,
            .encoding         = cid == 1 ? VIDEO_STREAM_ENCODING_H265
                                         : VIDEO_STREAM_ENCODING_H264,
            .camera_device_id = 0,
            .fps              = (gfloat)fps,
            .width            = (guint16)width,
            .height           = (guint16)height,
            .bitrate_kbps     = (guint32)bitrate,
        };
        if (mavlink_announce_init(&mav_cfg))
            g_timeout_add_seconds(1, mavlink_announce_tick, NULL);
        g_free(uri);
    }

    g_main_loop_run(g_loop);

    mavlink_announce_close();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(g_loop);

    g_print("[sender] Stopped.\n");
    return 0;
}
