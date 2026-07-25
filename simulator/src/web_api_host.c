/**
 * The parts of the HTTP API's surroundings that have no simulator equivalent.
 *
 * The server itself, its routing and every handler are the firmware's own
 * sources. What they lean on that this build cannot provide is the network
 * stack: lwIP, the mDNS responder and the load estimator that decides whether
 * the stack is too busy to accept another connection. Those are answered here.
 */
#include "web_api_host.h"

#include <furi.h>

#include <intercom/intercom.h>
#include <netstat/netstat.h>
#include <network/network.h>
#include <sysctl/sysctl.h>

#include <web_server_i.h>
#include <http_api/http_api.h>

#include <stdio.h>

#define TAG "WebApiHost"

static struct {
    char listen_url[64];
    int network_instance;
    int accesslog_level;
} web_api_host;

void web_api_host_init(uint16_t port) {
    snprintf(web_api_host.listen_url, sizeof(web_api_host.listen_url), "http://0.0.0.0:%u", port);

    /* Sets mongoose's log level. On the device the network startup calls this;
     * there is no network service here, and without it mongoose defaults to
     * logging every socket read and write. */
    mg_init_early();

    furi_record_create(RECORD_NETWORK, &web_api_host.network_instance);

    FURI_LOG_I(TAG, "http api on %s", web_api_host.listen_url);
}

const char* web_srv_host_listen_url(void) {
    return web_api_host.listen_url;
}

/* --- Network -------------------------------------------------------------
 *
 * mongoose talks to the host's sockets directly, so nothing registers a netif.
 * api_root.c asks which interface a request arrived on to report the transport
 * as "wifi" or "usb"; with no interfaces it always answers "usb", which is
 * what a cable-connected device reports and the closer of the two to loopback.
 */

struct netif* network_find_netif(NetworkNetif netif) {
    UNUSED(netif);
    return NULL;
}

void network_netif_assign_name(struct netif* netif, NetworkNetif id) {
    UNUSED(netif);
    UNUSED(id);
}

const ip4_addr_t* netif_ip4_addr(const struct netif* netif) {
    UNUSED(netif);
    return NULL;
}

void network_init_current_thread(Network* instance) {
    UNUSED(instance);
}

void network_deinit_current_thread(Network* instance) {
    UNUSED(instance);
}

/* --- Netstat -------------------------------------------------------------
 *
 * On the device this reports whether lwIP has run out of pbufs or sockets, and
 * handlers refuse work when it has. The host's stack has no such ceiling to
 * report, so it is never overloaded.
 */

bool netstat_is_overloaded(NetstatLog log) {
    UNUSED(log);
    return false;
}

/* --- sysctl --------------------------------------------------------------
 *
 * Runtime knobs the device keeps in its settings partition. Only the access
 * log level is read on the request path; the rest of sysctl is already stubbed
 * in platform_services_host.c.
 */

int sysctl_get_websrv_accesslog_level(void) {
    return web_api_host.accesslog_level;
}

void sysctl_set_websrv_accesslog_level(int level) {
    web_api_host.accesslog_level = level;
}

/* --- intercom ------------------------------------------------------------
 *
 * The version string of the Si917 co-processor's firmware, read over the
 * inter-chip link. There is no co-processor.
 */

const char* intercom_get_version_string(void) {
    return "";
}

