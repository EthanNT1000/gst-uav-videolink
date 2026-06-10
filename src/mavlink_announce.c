#include "mavlink_announce.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* MAVLink IDs for a camera component on the vehicle */
#define SYSID   1                       /* vehicle system id   */
#define COMPID  MAV_COMP_ID_CAMERA      /* = 100 in common.h   */

static int                g_sock = -1;
static struct sockaddr_in g_gcs_addr;
static MavlinkVideoConfig g_cfg;

gboolean mavlink_announce_init(const MavlinkVideoConfig *cfg)
{
    g_cfg = *cfg;

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        perror("[mavlink] socket");
        return FALSE;
    }

    memset(&g_gcs_addr, 0, sizeof(g_gcs_addr));
    g_gcs_addr.sin_family = AF_INET;
    g_gcs_addr.sin_port   = htons(cfg->gcs_port);
    if (inet_pton(AF_INET, cfg->gcs_host, &g_gcs_addr.sin_addr) != 1) {
        fprintf(stderr, "[mavlink] Invalid GCS address: %s\n", cfg->gcs_host);
        close(g_sock);
        g_sock = -1;
        return FALSE;
    }

    g_print("[mavlink] Announcing stream to %s:%u\n",
            cfg->gcs_host, (unsigned)cfg->gcs_port);
    return TRUE;
}

/* Internal: serialize and send a packed MAVLink message */
static void send_msg(const mavlink_message_t *msg)
{
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    int len = (int)mavlink_msg_to_send_buffer(buf, msg);
    sendto(g_sock, buf, (size_t)len, 0,
           (const struct sockaddr *)&g_gcs_addr, sizeof(g_gcs_addr));
}

static void send_heartbeat(void)
{
    mavlink_message_t msg;
    mavlink_msg_heartbeat_pack(SYSID, COMPID, &msg,
        MAV_TYPE_CAMERA,
        MAV_AUTOPILOT_INVALID,
        0, 0,
        MAV_STATE_ACTIVE);
    send_msg(&msg);
}

static void send_video_stream_info(void)
{
    /*
     * VIDEO_STREAM_INFORMATION (#269) — tells the GCS where to find the
     * stream and what codec/resolution/bitrate to expect.  QGroundControl
     * subscribes to this message and opens the player automatically.
     *
     * name: max 32 bytes, uri: max 160 bytes (MAVLink field limits).
     */
    char name[32];
    char uri[160];

    strncpy(name, "UAV Camera",          sizeof(name) - 1);
    strncpy(uri,  g_cfg.stream_uri,      sizeof(uri)  - 1);
    name[sizeof(name) - 1] = '\0';
    uri[sizeof(uri)  - 1] = '\0';

    mavlink_message_t msg;
    mavlink_msg_video_stream_information_pack(SYSID, COMPID, &msg,
        1,                                  /* stream_id (1-based) */
        1,                                  /* count — streams on this camera */
        g_cfg.stream_type,
        VIDEO_STREAM_STATUS_FLAGS_RUNNING,
        g_cfg.fps,
        g_cfg.width,
        g_cfg.height,
        g_cfg.bitrate_kbps * 1000u,         /* field is in bps */
        0,                                  /* rotation degrees */
        0,                                  /* horizontal FOV (0 = unknown) */
        name,
        uri,
        g_cfg.encoding,                     /* VIDEO_STREAM_ENCODING_H264/H265 */
        g_cfg.camera_device_id);            /* 0 = default camera */
    send_msg(&msg);
}

gboolean mavlink_announce_tick(gpointer user_data)
{
    (void)user_data;
    if (g_sock < 0)
        return G_SOURCE_REMOVE;

    send_heartbeat();
    send_video_stream_info();
    return G_SOURCE_CONTINUE;
}

void mavlink_announce_close(void)
{
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}
