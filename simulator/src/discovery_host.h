#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Start announcing the simulator on the local network over mDNS.
 *
 * @param port     the port the HTTP API is actually listening on
 * @param announce false to stay silent, as the simulator used to
 */
void discovery_host_init(uint16_t port, bool announce);
