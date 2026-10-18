#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_OFF,            /* no network saved */
    NET_CONNECTING,
    NET_CONNECTED,      /* has an IP address */
    NET_FAILED,         /* last attempt failed (keeps retrying) */
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_ap_t;

/* WiFi station: connects to the saved network (kept in NVS by ESP-IDF),
 * reconnects when the link drops, sets the clock over NTP once online.
 * Needs nvs_flash_init() first.
 */
bool net_init(void);

net_state_t net_state(void);
const char *net_ssid(void);             /* saved/target network, "" if none */
void net_ip(char *buf, int size);

/* Asynchronous scan: start, then poll for results. While not connected,
 * scanning pauses the reconnect attempts; net_resume() restarts them.
 */
void net_scan_start(void);
bool net_scan_done(void);
int net_scan_results(net_ap_t *aps, int max);

void net_resume(void);

/* Connect to a network and save it for next time */
void net_connect(const char *ssid, const char *password);

/* WiFi modem sleep off (on: true) or back on. Asleep, the radio wakes at
 * the access point's beacons: a request waits ~100 ms for the board (the
 * Tab5's C6 more), which a debugger's steps feel
 */
void net_low_latency(bool on);

/* Where the clock came from: the board's own clock at boot (the Tab5's
 * RTC), then the internet (which also sets the board's clock)
 */
typedef enum {
    NET_TIME_NONE,
    NET_TIME_BOARD,
    NET_TIME_INTERNET,
} net_time_t;

net_time_t net_time_source(void);
