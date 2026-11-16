// pong-locd: the location provider of the SM-T970 port (work/gps_camera_scope/DESIGN.md in iOSPort).
// locationd has no GPS of its own here (J171 is a Wi-Fi iPad: no gps-capable, no AppleEmbeddedGPS), so this daemon hands it
// positions through CoreLocation's simulation interface (CLSimulationManager, the path Xcode's "simulate location" uses;
// entitlement com.apple.locationd.simulation). Every app then gets them through CLLocationManager as usual.
//
// Sources, first that has a position wins:
//   1. NMEA from /dev/pong_gps (bsd/dev/pong_gps.c): $G?RMC (position, speed, course, valid flag) and $G?GGA (altitude,
//      HDOP -> accuracy 5 m x HDOP). Only if the GNSS chip speaks NMEA, which is not known yet.
//   2. /private/var/mobile/Library/Pong/location.txt: "lat lon [accuracy_m [altitude_m]]" in decimal degrees, a fixed place
//      set by hand (fsput), re-sent every 60 s with a fresh timestamp.
// Off unless boot argument pong_loc bit 0 (it only sleeps); bit 1 also turns Location Services on
// (+[CLLocationManager setLocationServicesEnabled:], as Settings does). Status: /private/var/mobile/Library/Pong/locd.txt.
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#import <objc/message.h>
#import <sys/sysctl.h>
#import <fcntl.h>
#import <unistd.h>
#import <poll.h>
#import <errno.h>
#import <math.h>
#import <stdlib.h>

static NSString *const kFixed = @"/private/var/mobile/Library/Pong/location.txt";
static NSString *const kStatus = @"/private/var/mobile/Library/Pong/locd.txt";

static unsigned bootFlag(void) {
    char args[1024] = {0};
    size_t len = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) return 0;
    const char *p = strstr(args, "pong_loc=");
    return p ? (unsigned)strtoul(p + 9, NULL, 0) : 0;
}

static id gSim;
static unsigned long gSent;

static void status(NSString *what) {
    NSString *line = [NSString stringWithFormat:@"%@ sent=%lu %@\n", [NSDate date], gSent, what];
    [line writeToFile:kStatus atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSLog(@"pong-locd: %@", what);
}

static void send(CLLocationCoordinate2D c, double alt, double acc, double speed, double course) {
    CLLocation *loc = [[CLLocation alloc] initWithCoordinate:c altitude:alt horizontalAccuracy:acc verticalAccuracy:(alt == 0 ? -1 : acc * 1.5)
                                                     course:course speed:speed timestamp:[NSDate date]];
    ((void (*)(id, SEL))objc_msgSend)(gSim, @selector(stopLocationSimulation));
    ((void (*)(id, SEL))objc_msgSend)(gSim, @selector(clearSimulatedLocations));
    ((void (*)(id, SEL, id))objc_msgSend)(gSim, @selector(appendSimulatedLocation:), loc);
    if ([gSim respondsToSelector:@selector(flush)]) ((void (*)(id, SEL))objc_msgSend)(gSim, @selector(flush));
    ((void (*)(id, SEL))objc_msgSend)(gSim, @selector(startLocationSimulation));
    gSent++;
}

// ddmm.mmmm + hemisphere -> degrees
static double nmeaDeg(const char *v, const char *h) {
    if (!*v) return NAN;
    double x = atof(v), d = floor(x / 100);
    double deg = d + (x - d * 100) / 60;
    return (*h == 'S' || *h == 'W') ? -deg : deg;
}

static struct { double alt, acc; } gGga = {0, 50};

// One NMEA sentence (without the line end); returns 1 when a valid RMC position was sent.
static int nmea(char *s) {
    char *f[20] = {0};
    int n = 0;
    char *star = strchr(s, '*');
    if (s[0] != '$' || s[1] != 'G' || strlen(s) < 7) return 0;
    if (star) {
        unsigned sum = 0;
        for (char *p = s + 1; p < star; p++) sum ^= (unsigned char)*p;
        if (sum != strtoul(star + 1, NULL, 16)) return 0;
        *star = 0;
    }
    for (char *p = s; p && n < 20; ) {
        f[n++] = p;
        p = strchr(p, ',');
        if (p) *p++ = 0;
    }
    if (n >= 10 && !strcmp(f[0] + 3, "GGA") && *f[6] != '0') {
        double hdop = atof(f[8]);
        gGga.acc = hdop > 0 ? hdop * 5 : 50;
        gGga.alt = atof(f[9]);
    }
    if (n >= 9 && !strcmp(f[0] + 3, "RMC") && *f[2] == 'A') {
        CLLocationCoordinate2D c = CLLocationCoordinate2DMake(nmeaDeg(f[3], f[4]), nmeaDeg(f[5], f[6]));
        if (!CLLocationCoordinate2DIsValid(c)) return 0;
        double speed = *f[7] ? atof(f[7]) * 0.514444 : -1, course = *f[8] ? atof(f[8]) : -1;
        send(c, gGga.alt, gGga.acc, speed, course);
        return 1;
    }
    return 0;
}

// The fixed place, if the file holds one.
static BOOL fixed(void) {
    NSString *t = [NSString stringWithContentsOfFile:kFixed encoding:NSUTF8StringEncoding error:nil];
    double v[4] = {NAN, NAN, 50, 0};
    int n = t ? sscanf(t.UTF8String, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]) : 0;
    if (n < 2) return NO;
    CLLocationCoordinate2D c = CLLocationCoordinate2DMake(v[0], v[1]);
    if (!CLLocationCoordinate2DIsValid(c)) return NO;
    send(c, v[3], v[2] > 0 ? v[2] : 50, -1, -1);
    status([NSString stringWithFormat:@"fixed %.5f %.5f acc %.0f", v[0], v[1], v[2]]);
    return YES;
}

int main(void) {
    @autoreleasepool {
        const unsigned flag = bootFlag();
        if (!(flag & 1)) for (;;) pause();   // off: idle (the job's KeepAlive would restart an exit), no hardware touched
        Class sim = NSClassFromString(@"CLSimulationManager");
        if (!sim) {status(@"error: no CLSimulationManager"); return 1;}
        gSim = [sim new];
        if (flag & 2) {
            SEL en = NSSelectorFromString(@"setLocationServicesEnabled:");
            if ([CLLocationManager respondsToSelector:en]) ((void (*)(id, SEL, BOOL))objc_msgSend)([CLLocationManager class], en, YES);
        }
        status(@"started");
        char line[256];
        size_t used = 0;
        time_t lastFix = 0, lastFixed = 0;
        int fd = -1;
        for (;;) {
            @autoreleasepool {
                if (fd < 0) fd = open("/dev/pong_gps", O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    struct pollfd p = {fd, POLLIN, 0};
                    char buf[512];
                    (void)poll(&p, 1, 1000);
                    ssize_t got = read(fd, buf, sizeof(buf));
                    if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {close(fd); fd = -1;}
                    for (ssize_t i = 0; i < got; i++) {
                        const char ch = buf[i];
                        if (ch == '\r' || ch == '\n') {
                            line[used] = 0;
                            if (used && nmea(line)) {
                                if (time(NULL) - lastFix >= 30) status(@"nmea fix");
                                lastFix = time(NULL);
                            }
                            used = 0;
                        } else if (used < sizeof(line) - 1) {
                            line[used++] = ch;
                        } else {
                            used = 0;
                        }
                    }
                } else {
                    sleep(1);
                }
                // no NMEA fix in the last 30 s: the fixed place, every 60 s
                if (time(NULL) - lastFix > 30 && time(NULL) - lastFixed >= 60) {
                    lastFixed = time(NULL);
                    if (!fixed() && gSent == 0) status(fd >= 0 ? @"waiting: /dev/pong_gps open, no NMEA fix, no location.txt"
                                                              : @"waiting: no /dev/pong_gps, no location.txt");
                }
            }
        }
    }
}
