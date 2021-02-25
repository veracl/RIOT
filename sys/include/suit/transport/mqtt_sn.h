/*
 * Copyright (C) 2020 Vera Clemens <mail@veraclemens.org>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     sys_suit
 * @defgroup    sys_suit_transport_mqtt_sn SUIT firmware MQTT-SN transport
 * @brief       SUIT secure firmware updates over MQTT-SN
 *
 * @{
 *
 * @brief       SUIT MQTT-SN helper API
 * @author      Vera Clemens <mail@veraclemens.org>
 *
 */

#ifndef SUIT_TRANSPORT_MQTT_SN_H
#define SUIT_TRANSPORT_MQTT_SN_H

#include "suit.h"
#include "net/emcute.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT-SN block-wise-transfer size used for SUIT
 */
#ifndef CONFIG_SUIT_MQTT_SN_BLOCKSIZE
#define CONFIG_SUIT_MQTT_SN_BLOCKSIZE  64
#endif

typedef struct {
  uint16_t num;
  uint8_t data[CONFIG_SUIT_MQTT_SN_BLOCKSIZE];
  size_t len;
} suit_mqtt_sn_firmware_block_t;

typedef struct {
  uint16_t num;
  size_t len;
} suit_mqtt_sn_manifest_block_t;

/**
 * @brief   MQTT-SN blockwise fetch callback descriptor
 *
 * @param[in] topic    Topic where data was received
 * @param[in] data     Pointer to received data
 * @param[in] len      Length of the received data
 *
 * @returns    0       on success
 * @returns   -1       on error
 */
typedef void (*mqtt_sn_fetch_cb_t)(const emcute_topic_t *topic, void *data,
                                   size_t len);

/**
 * @brief    Start SUIT MQTT-SN thread
 */
void suit_mqtt_sn_run(void);

/**
 * @brief    Fetches payload from the specified MQTT topic.
 *
 * This function will fetch the content of the specified MQTT topic via
 * block-wise transfer. A mqtt_sn_fetch_cb_t will be called on each received
 * block.
 *
 * @param[in]   topic      MQTT topic
 * @param[in]   callback   callback to be executed on each received block
 * @param[in]   arg        optional function arguments
 *
 * @returns     -EINVAL    if an invalid url is provided
 * @returns     -1         if failed to fetch the url content
 * @returns      0         on success
 */
int suit_mqtt_sn_fetch(const char *topic, mqtt_sn_fetch_cb_t callback,
                       void *arg);

/**
 * @brief   Trigger a SUIT update
 *
 * @param[in] topic     MQTT-SN topic(s) where the manifest has been published
 * @param[in] len       length of the topic
 */
void suit_mqtt_sn_trigger(const char *topic, size_t len);

/**
 * @brief   Handle a received firmware block
 */
void suit_mqtt_sn_on_pub_firmware(const emcute_topic_t *topic, void *data,
                                  size_t len);

void suit_mqtt_sn_run(void);

int cmd_con(int argc, char **argv);

int cmd_sub(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* SUIT_TRANSPORT_MQTT_SN_H */
/** @} */
