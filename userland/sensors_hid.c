/* Pong sensors-to-HID bridge (SM-T970): the SLPI's light level and gravity as built-in HID services in backboardd's event server.
 * Off unless boot argument pong_sensors_hid is set (bits: 1 ambient light, 2 accelerometer + gyro). The values come from the
 * kernel's kern.pong_sensors (bsd/dev/pong_ssc.c); pong_sensors_fake (bits: 1 light cycles 5..2000 lux every 4 s, 2 gravity
 * holds portrait, 4 gravity cycles the four orientations every 8 s) replaces them, to test this side without the SLPI.
 *
 * Usages and event fields are the ones the 22H374 shared cache uses: AppleVendor page 0xff00, usage 4 ambient light, 3
 * accelerometer, 9 gyro (the motion pair the precedent rui12366B/UniXNU-gts7xlwifi found locationd follows); the light event
 * is IOHIDEventCreateAmbientLightSensorEvent with the illuminance (lux, double) in field 0xc000b (HID.framework's
 * -setAmbientLightSensorIlluminance:), the accelerometer event takes x, y, z in g. Not tested on the device when written.
 * Decision points (iOSPort work/sensors_scope/SCOPE.md §3 stage 4): whether CoreBrightness and locationd take virtual services,
 * whether rotation needs the device-orientation service as well. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "touch.h"

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
IOHIDServiceClientRef pong_hid_make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display);

static IOHIDEventSystemClientRef (*CreateClient)(CFAllocatorRef, int, CFDictionaryRef);
static void (*Schedule)(IOHIDEventSystemClientRef, dispatch_queue_t);
static Boolean (*Dispatch)(IOHIDServiceClientRef, IOHIDEventRef);
static IOHIDEventRef (*CreateLight)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static IOHIDEventRef (*CreateAccel)(CFAllocatorRef, uint64_t, double, double, double, uint32_t);
static IOHIDEventRef (*CreateGyro)(CFAllocatorRef, uint64_t, double, double, double, uint32_t);
static void (*SetFloat)(IOHIDEventRef, uint32_t, double);
static void (*SetInteger)(IOHIDEventRef, uint32_t, CFIndex);

#define PAGE_APPLE_VENDOR       0xff00
#define USAGE_LIGHT             0x4
#define USAGE_ACCEL             0x3
#define USAGE_GYRO              0x9
#define FIELD_IS_BUILT_IN       0x4
#define FIELD_LIGHT_ILLUMINANCE 0xc000b
#define say(...) fprintf(stderr, __VA_ARGS__)

static unsigned arg(const char *bootargs, const char *name)
{
    const char *p = strstr(bootargs, name);
    return p ? (unsigned)strtoul(p + strlen(name), NULL, 0) : 0;
}

/* kern.pong_sensors: [0] sequence [1] lux x 100 [2..4] gravity mg [5] flags (1 lux, 2 gravity) [6..7] sample counts */
static bool read_kernel(int32_t v[8])
{
    size_t length = 8 * sizeof(int32_t);
    return sysctlbyname("kern.pong_sensors", v, &length, NULL, 0) == 0 && length == 8 * sizeof(int32_t);
}

static void fake(unsigned mode, uint64_t tick, int32_t v[8])
{
    static const int32_t lux[4] = {5, 80, 600, 2000};
    static const int32_t g[4][3] = {{0, -1000, 0}, {-1000, 0, 0}, {0, 1000, 0}, {1000, 0, 0}};
    const unsigned which = (mode & 4) ? (unsigned)(tick / 80) % 4 : 0;
    v[0] = (int32_t)tick;
    if (mode & 1) {v[1] = lux[(tick / 40) % 4] * 100; v[5] |= 1;}
    if (mode & 6) {v[2] = g[which][0]; v[3] = g[which][1]; v[4] = g[which][2]; v[5] |= 2;}
}

static void *sensors_thread(void *context)
{
    const unsigned mode = (unsigned)(uintptr_t)context & 0xff, fake_mode = (unsigned)(uintptr_t)context >> 8;
    IOHIDEventSystemClientRef client = CreateClient(NULL, 4, NULL);   /* a client that only sends, as touch.c */
    IOHIDServiceClientRef light = NULL, accel = NULL, gyro = NULL;
    int32_t last_lux = -1, v[8];
    unsigned missing = 0;

    pthread_setname_np("Pong sensors HID");
    if (!client) {say("Pong sensors HID: no event system client; SKIP\n"); return NULL;}
    Schedule(client, dispatch_queue_create("pong.sensors.hid", NULL));
    if ((mode & 1) && CreateLight) light = pong_hid_make_service(client, "Pong ambient light", PAGE_APPLE_VENDOR, USAGE_LIGHT, false);
    if ((mode & 2) && CreateAccel) accel = pong_hid_make_service(client, "Pong accelerometer", PAGE_APPLE_VENDOR, USAGE_ACCEL, false);
    if ((mode & 2) && CreateGyro) gyro = pong_hid_make_service(client, "Pong gyro", PAGE_APPLE_VENDOR, USAGE_GYRO, false);
    say("Pong sensors HID: light %s, accelerometer %s, gyro %s, source %s\n", light ? "opened" : "-", accel ? "opened" : "-",
        gyro ? "opened" : "-", fake_mode ? "fake" : "kern.pong_sensors");
    if (!light && !accel) return NULL;
    for (uint64_t tick = 0;; tick++, usleep(100000)) {   /* 10 Hz */
        memset(v, 0, sizeof(v));
        if (fake_mode) fake(fake_mode, tick, v);
        else if (!read_kernel(v)) {if (missing++ == 0) say("Pong sensors HID: kern.pong_sensors unreadable\n"); continue;}
        const uint64_t now = mach_absolute_time();
        /* light: on a change, and once a second anyway (CoreBrightness filters the level itself) */
        if (light && (v[5] & 1) && (v[1] != last_lux || tick % 10 == 0)) {
            IOHIDEventRef e = CreateLight(NULL, now, (uint32_t)(v[1] / 100), 0, 0, 0, 0, 0);
            if (e) {
                if (SetFloat) SetFloat(e, FIELD_LIGHT_ILLUMINANCE, v[1] / 100.0);
                SetInteger(e, FIELD_IS_BUILT_IN, 1);
                Dispatch(light, e);
                CFRelease(e);
            }
            if (v[1] != last_lux && (tick < 600 || tick % 300 == 0)) say("Pong sensors HID: light %d.%02d lux\n", v[1] / 100, v[1] % 100);
            last_lux = v[1];
        }
        if (accel && (v[5] & 2)) {
            IOHIDEventRef e = CreateAccel(NULL, now, v[2] / 1000.0, v[3] / 1000.0, v[4] / 1000.0, 0);
            if (e) {SetInteger(e, FIELD_IS_BUILT_IN, 1); Dispatch(accel, e); CFRelease(e);}
            /* no gyro stream yet: a tablet at rest, which locationd's orientation detector wants to see (precedent) */
            if (gyro && (e = CreateGyro(NULL, now, 0, 0, 0, 0))) {SetInteger(e, FIELD_IS_BUILT_IN, 1); Dispatch(gyro, e); CFRelease(e);}
        }
    }
    return NULL;
}

void pong_sensors_start(void)
{
    char bootargs[1024] = {0};
    size_t length = sizeof(bootargs) - 1;
    if (sysctlbyname("kern.bootargs", bootargs, &length, NULL, 0) != 0) return;
    const unsigned mode = arg(bootargs, "pong_sensors_hid=") & 3, fake_mode = arg(bootargs, "pong_sensors_fake=") & 7;
    if (!mode) return;
#define RESOLVE(var, name) var = (__typeof__(var))dlsym(RTLD_DEFAULT, name)
    RESOLVE(CreateClient, "IOHIDEventSystemClientCreateWithType");
    RESOLVE(Schedule, "IOHIDEventSystemClientScheduleWithDispatchQueue");
    RESOLVE(Dispatch, "IOHIDVirtualServiceClientDispatchEvent");
    RESOLVE(CreateLight, "IOHIDEventCreateAmbientLightSensorEvent");
    RESOLVE(CreateAccel, "IOHIDEventCreateAccelerometerEvent");
    RESOLVE(CreateGyro, "IOHIDEventCreateGyroEvent");
    RESOLVE(SetFloat, "IOHIDEventSetFloatValue");
    RESOLVE(SetInteger, "IOHIDEventSetIntegerValue");
#undef RESOLVE
    if (!CreateClient || !Schedule || !Dispatch || !SetInteger) {say("Pong sensors HID: event system API missing; SKIP\n"); return;}
    pthread_t thread;
    if (!pthread_create(&thread, NULL, sensors_thread, (void *)(uintptr_t)(mode | fake_mode << 8))) pthread_detach(thread);
}
