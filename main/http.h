/*
 * http.h
 *
 */

#ifndef ROCOM_HTTP_H_
#define ROCOM_HTTP_H_

#include <esp_http_server.h>

/* start the http server */
httpd_handle_t webserver_start(void);

#endif /* ROCOM_HTTP_H_ */
