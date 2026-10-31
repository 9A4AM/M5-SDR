/*
 * ui.h — landscape touch UI (spectrum/waterfall + tune/volume/squelch).
 *
 * Renders into a 1280x720 landscape buffer and uses the P4 PPA to rotate it 90°
 * onto the portrait panel. Owns the render+touch task.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the UI task. Call after sdr_stream_start() + dsp_start(). */
void ui_start(void);

/* ---- web mirror (see web_ui.c) -------------------------------------------------
 * The UI keeps a snapshot of its 640x360 RGB565 landscape frame while a web
 * client is polling. ui_web_lock_frame() returns it (waits up to ~1 s for the
 * first frame) and holds a mutex until ui_web_unlock_frame(); copy it out
 * quickly. The snapshot is not updated while the SETTINGS screens are open, so
 * the WiFi password is never exposed over the web. */
#define UI_FB_W 640
#define UI_FB_H 360
#include <stdbool.h>
#include <stdint.h>
bool ui_web_lock_frame(const uint16_t **fb);
void ui_web_unlock_frame(void);

/* Remote touch from the browser, in landscape UI coordinates (0..639, 0..359).
 * down=true while the pointer is pressed, false on release. Ignored while the
 * SETTINGS screens are open (they are not mirrored). */
void ui_web_touch(int x, int y, bool down);

#ifdef __cplusplus
}
#endif
