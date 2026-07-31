/*
 * magnet_ot.c — OpenThread bringup + CoAP /magnet transport (E-Phase B).
 *
 * Unified single-role node (§6 Phase 0 logic, in C): every node commits the
 * same fixed dataset, enables router eligibility, and lets Thread's native
 * MLE/self-healing handle discovery, attach, leader election, and failover.
 * No scan, no isLeader, no `dataset init new`.
 *
 * The dev dataset (channel 24, well-known key) matches the v0.0.6 Arduino
 * prototype so mixed benches mesh together; passphrase-derived datasets and
 * the §11.1 key hierarchy land in E-Phase D.
 *
 * Deadlock discipline: this file's callbacks run in the OT mainloop task and
 * only post to the event pump (mn_post_*). mn_ot_send() is called from the
 * HCP/Forth side and takes the OT lock around the CoAP API.
 *
 * Build MN_ENABLE_OPENTHREAD=0 (env esp32c6_noot) to compile this out.
 */
#include "magnet.h"

#if MN_ENABLE_OPENTHREAD

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"

#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"   /* otThreadSetRouterEligible (FTD build) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- E-B fixed dev network (interops with the v0.0.6 Arduino bench) ---- */
#define MN_OT_CHANNEL      24
#define MN_OT_PANID        0x4d4e                     /* "MN" */
#define MN_OT_NETWORK_NAME "MagNET-dev"
/* E-D: the multicast group is DERIVED from the channel root_secret
 * (ff05::<suffix>, §11.1.3) — no fixed group anymore. */
static const uint8_t MN_OT_NETWORK_KEY[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};
static const uint8_t MN_OT_EXT_PANID[8] = { 'M','a','g','N','E','T','v','2' };

static const esp_openthread_platform_config_t s_ot_config = {
    .radio_config = {
        .radio_mode = RADIO_MODE_NATIVE,           /* C6 native 802.15.4 */
    },
    .host_config = {
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,
    },
    .port_config = {
        .storage_partition_name = "nvs",
        .netif_queue_size = 10,
        .task_queue_size  = 10,
    },
};

static otInstance  *s_inst = NULL;                    /* set once config is done */
static char         s_role[12] = "-";
static otIp6Address s_mcast;

const char *mn_ot_role_name(void) { return s_role; }

static void mcast_from_suffix(otIp6Address *a, const uint8_t suffix[4]) {
    memset(a, 0, sizeof(*a));
    a->mFields.m8[0] = 0xff;
    a->mFields.m8[1] = 0x05;
    memcpy(&a->mFields.m8[12], suffix, 4);
}

int mn_ot_set_mcast(const uint8_t suffix[4]) {
    if (!s_inst) return -1;                          /* applied at bringup */
    otIp6Address next;
    mcast_from_suffix(&next, suffix);
    esp_openthread_lock_acquire(portMAX_DELAY);
    otIp6UnsubscribeMulticastAddress(s_inst, &s_mcast);
    s_mcast = next;
    otError err = otIp6SubscribeMulticastAddress(s_inst, &s_mcast);
    esp_openthread_lock_release();
    return err == OT_ERROR_NONE ? 0 : -2;
}

/* ---- CoAP /magnet (runs in OT mainloop task — post, don't emit) ---- */
static void coap_magnet_handler(void *ctx, otMessage *msg, const otMessageInfo *mi) {
    (void)ctx;
    uint16_t off = otMessageGetOffset(msg);
    uint16_t len = otMessageGetLength(msg) - off;
    uint8_t  buf[512];
    if (len > sizeof(buf)) len = sizeof(buf);
    otMessageRead(msg, off, buf, len);

    char src[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&mi->mPeerAddr, src, sizeof(src));
    bool mcast = otIp6IsAddressEqual(&mi->mSockAddr, &s_mcast);

    /* CON PUT (DMs) wants an ACK so the sender's retransmit stops */
    if (otCoapMessageGetType(msg) == OT_COAP_TYPE_CONFIRMABLE) {
        otMessage *rsp = otCoapNewMessage(s_inst, NULL);
        if (rsp) {
            otCoapMessageInitResponse(rsp, msg, OT_COAP_TYPE_ACKNOWLEDGMENT,
                                      OT_COAP_CODE_CHANGED);
            if (otCoapSendResponse(s_inst, rsp, mi) != OT_ERROR_NONE) {
                otMessageFree(rsp);
            }
        }
    }

    mn_post_rx(buf, len, src, mcast);
}

static otCoapResource s_magnet_res = {
    .mUriPath = "magnet",
    .mHandler = coap_magnet_handler,
    .mContext = NULL,
    .mNext    = NULL,
};

static void on_ot_state_changed(otChangedFlags flags, void *ctx) {
    (void)ctx;
    if (!(flags & OT_CHANGED_THREAD_ROLE)) return;
    const char *name = otThreadDeviceRoleToString(otThreadGetDeviceRole(s_inst));
    strlcpy(s_role, name, sizeof(s_role));
    mn_post_role(name);
}

/* ---- outbound (called from HCP/Forth tasks; takes the OT lock) ---- */
int mn_ot_send(const uint8_t *buf, size_t len, const char *dst_ipv6, bool confirmable) {
    if (!s_inst) return -1;                       /* radio not up yet */

    otIp6Address dst;
    if (dst_ipv6) {
        if (otIp6AddressFromString(dst_ipv6, &dst) != OT_ERROR_NONE) return -2;
    } else {
        dst = s_mcast;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = OT_ERROR_NO_BUFS;
    otMessage *msg = otCoapNewMessage(s_inst, NULL);
    if (msg) {
        otCoapMessageInit(msg,
                          confirmable ? OT_COAP_TYPE_CONFIRMABLE
                                      : OT_COAP_TYPE_NON_CONFIRMABLE,
                          OT_COAP_CODE_PUT);
        err = otCoapMessageAppendUriPathOptions(msg, "magnet");
        if (err == OT_ERROR_NONE) err = otCoapMessageSetPayloadMarker(msg);
        if (err == OT_ERROR_NONE) err = otMessageAppend(msg, buf, (uint16_t)len);
        if (err == OT_ERROR_NONE) {
            otMessageInfo mi;
            memset(&mi, 0, sizeof(mi));
            mi.mPeerAddr = dst;
            mi.mPeerPort = OT_DEFAULT_COAP_PORT;
            err = otCoapSendRequest(s_inst, msg, &mi, NULL, NULL);
        }
        if (err != OT_ERROR_NONE) otMessageFree(msg);
    }
    esp_openthread_lock_release();
    return (err == OT_ERROR_NONE) ? 0 : -3;
}

int mn_ot_local_eid(char *buf, size_t cap) {
    if (!s_inst || cap < OT_IP6_ADDRESS_STRING_SIZE) return -1;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otIp6Address eid = *otThreadGetMeshLocalEid(s_inst);
    esp_openthread_lock_release();
    otIp6AddressToString(&eid, buf, (uint16_t)cap);
    return 0;
}

/* Thread detail. Snapshot under the OT lock, emit AFTER releasing it — never
 * take the TX mutex while holding the OT lock (see magnet.h). */
#define MN_MESH_NEIGHBOR_MAX 8

void mn_mesh_print(void) {
    if (!s_inst) { mn_emit_event("# mesh: radio not up"); return; }

    struct { uint16_t rloc16; int8_t rssi; uint8_t lqi; uint32_t age; bool child; }
        nb[MN_MESH_NEIGHBOR_MAX];
    int nb_n = 0;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role   = otThreadGetDeviceRole(s_inst);
    uint32_t partition  = otThreadGetPartitionId(s_inst);
    uint16_t rloc16     = otThreadGetRloc16(s_inst);
    uint8_t  channel    = otLinkGetChannel(s_inst);
    uint16_t panid      = otLinkGetPanId(s_inst);
    otIp6Address eid    = *otThreadGetMeshLocalEid(s_inst);
    otLeaderData leader;
    bool has_leader = (otThreadGetLeaderData(s_inst, &leader) == OT_ERROR_NONE);

    otNeighborInfo         info;
    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    while (nb_n < MN_MESH_NEIGHBOR_MAX &&
           otThreadGetNextNeighborInfo(s_inst, &it, &info) == OT_ERROR_NONE) {
        nb[nb_n].rloc16 = info.mRloc16;
        nb[nb_n].rssi   = info.mAverageRssi;
        nb[nb_n].lqi    = info.mLinkQualityIn;
        nb[nb_n].age    = info.mAge;
        nb[nb_n].child  = info.mIsChild;
        nb_n++;
    }
    esp_openthread_lock_release();

    char eid_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&eid, eid_str, sizeof(eid_str));

    mn_emit_event("# mesh role=%s partition=0x%08lx rloc16=0x%04x chan=%u pan=0x%04x",
                  otThreadDeviceRoleToString(role), (unsigned long)partition,
                  rloc16, channel, panid);
    char mc[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&s_mcast, mc, sizeof(mc));
    mn_emit_event("# mesh ml-eid=%s mcast=%s", eid_str, mc);
    if (has_leader) {
        mn_emit_event("# mesh leader-rloc=0x%04x weight=%u",
                      (unsigned)((uint16_t)leader.mLeaderRouterId << 10),
                      leader.mWeighting);
    }
    for (int i = 0; i < nb_n; i++) {
        mn_emit_event("# mesh neighbor rloc16=0x%04x rssi=%d lqi=%u age=%lus %s",
                      nb[i].rloc16, nb[i].rssi, nb[i].lqi,
                      (unsigned long)nb[i].age, nb[i].child ? "child" : "router");
    }
    if (!nb_n) mn_emit_event("# mesh neighbors: none");
}

/* ---- bringup ---- */
static void ot_configure(otInstance *inst) {
    /* device_id comes from the ECDSA identity (SHA256(pk)[0:4]) since E-D */
    /* fixed dataset, identical on every node (§6 Phase 0 fix #5) */
    otOperationalDataset ds;
    memset(&ds, 0, sizeof(ds));
    ds.mActiveTimestamp.mSeconds        = 1;
    ds.mComponents.mIsActiveTimestampPresent = true;
    ds.mChannel                         = MN_OT_CHANNEL;
    ds.mComponents.mIsChannelPresent    = true;
    ds.mPanId                           = MN_OT_PANID;
    ds.mComponents.mIsPanIdPresent      = true;
    memcpy(ds.mNetworkKey.m8, MN_OT_NETWORK_KEY, sizeof(MN_OT_NETWORK_KEY));
    ds.mComponents.mIsNetworkKeyPresent = true;
    memcpy(ds.mExtendedPanId.m8, MN_OT_EXT_PANID, sizeof(MN_OT_EXT_PANID));
    ds.mComponents.mIsExtendedPanIdPresent = true;
    strlcpy(ds.mNetworkName.m8, MN_OT_NETWORK_NAME, sizeof(ds.mNetworkName.m8));
    ds.mComponents.mIsNetworkNamePresent = true;
    ESP_ERROR_CHECK(otDatasetSetActive(inst, &ds) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);

    otSetStateChangedCallback(inst, on_ot_state_changed, NULL);
    otThreadSetRouterEligible(inst, true);        /* all nodes REED (§4.3) */

    ESP_ERROR_CHECK(otIp6SetEnabled(inst, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(otCoapStart(inst, OT_DEFAULT_COAP_PORT) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
    otCoapAddResource(inst, &s_magnet_res);

    mcast_from_suffix(&s_mcast, mn_channel_mcast_suffix());
    ESP_ERROR_CHECK(otIp6SubscribeMulticastAddress(inst, &s_mcast) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);

    /* join-or-form; Thread self-heals from here (§3.4) */
    ESP_ERROR_CHECK(otThreadSetEnabled(inst, true) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
}

static void ot_main_task(void *arg) {
    (void)arg;
    mn_set_state(MN_ATTACHING);

    /* eventfd is required by the OpenThread platform port. */
    esp_vfs_eventfd_config_t eventfd_cfg = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_cfg));

    ESP_ERROR_CHECK(esp_openthread_init(&s_ot_config));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&s_ot_config)));

    otInstance *inst = esp_openthread_get_instance();
    ot_configure(inst);
    s_inst = inst;                       /* publish only after config is complete */

    mn_emit_event("# openthread up: chan=%d pan=0x%04x net=%s (mcast derived from channel)",
                  MN_OT_CHANNEL, MN_OT_PANID, MN_OT_NETWORK_NAME);

    /* Blocks running the OpenThread stack (tasklets, radio, CoAP RX). */
    esp_openthread_launch_mainloop();

    /* only reached on shutdown */
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(netif);
    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

void mn_openthread_start(void) {
    /* esp_event + netif + default NVS must exist before OT init. NVS is also
     * where the Thread dataset (and later our identity/counter) is stored. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Generous stack — OpenThread mainloop + lwIP need headroom. */
    xTaskCreate(ot_main_task, "ot_main", 8192, NULL, 5, NULL);
}

#else  /* MN_ENABLE_OPENTHREAD == 0 */

const char *mn_ot_role_name(void) { return "-"; }

void mn_mesh_print(void) { mn_emit_event("# mesh: openthread disabled in this build"); }

int mn_ot_local_eid(char *buf, size_t cap) { (void)buf; (void)cap; return -1; }

int mn_ot_set_mcast(const uint8_t suffix[4]) { (void)suffix; return -1; }

int mn_ot_send(const uint8_t *buf, size_t len, const char *dst_ipv6, bool confirmable) {
    (void)buf; (void)confirmable;
    mn_emit_event("# (no-ot build) would send %u bytes to %s",
                  (unsigned)len, dst_ipv6 ? dst_ipv6 : "multicast");
    return 0;
}

void mn_openthread_start(void) {
    mn_emit_event("# openthread DISABLED in this build (footprint baseline)");
}

#endif
