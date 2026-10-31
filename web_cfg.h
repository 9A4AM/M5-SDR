/*
 * web_cfg.h — web mirror / remote control login and fallback access point.
 *
 * EDIT THIS FILE before flashing.
 */
#pragma once

/* Login for the browser mirror (HTTP Basic authentication). While
 * WEB_PASSWORD is empty the page is view-only and remote control is disabled. */
#define WEB_USER      "admin"
#define WEB_PASSWORD  ""

/* Fallback access point, started when the station cannot join WiFi.
 * WPA2 needs a password of 8..63 characters (shorter ones are rejected by the
 * WiFi stack). The AP address is the ESP-IDF default, 192.168.4.1. */
#define AP_SSID       "M5-SDR"
#define AP_PASSWORD   "passw0rd"
