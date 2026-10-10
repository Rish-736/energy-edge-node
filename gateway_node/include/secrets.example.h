#pragma once
/* ---------------------------------------------------------------------------
 * secrets.example.h -- copy to secrets.h and fill in.
 *
 *   cp include/secrets.example.h include/secrets.h
 *
 * secrets.h is gitignored. Credentials do not belong in a public repository,
 * and a committed WiFi password is the kind of thing that gets found by
 * automated scanners within minutes of a push.
 *
 * config.h includes this only if it exists, so the firmware still compiles
 * without it -- it just cannot reach the network.
 * -------------------------------------------------------------------------*/

#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASS      "your-wifi-password"

/* The machine running Mosquitto -- usually your laptop's LAN address.
 * Find it with `ipconfig` (Windows) or `ip addr` (Linux). */
#define MQTT_HOST      "192.168.1.100"
#define MQTT_PORT      1883

/* Leave empty for an unauthenticated local broker. Phase 4 adds TLS and
 * real credentials. */
#define MQTT_USER      ""
#define MQTT_PASS      ""
