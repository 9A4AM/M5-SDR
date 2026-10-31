/*
 * web_ui.h — tiny HTTP server that mirrors the touch UI in a browser.
 *
 *   GET /            page with a canvas that polls /frame
 *   GET /frame       one frame, 640x360, raw little-endian RGB565 (460800 bytes)
 *   GET /frame?f=8   same frame as RGB332 (230400 bytes), for slow links
 *   POST /t?x=&y=&d= remote touch (d=1 pressed, 0 released), landscape coords
 *
 * The server listens on all interfaces, so it works both on the home network
 * (STA) and on the fallback access point. Everything is protected by HTTP
 * Basic authentication (WEB_USER / WEB_PASSWORD in web_cfg.h). With an empty
 * WEB_PASSWORD the page is view-only and remote control is refused. Basic
 * authentication is not encrypted: on a shared LAN use a password you do not
 * reuse; the WPA2 fallback AP encrypts the link itself.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server (port 80). Call after wifi_net_init(). */
void web_ui_start(void);

#ifdef __cplusplus
}
#endif
