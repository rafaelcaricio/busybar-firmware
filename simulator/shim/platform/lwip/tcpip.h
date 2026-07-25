/**
 * Stand-in for lwIP's tcpip.h.
 *
 * api_root.c reaches for LOCK_TCPIP_CORE around the netif walk that answers
 * /api/transport. There is no lwIP core to lock here -- the interface list
 * comes from getifaddrs() in src/web_api_host.c -- so the macros expand to
 * nothing.
 */
#pragma once

#define LOCK_TCPIP_CORE()   ((void)0)
#define UNLOCK_TCPIP_CORE() ((void)0)
