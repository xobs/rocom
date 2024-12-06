/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <usb/cdc_acm_host.h>
#include <usb/usb_host.h>
#include <esp_private/usb_phy.h>

#include <soc/usb_pins.h>

#include "wilma.h"

#define EXAMPLE_USB_HOST_PRIORITY (20)
#define EXAMPLE_USB_DEVICE_VID (0x10C4)
#define EXAMPLE_USB_DEVICE_PID (0xEA60)      // 0x303A:0x4001 (TinyUSB CDC device)
#define EXAMPLE_USB_DEVICE_DUAL_PID (0x4002) // 0x303A:0x4002 (TinyUSB Dual CDC device)
#define EXAMPLE_TX_STRING ("CDC test string!")
#define EXAMPLE_TX_TIMEOUT_MS (1000)

static const char *TAG = "rocom";
static SemaphoreHandle_t device_disconnected_sem;

/**
 * @brief Data received callback
 *
 * @param[in] data     Pointer to received data
 * @param[in] data_len Length of received data in bytes
 * @param[in] arg      Argument we passed to the device open function
 * @return
 *   true:  We have processed the received data
 *   false: We expect more data
 */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    ESP_LOGI(TAG, "Data received");
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    return true;
}

/**
 * @brief Device event callback
 *
 * Apart from handling device disconnection it doesn't do anything useful
 *
 * @param[in] event    Device event type and data
 * @param[in] user_ctx Argument we passed to the device open function
 */
static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type)
    {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %i", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device suddenly disconnected");
        ESP_ERROR_CHECK(cdc_acm_host_close(event->data.cdc_hdl));
        xSemaphoreGive(device_disconnected_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        ESP_LOGW(TAG, "Unsupported CDC event: %i", event->type);
        break;
    }
}

/**
 * @brief USB Host library handling task
 *
 * @param arg Unused
 */
static void usb_lib_task(void *arg)
{
    while (1)
    {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG, "USB: All devices freed");
            // Continue handling USB events to allow device reconnection
        }
    }
}

static usb_phy_handle_t install_external_phy(void)
{
    usb_phy_handle_t phy_handle;

    // External PHY IOs config
    const usb_phy_ext_io_conf_t ext_io_conf = {
        .vp_io_num = USBPHY_VP_NUM,
        .vm_io_num = USBPHY_VM_NUM,
        .rcv_io_num = USBPHY_RCV_NUM,
        .oen_io_num = USBPHY_OEN_NUM,
        .vpo_io_num = USBPHY_VPO_NUM,
        .vmo_io_num = USBPHY_VMO_NUM,
    };

    ESP_LOGI(TAG, "Installig USB PHY");
    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_EXT,
        .otg_mode = USB_OTG_MODE_HOST,
        // In Host mode, the speed is determined by the connected device
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = &ext_io_conf,
        .otg_io_conf = NULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_config, &phy_handle));
    return phy_handle;
}

static void rocom_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New Device");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "Device Gone");
        break;
    default:
        ESP_LOGI(TAG, "Unknown event message: %08x", event_msg->event);
        // abort();    // Should never occur in this test
        break;
    }
}


/**
 * @brief Main application
 *
 * Here we open a USB CDC device and send some data to it
 */
void app_main(void)
{
    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    wilma_start();

    usb_phy_handle_t phy_handle = install_external_phy();

    // Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), EXAMPLE_USB_HOST_PRIORITY, NULL);
    assert(task_created == pdTRUE);

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx};

    while (true) {
        cdc_acm_dev_hdl_t cdc_dev = NULL;

        // Open USB device from tusb_serial_device example example. Either single or dual port configuration.
        ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X...", EXAMPLE_USB_DEVICE_VID, EXAMPLE_USB_DEVICE_PID);
        esp_err_t err = cdc_acm_host_open(EXAMPLE_USB_DEVICE_VID, EXAMPLE_USB_DEVICE_PID, 0, &dev_config, &cdc_dev);
        if (ESP_OK != err) {
            ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X...", EXAMPLE_USB_DEVICE_VID, EXAMPLE_USB_DEVICE_DUAL_PID);
            err = cdc_acm_host_open(EXAMPLE_USB_DEVICE_VID, EXAMPLE_USB_DEVICE_DUAL_PID, 0, &dev_config, &cdc_dev);
            if (ESP_OK != err) {
                ESP_LOGI(TAG, "Failed to open device");
                continue;
            }
        }
        cdc_acm_host_desc_print(cdc_dev);
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "Setting DTR");
        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(cdc_dev, true, false));
        vTaskDelay(pdMS_TO_TICKS(100));

        // Test sending and receiving: responses are handled in handle_rx callback
        ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(cdc_dev, (const uint8_t *)EXAMPLE_TX_STRING, strlen(EXAMPLE_TX_STRING), EXAMPLE_TX_TIMEOUT_MS));
        vTaskDelay(pdMS_TO_TICKS(100));

        // Test Line Coding commands: Get current line coding, change it 9600 7N1 and read again
        ESP_LOGI(TAG, "Setting up line coding");

        cdc_acm_line_coding_t line_coding;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
        ESP_LOGI(TAG, "Line Get: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
                 line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        line_coding.dwDTERate = 9600;
        line_coding.bDataBits = 7;
        line_coding.bParityType = 1;
        line_coding.bCharFormat = 1;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(cdc_dev, &line_coding));
        ESP_LOGI(TAG, "Line Set: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
                 line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
        ESP_LOGI(TAG, "Line Get: Rate: %"PRIu32", Stop bits: %"PRIu8", Parity: %"PRIu8", Databits: %"PRIu8"",
                 line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(cdc_dev, true, false));

        // We are done. Wait for device disconnection and start over
        ESP_LOGI(TAG, "Example finished successfully! You can reconnect the device to run again.");
        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
    }

    // ESP_LOGI(TAG, "Registering client to USB Host");
    //     usb_host_client_config_t client_config = {
    //     .is_synchronous = false,
    //     .max_num_event_msg = 5,
    //     .async = {
    //         .client_event_callback = rocom_client_event_cb,
    //         .callback_arg = NULL,
    //     },
    // };
    // usb_host_client_handle_t client_hdl;
    // ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));

    // while (true)
    // {
    //     uint8_t dev_addr_list[127];
    //     int num_of_devices;
    //     usb_device_handle_t current_device_handle;
    //     usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_of_devices);
    //     ESP_LOGI(TAG, "Found %d devices", num_of_devices);

    //     for (size_t offset = 0; offset < num_of_devices; offset++)
    //     {
    //         usb_host_device_open(client_hdl, dev_addr_list[offset], &current_device_handle);
    //         const usb_device_desc_t *device_desc;
    //         usb_host_get_device_descriptor(current_device_handle, &device_desc);
    //         ESP_LOGI(TAG, "Device %02d VID: 0x%04X, PID: 0x%04X", offset, device_desc->idVendor, device_desc->idProduct);
    //         usb_host_device_close(client_hdl, current_device_handle);
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
}
