/*
 * m20.c — Meteomodem M20 radiosonde decoder. See m20.h.
 *
 * Bit sync mirrors rs41.c: ten shift registers (one per sample phase) hold the
 * hard-sliced symbols sampled at that phase; when one equals the 32-symbol raw
 * header (either polarity, max 2 errors) we lock, choose the best phase from
 * the next 10 samples and capture the frame at one symbol per 10 samples.
 * Two symbols make one bit (see m20.h).
 */
#include "m20.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "m20";

#define SPS          10          /* samples per symbol @ 96 kHz / 9600 baud */
#define HDR_BITS     32
#define MEAN_WIN     (HDR_BITS * SPS)
#define HDR_MAX_ERR  2
#define MA_LEN       8           /* discriminator smoothing (samples @96 kHz) */
#define MAX_FRAME    170
#define GPS_UTC_LEAP 18          /* GPS − UTC in seconds */
#define T_LIMIT      1.0f
#define T_LEAK       0.94f

static const uint32_t HDR = 0x99994C99u; /* raw header symbols, first = MSB */

/* ---- M10/M20 checksum -------------------------------------------------------- */

static int update_check(int c, uint8_t b)
{
    int c1 = c & 0xFF;
    /* B */
    b = (uint8_t)((b >> 1) | ((b & 1) << 7));
    b ^= (uint8_t)((b >> 2) & 0xFF);
    /* A1 */
    int t6 = (c & 1) ^ ((c >> 2) & 1) ^ ((c >> 4) & 1);
    int t7 = ((c >> 1) & 1) ^ ((c >> 3) & 1) ^ ((c >> 5) & 1);
    int t = (c & 0x3F) | (t6 << 6) | (t7 << 7);
    /* A2 */
    int s = (c >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;
    int c0 = b ^ t ^ s;
    return ((c1 << 8) | c0) & 0xFFFF;
}

/* ---- shared telemetry (DSP task writes, UI/log tasks read; seqlock) ---------- */

static m20_info_t s_info;               /* written only by the DSP task */
static m20_info_t s_pub;
static volatile uint32_t s_lock;

static void publish(void)
{
    __atomic_store_n(&s_lock, s_lock + 1, __ATOMIC_RELEASE);
    s_pub = s_info;
    __atomic_store_n(&s_lock, s_lock + 1, __ATOMIC_RELEASE);
}

bool m20_get_info(m20_info_t *out)
{
    for (int tries = 0; tries < 4; tries++) {
        uint32_t a = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (a & 1) continue;
        *out = s_pub;
        uint32_t b = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
        if (a == b) return out->seq != 0;
    }
    return false;
}

/* ---- frame -> telemetry ------------------------------------------------------- */

static int be16s(const uint8_t *f, int p)
{
    int v = (f[p] << 8) | f[p + 1];
    return v > 32767 ? v - 65536 : v;
}

static int32_t be32s(const uint8_t *f, int p)
{
    return (int32_t)(((uint32_t)f[p] << 24) | ((uint32_t)f[p + 1] << 16) | ((uint32_t)f[p + 2] << 8) | f[p + 3]);
}

/* f[0] = frame length (0x45), f[1] = 0x20. n = bytes captured. */
static bool decode_frame(const uint8_t *f, int n)
{
    int flen = f[0];
    if (n < flen + 1 || flen < 0x43) return false;

    /* checksum over bytes 0..flen-2, stored big-endian at flen-1..flen */
    int pc = flen - 1;
    int cs1 = (f[pc] << 8) | f[pc + 1];
    int cs = 0;
    for (int i = 0; i < pc; i++) cs = update_check(cs, f[i]);
    if (cs != cs1 || f[1] != 0x20) return false;

    /* GPS week / time of week (seconds) */
    uint32_t tow = ((uint32_t)f[0x0F] << 16) | ((uint32_t)f[0x10] << 8) | f[0x11];
    int day = (int)(tow / 86400u);
    int wk = (f[0x1A] << 8) | f[0x1B];
    if (day > 6 || wk > 4000) return false;
    if (wk < 1304) wk += 1024;

    /* position */
    double la = be32s(f, 0x1C) / 1e6;
    double lo = be32s(f, 0x20) / 1e6;
    double al = (double)(((uint32_t)f[0x08] << 16) | ((uint32_t)f[0x09] << 8) | f[0x0A]) / 100.0;
    if (fabs(la) > 90 || fabs(lo) > 180 || al < -1000 || al > 80000) return false;

    /* velocities (E, N, U) in cm/s */
    double vE = be16s(f, 0x0B) / 100.0;
    double vN = be16s(f, 0x0D) / 100.0;
    double vU = be16s(f, 0x18) / 100.0;
    double dir = atan2(vE, vN) * 180.0 / M_PI;
    if (dir < 0) dir += 360.0;

    /* serial number */
    uint32_t sn24 = ((uint32_t)f[0x14] << 16) | ((uint32_t)f[0x13] << 8) | f[0x12];
    int ym = (int)(sn24 & 0x7F);
    int yy = ym / 12;
    int mo = (ym % 12) + 1;
    char a[8];
    snprintf(a, sizeof a, "%d%02d", yy, mo);
    a[3] = 0;
    int sub = (int)((sn24 >> 7) & 7) + 1;
    int hi = (int)((sn24 >> 23) & 1);
    int serial = (int)((sn24 >> 10) & 0x1FFF);
    if (sn24 == 0) snprintf(s_info.id, sizeof s_info.id, "000-0-00000");
    else snprintf(s_info.id, sizeof s_info.id, "%s-%d-%d%04d", a, sub, hi, serial);

    /* everything checked — publish into the info block */
    s_info.week = (uint16_t)wk;
    s_info.tow_s = tow;
    int sec = (int)(tow % 86400u) - GPS_UTC_LEAP;
    if (sec < 0) sec += 86400;
    s_info.hh = sec / 3600;
    s_info.mm = (sec % 3600) / 60;
    s_info.ss = sec % 60;
    s_info.have_time = true;
    s_info.lat = la;
    s_info.lon = lo;
    s_info.alt = al;
    s_info.vh = (float)sqrt(vE * vE + vN * vN);
    s_info.vd = (float)dir;
    s_info.vv = (float)vU;
    s_info.have_pos = true;
    s_info.frame_no = f[0x15];
    s_info.have_id = true;
    return true;
}

/* ---- bit sync + frame capture --------------------------------------------------- */

enum { ST_SEARCH, ST_SETTLE, ST_CAPTURE };

static int s_state;
static uint32_t s_n;

static float s_ma[MA_LEN];
static int s_ma_i;
static float s_ma_sum;

static float s_hist[8];
static int s_hi;
static float s_tacc;
#define BACK(k) s_hist[(s_hi - 1 - (k)) & 7]

static float s_ring[MEAN_WIN];
static int s_ring_i;
static float s_ring_sum;

static uint32_t s_sr[SPS];

static int s_settle_k, s_pol;
static int s_dist[SPS];
static float s_thr, s_eye;

static int s_wait;
static uint8_t s_raw[MAX_FRAME];
static int s_nbytes, s_len, s_bitcnt;
static uint8_t s_cur;
static int s_sym_half;
static float s_s1;
static int s_mprev;
static bool s_first_bit;

void m20_reset(void)
{
    s_state = ST_SEARCH;
    s_n = 0;
    memset(s_ma, 0, sizeof s_ma);
    s_ma_sum = 0;
    memset(s_ring, 0, sizeof s_ring);
    s_ma_i = s_ring_i = 0;
    s_hi = 0;
    memset(s_hist, 0, sizeof s_hist);
    s_tacc = 0;
    s_ring_sum = 0;
    memset(s_sr, 0, sizeof s_sr);
}

static void abort_frame(void)
{
    s_info.frames_bad++;
    s_state = ST_SEARCH;
    memset(s_sr, 0, sizeof s_sr);
}

static void finish_frame(void)
{
    bool ok = decode_frame(s_raw, s_len);
    s_info.eye = fabsf(s_eye);
    if (ok) {
        s_info.frames_ok++;
        s_info.seq++;
        ESP_LOGI(TAG, "%s #%u  %.5f %.5f  %.0f m  eye %.2f", s_info.id, (unsigned)s_info.frame_no,
                 s_info.lat, s_info.lon, s_info.alt, s_info.eye);
    } else {
        s_info.frames_bad++;
        ESP_LOGW(TAG, "bad frame (checksum/plausibility), eye %.2f rad/sample", s_info.eye);
    }
    publish();
    s_state = ST_SEARCH;
    memset(s_sr, 0, sizeof s_sr);
}

static inline int hdr_dist(uint32_t sr, int pol)
{
    int d = __builtin_popcount(sr ^ HDR);
    return pol ? HDR_BITS - d : d;
}

void m20_feed(float d)
{
    /* moving average of the discriminator output */
    s_ma_sum += d - s_ma[s_ma_i];
    s_ma[s_ma_i] = d;
    if (++s_ma_i >= MA_LEN) s_ma_i = 0;
    float s = s_ma_sum * (1.0f / MA_LEN);

    /* running mean over one header length = decision threshold */
    s_ring_sum += s - s_ring[s_ring_i];
    s_ring[s_ring_i] = s;
    if (++s_ring_i >= MEAN_WIN) s_ring_i = 0;
    float thr = s_ring_sum * (1.0f / MEAN_WIN);
    s_hist[s_hi] = s;
    s_hi = (s_hi + 1) & 7;

    int p = (int)(s_n % SPS);
    s_n++;

    switch (s_state) {
    case ST_SEARCH: {
        s_sr[p] = (s_sr[p] << 1) | (s > thr ? 1u : 0u);
        int dn = __builtin_popcount(s_sr[p] ^ HDR);
        if (dn <= HDR_MAX_ERR || HDR_BITS - dn <= HDR_MAX_ERR) {
            s_pol = (dn > HDR_BITS / 2);
            s_settle_k = 0;
            s_dist[0] = hdr_dist(s_sr[p], s_pol);
            s_state = ST_SETTLE;
        }
        break;
    }
    case ST_SETTLE: {
        s_sr[p] = (s_sr[p] << 1) | (s > thr ? 1u : 0u);
        s_settle_k++;
        s_dist[s_settle_k] = hdr_dist(s_sr[p], s_pol);
        if (s_settle_k >= SPS - 1) {
            int best = 99;
            for (int k = 0; k < SPS; k++) if (s_dist[k] < best) best = s_dist[k];
            int lo = -1, hi = -1;
            for (int k = 0; k < SPS; k++) {
                if (s_dist[k] <= best + 1) { if (lo < 0) lo = k; hi = k; }
            }
            int j = (lo + hi) / 2;
            s_wait = j + 1 + 3;
            s_tacc = 0;
            {
                float fh = 0, fl = 0;
                int nh = 0, nl = 0;
                for (int k = 0; k < HDR_BITS; k++) {
                    int back = (SPS - 1 - j) + SPS * k;
                    int idx = s_ring_i - 1 - back;
                    if (idx < 0) idx += MEAN_WIN;
                    if (((int)((HDR >> k) & 1u)) ^ s_pol) { fh += s_ring[idx]; nh++; }
                    else { fl += s_ring[idx]; nl++; }
                }
                if (nh && nl) {
                    fh /= nh; fl /= nl;
                    s_thr = 0.5f * (fh + fl);
                    s_eye = fh - fl;
                } else {
                    s_thr = thr;
                    s_eye = 0;
                }
            }
            s_nbytes = 0;
            s_bitcnt = 0;
            s_cur = 0;
            s_len = MAX_FRAME;
            s_sym_half = 0;
            s_mprev = 0;
            s_first_bit = true;
            s_state = ST_CAPTURE;
        }
        break;
    }
    case ST_CAPTURE:
        if (--s_wait > 0) break;
        s_wait = SPS;
        {
            float c = BACK(3), e = BACK(6), l = BACK(0);

            /* early/late timing tracker, once per symbol */
            float sw = fabsf(s_eye);
            if (sw < 1e-4f) sw = 1e-4f;
            float me = fabsf(e - s_thr), ml = fabsf(l - s_thr);
            s_tacc = s_tacc * T_LEAK + (me - ml) / sw;
            if (s_tacc > T_LIMIT) { s_wait = SPS - 1; s_tacc = 0; }
            else if (s_tacc < -T_LIMIT) { s_wait = SPS + 1; s_tacc = 0; }

            if (s_sym_half == 0) {
                s_s1 = c;
                s_sym_half = 1;
            } else {
                s_sym_half = 0;
                int mv = (c - s_s1 >= 0) ? 1 : 0;
                int bv = 0;
                if (s_first_bit) s_first_bit = false; /* first frame bit (MSB of 0x45) is always 0 */
                else if (mv == s_mprev) bv = 1;
                s_mprev = mv;

                s_cur = (uint8_t)((s_cur << 1) | bv); /* MSB first */
                if (++s_bitcnt == 8) {
                    s_raw[s_nbytes++] = s_cur;
                    s_cur = 0;
                    s_bitcnt = 0;
                    if (s_nbytes == 1) {
                        int fl = s_raw[0];
                        if (fl < 0x40 || fl > 0x45 + 64) { abort_frame(); return; }
                        s_len = fl + 1;
                    }
                    if (s_nbytes >= s_len) finish_frame();
                }
            }
        }
        break;
    }
}
