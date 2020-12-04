#!/usr/bin/env python3

#
# Copyright (C) 2020 Vera Clemens <mail@veraclemens.org>
#
# This file is subject to the terms and conditions of the GNU Lesser
# General Public License v2.1. See the file LICENSE in the top level
# directory for more details.
#

import argparse
import math
import os

import paho.mqtt.client as mqtt


def parse_arguments():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog='Tool for block-wise publishing of a file to an MQTT topic.')
    parser.add_argument('--file', '-f', required=True,
                        help='Path of file to publish')
    parser.add_argument('--block-size', '-s', type=int, default=64,
                        help='Block size')
    parser.add_argument('--mqtt-topic', '-t', default='suit/nrf52dk/'
                        'suit_update-riot.suit_signed.latest.bin',
                        help='MQTT topic to publish to (prefix)')
    parser.add_argument('--mqtt-host', '-b', default='localhost',
                        help='MQTT broker host name or IP address')
    parser.add_argument('--mqtt-port', '-p', type=int, default=1883,
                        help='MQTT broker port')
    parser.add_argument('--mqtt-qos', '-q', type=int, default=1,
                        choices=range(0, 3),
                        help='MQTT Quality of Service level')
    parser.add_argument('--mqtt-no-retain', '-r0', dest='mqtt_no_retain',
                        action='store_true',
                        help='Disable MQTT Retain flag')
    parser.set_defaults(mqtt_no_retain=False)
    return parser.parse_args()


def main(args):
    client = mqtt.Client(client_id="suit_publisher", userdata=args)
    client.connect(host=args.mqtt_host, port=args.mqtt_port, keepalive=60)
    client.loop_start()

    try:
        file_size = os.path.getsize(args.file)
        num_blocks = math.ceil(file_size / args.block_size)

        res = client.publish(topic=args.mqtt_topic,
                            payload=num_blocks,
                            qos=args.mqtt_qos,
                            retain=not args.mqtt_no_retain)
        res.wait_for_publish()
    except OSError:
        print("Error: File does not exist")
        client.disconnect()
        return

    with open(args.file, 'rb') as f:
        i = 0
        while True:
            read_data = f.read(args.block_size)
            if read_data == b'':
                break

            res = client.publish(topic=args.mqtt_topic + str(i),
                                 payload=read_data,
                                 qos=args.mqtt_qos,
                                 retain=not args.mqtt_no_retain)
            res.wait_for_publish()
            i += 1

    print("Published \"{:s}\"".format(args.file))
    print("       to \"{:s}/#\"".format(args.mqtt_topic))
    client.disconnect()


if __name__ == "__main__":
    _args = parse_arguments()

    # Add trailing slash
    if not _args.mqtt_topic.endswith('/'):
        _args.mqtt_topic = _args.mqtt_topic + '/'

    main(_args)
