#include <esp_log.h>
#include <mdns.h>
#include "sdkconfig.h"

#ifndef CONFIG_PRODUCT_NAME
#define CONFIG_PRODUCT_NAME CONFIG_WILMA_AP_SSID_PREFIX
#endif

static const char *TAG = CONFIG_PRODUCT_NAME "-mdns";

void wilma_unique_words(const char **name1, const char **name2);

void initialise_mdns(const char *hostname)
{
	ESP_ERROR_CHECK(mdns_init());
	if (hostname == NULL) {
		char generated_hostname[32];
		const char *name1;
		const char *name2;
		wilma_unique_words(&name1, &name2);
		snprintf(generated_hostname, sizeof(generated_hostname) - 1, CONFIG_PRODUCT_NAME "-%s-%s", name1, name2);
		ESP_ERROR_CHECK(mdns_hostname_set(generated_hostname));
		ESP_LOGI(TAG, "mdns hostname set to: [%s]", generated_hostname);
	} else {
		//set mDNS hostname (required if you want to advertise services)
		ESP_ERROR_CHECK(mdns_hostname_set(hostname));
		ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);
	}
	//set default mDNS instance name
	ESP_ERROR_CHECK(mdns_instance_name_set(CONFIG_WILMA_AP_SSID_PREFIX " Debugger"));

	//structure with TXT records
	mdns_txt_item_t serviceTxtData[3] = {{"board", CONFIG_PRODUCT_NAME}, {"u", "user"}, {"p", "password"}};

	//initialize service
	ESP_ERROR_CHECK(
		mdns_service_add(CONFIG_WILMA_AP_SSID_PREFIX "-WebServer", "_http", "_tcp", 80, serviceTxtData, 3));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(
		CONFIG_WILMA_AP_SSID_PREFIX "-WebServer", "_http", "_tcp", NULL, "_server"));
}
