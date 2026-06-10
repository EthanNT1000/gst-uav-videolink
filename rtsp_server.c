/*
 * UAV RTSP server — exposes the video stream at rtsp://<host>:8554/uav
 * so that any standard RTSP client (VLC, QGC, ffplay) can connect on demand.
 *
 * RTSP vs raw RTP/UDP:
 *   Raw RTP is lower overhead and preferred for fixed ground-station setups.
 *   RTSP adds DESCRIBE/SETUP/PLAY negotiation which lets clients discover
 *   codec parameters, seek, and reconnect without prior SDP knowledge.
 *
 * Usage: rtsp_server [-p PORT] [-b KBPS] [-c h264|h265]
 *                    [-g GCS_HOST] [-G GCS_PORT] [-u STREAM_URI]
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
#include <string.h>
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
    const char *codec      = "h264";
    const char *gcs_host   = NULL;   /* NULL → skip MAVLink announce */
    int         gcs_port   = 14550;
    const char *stream_uri = NULL;   /* NULL → auto-build from port */

    int opt;
    while ((opt = getopt(argc, argv, "p:b:c:g:G:u:")) != -1) {
        switch (opt) {
        case 'p': port       = atoi(optarg); break;
        case 'b': bitrate    = atoi(optarg); break;
        case 'c': codec      = optarg;       break;
        case 'g': gcs_host   = optarg;       break;
        case 'G': gcs_port   = atoi(optarg); break;
        case 'u': stream_uri = optarg;       break;
        default:
            fprintf(stderr,
                "Usage: %s [-p PORT] [-b KBPS] [-c h264|h265]"
                " [-g GCS_HOST] [-G GCS_PORT] [-u STREAM_URI]\n", argv[0]);
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
    gchar *launch;
    if (strcmp(codec, "h265") == 0) {
        launch = g_strdup_printf(
            "( videotestsrc pattern=ball is-live=true "
            "! video/x-raw,width=1280,height=720,framerate=30/1 "
            "! videoconvert "
            "! x265enc tune=zerolatency bitrate=%d key-int-max=30 speed-preset=ultrafast "
            "! rtph265pay name=pay0 config-interval=1 pt=96 )",
            bitrate);
    } else {
        launch = g_strdup_printf(
            "( videotestsrc pattern=ball is-live=true "
            "! video/x-raw,width=1280,height=720,framerate=30/1 "
            "! videoconvert "
            "! x264enc tune=zerolatency bitrate=%d key-int-max=30 speed-preset=ultrafast "
            "! rtph264pay name=pay0 config-interval=1 pt=96 )",
            bitrate);
    }

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
    g_free(launch);
    g_free(port_str);

    g_signal_connect(server, "client-connected",
                     G_CALLBACK(on_client_connected), NULL);

    if (gst_rtsp_server_attach(server, NULL) == 0) {
        g_printerr("[rtsp] Failed to attach server to port %d\n", port);
        return 1;
    }

    g_loop = g_main_loop_new(NULL, FALSE);

    g_print("[rtsp] Server ready at rtsp://127.0.0.1:%d/uav (%s %d kbps)\n",
            port, codec, bitrate);
    g_print("[rtsp] Connect with:  vlc rtsp://127.0.0.1:%d/uav\n", port);

    if (gcs_host) {
        /* Build URI from port unless the caller provided one explicitly.
         * In a real deployment the URI should use the UAV's reachable IP. */
        gchar *auto_uri = NULL;
        if (!stream_uri) {
            auto_uri   = g_strdup_printf("rtsp://127.0.0.1:%d/uav", port);
            stream_uri = auto_uri;
        }
        gboolean is_h265 = (strcmp(codec, "h265") == 0);
        MavlinkVideoConfig mav_cfg = {
            .gcs_host         = gcs_host,
            .gcs_port         = (guint16)gcs_port,
            .stream_uri       = stream_uri,
            .stream_type      = VIDEO_STREAM_TYPE_RTSP,
            .encoding         = is_h265 ? VIDEO_STREAM_ENCODING_H265
                                        : VIDEO_STREAM_ENCODING_H264,
            .camera_device_id = 0,
            .fps              = 30.0f,
            .width            = 1280,
            .height           = 720,
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
