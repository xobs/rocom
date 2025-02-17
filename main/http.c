#include <esp_attr.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/list.h>
#include "hashmap.h"
#include <string.h>
#include <stdio.h>
// #include "ota-http.h"
// #include "websocket.h"
#include "wilma_cgi.h"
// #include "driver/uart.h"
// #include "uart.h"
// #include "version.h"

const static char http_cache_control_hdr[] = "Cache-Control";
const static char http_cache_control_no_cache[] = "no-store, no-cache, must-revalidate, max-age=0";
const static char http_pragma_hdr[] = "Pragma";
const static char http_pragma_no_cache[] = "no-cache";

httpd_handle_t http_daemon;

#define TAG "httpd"

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static int task_status_cmp(const void *a, const void *b)
{
	TaskStatus_t *ta = (TaskStatus_t *)a;
	TaskStatus_t *tb = (TaskStatus_t *)b;
	return ta->xTaskNumber - tb->xTaskNumber;
}
#endif

static const char *const task_state_name[] = {
	"eRunning",	  /* A task is querying the state of itself, so must be running. */
	"eReady",	  /* The task being queried is in a read or pending ready list. */
	"eBlocked",	  /* The task being queried is in the Blocked state. */
	"eSuspended", /* The task being queried is in the Suspended state, or is in the Blocked state with an infinite time out. */
	"eDeleted",	  /* The task being queried has been deleted, but its TCB has not yet been freed. */
	"eInvalid"	  /* Used as an 'invalid state' value. */
};

#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
static const char *core_str(int core_id)
{
	switch (core_id)
	{
	case 0:
		return "0";
	case 1:
		return "1";
	case 2147483647:
		return "ANY";
	case -1:
		return "any";
	default:
		return "???";
	}
}
#endif

static const char *ESP_RESET_REASONS[] = {
	"ESP_RST_UNKNOWN",
	"ESP_RST_POWERON",
	"ESP_RST_EXT",
	"ESP_RST_SW",
	"ESP_RST_PANIC",
	"ESP_RST_INT_WDT",
	"ESP_RST_TASK_WDT",
	"ESP_RST_WDT",
	"ESP_RST_DEEPSLEEP",
	"ESP_RST_BROWNOUT",
	"ESP_RST_SDIO",
	"ESP_RST_USB",
	"ESP_RST_JTAG",
};

static const char *ESP_RESET_DESCRIPTIONS[] = {
	"Reset reason can not be determined",
	"Reset due to power-on event",
	"Reset by external pin (not applicable for ESP32)",
	"Software reset via esp_restart",
	"Software reset due to exception/panic",
	"Reset (software or hardware) due to interrupt watchdog",
	"Reset due to task watchdog",
	"Reset due to other watchdogs",
	"Reset after exiting deep sleep mode",
	"Brownout reset (software or hardware)",
	"Reset over SDIO",
	"Reset by USB peripheral",
	"Reset by JTAG",
};

static esp_err_t cgi_system_status_header(httpd_req_t *req)
{
	char buffer[256];

	snprintf(buffer, sizeof(buffer),
			 "free_heap: %" PRIu32 "\n"
			 "uptime: %" PRIu32 "\n",
			 esp_get_free_heap_size(), xTaskGetTickCount() * portTICK_PERIOD_MS);
	httpd_resp_sendstr_chunk(req, buffer);

	snprintf(buffer, sizeof(buffer), "reset_reason: %d %s -- %s\n", esp_reset_reason(),
			 ESP_RESET_REASONS[esp_reset_reason()], ESP_RESET_DESCRIPTIONS[esp_reset_reason()]);
	httpd_resp_sendstr_chunk(req, buffer);

	const esp_partition_t *current_partition = esp_ota_get_running_partition();
	const esp_partition_t *next_partition = NULL;
	if (current_partition != NULL)
	{
		next_partition = esp_ota_get_next_update_partition(current_partition);
	}
	esp_ota_img_states_t current_partition_state = ESP_OTA_IMG_UNDEFINED;
	esp_ota_img_states_t next_partition_state = ESP_OTA_IMG_UNDEFINED;
	uint32_t next_partition_address = 0;

	esp_ota_get_state_partition(current_partition, &current_partition_state);

	esp_ota_get_state_partition(next_partition, &next_partition_state);

	if (next_partition != NULL)
	{
		next_partition_address = next_partition->address;
	}
	const char *update_status = "update valid\n";
	if (next_partition_state != ESP_OTA_IMG_VALID)
	{
		update_status = "UPDATE FAILED\n";
	}

	snprintf(buffer, sizeof(buffer),
			 "current partition: 0x%08" PRIx32 " %d\n"
			 "next partition: 0x%08" PRIx32 " %d\n"
			 "%s",
			 current_partition->address, current_partition_state, next_partition_address, next_partition_state,
			 update_status);
	httpd_resp_sendstr_chunk(req, buffer);

	httpd_resp_sendstr_chunk(req, "tasks:\n");

	return ESP_OK;
}

static esp_err_t cgi_system_status(httpd_req_t *req)
{
	TaskStatus_t *pxTaskStatusArray;
	int i;
	int uxArraySize;
	uint32_t totalRuntime;

	static hashmap *task_times;
	if (!task_times)
	{
		task_times = hashmap_new();
	}

	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_set_hdr(req, "Refresh", "1");

	cgi_system_status_header(req);

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
	uxArraySize = uxTaskGetNumberOfTasks();
	pxTaskStatusArray = malloc(uxArraySize * sizeof(TaskStatus_t));
	uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &totalRuntime);
	qsort(pxTaskStatusArray, uxArraySize, sizeof(TaskStatus_t), task_status_cmp);
#else
	pxTaskStatusArray = NULL;
	uxArraySize = 0;
	totalRuntime = 0;
	httpd_resp_sendstr_chunk(req, "    CONFIG_FREERTOS_USE_TRACE_FACILITY is disabled -- task listing unsupported\n");
#endif
	i = 0;

	uint32_t tmp;
	static uint32_t lastTotalRuntime;
	/* Generate the (binary) data. */
	tmp = totalRuntime;
	totalRuntime = totalRuntime - lastTotalRuntime;
	lastTotalRuntime = tmp;
	totalRuntime /= 100;
	if (totalRuntime == 0)
	{
		totalRuntime = 1;
	}

	for (i = 0; (pxTaskStatusArray != NULL) && (i < uxArraySize); i++)
	{
		int len;
		char buff[256];
		TaskStatus_t *tsk = &pxTaskStatusArray[i];

		uint32_t last_task_time = tsk->ulRunTimeCounter;
		hashmap_get(task_times, tsk->xTaskNumber, &last_task_time);
		hashmap_set(task_times, tsk->xTaskNumber, tsk->ulRunTimeCounter);
		tsk->ulRunTimeCounter -= last_task_time;

		len = snprintf(buff, sizeof(buff),
					   "\tid: %3u, name: %16s, prio: %3d, state: %10s, stack_hwm: %5" PRIu32 ", "
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
					   "core: %3s, "
#endif
					   "cpu: %3" PRId32 "%%, pc: 0x%08" PRIx32 "\n",
					   tsk->xTaskNumber, tsk->pcTaskName, tsk->uxCurrentPriority, task_state_name[tsk->eCurrentState],
					   tsk->usStackHighWaterMark,
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
					   core_str((int)tsk->xCoreID),
#endif
					   tsk->ulRunTimeCounter / totalRuntime, (*((uint32_t **)tsk->xHandle))[1]);
		httpd_resp_send_chunk(req, buff, len);
	}

	if (pxTaskStatusArray != NULL)
	{
		free(pxTaskStatusArray);
	}

	// ESP_LOGI(__func__, "finishing connection");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

// // Use this as a cgi function to redirect one url to another.
// static esp_err_t cgi_redirect(httpd_req_t *req)
// {
// 	httpd_resp_set_type(req, "text/html");
// 	httpd_resp_set_status(req, "302 Moved");
// 	httpd_resp_set_hdr(req, "Location", (const char *)req->user_ctx);
// 	httpd_resp_send(req, NULL, 0);
// 	return ESP_OK;
// }

static esp_err_t send_status(httpd_req_t *req) {
    extern const char status_html_start[] asm("_binary_status_html_start");
    extern const char status_html_end[]   asm("_binary_status_html_end");

	return httpd_resp_send(req, status_html_start, status_html_end - status_html_start);
}

static esp_err_t send_wifi(httpd_req_t *req) {
    extern const char wifi_html_start[] asm("_binary_wifi_html_start");
    extern const char wifi_html_end[]   asm("_binary_wifi_html_end");

	return httpd_resp_send(req, wifi_html_start, wifi_html_end - wifi_html_start);
}

static const httpd_uri_t basic_handlers[] = {
	// New API style
	{
		.uri = "/api/status",
		.method = HTTP_GET,
		.handler = cgi_system_status,
	},

	// OTA updates
	// {
	// 	.uri = "/api/flash",
	// 	.method = HTTP_GET,
	// 	.handler = cgi_redirect,
	// 	.user_ctx = (void *)"/flash/",
	// },
	// {
	// 	.uri = "/api/flash/init",
	// 	.method = HTTP_GET,
	// 	.handler = cgi_flash_init,
	// },
	// {
	// 	.uri = "/api/flash/upload",
	// 	.method = HTTP_POST,
	// 	.handler = cgi_flash_upload,
	// },
	// {
	// 	.uri = "/api/flash/reboot",
	// 	.method = HTTP_GET,
	// 	.handler = cgi_flash_reboot,
	// },
	// {
	// 	.uri = "/api/flash/progress",
	// 	.method = HTTP_GET,
	// 	.handler = cgi_flash_progress,
	// },
	// {
	// 	.uri = "/api/flash/status",
	// 	.method = HTTP_GET,
	// 	.handler = cgi_flash_status,
	// },
	// {
	// 	.uri = "/api/storage",
	// 	.method = HTTP_DELETE,
	// 	.handler = cgi_storage_delete,
	// },

	// Various status pages
	{
		.uri = "/status",
		.method = HTTP_GET,
		.handler = cgi_system_status,
	},

	// Wilma Manager
	{
		.uri = "/api/sta",
		.handler = wilma_cgi_sta_scan_results_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/api/sta/status",
		.method = HTTP_GET,
		.handler = wilma_cgi_sta_status_json,
	},
	{
		.uri = "/api/ap",
		.handler = wilma_cgi_ap_config_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/api/ap",
		.handler = wilma_cgi_ap_configure,
		.method = HTTP_POST,
	},
	{
		.uri = "/api/sta/scan",
		.handler = wilma_cgi_sta_start_scan_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/api/sta/connect",
		.method = HTTP_POST,
		.handler = wilma_cgi_sta_connect_json,
	},
	{
		.uri = "/api/sta/connect",
		.method = HTTP_DELETE,
		.handler = wilma_cgi_sta_connect_json,
	},

	{
		.uri = "/status.html",
		.method = HTTP_GET,
		.handler = send_status,
	},
	{
		.uri = "/wifi.html",
		.method = HTTP_GET,
		.handler = send_wifi,
	},
	// // Catch-all cgi function for the filesystem
	// {
	// 	.uri = "*",
	// 	.handler = cgi_frog_fs_hook,
	// 	.method = HTTP_GET,
	// },
};

static const int basic_handlers_count = sizeof(basic_handlers) / sizeof(*basic_handlers);

httpd_handle_t webserver_start(void)
{
	int i;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.max_uri_handlers = basic_handlers_count + 5;
	config.server_port = 80;
	config.uri_match_fn = httpd_uri_match_wildcard;

	/* This check should be a part of http_server */
	config.max_open_sockets = (CONFIG_LWIP_MAX_SOCKETS - 4);

	if (httpd_start(&http_daemon, &config) != ESP_OK)
	{
		ESP_LOGE(TAG, "Unable to start HTTP server");
		return NULL;
	}

	for (i = 0; i < basic_handlers_count; i++)
	{
		httpd_register_uri_handler(http_daemon, &basic_handlers[i]);
	}

	ESP_LOGI(TAG, "Started HTTP server on port: '%d'", config.server_port);
	ESP_LOGI(TAG, "Max URI handlers: '%d'", config.max_uri_handlers);
	ESP_LOGI(TAG, "Max Open Sessions: '%d'", config.max_open_sockets);
	ESP_LOGI(TAG, "Max Header Length: '%d'", HTTPD_MAX_REQ_HDR_LEN);
	ESP_LOGI(TAG, "Max URI Length: '%d'", HTTPD_MAX_URI_LEN);
	ESP_LOGI(TAG, "Max Stack Size: '%d'", config.stack_size);

	return http_daemon;
}
