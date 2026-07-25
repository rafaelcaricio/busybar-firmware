/**
 * Stand-in for lwIP's netif.h.
 *
 * The simulator serves the HTTP API over the host's own sockets, so lwIP is
 * not built. Its interface type is still nameable across the network and web
 * server headers, and api_root.c compares a connection's local address against
 * an interface's to decide whether a client arrived over Wi-Fi, so the address
 * type and accessor are declared here and answered in src/web_api_host.c.
 */
#pragma once

#include <stdint.h>

struct netif;

typedef struct {
    uint32_t addr;
} ip4_addr_t;

const ip4_addr_t* netif_ip4_addr(const struct netif* netif);
