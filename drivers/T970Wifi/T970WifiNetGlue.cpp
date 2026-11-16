// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM-T970: starts PongWifi's network side (drivers/PongWifi/PongWifiNet.cpp: the en0 interface, the registry entries
// by which the wireless daemon finds it, and the Apple80211 requests) for the ath11k station that the kernel's
// bsd/dev/pong_wifi_stack.c runs. Both entry points are called from that worker thread, nothing runs at boot.
//
// PongWifiNet.cpp's IOService classes are not registered by the kernel's own initialisers: the build renames the
// object's initialiser section, and the classes register here between preModLoad and postModLoad, once the radio
// has a hardware address (the same staging as the Nothing Phone's board file).
//
// The USB network (bsd/dev/pong_usbnet.c, boot-arg pong_usb_ncm=1) needs the same classes for its en1 registry
// entries (configd lists interfaces by the class IONetworkInterface), so whichever side comes first registers them,
// once; t970_usbnet_publish() then makes en1's entries by class name.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

extern "C" {
bool pong_wifi_net_prepare(void);
void pong_wifi_net_start(const uint8_t mac[6]);
void PongWifiNetStaticInit(void);
int t970_wifi_net_prepare(void);
int t970_wifi_net_up(const uint8_t mac[6]);
int t970_usbnet_publish(const uint8_t mac[6], unsigned unit);
}
extern kmod_info_t g_kernel_kmod_info;

// 0 not yet, 1 registering, 2 registered, negative: failed with that code (never retried: the initialiser runs once)
static volatile SInt32 gNetClasses;

static int net_classes(void)
{
    for (;;) {
        SInt32 expected = 0;
        if (__atomic_compare_exchange_n(&gNetClasses, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int rc = 0;
            void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
            if (!handle) {
                rc = -2;
            } else {
                PongWifiNetStaticInit();
                if (OSMetaClass::postModLoad(handle) != kIOReturnSuccess) rc = -3;
            }
            __atomic_store_n(&gNetClasses, rc ? rc : 2, __ATOMIC_RELEASE);
            return rc;
        }
        const SInt32 state = __atomic_load_n(&gNetClasses, __ATOMIC_ACQUIRE);
        if (state == 2) return 0;
        if (state < 0) return state;
        IOSleep(10);
    }
}

// Before nw_wifi_start: the module's station events must have their handlers (nw_abi.h).
extern "C" int t970_wifi_net_prepare(void)
{
    return pong_wifi_net_prepare() ? 0 : -1;
}

// Once the station is ready and reports a valid address. 0 when en0 was published (or already is).
extern "C" int t970_wifi_net_up(const uint8_t mac[6])
{
    static bool up;

    if (up) return 0;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        chosen->setProperty("mac-address-wifi0", const_cast<uint8_t *>(mac), 6);
        chosen->release();
    }
    const int rc = net_classes();
    if (rc) return rc;
    pong_wifi_net_start(mac);
    up = true;
    return 0;
}

// An instance of a registered class by name (the classes are PongWifiNet.cpp's), attached under provider and started.
static IOService *publish_named(const char *className, IOService *provider, const char *name, OSDictionary *props)
{
    OSObject *object = OSMetaClass::allocClassWithName(className);
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) { OSSafeReleaseNULL(object); return nullptr; }
    if (!service->init(props) || !service->attach(provider)) { service->release(); return nullptr; }
    service->setName(name);
    if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
    service->registerService();
    return service;
}

static void set_number(OSDictionary *d, const char *key, unsigned long long value)
{
    if (OSNumber *n = OSNumber::withNumber(value, 32)) { d->setObject(key, n); n->release(); }
}

// en<unit>'s registry entries for configd: a controller (plain IOService: SCNetworkInterface calls any controller
// that is not IO80211Controller Ethernet) and an IONetworkInterface under it, plus IONetworkStack when the Wi-Fi side
// has not published one yet (the interface namer opens it). 0 on success, negative on failure.
extern "C" int t970_usbnet_publish(const uint8_t mac[6], unsigned unit)
{
    static bool done;
    char name[8];

    if (done) return 0;
    const int rc = net_classes();
    if (rc) return rc;
    IOService *platform = IOService::getPlatform();
    if (!platform) return -4;
    if (OSDictionary *match = IOService::serviceMatching("IONetworkStack")) {
        IOService *stack = IOService::copyMatchingService(match);
        match->release();
        if (stack) stack->release();
        else if (IOService *made = publish_named("IONetworkStack", platform, "IONetworkStack", nullptr)) made->release();
    }

    OSDictionary *cp = OSDictionary::withCapacity(4);
    OSDictionary *ip = OSDictionary::withCapacity(12);
    OSData *address = OSData::withBytes(mac, 6);
    if (!cp || !ip || !address) { OSSafeReleaseNULL(cp); OSSafeReleaseNULL(ip); OSSafeReleaseNULL(address); return -5; }
    snprintf(name, sizeof(name), "en%u", unit);
    cp->setObject("IOMACAddress", address);
    cp->setObject("IOBuiltin", kOSBooleanTrue);
    cp->setObject("SelfNamed", kOSBooleanTrue);   // en1 is attached and named by pong_usbnet.c already
    ip->setObject("IOMACAddress", address);
    ip->setObject("IOBuiltin", kOSBooleanTrue);
    ip->setObject("IOPrimaryInterface", kOSBooleanFalse);
    if (const OSSymbol *s = OSSymbol::withCString(name)) {
        ip->setObject("IOInterfaceName", s); ip->setObject("BSD Name", s); s->release();
    }
    if (const OSSymbol *s = OSSymbol::withCString("en")) { ip->setObject("IOInterfaceNamePrefix", s); s->release(); }
    set_number(ip, "IOInterfaceType", 6);         // IFT_ETHER
    set_number(ip, "IOInterfaceUnit", unit);
    set_number(ip, "IOMaxTransferUnit", 1500);
    address->release();

    int result = -6;
    IOService *controller = publish_named("IOService", platform, "PongUsbNcmController", cp);
    if (controller) {
        IOService *interface = publish_named("IONetworkInterface", controller, "PongUsbNcm", ip);
        if (interface) { interface->release(); result = 0; done = true; } else result = -7;
        controller->release();
    }
    cp->release();
    ip->release();
    return result;
}
