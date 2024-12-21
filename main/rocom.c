/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <usb/usb_host.h>

#include "kacha.h"
#include "wilma.h"

static const char *TAG = "rocom";

#define HOST_LIB_TASK_PRIORITY 9

extern void serial_port_relay(void);

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_host_lib_task(void *arg)
{
    configure_external_phy();

    const usb_host_config_t host_config = {
        .skip_phy_setup = true,
        // It's unclear why this must be `false`, but the target doesn't
        // enumerate if it's `true`.
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#ifdef ENABLE_ENUM_FILTER_CALLBACK
        .enum_filter_cb = set_config_cb,
#else
        .enum_filter_cb = NULL,
#endif // ENABLE_ENUM_FILTER_CALLBACK
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Signalize the app_main, the USB host library has been installed
    xTaskNotifyGive(arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients)
    {
        uint32_t event_flags;
        // ESP_ERROR_CHECK(usb_host_client_handle_events(handle, portMAX_DELAY));
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        // ESP_LOGI(TAG, "USB event flags: 0x%08" PRIx32, event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
            if (ESP_OK == usb_host_device_free_all())
            {
                ESP_LOGI(TAG, "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
                has_clients = false;
            }
            else
            {
                ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
                has_devices = true;
            }
        }
        if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
            has_clients = false;
        }
    }
    ESP_LOGI(TAG, "No more clients and devices, uninstall USB Host library");

    // Uninstall the USB Host Library
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskSuspend(NULL);
}

void _client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        ESP_LOGI("USB_HOST_CLIENT_EVENT_NEW_DEV", "client event: %d, address: %d", event_msg->event, event_msg->new_dev.address);
    }
    else
    {
        ESP_LOGI("USB_HOST_CLIENT_EVENT_DEV_GONE", "client event: %d", event_msg->event);
    }
}

void simple_wifi(void);
/**
 * @brief Main application
 *
 * Here we open a USB CDC device and send some data to it
 */
void app_main(void)
{
    TaskHandle_t host_lib_task_hdl;

    wilma_start();
    // simple_wifi();
    configure_reset_boot();
    reset_target_bootloader();

    // Create usb host lib task
    BaseType_t task_created;
    task_created = xTaskCreatePinnedToCore(usb_host_lib_task,
                                           "usb_host",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           HOST_LIB_TASK_PRIORITY,
                                           &host_lib_task_hdl,
                                           0);
    assert(task_created == pdTRUE);

    // Wait unit the USB host library is installed
    ulTaskNotifyTake(false, 1000);

    serial_port_relay();
    // tristate_usb();
}
