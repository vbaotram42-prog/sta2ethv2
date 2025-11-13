/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-FileCopyrightText: Unlicense OR CC0-1.0
 */
/* sta2eth (WiFi STA to Ethernet packet forwarding) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth_driver.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_private/wifi.h"
#include "ethernet_init.h"

static const char *TAG = "sta2eth_example";
static esp_eth_handle_t s_eth_handle = NULL;
static QueueHandle_t flow_control_queue = NULL;
static bool s_sta_is_connected = false;
static bool s_ethernet_is_connected = false;
static uint8_t s_wifi_mac[6];

#define FLOW_CONTROL_QUEUE_TIMEOUT_MS (100)
#define FLOW_CONTROL_QUEUE_LENGTH (40)
#define FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS (100)

typedef struct {
    void *packet;
    uint16_t length;
} flow_control_msg_t;

// Forward packets from Ethernet to Wi-Fi STA
static esp_err_t pkt_eth2wifi(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv)
{
    if (s_sta_is_connected) {
        if (esp_wifi_internal_tx(WIFI_IF_STA, buffer, len) != ESP_OK) {
            ESP_LOGE(TAG, "WiFi send packet failed");
        }
    }
    free(buffer);
    return ESP_OK;
}

// Forward packets from Wi-Fi to Ethernet
// Note that, Wi-Fi works slower than Ethernet on ESP32,
// so we need to add an extra queue to balance their speed difference.
static esp_err_t pkt_wifi2eth(void *buffer, uint16_t len, void *eb)
{
    esp_err_t ret = ESP_OK;
    flow_control_msg_t msg = {
        .packet = buffer,
        .length = len
    };
    if (xQueueSend(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "send flow control message failed or timeout");
        esp_wifi_internal_free_rx_buffer(eb);
        ret = ESP_FAIL;
    }
    return ret;
}

// This task will fetch the packet from the queue, and then send out through Ethernet.
// Ethernet handles packets faster than Wi-Fi, but we add flow control for consistency.
static void wifi2eth_flow_control_task(void *args)
{
    flow_control_msg_t msg;
    int res = 0;
    uint32_t timeout = 0;
    while (1) {
        if (xQueueReceive(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            timeout = 0;
            if (s_ethernet_is_connected && msg.length) {
                do {
                    vTaskDelay(pdMS_TO_TICKS(timeout));
                    timeout += 2;
                    res = esp_eth_transmit(s_eth_handle, msg.packet, msg.length);
                } while (res && timeout < FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS);
                if (res != ESP_OK) {
                    ESP_LOGE(TAG, "Ethernet send packet failed: %d", res);
                }
            }
            free(msg.packet);
        }
    }
    vTaskDelete(NULL);
}

// Event handler for Ethernet
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Up");
        s_ethernet_is_connected = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        s_ethernet_is_connected = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

// Event handler for Wi-Fi
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Wi-Fi STA started, connecting to AP...");
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi STA connected to AP (L2 Bridge Mode - No IP)");
        s_sta_is_connected = true;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, pkt_wifi2eth);
        // Get WiFi MAC and set it to Ethernet
        esp_wifi_get_mac(WIFI_IF_STA, s_wifi_mac);
        esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, s_wifi_mac);
        ESP_LOGI(TAG, "L2 Bridge established: WiFi MAC " MACSTR " set to Ethernet", MAC2STR(s_wifi_mac));
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi STA disconnected from AP");
        s_sta_is_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        ESP_LOGI(TAG, "Reconnecting to AP...");
        esp_wifi_connect();
        break;
    default:
        break;
    }
}

static void initialize_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet in L2 bridge mode (no IP stack)");
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));
    if (eth_port_cnt > 1) {
        ESP_LOGW(TAG, "multiple Ethernet devices detected, the first initialized is to be used!");
    }
    s_eth_handle = eth_handles[0];
    free(eth_handles);
    
    // Set up L2 packet forwarding - no IP processing
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth_handle, pkt_eth2wifi, NULL));
    
    // Enable promiscuous mode to receive all packets for bridging
    bool eth_promiscuous = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous));
    ESP_LOGI(TAG, "Ethernet promiscuous mode enabled for L2 bridging");
    
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}

static void initialize_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi in L2 bridge mode (no IP stack)");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    // Configure WiFi STA for L2 bridge mode
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "xinxin",
            .password = "19840226",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Note: No esp_netif is created for WiFi, ensuring pure L2 operation
    // WiFi will operate in bridge mode using esp_wifi_internal_* APIs
    ESP_LOGI(TAG, "WiFi configured for L2-only operation (slave/STA side)");
    
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t initialize_flow_control(void)
{
    flow_control_queue = xQueueCreate(FLOW_CONTROL_QUEUE_LENGTH, sizeof(flow_control_msg_t));
    if (!flow_control_queue) {
        ESP_LOGE(TAG, "create flow control queue failed");
        return ESP_FAIL;
    }
    BaseType_t ret = xTaskCreate(wifi2eth_flow_control_task, "flow_ctl", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "create flow control task failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== STA2ETH L2 Bridge Mode ===");
    ESP_LOGI(TAG, "Starting pure Layer 2 bridge: WiFi STA (slave) <-> Ethernet");
    ESP_LOGI(TAG, "No IP stack - transparent packet forwarding only");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Create event loop but do NOT initialize esp_netif (no IP stack)
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Event loop created (esp_netif NOT initialized - L2 only)");
    
    ESP_ERROR_CHECK(initialize_flow_control());
    initialize_wifi();
    initialize_ethernet();
    
    ESP_LOGI(TAG, "L2 bridge initialization complete");
}
