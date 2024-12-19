#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_private/usb_phy.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_sig_map.h>
#include <soc/usb_pins.h>
#include <usb/usb_host.h>

#define USB_DM_PD 17
#define USB_DP_PD 18

#define USB_DM 14
#define USB_DP 21

#define USB_DMO 38
#define USB_DPO 39
#define USB_OEN 40

#define TARGET_BOOT_NUM 5
#define TARGET_RESET_NUM 8

static usb_phy_handle_t usb_ext_phy_hdl;

esp_err_t configure_external_phy(void)
{
    // Enable 15k pull-downs on USB lines
    {
        gpio_set_level(USB_DM_PD, 0);
        gpio_set_level(USB_DP_PD, 0);
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << USB_DM_PD) | (1ULL << USB_DP_PD);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
    }

    // Drive 0 out D+ in order to override the 1.5k pullup
    // and force a reset, since we're changing which PHY
    // is driving the bus and the host may not realize that
    // the part needs re-enumeration.
    {
        // zero-initialize the config structure.
        const gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = ((1ULL << USB_OEN) | (1ULL << USB_DPO)),
            .pull_down_en = 0,
            .pull_up_en = 0,
        };
        // configure GPIO with the given settings
        gpio_config(&io_conf);
        // Drive the VPO line low, which will coutneract the 1.5k pullup
        // (which we cannot otherwise disable).
        gpio_set_level(USB_DPO, 0);
        // Enable the buffers to drive the value out to override the 1.5k pullup
        gpio_set_level(USB_OEN, 0);

        // USB resets require 5ms minimum
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Configure external PHY pins
    const usb_phy_ext_io_conf_t ext_io_conf = {
        .vp_io_num = USB_DP,
        .vm_io_num = USB_DM,
        .rcv_io_num = USB_DP,
        .oen_io_num = USB_OEN,
        .vpo_io_num = USB_DPO,
        .vmo_io_num = USB_DMO,
    };

    const usb_phy_config_t usb_otg_phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_EXT,
        .otg_mode = USB_PHY_MODE_DEFAULT,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = &ext_io_conf,
    };

    ESP_RETURN_ON_ERROR(usb_new_phy(&usb_otg_phy_conf, &usb_ext_phy_hdl), "usbphy-ext", "Install USB EXT PHY failed");
    return ESP_OK;
}

esp_err_t free_external_phy(void)
{
    return usb_del_phy(usb_ext_phy_hdl);
    return ESP_OK;
}

static void drive_reset_boot(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << TARGET_BOOT_NUM) | (1ULL << TARGET_RESET_NUM);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

static void tristate_reset_boot(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TARGET_BOOT_NUM) | (1ULL << TARGET_RESET_NUM);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

void reset_target_run(void)
{
    drive_reset_boot();
    gpio_set_level(TARGET_BOOT_NUM, 1);
    gpio_set_level(TARGET_RESET_NUM, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TARGET_RESET_NUM, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tristate_reset_boot();
}

void reset_target_bootloader(void)
{
    drive_reset_boot();
    gpio_set_level(TARGET_RESET_NUM, 0);
    gpio_set_level(TARGET_BOOT_NUM, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TARGET_RESET_NUM, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    tristate_reset_boot();
}

void configure_reset_boot(void)
{
    gpio_set_level(TARGET_RESET_NUM, 1);
    gpio_set_level(TARGET_BOOT_NUM, 1);
    tristate_reset_boot();
}

void tristate_usb(void)
{

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << USB_DM_PD) | (1ULL << USB_DP_PD) | (1ULL << USB_DM) | (1ULL << USB_DP) | (1ULL << USB_DMO) | (1ULL << USB_DPO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << USB_OEN);
    gpio_config(&io_conf);

    esp_rom_gpio_connect_out_signal(USB_OEN, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level((gpio_num_t)USB_OEN, 1);
}

void drive_usb(void)
{
    esp_rom_gpio_pad_select_gpio(USB_OEN);
    esp_rom_gpio_connect_out_signal(USB_OEN, USB_EXTPHY_OEN_IDX, false, false);
    esp_rom_gpio_pad_unhold(USB_OEN);
}
