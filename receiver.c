/*
 * UAV video receiver — accepts an RTP/UDP stream from the air vehicle,
 * buffers jitter, decodes H.264 or H.265, and either renders to screen
 * or saves directly to an MP4 file without re-encoding.
 *
 * Usage: receiver [-p PORT] [-c h264|h265] [-l LATENCY_MS] [-s OUT.mp4] [-d]
 */

#include <gst/gst.h>
#include <glib.h>
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
        g_printerr("[receiver] Error: %s\n", err->message);
        if (dbg) g_printerr("[receiver] Debug: %s\n", dbg);
        g_clear_error(&err);
        g_free(dbg);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("[receiver] EOS\n");
        g_main_loop_quit(loop);
        break;
    default:
        break;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    int         port      = 5600;
    const char *codec     = "h264";
    int         latency   = 200;  /* ms — trades added latency for smoothness */
    const char *save_path = NULL; /* NULL → display on screen */
    int         display   = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:c:l:s:d")) != -1) {
        switch (opt) {
        case 'p': port      = atoi(optarg); break;
        case 'c': codec     = optarg;       break;
        case 'l': latency   = atoi(optarg); break;
        case 's': save_path = optarg;       break;
        case 'd': display   = 1;            break;
        default:
            fprintf(stderr,
                "Usage: %s [-p PORT] [-c h264|h265] [-l LATENCY_MS] [-s OUT.mp4] [-d]\n",
                argv[0]);
            return 1;
        }
    }

    gst_init(NULL, NULL);
    signal(SIGINT, on_sigint);

    const char *enc_name, *depay, *parse, *decode;
    if (strcmp(codec, "h265") == 0) {
        enc_name = "H265";
        depay    = "rtph265depay";
        parse    = "h265parse";
        decode   = "avdec_h265";
    } else {
        enc_name = "H264";
        depay    = "rtph264depay";
        parse    = "h264parse";
        decode   = "avdec_h264";
    }

    /*
     * udpsrc caps must match what the sender negotiated so GStreamer can
     * pick the correct depayloader without relying on SDP signalling.
     *
     * When saving, the raw Annex-B bitstream from h264parse/h265parse is
     * muxed directly into MP4 — no decode/re-encode round-trip.
     */
    gchar *desc;
    if (save_path) {
        desc = g_strdup_printf(
            "udpsrc port=%d "
            "caps=\"application/x-rtp,media=video,clock-rate=90000,"
                   "encoding-name=%s,payload=96\" "
            "! rtpjitterbuffer latency=%d "
            "! %s ! %s "
            "! mp4mux ! filesink location=%s",
            port, enc_name, latency, depay, parse, save_path);
    } else {
        const char *sink = display
            ? "fpsdisplaysink video-sink=autovideosink sync=false"
            : "autovideosink sync=false";
        desc = g_strdup_printf(
            "udpsrc port=%d "
            "caps=\"application/x-rtp,media=video,clock-rate=90000,"
                   "encoding-name=%s,payload=96\" "
            "! rtpjitterbuffer latency=%d "
            "! %s ! %s ! %s "
            "! videoconvert ! %s",
            port, enc_name, latency, depay, parse, decode, sink);
    }

    g_print("[receiver] Pipeline: %s\n\n", desc);

    GError     *err      = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);
    g_free(desc);

    if (!pipeline) {
        g_printerr("[receiver] Failed to build pipeline: %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    g_loop = g_main_loop_new(NULL, FALSE);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_cb, g_loop);
    gst_object_unref(bus);

    g_print("[receiver] Listening on UDP :%d (%s), jitter buffer %d ms",
            port, codec, latency);
    if (save_path)
        g_print(" → saving to %s", save_path);
    g_print("\n");

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(g_loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(g_loop);

    g_print("[receiver] Stopped.\n");
    return 0;
}
