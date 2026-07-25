#pragma once

#include <stdint.h>

/** Register the records the web server opens that have no simulator service
 * behind them. Call after furi_init() and before the server thread starts. */
void web_api_host_init(uint16_t port);

/** Listen address for the patched web_srv_start(), as a mongoose URL. */
const char* web_srv_host_listen_url(void);
