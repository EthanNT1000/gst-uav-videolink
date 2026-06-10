/*
 * UAV video sender — encodes camera frames as H.264 or H.265 and transmits
 * them over RTP/UDP.  Designed for low-latency air-to-ground video links.
 *
 * Default port 5600 matches QGroundControl's expected MAVLink video stream.
 * Pass -g/-G to simultaneously announce the stream over MAVLink so QGC
 * auto-discovers and opens the feed.
 *
 * Usage: sender [-h HOST] [-p PORT] [-b KBPS] [-c h264|h265]
 *               [-W WIDTH] [-H HEIGHT] [-f FPS]
 *               [-g GCS_HOST] [-G GCS_PORT]
 */

#include <gst/gst.h>
#include <glib.h>
#include "mavlink_announce.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const char *host     = "127.0.0.1";
    int         port     = 5600;   /* QGroundControl default video port */
    int         bitrate  = 2000;   /* kbps */
    const char *codec    = "h264";
    int         width    = 1280;
    int         height   = 720;
    int         fps      = 30;
    const char *gcs_host = NULL;   /* NULL → skip MAVLink announce */
    int         gcs_port = 14550;

    int opt;
    while ((opt = getopt(argc, argv, "h:p:b:c:W:H:f:g:G:")) != -1) {
        switch (opt) {
        case 'h': host     = optarg;       break;
        case 'p': port     = atoi(optarg); break;
        case 'b': bitrate  = atoi(optarg); break;
        case 'c': codec    = optarg;       break;
        case 'W': width    = atoi(optarg); break;
        case 'H': height   = atoi(optarg); break;
        case 'f': fps      = atoi(optarg); break;
        case 'g': gcs_host = optarg;       break;
        case 'G': gcs_port = atoi(optarg); break;
        default:
            fprintf(stderr,
                "Usage: %s [-h HOST] [-p PORT] [-b KBPS] [-c h264|h265]"
                " [-W WIDTH] [-H HEIGHT] [-f FPS]"
                " [-g GCS_HOST] [-G GCS_PORT]\n", argv[0]);
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
    gchar *enc_pay;
    if (strcmp(codec, "h265") == 0) {
        enc_pay = g_strdup_printf(
            "x265enc tune=zerolatency bitrate=%d key-int-max=30 speed-preset=ultrafast "
            "! rtph265pay config-interval=1 pt=96",
            bitrate);
    } else {
        enc_pay = g_strdup_printf(
            "x264enc tune=zerolatency bitrate=%d key-int-max=30 speed-preset=ultrafast "
            "! rtph264pay config-interval=1 pt=96",
            bitrate);
    }

    gchar *desc = g_strdup_printf(
        "videotestsrc pattern=ball is-live=true "
        "! video/x-raw,width=%d,height=%d,framerate=%d/1 "
        "! videoconvert ! %s "
        "! udpsink host=%s port=%d sync=false async=false",
        width, height, fps, enc_pay, host, port);

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
            codec, width, height, fps, host, port);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    if (gcs_host) {
        /* Announce stream URI as "udp://0.0.0.0:PORT" — QGC listens on that port */
        gchar *uri = g_strdup_printf("udp://0.0.0.0:%d", port);
        gboolean is_h265 = (strcmp(codec, "h265") == 0);
        MavlinkVideoConfig mav_cfg = {
            .gcs_host         = gcs_host,
            .gcs_port         = (guint16)gcs_port,
            .stream_uri       = uri,
            .stream_type      = VIDEO_STREAM_TYPE_RTPUDP,
            .encoding         = is_h265 ? VIDEO_STREAM_ENCODING_H265
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
