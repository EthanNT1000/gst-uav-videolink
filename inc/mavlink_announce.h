#pragma once

/*
 * Thin wrapper around MAVLink HEARTBEAT + VIDEO_STREAM_INFORMATION.
 *
 * Call pattern (integrates with the GMainLoop already used by GStreamer):
 *
 *   MavlinkVideoConfig cfg = { .gcs_host = "192.168.1.1", .gcs_port = 14550, ... };
 *   if (mavlink_announce_init(&cfg)) {
 *       g_timeout_add_seconds(1, mavlink_announce_tick, NULL);
 *   }
 *   g_main_loop_run(loop);          // blocks
 *   mavlink_announce_close();
 *
 * The timeout callback fires in the GLib main loop, on the same thread as
 * the GStreamer bus watch, so no locking is needed.
 *
 * Requires: third_party/mavlink (c_library_v2 git submodule).
 * Add as: git submodule add https://github.com/mavlink/c_library_v2.git third_party/mavlink
 */

#include <glib.h>
#include <mavlink/common/mavlink.h>   /* VIDEO_STREAM_TYPE, VIDEO_STREAM_STATUS_FLAGS */

typedef struct {
    const char *gcs_host;         /* GCS IP address, e.g. "192.168.1.1" */
    guint16     gcs_port;         /* MAVLink UDP port, typically 14550 */
    const char *stream_uri;       /* e.g. "udp://0.0.0.0:5600" or "rtsp://..." */
    guint8      stream_type;      /* VIDEO_STREAM_TYPE_* from mavlink/common */
    guint8      encoding;         /* VIDEO_STREAM_ENCODING_H264 / _H265 / _UNKNOWN */
    guint8      camera_device_id; /* 0 = all/default camera on this component */
    gfloat      fps;
    guint16     width;
    guint16     height;
    guint32     bitrate_kbps;
} MavlinkVideoConfig;

/* Open UDP socket to the GCS.  Returns FALSE and prints to stderr on failure. */
gboolean mavlink_announce_init(const MavlinkVideoConfig *cfg);

/*
 * Send one HEARTBEAT + VIDEO_STREAM_INFORMATION to the GCS.
 * Signature matches GSourceFunc — pass directly to g_timeout_add_seconds().
 * Returns G_SOURCE_CONTINUE unless the socket has been closed.
 */
gboolean mavlink_announce_tick(gpointer user_data);

/* Close the UDP socket. Safe to call even if init was never called. */
void mavlink_announce_close(void);
