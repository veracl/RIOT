/*
 * Copyright (C) 2019 Kaspar Schleiser <kaspar@schleiser.de>
 *               2020 Vera Clemens <mail@veraclemens.org>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     examples
 * @{
 *
 * @file
 * @brief       SUIT updates over CoAP example server application (using nanocoap
 *              or emcute)
 *
 * @author      Kaspar Schleiser <kaspar@schleiser.de>
 * @author      Vera Clemens <mail@veraclemens.org>
 * @}
 */

#include <stdio.h>

#include "thread.h"
#include "irq.h"
#include "xtimer.h"

#include "shell.h"

#include "riotboot/slot.h"

#ifdef MODULE_SUIT_TRANSPORT_COAP
#include "suit/transport/coap.h"
#include "net/nanocoap_sock.h"
#endif

#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
#include "suit/transport/mqtt_sn.h"
#include "net/emcute.h"

#include "net/ipv6/addr.h"
#include "net/gnrc.h"
#include "net/gnrc/netif.h"
#endif

#ifdef MODULE_PERIPH_GPIO
#include "periph/gpio.h"
#endif

#define MAIN_QUEUE_SIZE     (8)
static msg_t _main_msg_queue[MAIN_QUEUE_SIZE];

#ifdef MODULE_SUIT_TRANSPORT_COAP
#define COAP_INBUF_SIZE (256U)

/* Extend stacksize of nanocoap server thread */
static char _nanocoap_server_stack[THREAD_STACKSIZE_DEFAULT + THREAD_EXTRA_STACKSIZE_PRINTF];
#define NANOCOAP_SERVER_QUEUE_SIZE     (8)
static msg_t _nanocoap_server_msg_queue[NANOCOAP_SERVER_QUEUE_SIZE];
#endif

#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
static char emcute_stack[THREAD_STACKSIZE_DEFAULT];
#endif

#ifdef MODULE_SUIT_TRANSPORT_COAP
static void *_nanocoap_server_thread(void *arg)
{
    (void)arg;

    /* nanocoap_server uses gnrc sock which uses gnrc which needs a msg queue */
    msg_init_queue(_nanocoap_server_msg_queue, NANOCOAP_SERVER_QUEUE_SIZE);

    /* initialize nanocoap server instance */
    uint8_t buf[COAP_INBUF_SIZE];
    sock_udp_ep_t local = { .port=COAP_PORT, .family=AF_INET6 };
    nanocoap_server(&local, buf, sizeof(buf));

    return NULL;
}
#endif

#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
static void *emcute_thread(void *arg)
{
    (void)arg;
    emcute_run(CONFIG_EMCUTE_DEFAULT_PORT, SUIT_ID);
    return NULL;    /* should never be reached */
}
#endif

/* assuming that first button is always BTN0 */
#if defined(MODULE_PERIPH_GPIO_IRQ) && defined(BTN0_PIN)
static void cb(void *arg)
{
    (void) arg;
    printf("Button pressed! Triggering suit update! \n");

#ifdef MODULE_SUIT_TRANSPORT_COAP
    suit_coap_trigger((uint8_t *) SUIT_MANIFEST_RESOURCE, sizeof(SUIT_MANIFEST_RESOURCE));
#endif

#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
    suit_mqtt_sn_trigger(SUIT_MANIFEST_RESOURCE, sizeof(SUIT_MANIFEST_RESOURCE));
#endif
}
#endif

static int cmd_print_riotboot_hdr(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int current_slot = riotboot_slot_current();
    if (current_slot != -1) {
        /* Sometimes, udhcp output messes up the following printfs.  That
         * confuses the test script. As a workaround, just disable interrupts
         * for a while.
         */
        unsigned state = irq_disable();
        riotboot_slot_print_hdr(current_slot);
        irq_restore(state);
    }
    else {
        printf("[FAILED] You're not running riotboot\n");
    }
    return 0;
}

static int cmd_print_current_slot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Sometimes, udhcp output messes up the following printfs.  That
     * confuses the test script. As a workaround, just disable interrupts
     * for a while.
     */
    unsigned state = irq_disable();
    printf("Running from slot %d\n", riotboot_slot_current());
    irq_restore(state);
    return 0;
}

static const shell_command_t shell_commands[] = {
    { "current-slot", "Print current slot number", cmd_print_current_slot },
    { "riotboot-hdr", "Print current slot header", cmd_print_riotboot_hdr },
#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
    { "con", "connect to MQTT-SN gateway and publish device status", cmd_con },
    { "sub", "subscribe to MQTT-SN topic", cmd_sub },
#endif
    { NULL, NULL, NULL }
};


int main(void)
{
    puts("RIOT SUIT update example application");

#if defined(MODULE_PERIPH_GPIO_IRQ) && defined(BTN0_PIN)
    /* initialize a button to manually trigger an update */
    gpio_init_int(BTN0_PIN, BTN0_MODE, GPIO_FALLING, cb, NULL);
#endif

    cmd_print_current_slot(0, NULL);
    cmd_print_riotboot_hdr(0, NULL);

#ifdef MODULE_SUIT_TRANSPORT_COAP
    puts("Using CoAP transport");
    /* start suit coap updater thread */
    suit_coap_run();

    /* start nanocoap server thread */
    thread_create(_nanocoap_server_stack, sizeof(_nanocoap_server_stack),
                  THREAD_PRIORITY_MAIN - 1,
                  THREAD_CREATE_STACKTEST,
                  _nanocoap_server_thread, NULL, "nanocoap server");
#endif

#ifdef MODULE_SUIT_TRANSPORT_MQTT_SN
    puts("Using MQTT-SN transport");

    /* start the emcute thread */
    thread_create(emcute_stack, sizeof(emcute_stack),
                  THREAD_PRIORITY_MAIN - 1,
                  THREAD_CREATE_STACKTEST,
                  emcute_thread, NULL, "emcute");

    suit_mqtt_sn_run();
#endif

    /* the shell contains commands that receive packets via GNRC and thus
       needs a msg queue */
    msg_init_queue(_main_msg_queue, MAIN_QUEUE_SIZE);

    puts("Starting the shell");
    char line_buf[SHELL_DEFAULT_BUFSIZE];
    shell_run(shell_commands, line_buf, SHELL_DEFAULT_BUFSIZE);

    return 0;
}
