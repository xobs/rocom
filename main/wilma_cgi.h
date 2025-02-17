#ifndef WILMA_CGI_H_
#define WILMA_CGI_H_

#include <esp_http_server.h>

esp_err_t wilma_cgi_sta_scan_results_json(httpd_req_t *req);
esp_err_t wilma_cgi_ap_config_json(httpd_req_t *req);
esp_err_t wilma_cgi_sta_start_scan_json(httpd_req_t *req);
esp_err_t wilma_cgi_sta_connect_json(httpd_req_t *req);
esp_err_t wilma_cgi_sta_status_json(httpd_req_t *req);
esp_err_t wilma_cgi_ap_configure(httpd_req_t *req);

#endif /* WILMA_CGI_H_ */
