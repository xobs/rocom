
#include <sys/param.h>

#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_flash_partitions.h>
#include <esp_image_format.h>
#include <esp_err.h>

static const char *TAG = "ota-http";
const static char http_content_type_json[] = "application/json";
static size_t total;
static size_t remaining;

esp_err_t cgi_flash_init(httpd_req_t *req)
{
	const char msg[] = "rocom.bin";
	httpd_resp_send(req, msg, sizeof(msg) - 1);
	return ESP_OK;
}

esp_err_t cgi_flash_progress(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	char buff[64];
	int len = snprintf(buff, sizeof(buff), "{\"total\": %u, \"remaining\": %u}", total, remaining);
	httpd_resp_send(req, buff, len);
	return ESP_OK;
}

esp_err_t cgi_flash_status(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	char buff[256];

	const esp_partition_t *current_partition = esp_ota_get_running_partition();
	const esp_partition_t *next_partition = NULL;
	if (current_partition != NULL) {
		next_partition = esp_ota_get_next_update_partition(current_partition);
	}
	esp_ota_img_states_t current_partition_state = ESP_OTA_IMG_UNDEFINED;
	esp_ota_img_states_t next_partition_state = ESP_OTA_IMG_UNDEFINED;
	uint32_t next_partition_address = 0;

	esp_ota_get_state_partition(current_partition, &current_partition_state);
	esp_ota_get_state_partition(next_partition, &next_partition_state);

	if (next_partition != NULL) {
		next_partition_address = next_partition->address;
	}
	const char *update_status = "valid";
	if (next_partition_state != ESP_OTA_IMG_VALID) {
		update_status = "fail";
	}

	int len = snprintf(buff, sizeof(buff),
		"{"
		"\"ota\":{"
		"\"current\":{"
		"\"address\":%" PRId32 ","
		"\"state\":%d"
		"},"
		"\"next\":{"
		"\"address\":%" PRId32 ","
		"\"state\":%d"
		"},"
		"\"status\":\"%s\""
		"}}",
		current_partition->address, current_partition_state, next_partition_address, next_partition_state,
		update_status);
	httpd_resp_send(req, buff, len);
	return ESP_OK;
}

esp_err_t cgi_flash_upload(httpd_req_t *req)
{
	const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
	esp_ota_handle_t update_handle;
	esp_err_t err;

	ESP_LOGI(TAG, "beginning update of %d bytes", req->content_len);
	httpd_resp_set_type(req, http_content_type_json);

	err = esp_ota_begin(update_part, req->content_len > 0 ? req->content_len : OTA_SIZE_UNKNOWN, &update_handle);
	if (err != ESP_OK) {
		const char msg[] = "{\"error\": \"Failed to begin the update\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
		return ESP_OK;
	}
	ESP_LOGI(TAG, "esp_ota_begin succeeded");
	httpd_resp_set_hdr(req, "Connection", "close");

	remaining = req->content_len;
	total = remaining;

	while (remaining > 0) {
		char buff[256];
		int received;
		if ((received = httpd_req_recv(req, buff, MIN(remaining, sizeof(buff)))) <= 0) {
			if (received == HTTPD_SOCK_ERR_TIMEOUT) {
				/* Retry if timeout occurred */
				continue;
			}

			/* In case of unrecoverable error,
             * close and delete the unfinished file*/
			esp_ota_abort(update_handle);

			ESP_LOGE(TAG, "File reception failed!");
			const char msg[] = "{\"error\": \"Failed to receive file\"}";
			httpd_resp_send(req, msg, sizeof(msg) - 1);
			return ESP_OK;
		}

		err = esp_ota_write(update_handle, buff, received);
		if (err != ESP_OK) {
			const char msg[] = "{\"error\": \"Failed to write OTA\"}";
			httpd_resp_send(req, msg, sizeof(msg) - 1);
			ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
			esp_ota_abort(update_handle);
			return ESP_OK;
		}

		/* Keep track of remaining size of
         * the file left to be uploaded */
		remaining -= received;
	}

	err = esp_ota_end(update_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end failed! err=0x%x", err);
		const char msg[] = "{\"error\": \"Failed to complete OTA\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		return ESP_OK;
	}

	esp_ota_set_boot_partition(update_part);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
		const char msg[] = "{\"error\": \"Failed to set new boot partition\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		return ESP_OK;
	}

	ESP_LOGI(TAG, "esp ota succeeded");

	const char response[] = "{\"success\": true}";
	httpd_resp_send(req, response, sizeof(response) - 1);

	return ESP_OK;
}

esp_err_t cgi_flash_reboot(httpd_req_t *req)
{
	httpd_resp_sendstr(req, "Initiating reboot...");
	ESP_LOGI(TAG, "preparing to reboot in 500ms...");
	vTaskDelay(pdMS_TO_TICKS(500));
	ESP_LOGI(TAG, "Initiating a reboot...");
	esp_restart();
	return ESP_OK;
}
