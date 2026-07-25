/**
 * Host implementation of the discovery service.
 *
 * The device answers mDNS itself, out of lwIP's responder. Here the platform
 * already runs one -- mDNSResponder on macOS, avahi's compatibility layer on
 * Linux -- and registering with it is both less code and better behaved: it
 * handles name conflicts, interface changes and goodbye packets, which matters
 * when the "device" is a laptop that sleeps and moves between networks.
 *
 * Two services are announced, and only one of them is the firmware's:
 *
 *   _http._tcp      what web_server asks for through discovery_service_add(),
 *                   forwarded unchanged except for the port, which is the one
 *                   the simulator is really listening on rather than 80.
 *
 *   _busybar._tcp   what BusyBarDevices.discover() in busylib browses for.
 *                   This firmware does not announce it -- see the note in
 *                   simulator/README.md -- so the simulator adds it in order to
 *                   be discoverable by the client library.
 *
 * Both carry simulator=1, and the instance name says so too, so nothing found
 * on the network can be mistaken for hardware.
 */
#include "discovery_host.h"

#include <furi.h>

#include <device_name/device_name.h>
#include <discovery/discovery.h>

#include <dns_sd.h>

#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

#define TAG "DiscoveryHost"

#define BUSYLIB_SERVICE "_busybar._tcp"

/* mDNSResponder allows more, but a long instance name is unreadable in a
 * browser listing and the hostname is only there to tell two simulators
 * apart. */
#define HOST_LABEL_SIZE (32)

#define MAX_SERVICES (4)

/* Built while the service's callback feeds items, then handed to Bonjour. */
struct DiscoveryRequest {
    TXTRecordRef* txt;
};

static struct {
    uint16_t port;
    bool announce;
    int instance;

    DNSServiceRef refs[MAX_SERVICES];
    size_t count;
} discovery_host;

/** The part of the hostname before the first dot, lowercased. */
static void discovery_host_label(char* out, size_t out_size) {
    char hostname[256];

    if(gethostname(hostname, sizeof(hostname)) != 0) {
        snprintf(out, out_size, "host");
        return;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    char* dot = strchr(hostname, '.');
    if(dot) *dot = '\0';

    snprintf(out, out_size, "%s", hostname);
    for(char* c = out; *c; c++) {
        if(*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    }
}

/** Split "key=value" the way the firmware's callbacks write it. */
static void discovery_host_txt_add(TXTRecordRef* txt, const char* item) {
    const char* separator = strchr(item, '=');

    if(!separator) {
        TXTRecordSetValue(txt, item, 0, NULL);
        return;
    }

    char key[64];
    const size_t key_length = (size_t)(separator - item);
    if(key_length >= sizeof(key)) return;

    memcpy(key, item, key_length);
    key[key_length] = '\0';

    const char* value = separator + 1;
    TXTRecordSetValue(txt, key, (uint8_t)strlen(value), value);
}

/** Drain Bonjour's replies so name conflicts are resolved and reported.
 *
 * Registration reaches the responder when DNSServiceRegister() writes it, so
 * this is not what makes the service appear; it is what tells us the name we
 * ended up with when something else on the network already had it.
 */
static void discovery_host_reply(
    DNSServiceRef ref,
    DNSServiceFlags flags,
    DNSServiceErrorType error,
    const char* name,
    const char* regtype,
    const char* domain,
    void* context) {
    UNUSED(ref);
    UNUSED(flags);
    UNUSED(domain);
    UNUSED(context);

    if(error != kDNSServiceErr_NoError) {
        FURI_LOG_E(TAG, "%s registration failed (%d)", regtype, (int)error);
        return;
    }

    FURI_LOG_I(TAG, "announcing %s as \"%s\" on port %u", regtype, name, discovery_host.port);
}

static int32_t discovery_host_srv(void* context) {
    UNUSED(context);

    for(;;) {
        for(size_t i = 0; i < discovery_host.count; i++) {
            DNSServiceRef ref = discovery_host.refs[i];

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(DNSServiceRefSockFD(ref), &readable);

            /* Non-blocking: the FreeRTOS tick interrupts a blocking select
             * constantly, and there is nothing here worth waking for. */
            struct timeval immediately = {0, 0};
            if(select(DNSServiceRefSockFD(ref) + 1, &readable, NULL, NULL, &immediately) > 0) {
                DNSServiceProcessResult(ref);
            }
        }

        furi_delay_ms(250);
    }

    return 0;
}

static void discovery_host_register(
    const char* name,
    const char* regtype,
    uint16_t port,
    TXTRecordRef* txt) {
    if(discovery_host.count >= MAX_SERVICES) {
        FURI_LOG_E(TAG, "no room to announce %s", regtype);
        return;
    }

    DNSServiceRef ref = NULL;
    const DNSServiceErrorType error = DNSServiceRegister(
        &ref,
        0,
        kDNSServiceInterfaceIndexAny,
        name,
        regtype,
        NULL,
        NULL,
        htons(port),
        TXTRecordGetLength(txt),
        TXTRecordGetBytesPtr(txt),
        discovery_host_reply,
        NULL);

    if(error != kDNSServiceErr_NoError) {
        FURI_LOG_E(TAG, "cannot announce %s (%d)", regtype, (int)error);
        return;
    }

    discovery_host.refs[discovery_host.count++] = ref;
}

/** Announce the service busylib's discover() browses for.
 *
 * The name TXT item is what BusyBarDevices.discover() shows as the device
 * name; the instance name is what it reports as the temporary id.
 */
static void discovery_host_announce_busylib(const char* host_label) {
    FuriString* device_name = furi_string_alloc();
    DeviceName* names = furi_record_open(RECORD_DEVICE_NAME);
    device_name_get(names, device_name);
    furi_record_close(RECORD_DEVICE_NAME);

    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, NULL);
    discovery_host_txt_add(&txt, "path=/");
    discovery_host_txt_add(&txt, "simulator=1");

    FuriString* name_item = furi_string_alloc_printf("name=%s", furi_string_get_cstr(device_name));
    discovery_host_txt_add(&txt, furi_string_get_cstr(name_item));
    furi_string_free(name_item);

    FuriString* instance = furi_string_alloc_printf("busybar-sim-%s", host_label);
    discovery_host_register(
        furi_string_get_cstr(instance), BUSYLIB_SERVICE, discovery_host.port, &txt);

    furi_string_free(instance);
    furi_string_free(device_name);
    TXTRecordDeallocate(&txt);
}

void discovery_host_init(uint16_t port, bool announce) {
    discovery_host.port = port;
    discovery_host.announce = announce;

    furi_record_create(RECORD_DISCOVERY, &discovery_host.instance);

    if(!announce) {
        FURI_LOG_I(TAG, "mDNS announcements are off");
        return;
    }

    char host_label[HOST_LABEL_SIZE];
    discovery_host_label(host_label, sizeof(host_label));

    discovery_host_announce_busylib(host_label);

    FuriThread* thread = furi_thread_alloc_ex("discovery", 8 * 1024, discovery_host_srv, NULL);
    furi_thread_start(thread);
}

/* -- the firmware's API -------------------------------------------------- */

void discovery_service_add(Discovery* discovery, const DiscoveryInfo* info, void* context) {
    UNUSED(discovery);
    furi_check(info);

    if(!discovery_host.announce) {
        FURI_LOG_I(TAG, "not announcing %s.%s", info->name, info->service);
        return;
    }

    TXTRecordRef txt;
    TXTRecordCreate(&txt, 0, NULL);

    if(info->txt) {
        DiscoveryRequest request = {.txt = &txt};
        info->txt(&request, context);
    }
    discovery_host_txt_add(&txt, "simulator=1");

    char host_label[HOST_LABEL_SIZE];
    discovery_host_label(host_label, sizeof(host_label));

    /* The service asks for the device's port; the simulator is not on it. */
    FuriString* instance = furi_string_alloc_printf("%s-sim-%s", info->name, host_label);

    FuriString* regtype = furi_string_alloc_printf(
        "%s.%s", info->service, info->transport == DiscoveryTransportUdp ? "_udp" : "_tcp");

    discovery_host_register(
        furi_string_get_cstr(instance),
        furi_string_get_cstr(regtype),
        discovery_host.port,
        &txt);

    furi_string_free(regtype);
    furi_string_free(instance);
    TXTRecordDeallocate(&txt);
}

void discovery_request_feed_txt(DiscoveryRequest* request, const char* txt) {
    furi_check(request);
    furi_check(txt);

    discovery_host_txt_add(request->txt, txt);
}
