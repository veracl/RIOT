/*
 * Copyright (C) 2021 Vera Clemens <mail@veraclemens.org>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     sys_suit
 * @{
 *
 * @file
 * @brief       SUIT over MQTT-SN
 *
 * @author      Vera Clemens <mail@veraclemens.org>
 * @}
 */

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "msg.h"
#include "net/emcute.h"
#include "thread.h"
#include "periph/pm.h"
#include "xtimer.h"

#include "suit/transport/mqtt_sn.h"
#include "net/sock/util.h"

#ifdef MODULE_RIOTBOOT_SLOT
#include "riotboot/slot.h"
#endif

#ifdef MODULE_SUIT
#include "suit.h"
#include "suit/handlers.h"
#include "suit/storage.h"
#endif

#if defined(MODULE_PROGRESS_BAR)
#include "progress_bar.h"
#endif

#define LOG_PREFIX "suit_mqtt_sn: "
#include "log.h"

#define ENABLE_DEBUG              0
#include "debug.h"

#ifndef SUIT_MQTT_SN_STACKSIZE
/* allocate stack needed to do manifest validation */
#define SUIT_MQTT_SN_STACKSIZE    (3 * THREAD_STACKSIZE_LARGE)
#endif

#ifndef SUIT_MQTT_SN_PRIO
#define SUIT_MQTT_SN_PRIO         THREAD_PRIORITY_MAIN - 2
#endif

#ifndef SUIT_TOPIC_MAX
#define SUIT_TOPIC_MAX            128
#endif

#ifndef SUIT_BLOCK_DEC_PLACES_MAX
#define SUIT_BLOCK_DEC_PLACES_MAX 4
#endif

#ifndef SUIT_MANIFEST_BUFSIZE
#define SUIT_MANIFEST_BUFSIZE     640
#endif

#define DEFAULT_GATEWAY_PORT      10000

#define TFLAGS_TRIGGER            (0x0001)
#define TFLAGS_PUB_RECVD          (0x0002)
#define TFLAGS_PUB_ERR            (0x0004)
#define TFLAGS_PUB_RESP           (TFLAGS_PUB_RECVD | TFLAGS_PUB_ERR)
#define TFLAGS_ANY                (TFLAGS_TRIGGER | TFLAGS_PUB_RECVD | TFLAGS_PUB_ERR)

static emcute_sub_t _trigger_sub;
static char _trigger_topic[SUIT_TOPIC_MAX];
static emcute_sub_t _block_sub;
static char _block_topic[SUIT_TOPIC_MAX];

static char _stack[SUIT_MQTT_SN_STACKSIZE];

static uint8_t _manifest_buf[SUIT_MANIFEST_BUFSIZE];
static suit_manifest_t suit_manifest;

static char _topic[SUIT_TOPIC_MAX];

suit_mqtt_sn_blockwise_t blockwise_transfer_state;

sock_udp_ep_t last_known_good_gw;

static kernel_pid_t _suit_mqtt_sn_pid;


static int _sub(const char* topic, size_t len, emcute_cb_t on_pub, bool long_term)
{
    if (len > SUIT_TOPIC_MAX - 1) {
        LOG_ERROR(LOG_PREFIX
                  "unable to subscribe to topic '%s': max length exceeded (%d/%d)\n",
                  topic, len, SUIT_TOPIC_MAX - 1);
        return -1;
    }

    char *t;
    emcute_sub_t *s;
    if (long_term) {
        t = _trigger_topic;
        s = &_trigger_sub;
    }
    else {
        t = _block_topic;
        s = &_block_sub;
    }

    int res;
    /*
    if (s->topic.name && (res = emcute_unsub(s)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to unsubscribe from topic '%s': %d\n",
                  t, res);
        return -1;
    }
    */

    s->cb = on_pub;
    strncpy(t, topic, len);
    t[len] = '\0';
    s->topic.name = t;

sub:
    if ((res = emcute_sub(s, EMCUTE_QOS_1)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to subscribe to topic '%s': %d\n",
                  topic, res);

        if (res == EMCUTE_GWDISCON) {
            LOG_INFO(LOG_PREFIX "gateway disconnected, trying to reconnect\n");
            if ((res = emcute_con(&last_known_good_gw, true, NULL, NULL,
                                  0, 0)) == EMCUTE_OK) {
                LOG_INFO(LOG_PREFIX "successfully reconnected to gateway\n");
                goto sub;
            } else {
                LOG_ERROR(LOG_PREFIX "reconnect to gateway failed\n");
            }
        }
        return -1;
    }

    return 0;
}

int _get_blockwise(const char *topic, size_t len, emcute_cb_t on_pub_size,
                   emcute_cb_t on_pub_block)
{
    /* get total number of blocks from parent topic */
    _sub(topic, len, on_pub_size, false);

    thread_flags_t flags = thread_flags_wait_any(TFLAGS_PUB_RESP);

    if (flags & TFLAGS_PUB_ERR) {
        return -1;
    }
    else
    {
        LOG_INFO(LOG_PREFIX "expecting %i blocks\n",
                blockwise_transfer_state.num_blocks_total);

        /* get blocks */
        for (int i = 0; i < blockwise_transfer_state.num_blocks_total; i++) {
            snprintf(_block_topic, len + SUIT_BLOCK_DEC_PLACES_MAX + 2,
                     "%s/%d", topic, i);
            _sub(_block_topic, strlen(_block_topic), on_pub_block, false);

            thread_flags_t flags = thread_flags_wait_any(TFLAGS_ANY);

            if (flags & TFLAGS_PUB_ERR || flags & TFLAGS_TRIGGER) {
                /* abort download on error or new update trigger */
                return -1;
            }
        }

        return 0;
    }
}

static int _parse_block_topic(const char *topic_name)
{
    char *final_delimiter = strrchr(topic_name, '/');

    if (final_delimiter == NULL) {
        LOG_ERROR(LOG_PREFIX "unexpected topic name %s\n", topic_name);
        return -1;
    }
    else
    {
        blockwise_transfer_state.num_blocks_rcvd++;
        blockwise_transfer_state.current_block_num = atoi(final_delimiter + 1);
        return 0;
    }
}

// TODO this is an unchanged copy from coap.c, can it be shared instead?
#ifdef MODULE_SUIT
static inline void _print_download_progress(suit_manifest_t *manifest,
                                            size_t offset, size_t len,
                                            size_t image_size)
{
    (void)manifest;
    (void)offset;
    (void)len;
    DEBUG("_suit_flashwrite(): writing %u bytes at pos %u\n", len, offset);
#if defined(MODULE_PROGRESS_BAR)
    if (image_size != 0) {
        char _suffix[7] = { 0 };
        uint8_t _progress = 100 * (offset + len) / image_size;
        sprintf(_suffix, " %3d%%", _progress);
        progress_bar_print("Fetching firmware ", _suffix, _progress);
        if (_progress == 100) {
            puts("");
        }
    }
#else
    (void) image_size;
#endif
}
#endif

void suit_mqtt_sn_trigger(const char *topic, size_t len)
{
    if (len > SUIT_TOPIC_MAX - SUIT_BLOCK_DEC_PLACES_MAX - 2) {
        /* extra length is used for appending /0, /1, ...
         * (up to SUIT_BLOCK_DEC_PLACES_MAX decimal places) */
        LOG_ERROR(LOG_PREFIX
                  "unable to handle trigger '%s': max length exceeded (%d/%d)",
                  topic, len, SUIT_TOPIC_MAX - SUIT_BLOCK_DEC_PLACES_MAX - 2);
        return;
    }

    if (strncmp(topic, "mqtt://", 7) == 0) {
        topic += 7;
    }

    memcpy(_topic, topic, len);
    _topic[len] = '\0';
    thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_TRIGGER);
}

static void _on_pub_trigger(const emcute_topic_t *topic, void *data, size_t len)
{
    (void)topic;

    DEBUG(LOG_PREFIX "received PUBLISH for trigger topic\n");

    /* payload contains topic name of manifest */
    suit_mqtt_sn_trigger((char *)data, len);
}

static void _on_pub_size(const emcute_topic_t *topic, void *data, size_t len)
{
    (void)topic;

    if (len > SUIT_BLOCK_DEC_PLACES_MAX) {
        LOG_ERROR(LOG_PREFIX
                  "unable to do blockwise transfer: "
                  "too many blocks (%d/(10**%d - 1))\n",
                  len, SUIT_BLOCK_DEC_PLACES_MAX);
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
    }

    DEBUG(LOG_PREFIX "received PUBLISH for size topic\n");

    char data_string[len + 1];
    memcpy(data_string, data, len);
    data_string[len] = '\0';
    blockwise_transfer_state.num_blocks_total = atoi(data_string);
    blockwise_transfer_state.num_blocks_rcvd = 0;
    thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_RECVD);
}

static void _on_pub_manifest(const emcute_topic_t *topic, void *data, size_t len)
{
    DEBUG(LOG_PREFIX "received PUBLISH for manifest topic '%s' (ID %i)\n",
             topic->name, (int)topic->id);

    if (_parse_block_topic(topic->name) != 0) {
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    DEBUG(LOG_PREFIX "received manifest block %i\n",
            blockwise_transfer_state.current_block_num);

    blockwise_transfer_state.current_block_len = len;

    /* payload contains manifest */
    memcpy(_manifest_buf + blockwise_transfer_state.current_block_num
                           * CONFIG_SUIT_MQTT_SN_BLOCKSIZE, data, len);

    thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_RECVD);
}

void _on_pub_firmware(const emcute_topic_t *topic,
                                  void *data, size_t len)
{
    DEBUG(LOG_PREFIX
          "received PUBLISH (%i bytes) for firmware topic '%s' (ID %i)\n",
          len, topic->name, (int)topic->id);

    if(_parse_block_topic(topic->name) != 0) {
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    /* Firmware blocks must be received in order. Otherwise, writing them to
       flash fails. */
    if (blockwise_transfer_state.current_block_num
        != blockwise_transfer_state.num_blocks_rcvd - 1) {
        LOG_ERROR(LOG_PREFIX "received firmware block %i out of order "
                "(expected: %i)\n",
                blockwise_transfer_state.current_block_num,
                blockwise_transfer_state.num_blocks_rcvd - 1);

        /* Do not count out-of-order blocks as received */
        blockwise_transfer_state.num_blocks_rcvd--;

        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    DEBUG(LOG_PREFIX "received firmware block %i\n",
            blockwise_transfer_state.current_block_num);

    blockwise_transfer_state.current_block_len = len;

    suit_manifest_t *manifest = &suit_manifest;
    size_t offset = blockwise_transfer_state.current_block_num
                    * CONFIG_SUIT_MQTT_SN_BLOCKSIZE;
    bool more = (blockwise_transfer_state.num_blocks_rcvd
                    < blockwise_transfer_state.num_blocks_total);

    uint32_t image_size;
    nanocbor_value_t param_size;
    size_t total = offset + blockwise_transfer_state.current_block_len;
    suit_component_t *comp = &manifest->components[manifest->component_current];
    suit_param_ref_t *ref_size = &comp->param_size;

    /* Grab the total image size from the manifest */
    if ((suit_param_ref_to_cbor(manifest, ref_size, &param_size) == 0) ||
            (nanocbor_get_uint32(&param_size, &image_size) < 0)) {
        /* Early exit if the total image size can't be determined */
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    if (image_size < offset + blockwise_transfer_state.current_block_len) {
        /* Extra newline at the start to compensate for the progress bar */
        LOG_ERROR(
            "\n" LOG_PREFIX "Image beyond size, offset + len=%u, "
            "image_size=%u\n", (unsigned)(total), (unsigned)image_size);
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    if (!more && image_size != total) {
        LOG_INFO(LOG_PREFIX "Incorrect size received, got %u, expected %u\n",
                    (unsigned)total, (unsigned)image_size);
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    _print_download_progress(manifest, offset,
                                blockwise_transfer_state.current_block_len,
                                image_size);

    int res = suit_storage_write(comp->storage_backend, manifest,
                                 data,
                                 offset,
                                 blockwise_transfer_state.current_block_len);

    if (!more) {
        LOG_INFO(LOG_PREFIX "Finalizing payload store\n");
        /* Finalize the write if no more data available */
        res = suit_storage_finish(comp->storage_backend, manifest);
    }

    if (res != SUIT_OK) {
        LOG_ERROR(LOG_PREFIX
                    "Error writing firmware block to storage\n");
        thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_ERR);
        return;
    }

    thread_flags_set(thread_get(_suit_mqtt_sn_pid), TFLAGS_PUB_RECVD);
}

int suit_mqtt_sn_fetch(const char *topic, void *arg)
{
    memcpy(&suit_manifest, arg, sizeof(suit_manifest));

    if (strncmp(topic, "mqtt://", 7) == 0) {
        topic += 7;
    }

    strncpy(_topic, topic, strlen(topic));
    _topic[strlen(topic)] = '\0';

    int res = _get_blockwise(_topic, strlen(_topic),
                             &_on_pub_size, &_on_pub_firmware);

    return res;
}

int _pub_device_status(char *topic_name, char *data, size_t len) {
    int res;
    emcute_topic_t t;

    t.name = topic_name;
    if ((res = emcute_reg(&t)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to reg topic ID for %s: %d\n",
                  topic_name, res);
        return 1;
    }
    if ((res = emcute_pub(&t, data, len, EMCUTE_QOS_1)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to publish device status: %d\n", res);
        return 1;
    }
    return 0;
}

int cmd_con(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <ipv6 addr> [port]\n", argv[0]);
        return 1;
    }

    sock_udp_ep_t gw = { .family = AF_INET6, .port = DEFAULT_GATEWAY_PORT };

    if (ipv6_addr_from_str((ipv6_addr_t *)&gw.addr.ipv6, argv[1]) == NULL) {
        LOG_ERROR(LOG_PREFIX "error parsing IPv6 address of gateway\n");
        return 1;
    }

    if (argc >= 3) {
        gw.port = atoi(argv[2]);
    }

    int res;
    if ((res = emcute_con(&gw, true, NULL, NULL, 0, 0)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to connect to gateway at [%s]:%i: %d\n",
               argv[1], (int)gw.port, res);
        return 1;
    }
    LOG_INFO(LOG_PREFIX "connected to gateway at [%s]:%i\n",
           argv[1], (int)gw.port);

    /* store as last known good gateway */
    memcpy(&last_known_good_gw, &gw, sizeof(gw));

    /* publish device status */
    char slot_active = '0' + riotboot_slot_current();
    _pub_device_status(SUIT_RESOURCE_SLOT_ACTIVE "/" SUIT_ID, &slot_active, 1);
    char slot_inactive = '0' + riotboot_slot_other();
    _pub_device_status(SUIT_RESOURCE_SLOT_INACTIVE "/" SUIT_ID, &slot_inactive, 1);
    char version[10];
    sprintf(version, "%10d",
            (unsigned)riotboot_slot_get_hdr(riotboot_slot_current())->version);
    _pub_device_status(SUIT_RESOURCE_VERSION "/" SUIT_ID, version, strlen(version));

    return 0;
}

int cmd_sub(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <topic name>\n", argv[0]);
        return 1;
    }

    return _sub(argv[1], strlen(argv[1]), &_on_pub_trigger, true);
}

static void *_suit_mqtt_sn_thread(void *arg)
{
    (void)arg;

    LOG_INFO(LOG_PREFIX "started.\n");

    /* initialize message queue (for ...) */
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    _suit_mqtt_sn_pid = thread_getpid();

    while (true) {
        thread_flags_wait_any(TFLAGS_TRIGGER);

        LOG_INFO(LOG_PREFIX "trigger received\n");
        int res;
        res = _get_blockwise(_topic, strlen(_topic),
                             &_on_pub_size, &_on_pub_manifest);

        if (res < 0) {
            continue;
        }

#ifdef MODULE_SUIT
        suit_manifest_t manifest;
        memset(&manifest, 0, sizeof(manifest));

        manifest.urlbuf = _topic;
        manifest.urlbuf_len = SUIT_TOPIC_MAX;

        if ((res = suit_parse(&manifest, _manifest_buf,
                              (blockwise_transfer_state.num_blocks_total - 1)
                              * CONFIG_SUIT_MQTT_SN_BLOCKSIZE
                              + blockwise_transfer_state.current_block_len))
                != SUIT_OK) {
            LOG_INFO(LOG_PREFIX
                        "suit_parse() failed. res=%i\n", res);
            continue;
        }
#endif

        if (res == 0) {
            const riotboot_hdr_t *hdr = riotboot_slot_get_hdr(
                riotboot_slot_other());
            riotboot_hdr_print(hdr);
            xtimer_sleep(1);

            if (riotboot_hdr_validate(hdr) == 0) {
                LOG_INFO(LOG_PREFIX "rebooting...\n");
                pm_reboot();
            }
            else {
                LOG_INFO(LOG_PREFIX
                            "update failed, hdr invalid\n");
            }
        }
    }

    return NULL;
}

void suit_mqtt_sn_run(void)
{
    /* start thread responsible for handling incoming update triggers */
    thread_create(_stack, SUIT_MQTT_SN_STACKSIZE,
                  SUIT_MQTT_SN_PRIO,
                  THREAD_CREATE_STACKTEST,
                  _suit_mqtt_sn_thread, NULL, "suit_mqtt_sn");
}
