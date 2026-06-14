//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * ESP32forth ESPNOW v7.0.8.0
 * Revision: ffef117aea5fdb6a0efbb36320aefa271360c2c4
 */

// More documentation
// https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html

#include <esp_now.h>

static esp_err_t ESPNOW_add_peer(const uint8_t *broadcastAddress) {
  esp_now_peer_info_t peerInfo = {0};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  // use current wifi channel
  peerInfo.encrypt = false;
  return esp_now_add_peer(&peerInfo);
}

#define OPTIONAL_ESPNOW_VOCABULARY V(espnow)
#define OPTIONAL_ESPNOW_SUPPORT \
  XV(internals, "espnow-source", ESPNOW_SOURCE, \
      PUSH espnow_source; PUSH sizeof(espnow_source) - 1) \
  YV(espnow, espnow_init    , PUSH esp_now_init()) \
  YV(espnow, espnow_deinit  , PUSH esp_now_deinit()) \
  YV(espnow, espnow_add_peer, n0 = ESPNOW_add_peer(b0)) \
  YV(espnow, espnow_send    , n0 = esp_now_send(b2, b1, n0); NIPn(2)) \

const char espnow_source[] = R"""(
vocabulary espnow   espnow definitions
transfer espnow-builtins
DEFINED? ESPNOW_init [IF]
\ error codes from https://github.com/espressif/esp-idf/blob/master/components/esp_common/include/esp_err.h
0 constant ESP_OK
-1 constant ESP_FAIL
$101 constant ESP_ERR_NO_MEM              \ Out of memory
$102 constant ESP_ERR_INVALID_ARG         \ Invalid argument
$103 constant ESP_ERR_INVALID_STATE       \ Invalid state
$104 constant ESP_ERR_INVALID_SIZE        \ Invalid size
$105 constant ESP_ERR_NOT_FOUND           \ Requested resource not found
$106 constant ESP_ERR_NOT_SUPPORTED       \ Operation or feature not supported
$107 constant ESP_ERR_TIMEOUT             \ Operation timed out
$108 constant ESP_ERR_INVALID_RESPONSE    \ Received response was invalid
$109 constant ESP_ERR_INVALID_CRC         \ CRC or checksum was invalid
$10A constant ESP_ERR_INVALID_VERSION     \ Version was invalid
$10B constant ESP_ERR_INVALID_MAC         \ MAC address was invalid
$10C constant ESP_ERR_NOT_FINISHED        \ Operation has not fully completed

$3000 constant ESP_ERR_WIFI_BASE          \ Starting number of WiFi error codes
$3000 constant ESP_ERR_MESH_BASE          \ Starting number of MESH error codes
$6000 constant ESP_ERR_FLASH_BASE         \ Starting number of flash error codes
$c000 constant ESP_ERR_HW_CRYPTO_BASE     \ Starting number of HW cryptography module error codes
$d000 constant ESP_ERR_MEMPROT_BASE       \ Starting number of Memory Protection API error codes
[THEN]
forth definitions
)""";
