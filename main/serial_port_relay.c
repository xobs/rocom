#include <esp_err.h>
#include <esp_log.h>
#include <string.h>
#include <usb/cdc_acm_host.h>
#include <usb/usb_host.h>

#include "rfc2217_server.h"

// #define EXAMPLE_USB_DEVICE_VID 0x303a
// #define EXAMPLE_USB_DEVICE_PID 0x1001

static const char *TAG = "USB-CDC";

#ifndef CONFIG_UART_TCP_PORT
#define CONFIG_UART_TCP_PORT 7243
#endif

static bool s_client_connected;
static SemaphoreHandle_t s_device_disconnected_sem;
static rfc2217_server_t s_server;
static bool s_dtr;
static bool s_rts;
static cdc_acm_dev_hdl_t cdc_dev = NULL;

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
        ESP_LOGI(TAG, "Device disconnected");
        ESP_ERROR_CHECK(cdc_acm_host_close(event->data.cdc_hdl));
        xSemaphoreGive(s_device_disconnected_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state change 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        ESP_LOGW(TAG, "Unsupported CDC event: %i", event->type);
        break;
    }
}

static rfc2217_control_t on_control(void *ctx, rfc2217_control_t requested_control)
{
    if (requested_control == RFC2217_CONTROL_SET_DTR)
    {
        s_dtr = true;
    }
    else if (requested_control == RFC2217_CONTROL_CLEAR_DTR)
    {
        s_dtr = false;
    }
    else if (requested_control == RFC2217_CONTROL_SET_RTS)
    {
        s_rts = true;
    }
    else if (requested_control == RFC2217_CONTROL_CLEAR_RTS)
    {
        s_rts = false;
    }
    else
    {
        return requested_control;
    }
    if (cdc_dev != NULL)
    {
        cdc_acm_host_set_control_line_state(cdc_dev, s_dtr, s_rts);
    }
    return requested_control;
}

static unsigned on_baudrate(void *ctx, unsigned baudrate)
{
    if (cdc_dev == NULL)
    {
        return baudrate;
    }
    ESP_LOGI(TAG, "Setting baudrate: %d", baudrate);
    cdc_acm_line_coding_t line_coding;
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
    line_coding.dwDTERate = baudrate;
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(cdc_dev, &line_coding));
    return baudrate;
}

static void on_connected(void *ctx)
{
    ESP_LOGI(TAG, "RFC2217 client connected");
    s_client_connected = true;
}

static void on_disconnected(void *ctx)
{
    ESP_LOGI(TAG, "RFC2217 client disconnected");
    s_client_connected = false;
}

static void on_data_received_from_rfc2217(void *ctx, const uint8_t *data, size_t len)
{
    if (cdc_dev != NULL)
    {
        ESP_LOGI(TAG, "%d bytes received from TCP -- forwarding to device", len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_INFO);
        esp_err_t ret = cdc_acm_host_data_tx_blocking(cdc_dev, data, len, 1000);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Error sending data to USB: %d (%s)", ret, esp_err_to_name(ret));
        }
    }
}

static bool on_data_received_from_usb(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "%d bytes received from device -- forwarding to TCP", data_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    if (!s_client_connected)
    {
        return true;
    }
    rfc2217_server_send_data(s_server, data, data_len);

    return true;
}

void serial_port_relay(void)
{
    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    s_device_disconnected_sem = xSemaphoreCreateBinary();

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 1024,
        .in_buffer_size = 1024,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = on_data_received_from_usb,
    };

    rfc2217_server_config_t rfc2217_config = {
        .ctx = NULL,
        .on_client_connected = on_connected,
        .on_client_disconnected = on_disconnected,
        .on_baudrate = on_baudrate,
        .on_control = on_control,
        .on_purge = NULL,
        .on_data_received = on_data_received_from_rfc2217,
        .port = CONFIG_UART_TCP_PORT,
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_core_id = 0,
    };

    ESP_ERROR_CHECK(rfc2217_server_create(&rfc2217_config, &s_server));

    ESP_LOGI(TAG, "Starting RFC2217 server on port %u", rfc2217_config.port);

    ESP_ERROR_CHECK(rfc2217_server_start(s_server));

    while (true)
    {

        // Open USB device from tusb_serial_device example example. Either single or dual port configuration.
        ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X...", CDC_HOST_ANY_VID, CDC_HOST_ANY_PID);
        esp_err_t err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &dev_config, &cdc_dev);
        if (ESP_OK != err)
        {
            ESP_LOGI(TAG, "Failed to open device");
            continue;
        }
        // cdc_acm_host_desc_print(cdc_dev);

        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(cdc_dev, true, false));
        xSemaphoreTake(s_device_disconnected_sem, portMAX_DELAY);

        // bool cdc_device_active = true;
        // while (cdc_device_active)
        // {
        //     tcp_uart_await_connection();
        //     while ((bytes_read = tcp_uart_rx(rx_buffer, sizeof(rx_buffer))) > 0)
        //     {
        //         ESP_LOGI(TAG, "%d bytes received from TCP -- forwarding to device", bytes_read);
        //         esp_err_t ret = handle_telnet(cdc_dev, rx_buffer, bytes_read);
        //         if (ret != ESP_OK)
        //         {
        //             ESP_LOGE(TAG, "Error received from CDC device: %s", esp_err_to_name(ret));
        //             cdc_device_active = false;
        //             break;
        //         }
        //     }
        //     ESP_LOGI(TAG, "Read %d bytes", bytes_read);
        // }

        // cdc_acm_host_close(cdc_dev);
    }
}
