/*
 * m20.h — Meteomodem M20 radiosonde decoder (400–406 MHz, 9600 symbols/s).
 *
 * Input : FM-discriminator samples at 96 kHz (10 samples per symbol), fed one
 *         at a time by dsp.c while in DSP_MODE_M20. (RS41 runs at 48 kHz; the
 *         M20 symbol rate is twice as high, so dsp.c uses a 96 kHz IF for it.)
 * Output: decoded telemetry (serial, frame number, GPS position/velocity/time),
 *         readable from the UI task via m20_get_info().
 *
 * Line coding: every data bit is two symbols (Manchester-like); the bit is
 * recovered differentially from the symbol pair, so the demodulator polarity
 * does not matter. There is no FEC — a frame is accepted only if the M10/M20
 * 16-bit checksum matches and the decoded fields are plausible.
 * Port of the M20 decoder from rs1729's mXX.c (via the B4A M20 class).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[16];            /* serial, e.g. "123-4-05678"                      */
    uint8_t frame_no;       /* sonde frame counter                              */
    bool have_id;
    bool have_pos;
    bool have_time;

    double lat, lon;        /* degrees (WGS84)                                  */
    double alt;             /* metres                                           */
    float vh, vd, vv;       /* horizontal speed m/s, heading deg, climb m/s     */

    uint16_t week;          /* GPS week                                         */
    uint32_t tow_s;         /* GPS time of week, seconds                        */
    int hh, mm, ss;         /* UTC time of day (GPS time minus leap seconds)    */

    uint32_t frames_ok;     /* frames with a valid checksum                     */
    uint32_t frames_bad;    /* locked frames rejected (checksum / plausibility) */
    float eye;              /* header eye opening (rad/sample)                  */
    uint32_t seq;           /* increments on every valid decoded frame          */
} m20_info_t;

/* Forget sync state (call when entering M20 mode). Telemetry is kept. */
void m20_reset(void);

/* Feed one discriminator sample (rad/sample, 96 kHz). */
void m20_feed(float d);

/* Copy the latest telemetry. Returns false until a frame has been decoded. */
bool m20_get_info(m20_info_t *out);

#ifdef __cplusplus
}
#endif
