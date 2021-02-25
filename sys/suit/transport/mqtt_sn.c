/*
 * Copyright (C) 2020 Vera Clemens <mail@veraclemens.org>
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

#define ENABLE_DEBUG 0
#include "debug.h"

#ifndef SUIT_MQTT_SN_STACKSIZE
/* allocate stack needed to do manifest validation */
#define SUIT_MQTT_SN_STACKSIZE (3 * THREAD_STACKSIZE_LARGE)
#endif

#ifndef SUIT_MQTT_SN_PRIO
#define SUIT_MQTT_SN_PRIO THREAD_PRIORITY_MAIN - 2
#endif

#ifndef SUIT_TOPIC_MAX
#define SUIT_TOPIC_MAX            128
#endif

#ifndef SUIT_MANIFEST_BUFSIZE
#define SUIT_MANIFEST_BUFSIZE   640
#endif

#define GATEWAY_PORT        10000
#define GATEWAY_ADDRESS     "2001:db8::1"

#define SUB_MAXNUM          (3U)
#define TOPICS_PER_SUB      (1 + CONFIG_EMCUTE_SUBTOPICS_MAX)
#define TOPIC_MAXNUM        (TOPICS_PER_SUB * SUB_MAXNUM)
#define TOPIC_MAXLEN        (64U)

#define SUIT_MSG_TRIGGER    0x1234
#define SUIT_MSG_MANIFEST   0x1235
#define SUIT_MSG_FIRMWARE   0x1236

static int expected_manifest_block = 0;
static int num_manifest_blocks = 0;
static int expected_fw_block = 0;
static int num_fw_blocks = 0;

static emcute_sub_t subscriptions[SUB_MAXNUM];
static char topics[TOPIC_MAXNUM][TOPIC_MAXLEN];

static char _stack[SUIT_MQTT_SN_STACKSIZE];

static char _manifest_topic[SUIT_TOPIC_MAX];
static char _fw_topic[SUIT_TOPIC_MAX];

static uint8_t _manifest_buf[SUIT_MANIFEST_BUFSIZE];
static suit_manifest_t suit_manifest;

suit_mqtt_sn_firmware_block_t current_fw_block;
suit_mqtt_sn_manifest_block_t current_manifest_block;

sock_udp_ep_t last_known_good_gateway;

static kernel_pid_t _suit_mqtt_sn_pid;


static int sub(const char* topic, size_t len, emcute_pub_cb_t on_pub,
               emcute_reg_cb_t on_reg)
{
    if (len > TOPIC_MAXLEN) {
        LOG_ERROR(LOG_PREFIX
                  "unable to subscribe to topic '%s': max length exceeded (%d/%d)\n",
                  topic, len, TOPIC_MAXLEN);
        return 1;
    }

    unsigned i;
    if (strncmp(SUIT_TRIGGER, topic, len) == 0) {
        /* always store in first subscription slot */
        i = 0;
    }
    else {
        /* find empty subscription slot */
        for (i = 1; (i < SUB_MAXNUM) && (subscriptions[i].topics[0].id != 0); i++) {}
        if (i == SUB_MAXNUM) {
            // TODO replacement strategy?
            // replace least recently added (+ unsubscribe)?
            // currently, most recently added topic is replaced (+ no unsubscribe)
        }
    }
    subscriptions[i].pub_cb = on_pub;
    subscriptions[i].reg_cb = on_reg;

    strncpy(topics[i], topic, len);
    topics[i][len] = '\0';
    subscriptions[i].topics[0].name = topics[i];

    int res;
sub:
    if ((res = emcute_sub(&subscriptions[i], EMCUTE_QOS_1)) != EMCUTE_OK) {
        LOG_ERROR(LOG_PREFIX "unable to subscribe to topic '%s': %d\n",
                  topic, res);

        if (res == EMCUTE_GWDISCON) {
            LOG_INFO(LOG_PREFIX "gateway disconnected, trying to reconnect\n");
            if ((res = emcute_con(&last_known_good_gateway, true, NULL, NULL,
                                  0, 0)) == EMCUTE_OK) {
                LOG_INFO(LOG_PREFIX "successfully reconnected to gateway\n");
                goto sub;
            } else {
                LOG_ERROR(LOG_PREFIX "reconnect to gateway failed\n");
            }
        }
        return 1;
    }
    LOG_INFO(LOG_PREFIX "subscribed to topic '%s'\n", topic);
    return 0;
}

static int _suit_handle_topic(char *topic, emcute_pub_cb_t on_pub,
                              emcute_reg_cb_t on_reg)
{
    if (strncmp(topic, "mqtt://", 7)) {
        LOG_ERROR(LOG_PREFIX "topic doesn't start with \"mqtt://\"\n");
        return -EINVAL;
    }
    DEBUG(LOG_PREFIX "downloading from '%s'\n", topic);
    topic += 7;
    strcat(topic, "/#");
    return sub(topic, strlen(topic), on_pub, on_reg);
}

static int _parse_block_publish(const char *topic_name, void *data,
                                int *num_blocks, int *expected_block) {
    char *final_delimiter = strrchr(topic_name, '/');

    if (final_delimiter == NULL) {
        LOG_WARNING(LOG_PREFIX "unexpected topic name %s\n", topic_name);
        return -1;
    }
    else if (topic_name[strlen(topic_name) - 1] == '/') {
        *num_blocks = atoi((char *) data);
        DEBUG(LOG_PREFIX "expecting %i blocks\n", *num_blocks);
        return -1;
    }
    else {
        int block_num = atoi(final_delimiter + 1);

        if (block_num != *expected_block) {
            LOG_WARNING(LOG_PREFIX
                        "received unexpected block %i, expected %i\n",
                        block_num, *expected_block);
            return -1;
        }

        if (block_num == *num_blocks - 1) {
            *expected_block = 0;
        } else {
            *expected_block = block_num + 1;
        }

        return block_num;
    }
}

static void on_pub_trigger(const emcute_topic_t *topic, void *data, size_t len)
{
    /* Currently, the topic is irrelevant. It is simply assumed that all
     * incoming published messages are update triggers.
     */
    (void)topic;

    DEBUG(LOG_PREFIX "received PUBLISH for trigger topic\n");

    /* payload contains topic name of manifest */
    suit_mqtt_sn_trigger((char *)data, len);
}

static void on_reg(const void *s, const char *name, size_t len, uint16_t tid)
{
    emcute_sub_t *sub = (emcute_sub_t *)s;
    DEBUG(LOG_PREFIX "received REGISTER\n");

    /* find free slot for new subtopic */
    unsigned i = 1;
    for (i = 1; (i <= CONFIG_EMCUTE_SUBTOPICS_MAX) && (sub->topics[i].id != 0); i++) {}
    if (i == CONFIG_EMCUTE_SUBTOPICS_MAX + 1) {
        DEBUG(LOG_PREFIX "max number of subtopic ids exceeded\n");
        i -= 1; // TODO replacement strategy?
    }

    /* find free slot for new topic name */
    unsigned j;
    for (j = 0; (j < TOPIC_MAXNUM) && (topics[j][0] != '\0'); j++) {}
    if (j == TOPIC_MAXNUM) {
        DEBUG(LOG_PREFIX "max number of topic names exceeded\n");
        j -= 1; // TODO replacement strategy?
    }

    sub->topics[i].id = tid;
    strncpy(topics[j], name, len);
    topics[j][len] = '\0';
    sub->topics[i].name = topics[j];
}

void suit_mqtt_sn_trigger(const char *topic, size_t len)
{
    memcpy(_manifest_topic, topic, len);
    _manifest_topic[len] = '\0';
    msg_t m = { .type = SUIT_MSG_TRIGGER };
    msg_send(&m, _suit_mqtt_sn_pid);
}

static void suit_mqtt_sn_on_pub_manifest(const emcute_topic_t *topic,
                                         void *data, size_t len)
{
    DEBUG(LOG_PREFIX "received PUBLISH for manifest topic '%s' (ID %i)\n",
             topic->name, (int)topic->id);

    /* payload contains manifest */
    int manifest_block_num = _parse_block_publish(topic->name, data,
                                                  &num_manifest_blocks,
                                                  &expected_manifest_block);

    if (manifest_block_num >= 0) {
        DEBUG(LOG_PREFIX "received manifest block %i\n", manifest_block_num);

        current_manifest_block.num = manifest_block_num;
        current_manifest_block.len = len;

        memcpy(_manifest_buf + manifest_block_num * CONFIG_SUIT_MQTT_SN_BLOCKSIZE, data, len);

        msg_t m = { .type = SUIT_MSG_MANIFEST,
                    .content.value = manifest_block_num };
        msg_send(&m, _suit_mqtt_sn_pid);
    }
}

void suit_mqtt_sn_on_pub_firmware(const emcute_topic_t *topic,
                                  void *data, size_t len)
{
    LOG_DEBUG(LOG_PREFIX
              "received PUBLISH (%i bytes) for firmware topic '%s' (ID %i)\n",
              len, topic->name, (int)topic->id);

    /* payload contains firmware */
    int fw_block_num = _parse_block_publish(topic->name, data,
                                            &num_fw_blocks,
                                            &expected_fw_block);

    if (fw_block_num >= 0) {
        DEBUG(LOG_PREFIX "received firmware block %i\n", fw_block_num);

        current_fw_block.num = fw_block_num;
        current_fw_block.len = len;

        memcpy(current_fw_block.data, data, len);

        msg_t m = { .type = SUIT_MSG_FIRMWARE };
        msg_send(&m, _suit_mqtt_sn_pid);
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

int suit_mqtt_sn_fetch(const char *topic,
                       mqtt_sn_fetch_cb_t callback, void *arg)
{
    memcpy(&suit_manifest, arg, sizeof(suit_manifest));
    strncpy(_fw_topic, topic, SUIT_TOPIC_MAX);
    _fw_topic[SUIT_TOPIC_MAX - 1] = '\0';
    _suit_handle_topic(_fw_topic, callback, &on_reg);

    msg_t m;
    while (true) {
        msg_receive(&m);

        switch (m.type) {
            case SUIT_MSG_FIRMWARE:
                ;
                suit_manifest_t *manifest = &suit_manifest;
                size_t offset = current_fw_block.num * CONFIG_SUIT_MQTT_SN_BLOCKSIZE;
                bool more = (current_fw_block.num < (num_fw_blocks - 1));

                uint32_t image_size;
                nanocbor_value_t param_size;
                size_t total = offset + current_fw_block.len;
                suit_component_t *comp = &manifest->components[manifest->component_current];
                suit_param_ref_t *ref_size = &comp->param_size;

                /* Grab the total image size from the manifest */
                if ((suit_param_ref_to_cbor(manifest, ref_size, &param_size) == 0) ||
                        (nanocbor_get_uint32(&param_size, &image_size) < 0)) {
                    /* Early exit if the total image size can't be determined */
                    return -1;
                }

                if (image_size < offset + current_fw_block.len) {
                    /* Extra newline at the start to compensate for the progress bar */
                    LOG_ERROR(
                        "\n" LOG_PREFIX "Image beyond size, offset + len=%u, "
                        "image_size=%u\n", (unsigned)(total), (unsigned)image_size);
                    return -1;
                }

                if (!more && image_size != total) {
                    LOG_INFO(LOG_PREFIX "Incorrect size received, got %u, expected %u\n",
                            (unsigned)total, (unsigned)image_size);
                    return -1;
                }

                _print_download_progress(manifest, offset,
                                         current_fw_block.len, image_size);

                int res = suit_storage_write(comp->storage_backend, manifest,
                                             current_fw_block.data, offset,
                                             current_fw_block.len);

                if (!more) {
                    LOG_INFO(LOG_PREFIX "Finalizing payload store\n");
                    /* Finalize the write if no more data available */
                    res = suit_storage_finish(comp->storage_backend, manifest);
                }

                if (res != SUIT_OK) {
                    LOG_ERROR(LOG_PREFIX
                              "Error writing firmware block to storage\n");
                    return res;
                } else if (!more) {
                    return 0;
                }
                break;
            default:
                LOG_WARNING(LOG_PREFIX "warning: unhandled msg type\n");
                break;
        }
    }
}

int pub_device_status(char *topic_name, char *data, size_t len) {
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

    sock_udp_ep_t gw = { .family = AF_INET6, .port = GATEWAY_PORT };

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
    memcpy(&last_known_good_gateway, &gw, sizeof(gw));

    /* publish device status */
    char slot_active = '0' + riotboot_slot_current();
    pub_device_status(SUIT_RESOURCE_SLOT_ACTIVE, &slot_active, 1);
    char slot_inactive = '0' + riotboot_slot_other();
    pub_device_status(SUIT_RESOURCE_SLOT_INACTIVE, &slot_inactive, 1);
    char version[10];
    sprintf(version, "%10d",
            (unsigned)riotboot_slot_get_hdr(riotboot_slot_current())->version);
    pub_device_status(SUIT_RESOURCE_VERSION, version, strlen(version));

    return 0;
}

int cmd_sub(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <topic name>\n", argv[0]);
        return 1;
    }

    return sub(argv[1], sizeof(argv[1]), &on_pub_trigger, &on_reg);
}

static void *_suit_mqtt_sn_thread(void *arg)
{
    (void)arg;

    LOG_INFO(LOG_PREFIX "started.\n");
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    _suit_mqtt_sn_pid = thread_getpid();

    msg_t m;
    while (true) {
        msg_receive(&m);
        DEBUG(LOG_PREFIX "got msg with type %" PRIu32 "\n", m.content.value);
        switch (m.type) {
            case SUIT_MSG_TRIGGER:
                LOG_INFO(LOG_PREFIX "trigger received\n");
                _suit_handle_topic(_manifest_topic,
                                   &suit_mqtt_sn_on_pub_manifest,
                                   &on_reg);
                break;
            case SUIT_MSG_MANIFEST:
                if ((uint16_t) m.content.value == num_manifest_blocks - 1) {
#ifdef MODULE_SUIT
                    suit_manifest_t manifest;
                    memset(&manifest, 0, sizeof(manifest));

                    manifest.urlbuf = _manifest_topic;
                    manifest.urlbuf_len = SUIT_TOPIC_MAX;

                    int res;
                    if ((res = suit_parse(&manifest, _manifest_buf,
                                          (num_manifest_blocks - 1)
                                          * CONFIG_SUIT_MQTT_SN_BLOCKSIZE
                                          + current_manifest_block.len))
                         != SUIT_OK) {
                        LOG_INFO(LOG_PREFIX
                                 "suit_parse() failed. res=%i\n", res);
                        break;
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
                break;
            default:
                LOG_WARNING(LOG_PREFIX "warning: unhandled msg type\n");
        }
    }
    return NULL;
}

void suit_mqtt_sn_run(void)
{
    /* initialize buffers */
    memset(subscriptions, 0, (SUB_MAXNUM * sizeof(emcute_sub_t)));
    memset(topics, 0, (TOPIC_MAXNUM * TOPIC_MAXLEN * sizeof(char)));

    /* start thread responsible for handling incoming update triggers */
    thread_create(_stack, SUIT_MQTT_SN_STACKSIZE,
                  SUIT_MQTT_SN_PRIO,
                  THREAD_CREATE_STACKTEST,
                  _suit_mqtt_sn_thread, NULL, "suit_mqtt_sn");
}
