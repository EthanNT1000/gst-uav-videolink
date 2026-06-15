/*
 * UAV RTSP server — exposes the video stream at rtsp://<host>:8554/uav
 * so that any standard RTSP client (VLC, QGC, ffplay) can connect on demand.
 *
 * RTSP vs raw RTP/UDP:
 *   Raw RTP is lower overhead and preferred for fixed ground-station setups.
 *   RTSP adds DESCRIBE/SETUP/PLAY negotiation which lets clients discover
 *   codec parameters, seek, and reconnect without prior SDP knowledge.
 *
 * Usage: rtsp_server [-p PORT] [-b KBPS] [-c CODEC 0:h264 1:h265]
 *                    [-W WIDTH] [-H HEIGHT] [-f FPS]
 *                    [-P PRESET 0:ultrafast..8:veryslow]
 *                    [-g GCS_HOST] [-G GCS_PORT] [-u STREAM_URI]
 *                    [-s SOURCE 0:videotestsrc 1:libcamerasrc 2:v4l2src]
 *                    [-F INPUT_FORMAT 0:raw(YUYV) 1:mjpeg  (v4l2src only)]
 *                    [-T THREADS  encoder thread count (0=auto)]
 *                    [-d  show local FPS overlay via fpsdisplaysink]
 *
 * Connect with:
 *   gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/uav latency=200 \
 *     ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink
 *   vlc rtsp://127.0.0.1:8554/uav
 *   ffplay rtsp://127.0.0.1:8554/uav
 */

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
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

static void on_client_connected(GstRTSPServer *server,
                                 GstRTSPClient *client,
                                 gpointer       data)
{
    (void)server; (void)data;
    GstRTSPConnection *conn = gst_rtsp_client_get_connection(client);
    g_print("[rtsp] Client connected: %s\n",
            gst_rtsp_connection_get_ip(conn));
}

int main(int argc, char *argv[])
{
    int         port       = 8554;
    int         bitrate    = 2000;
    int         codec_id   = 0;
    int         width      = 1280;
    int         height     = 720;
    int         fps        = 30;
    const char *gcs_host   = NULL;   /* NULL → skip MAVLink announce */
    int         gcs_port   = 14550;
    const char *stream_uri = NULL;   /* NULL → auto-build from port */
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
    const char *source[]       = {"videotestsrc pattern=ball is-live=true",
                                   "libcamerasrc ! queue max-size-buffers=3 max-size-bytes=0 max-size-time=0",
                                   "v4l2src"};

    int opt;
    while ((opt = getopt(argc, argv, "p:b:c:W:H:f:g:G:u:s:P:F:T:d")) != -1) {
        switch (opt) {
        case 'p': port       = atoi(optarg); break;
        case 'b': bitrate    = atoi(optarg); break;
        case 'c': codec_id   = atoi(optarg); break;
        case 'W': width      = atoi(optarg); break;
        case 'H': height     = atoi(optarg); break;
        case 'f': fps        = atoi(optarg); break;
        case 'g': gcs_host   = optarg;       break;
        case 'G': gcs_port   = atoi(optarg); break;
        case 'u': stream_uri = optarg;       break;
        case 's': source_id  = atoi(optarg); break;
        case 'P': preset_id  = atoi(optarg); break;
        case 'F': input_fmt  = atoi(optarg); break;
        case 'T': threads    = atoi(optarg); break;
        case 'd': display    = 1;            break;
        default:
            fprintf(stderr,
                "Usage: %s [-p PORT] [-b KBPS]"
                " [-c CODEC 0:h264 1:h265]"
                " [-W WIDTH] [-H HEIGHT] [-f FPS]"
                " [-P PRESET 0:ultrafast 1:superfast 2:veryfast 3:faster 4:fast"
                " 5:medium 6:slow 7:slower 8:veryslow]"
                " [-g GCS_HOST] [-G GCS_PORT] [-u STREAM_URI]"
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
     * The launch string is wrapped in parentheses so gst-rtsp-server can
     * extract the pay0 payloader element and wire up per-client RTP sessions.
     * name=pay0 is the mandatory marker that tells the server which element
     * produces RTP packets.
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

    gchar *thr = (cid == 0) ? g_strdup_printf("threads=%d ", threads) : g_strdup("");

    gchar *launch;
    if (display) {
        launch = g_strdup_printf(
            "( %s "
            "! videoconvert %s! tee name=t "
            "t. ! queue ! %s tune=zerolatency bitrate=%d key-int-max=30 speed-preset=%s %s"
            "! %s name=pay0 config-interval=1 pt=96 "
            "t. ! queue ! fpsdisplaysink video-sink=autovideosink sync=false )",
            src_frag, i420,
            codec_enc[cid], bitrate, speed_preset[pid], thr, codec_pay[cid]);
    } else {
        launch = g_strdup_printf(
            "( %s "
            "! videoconvert %s"
            "! %s tune=zerolatency bitrate=%d key-int-max=30 speed-preset=%s %s"
            "! %s name=pay0 config-interval=1 pt=96 )",
            src_frag, i420,
            codec_enc[cid], bitrate, speed_preset[pid], thr, codec_pay[cid]);
    }
    g_free(src_frag);
    g_free(thr);

    gchar *port_str = g_strdup_printf("%d", port);

    GstRTSPServer       *server  = gst_rtsp_server_new();
    GstRTSPMountPoints  *mounts  = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    gst_rtsp_server_set_service(server, port_str);
    gst_rtsp_media_factory_set_launch(factory, launch);
    /* shared=TRUE: one pipeline feeds all connected clients */
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_mount_points_add_factory(mounts, "/uav", factory);

    g_object_unref(mounts);
    g_print("[rtsp] Pipeline: %s\n\n", launch);
    g_free(launch);
    g_free(port_str);

    g_signal_connect(server, "client-connected",
                     G_CALLBACK(on_client_connected), NULL);

    if (gst_rtsp_server_attach(server, NULL) == 0) {
        g_printerr("[rtsp] Failed to attach server to port %d\n", port);
        return 1;
    }

    g_loop = g_main_loop_new(NULL, FALSE);

    g_print("[rtsp] Server ready at rtsp://127.0.0.1:%d/uav (%s %dx%d %dfps %d kbps)\n",
            port, codec_name[cid], width, height, fps, bitrate);
    g_print("[rtsp] Connect with:  vlc rtsp://127.0.0.1:%d/uav\n", port);

    if (gcs_host) {
        /* Build URI from port unless the caller provided one explicitly.
         * In a real deployment the URI should use the UAV's reachable IP. */
        gchar *auto_uri = NULL;
        if (!stream_uri) {
            auto_uri   = g_strdup_printf("rtsp://127.0.0.1:%d/uav", port);
            stream_uri = auto_uri;
        }
        MavlinkVideoConfig mav_cfg = {
            .gcs_host         = gcs_host,
            .gcs_port         = (guint16)gcs_port,
            .stream_uri       = stream_uri,
            .stream_type      = VIDEO_STREAM_TYPE_RTSP,
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
        g_free(auto_uri);
    }

    g_main_loop_run(g_loop);

    mavlink_announce_close();
    g_object_unref(server);
    g_main_loop_unref(g_loop);

    g_print("[rtsp] Stopped.\n");
    return 0;
}
