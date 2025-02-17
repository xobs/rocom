/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

static const char *TAG = "simple-wifi";
static EventGroupHandle_t wifi_event_grp;

#define WIFI_CONNECTED_BIT BIT0

static void esp_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case IP_EVENT_STA_GOT_IP:
    {
        char myIP[20];
        memset(myIP, 0, 20);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        sprintf(myIP, IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGE(TAG, "got ip: %s", myIP);

        xEventGroupSetBits(wifi_event_grp, WIFI_CONNECTED_BIT);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "Event State: Disconnected from WiFi");

        if (1)
        {
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_grp, WIFI_CONNECTED_BIT);
            ESP_LOGI(TAG, "Retry connecting to the WiFi AP");
        }
        else
        {
            ESP_LOGI(TAG, "Connection to the WiFi AP failure\n");
        }
        break;

    default:
        break;
    }
}

static void wifi_init_sta(char *ssid, char *pass)
{

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid, ssid);
    if (pass)
        strcpy((char *)wifi_config.sta.password, pass);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    // esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "wifi_init_sta finished. SSID:%s password:%s",
             ssid, pass);
}

static esp_err_t wifi_init(void)
{
    wifi_event_grp = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_event_handler_instance_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, esp_event_handler, NULL, NULL);

    esp_netif_t *esp_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(esp_netif, "espressif-usbipd");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL) );
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_init_sta("Test Access Point", "Test Access Point Password");

    ESP_LOGI(TAG, "wifi_init finished.");

    return ESP_OK;
}

void simple_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init();
}
