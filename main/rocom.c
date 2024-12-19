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
#include <esp_private/usb_phy.h>

#include <soc/gpio_sig_map.h>
#include <soc/usb_pins.h>

#include "wilma.h"

static const char *TAG = "rocom";

#define HOST_LIB_TASK_PRIORITY 2
#define CLASS_TASK_PRIORITY 3

extern void class_driver_task(void *arg);
extern void class_driver_client_deregister(void);

#define TARGET_BOOT_NUM GPIO_NUM_5
#define TARGET_RESET_NUM GPIO_NUM_8

#define USB_DM_PD_NUM GPIO_NUM_17
#define USB_DP_PD_NUM GPIO_NUM_18

#define USB_DM_PU_NUM GPIO_NUM_14
#define USB_DP_PU_NUM GPIO_NUM_21

#define USB_PHY_VP_NUM USB_DP_PU_NUM  // Work around the fact that USB_JTAG_VP isn't connected
#define USB_PHY_VM_NUM USB_DM_PU_NUM  // Work around the fact that USB_JTAG_VM isn't connected
// #define USB_PHY_RCV_NUM GPIO_NUM_0 // Copy VP to RCV
#define USB_PHY_RCV_NUM USB_DP_PU_NUM // Copy VP to RCV
#define USB_PHY_OEN_NUM GPIO_NUM_40
#define USB_PHY_VPO_NUM GPIO_NUM_39
#define USB_PHY_VMO_NUM GPIO_NUM_38

static SemaphoreHandle_t device_disconnected_sem;

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_host_lib_task(void *arg)
{

    // Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = true,
        .root_port_unpowered = true,
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
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        ESP_LOGI(TAG, "UAB event flags: 0x%08"PRIx32, event_flags);
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

static usb_phy_handle_t install_external_phy(void)
{
    usb_phy_handle_t phy_handle;

    // Drive 0 out D+ in order to override the 1.5k pullup
    // and force a reset, since we're changing which PHY
    // is driving the bus and the host may not realize that
    // the part needs re-enumeration.
    {
        // zero-initialize the config structure.
        const gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = ((1ULL << USB_PHY_OEN_NUM) | (1ULL << USB_PHY_VPO_NUM)),
            .pull_down_en = 0,
            .pull_up_en = 0,
        };
        // configure GPIO with the given settings
        gpio_config(&io_conf);
        // Drive the VPO line low, which will coutneract the 1.5k pullup
        // (which we cannot otherwise disable).
        gpio_set_level(USB_PHY_VPO_NUM, 0);
        // Enable the buffers to drive the value out to override the 1.5k pullup
        gpio_set_level(USB_PHY_OEN_NUM, 0);

        // USB resets require 5ms minimum
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // External PHY IOs config
    const usb_phy_ext_io_conf_t ext_io_conf = {
        .vp_io_num = USB_PHY_VP_NUM,
        .vm_io_num = USB_PHY_VM_NUM,
        .rcv_io_num = USB_PHY_RCV_NUM,
        .oen_io_num = USB_PHY_OEN_NUM,
        .vpo_io_num = USB_PHY_VPO_NUM,
        .vmo_io_num = USB_PHY_VMO_NUM,
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

    // Enable 15k pull-downs on USB lines
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << USB_DM_PD_NUM) | (1ULL << USB_DP_PD_NUM);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(USB_DM_PD_NUM, 0);
    gpio_set_level(USB_DP_PD_NUM, 0);

    return phy_handle;
}

// static void tristate_usb(void)
// {
//     esp_rom_gpio_connect_out_signal(USB_PHY_OEN_NUM, SIG_GPIO_OUT_IDX, false, false);
//     gpio_set_level((gpio_num_t)USB_PHY_OEN_NUM, 1);
// }

// static void drive_usb(void)
// {
//     esp_rom_gpio_pad_select_gpio(USB_PHY_OEN_NUM);
//     esp_rom_gpio_connect_out_signal(USB_PHY_OEN_NUM, USB_EXTPHY_OEN_IDX, false, false);
//     esp_rom_gpio_pad_unhold(USB_PHY_OEN_NUM);
// }

// static void reset_target(void)
// {
//     gpio_set_level(TARGET_RESET_NUM, 0);
//     vTaskDelay(pdMS_TO_TICKS(1000));
//     gpio_set_level(TARGET_RESET_NUM, 1);
// }

// static void configure_reset_boot(void)
// {
//     gpio_config_t io_conf = {};
//     io_conf.intr_type = GPIO_INTR_DISABLE;
//     io_conf.mode = GPIO_MODE_INPUT;
//     io_conf.pin_bit_mask = (1ULL << TARGET_BOOT_NUM) | (1ULL << TARGET_RESET_NUM);
//     io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
//     io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
//     gpio_config(&io_conf);
// }

/**
 * @brief Main application
 *
 * Here we open a USB CDC device and send some data to it
 */
void app_main(void)
{
    TaskHandle_t host_lib_task_hdl, class_driver_task_hdl;

    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    // wilma_start();

    usb_phy_handle_t usb_phy = install_external_phy();

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

    // Create class driver task
    task_created = xTaskCreatePinnedToCore(class_driver_task,
                                           "class",
                                           5 * 1024,
                                           NULL,
                                           CLASS_TASK_PRIORITY,
                                           &class_driver_task_hdl,
                                           0);
    assert(task_created == pdTRUE);
    // Add a short delay to let the tasks run
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ESP_LOGI(TAG, "Resetting target");
    // configure_reset_boot();
    // reset_target();
    // tristate_usb();
    usb_phy_action(usb_phy, USB_PHY_ACTION_HOST_FORCE_DISCONN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    usb_phy_action(usb_phy, USB_PHY_ACTION_HOST_ALLOW_CONN);

    ESP_LOGI(TAG, "Waiting for device to be connected");
    while (true)
    {
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
