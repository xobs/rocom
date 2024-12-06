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
#include <usb/vcp_ch34x.hpp>
#include <usb/vcp_cp210x.hpp>
#include <usb/vcp_ftdi.hpp>
#include <usb/vcp.hpp>
#include <usb/usb_host.h>
#include <esp_private/usb_phy.h>

#include <soc/usb_pins.h>

#include "wilma.h"

static const char *TAG = "rocom";
using namespace esp_usb;

// Change these values to match your needs
#define EXAMPLE_BAUDRATE (115200)
#define EXAMPLE_STOP_BITS (0) // 0: 1 stopbit, 1: 1.5 stopbits, 2: 2 stopbits
#define EXAMPLE_PARITY (0)    // 0: None, 1: Odd, 2: Even, 3: Mark, 4: Space
#define EXAMPLE_DATA_BITS (8)

namespace
{
    static SemaphoreHandle_t device_disconnected_sem;

    /**
     * @brief Data received callback
     *
     * Just pass received data to stdout
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
        printf("%.*s", data_len, data);
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
            ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "Device suddenly disconnected");
            xSemaphoreGive(device_disconnected_sem);
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
            break;
        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
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

/**
 * @brief Main application
 *
 * Here we open a USB CDC device and send some data to it
 */
extern "C" void app_main(void)
{
    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    wilma_start();

    install_external_phy();

    // Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = true,
        .root_port_unpowered = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    assert(task_created == pdTRUE);

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    // Register VCP drivers to VCP service
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();

    // Do everything else in a loop, so we can demonstrate USB device reconnections
    while (true)
    {
        const cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 5000, // 5 seconds, enough time to plug the device in or experiment with timeout
            .out_buffer_size = 512,
            .in_buffer_size = 512,
            .event_cb = handle_event,
            .data_cb = handle_rx,
            .user_arg = NULL,
        };

        // You don't need to know the device's VID and PID. Just plug in any device and the VCP service will load correct (already registered) driver for the device
        ESP_LOGI(TAG, "Opening any VCP device...");
        auto vcp = std::unique_ptr<CdcAcmDevice>(VCP::open(&dev_config));

        if (vcp == nullptr)
        {
            ESP_LOGI(TAG, "Failed to open VCP device");
            continue;
        }
        vTaskDelay(10);

        ESP_LOGI(TAG, "Setting up line coding");
        cdc_acm_line_coding_t line_coding = {
            .dwDTERate = EXAMPLE_BAUDRATE,
            .bCharFormat = EXAMPLE_STOP_BITS,
            .bParityType = EXAMPLE_PARITY,
            .bDataBits = EXAMPLE_DATA_BITS,
        };
        ESP_ERROR_CHECK(vcp->line_coding_set(&line_coding));

        /*
        Now the USB-to-UART converter is configured and receiving data.
        You can use standard CDC-ACM API to interact with it. E.g.

        ESP_ERROR_CHECK(vcp->set_control_line_state(false, true));
        ESP_ERROR_CHECK(vcp->tx_blocking((uint8_t *)"Test string", 12));
        */

        // Send some dummy data
        ESP_LOGI(TAG, "Sending data through CdcAcmDevice");
        uint8_t data[] = "test_string";
        ESP_ERROR_CHECK(vcp->tx_blocking(data, sizeof(data)));
        ESP_ERROR_CHECK(vcp->set_control_line_state(true, true));

        // We are done. Wait for device disconnection and start over
        ESP_LOGI(TAG, "Done. You can reconnect the VCP device to run again.");
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
