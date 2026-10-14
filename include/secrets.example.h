#pragma once

// Copy this file to include/secrets.h when building the optional Wi-Fi UDP
// transport environment. The default USB serial build does not need it.

// Use a 2.4 GHz Wi-Fi network reachable by both the ESP32-S3 and the PC.
#define WIFI_SSID "YOUR_2_4_GHZ_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Set this to the local PC IPv4 address shown by ipconfig.
#define UDP_TARGET_IP "192.168.1.100"
#define UDP_TARGET_PORT 5005
#define UDP_LOCAL_PORT 5006
