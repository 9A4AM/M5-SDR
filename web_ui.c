/* web_ui.c — browser mirror of the UI. See web_ui.h. */
#include "web_ui.h"
#include "ui.h"
#include "web_cfg.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "web_ui";

#define FRAME_PX   ((size_t)UI_FB_W * UI_FB_H)
static uint8_t *s_tx; /* send buffer (PSRAM); httpd runs one handler at a time */
static httpd_handle_t s_httpd;

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>M5-SDR</title><style>"
"body{margin:0;background:#0a0e1e;color:#9ab;font:14px sans-serif;text-align:center}"
"canvas{width:100%;max-width:1280px;background:#000;image-rendering:pixelated;touch-action:none;cursor:pointer}"
"div{padding:6px}</style></head><body>"
"<canvas id=c width=640 height=360></canvas>"
"<div>M5-SDR &middot; <span id=s>connecting...</span> &middot; <span id=m></span> &middot; "
"<label><input type=checkbox id=f> low bandwidth</label></div>"
"<script>"
"const c=document.getElementById('c'),x=c.getContext('2d'),"
"img=x.createImageData(640,360),s=document.getElementById('s'),f=document.getElementById('f'),"
"m=document.getElementById('m');"
"let n=0,t0=performance.now();"
"async function loop(){"
"try{"
"const low=f.checked,r=await fetch('/frame?f='+(low?8:16),{cache:'no-store'});"
"if(!r.ok)throw 0;"
"const b=await r.arrayBuffer(),d=img.data;"
"if(low){const a=new Uint8Array(b);"
"for(let i=0,j=0;i<a.length;i++,j+=4){const v=a[i];"
"d[j]=(v>>5)*255/7;d[j+1]=((v>>2)&7)*255/7;d[j+2]=(v&3)*255/3;d[j+3]=255;}}"
"else{const a=new Uint16Array(b);"
"for(let i=0,j=0;i<a.length;i++,j+=4){const v=a[i];"
"d[j]=((v>>11)&31)*255/31;d[j+1]=((v>>5)&63)*255/63;d[j+2]=(v&31)*255/31;d[j+3]=255;}}"
"x.putImageData(img,0,0);n++;"
"const t=performance.now();"
"if(t-t0>=1000){s.textContent=(n*1000/(t-t0)).toFixed(1)+' fps';n=0;t0=t;}"
"setTimeout(loop,0);"
"}catch(e){s.textContent='waiting for frame...';setTimeout(loop,1000);}"
"}"
"loop();"
"let ctl=true,q=[],busy=false,down=false,lastMove=0;"
"m.textContent='control on';"
"function pos(e){const r=c.getBoundingClientRect();"
"return[Math.max(0,Math.min(639,Math.round((e.clientX-r.left)*640/r.width))),"
"Math.max(0,Math.min(359,Math.round((e.clientY-r.top)*360/r.height)))];}"
"async function pump(){if(busy)return;busy=true;"
"while(q.length){const a=q.shift();"
"try{const r=await fetch('/t?x='+a[0]+'&y='+a[1]+'&d='+a[2],{method:'POST',headers:{'X-M5':'1'}});"
"if(r.status==403){ctl=false;q=[];m.textContent='view only';}}catch(e){}}"
"busy=false;}"
"function send(px,py,d,merge){if(!ctl)return;"
"if(merge&&q.length&&q[q.length-1][3])q.pop();"
"q.push([px,py,d,merge]);pump();}"
"c.addEventListener('pointerdown',e=>{down=true;c.setPointerCapture(e.pointerId);"
"const p=pos(e);send(p[0],p[1],1,false);e.preventDefault();});"
"c.addEventListener('pointermove',e=>{if(!down)return;const t=performance.now();"
"if(t-lastMove<40)return;lastMove=t;const p=pos(e);send(p[0],p[1],1,true);});"
"const up=e=>{if(!down)return;down=false;const p=pos(e);send(p[0],p[1],0,false);};"
"c.addEventListener('pointerup',up);c.addEventListener('pointercancel',up);"
"</script></body></html>";

/* ---- HTTP Basic authentication ------------------------------------------------ */
static char s_auth[128];  /* expected "Authorization" header value */
static bool s_auth_on;    /* false: WEB_PASSWORD empty -> open view, no control */

static void b64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* Returns true if the request may proceed; otherwise a 401 has been sent. */
static bool authorized(httpd_req_t *req)
{
    if (!s_auth_on) return true;
    char hdr[128];
    size_t l = httpd_req_get_hdr_value_len(req, "Authorization");
    if (l > 0 && l < sizeof hdr && httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK) {
        size_t el = strlen(s_auth);
        if (l == el) { /* constant-time compare */
            uint8_t diff = 0;
            for (size_t i = 0; i < el; i++) diff |= (uint8_t)(hdr[i] ^ s_auth[i]);
            if (!diff) return true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500)); /* slow down password guessing */
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"M5-SDR\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t page_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t frame_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    bool low = false;
    char q[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "f", v, sizeof v) == ESP_OK && v[0] == '8') low = true;
    }

    const uint16_t *fb;
    if (!ui_web_lock_frame(&fb)) {
        httpd_resp_set_status(req, "503 Service Unavailable"); /* no HTTPD_503 code in IDF 5.4 */
        httpd_resp_send(req, "no frame yet", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t len;
    if (low) { /* RGB565 -> RGB332 */
        for (size_t i = 0; i < FRAME_PX; i++) {
            uint16_t p = fb[i];
            s_tx[i] = (uint8_t)(((p >> 8) & 0xE0) | ((p >> 6) & 0x1C) | ((p >> 3) & 0x03));
        }
        len = FRAME_PX;
    } else {
        memcpy(s_tx, fb, FRAME_PX * 2);
        len = FRAME_PX * 2;
    }
    ui_web_unlock_frame(); /* release the UI before the (slow) network send */

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)s_tx, len);
}

/* POST /t?x=..&y=..&d=1|0 — remote touch. Needs authentication AND the custom
 * X-M5 header: a browser will not add that header to a cross-site request
 * without a CORS preflight (which this server never answers), so another web
 * page cannot drive the radio with your saved login (CSRF protection). */
static esp_err_t touch_post(httpd_req_t *req)
{
    if (!s_auth_on) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "control disabled: set WEB_PASSWORD in web_cfg.h");
        return ESP_OK;
    }
    if (!authorized(req)) return ESP_OK;
    if (httpd_req_get_hdr_value_len(req, "X-M5") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing header");
        return ESP_OK;
    }
    char q[48], v[8];
    int x = -1, y = -1, d = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "x", v, sizeof v) == ESP_OK) x = atoi(v);
        if (httpd_query_key_value(q, "y", v, sizeof v) == ESP_OK) y = atoi(v);
        if (httpd_query_key_value(q, "d", v, sizeof v) == ESP_OK) d = atoi(v);
    }
    if (x < 0 || x >= UI_FB_W || y < 0 || y >= UI_FB_H || (d != 0 && d != 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
        return ESP_OK;
    }
    ui_web_touch(x, y, d == 1);
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

void web_ui_start(void)
{
    if (s_httpd) return;
    s_tx = heap_caps_malloc(FRAME_PX * 2, MALLOC_CAP_SPIRAM);
    if (!s_tx) { ESP_LOGE(TAG, "no PSRAM for the frame buffer"); return; }

    const char *pw = WEB_PASSWORD;
    if (pw[0]) {
        char cred[96], b[96];
        snprintf(cred, sizeof cred, "%s:%s", WEB_USER, pw);
        b64_encode((const uint8_t *)cred, strlen(cred), b);
        snprintf(s_auth, sizeof s_auth, "Basic %s", b);
        s_auth_on = true;
    } else {
        ESP_LOGW(TAG, "WEB_PASSWORD is empty: web page is view-only, remote control disabled");
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 5;
    cfg.recv_wait_timeout = 5;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return;
    }
    static const httpd_uri_t u_page  = { .uri = "/",      .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t u_frame = { .uri = "/frame", .method = HTTP_GET, .handler = frame_get };
    httpd_register_uri_handler(s_httpd, &u_page);
    static const httpd_uri_t u_touch = { .uri = "/t",     .method = HTTP_POST, .handler = touch_post };
    httpd_register_uri_handler(s_httpd, &u_frame);
    httpd_register_uri_handler(s_httpd, &u_touch);
    ESP_LOGI(TAG, "web mirror listening on port 80");
}
