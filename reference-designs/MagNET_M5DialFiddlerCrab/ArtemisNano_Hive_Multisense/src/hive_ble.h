/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>

/*
 * MagNET BLE for the Multisensor (M4).
 *
 * Advert (README 5.1): flags + HCP service UUID (as Hanasu) + manufacturer data,
 * company 0xFFFF (testing), 6-byte header:
 *   [0] version<<4 | role (0x1F)  [1..2] sensor bitmap LE  [3] battery %
 *   [4] sequence  [5] bundle tag
 * Scan response: "MagNET-xxxx" + manufacturer data = the same header + readings:
 *   [6..7] lux (u16 LE, saturating)  [8..9] CCT K (u16 LE)  [10..11] yaw deg (s16 LE)
 *   [12] stability  [13] significant-motion count (low byte)
 * (Android merges advert + scan response per company ID, so the scan response
 * repeats the header rather than carrying readings alone.)
 *
 * GATT: the Hanasu HCP service (…0000) with CMD (…0001), EVT (…0002, notify),
 * AUTH (…0003, read-encrypted). A small built-in dispatcher answers PING, HELP,
 * WHOAMI, STATUS, CAPS, SENSE, CHANNEL SHOW (none yet) and ADMIN LIST (empty)
 * until the full HCP core lands (M7). magnet_app's node screen needs the last two.
 */

/* Bring up the controller, load bonds, start advertising. */
int hive_ble_start(void);

/* Refresh advert + scan response from the latest snapshot. */
void hive_ble_update(void);

/*
 * True while a central is connected. Used by main() to slow the blue LED to a
 * heartbeat, so the board says which state it is in without a console.
 */
bool hive_ble_connected(void);
