#include "app_webui.h"
#include "app_driver.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <nvs.h>
#include <esp_http_server.h>
#include <mdns.h>


static const char *TAG = "app_webui";

/* The product's settings page (app_webui_set_settings_handler). */
static esp_err_t (*s_settings_handler)(void) = NULL;

void app_webui_set_settings_handler(esp_err_t (*handler)(void))
{
    s_settings_handler = handler;
}

/* vtype: 0 bool, 1 u8, 2 i16, 3 u16, 4 u32 (shared tag vocabulary). */
typedef struct {
    const char *label;
    const char *kind;   /* toggle | slider | value | binary */
    app_driver_param_id_t param;
    const char *param_name;
    uint8_t vtype;
    int32_t min;
    int32_t max;
    int32_t scale;      /* UI divides the raw value by this for display */
} zc_webui_entity_t;

static const zc_webui_entity_t s_entities[] = {

};
#define ZC_WEBUI_N (sizeof(s_entities) / sizeof(s_entities[0]))

/* The whole UI: plain-ASCII vanilla JS, no external assets. DOM rebuilds on
 * each poll unless a control is focused (mid-drag). */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>ZeroCode</title><style>"
"body{font-family:system-ui;margin:0;background:#141414;color:#eee}"
"h1{font-size:17px;padding:14px 16px;margin:0;border-bottom:1px solid #333;color:#e0a06a}"
".c{padding:14px 16px;border-bottom:1px solid #262626;display:flex;justify-content:space-between;align-items:center;gap:12px}"
".v{color:#7fd4a3;font-variant-numeric:tabular-nums}"
".on{color:#7fd4a3}.off{color:#888}"
"input[type=range]{width:46vw}"
"</style></head><body><h1>ZeroCode</h1><div id=root></div><script>"
"function post(p,v){fetch('/api/set?param='+encodeURIComponent(p)+'&value='+v,{method:'POST',headers:{'X-ZC-Request':'1'}});}"
"function row(e){"
"var d=document.createElement('div');d.className='c';"
"var l=document.createElement('span');l.textContent=e.label;d.appendChild(l);"
"if(e.kind=='toggle'){var i=document.createElement('input');i.type='checkbox';i.checked=e.value!=0;"
"i.onchange=function(){post(e.param,i.checked?1:0);};d.appendChild(i);}"
"else if(e.kind=='slider'){var w=document.createElement('span');"
"var i=document.createElement('input');i.type='range';i.min=e.min;i.max=e.max;i.value=e.value;"
"i.onchange=function(){post(e.param,i.value);};w.appendChild(i);d.appendChild(w);}"
"else if(e.kind=='binary'){var s=document.createElement('span');"
"s.textContent=e.value!=0?'ACTIVE':'clear';s.className=e.value!=0?'on':'off';d.appendChild(s);}"
"else{var s=document.createElement('span');s.className='v';"
"s.textContent=(e.scale>1?(e.value/e.scale).toFixed(2):e.value);d.appendChild(s);}"
"return d;}"
"function render(st){if(document.activeElement&&document.activeElement.tagName=='INPUT')return;"
"var r=document.getElementById('root');r.textContent='';"
"st.entities.forEach(function(e){r.appendChild(row(e));});}"
"function tick(){fetch('/api/state').then(function(r){return r.json();}).then(render).catch(function(){});}"
"tick();setInterval(tick,2500);"
"</script></body></html>";

static int32_t entity_value(const zc_webui_entity_t *e)
{
    app_driver_param_val_t v = {};
    app_driver_get_param(e->param, &v);
    switch (e->vtype) {
        case 0: return v.b ? 1 : 0;
        case 1: return v.u8;
        case 2: return v.i16;
        case 3: return v.u16;
        default: return (int32_t)v.u32;
    }
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t state_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"entities\":[");
    char buf[240];
    for (size_t i = 0; i < ZC_WEBUI_N; i++) {
        const zc_webui_entity_t *e = &s_entities[i];
        snprintf(buf, sizeof(buf),
                 "%s{\"label\":\"%s\",\"kind\":\"%s\",\"param\":\"%s\","
                 "\"min\":%ld,\"max\":%ld,\"scale\":%ld,\"value\":%ld}",
                 i ? "," : "", e->label, e->kind, e->param_name,
                 (long)e->min, (long)e->max, (long)e->scale, (long)entity_value(e));
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* POST only, and only with the X-ZC-Request header the page itself sends.
 * As a GET, any page the user opened anywhere could drive the device with an
 * <img src=…>; a custom header makes a cross-site request preflighted
 * (OPTIONS), which this server never answers, so only this page gets through. */
static esp_err_t set_post(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "X-ZC-Request") == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "use the device page");
        return ESP_FAIL;
    }
    char query[128] = {0};
    char pname[64] = {0};
    char valstr[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "param", pname, sizeof(pname)) != ESP_OK ||
        httpd_query_key_value(query, "value", valstr, sizeof(valstr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "param and value required");
        return ESP_FAIL;
    }
    long raw = strtol(valstr, NULL, 10);
    for (size_t i = 0; i < ZC_WEBUI_N; i++) {
        const zc_webui_entity_t *e = &s_entities[i];
        if (strcmp(e->param_name, pname) != 0) continue;
        if (raw < e->min) raw = e->min;
        if (raw > e->max) raw = e->max;
        app_driver_param_val_t v = {};
        switch (e->vtype) {
            case 0: v.b = raw != 0; break;
            case 1: v.u8 = (uint8_t)raw; break;
            case 2: v.i16 = (int16_t)raw; break;
            case 3: v.u16 = (uint16_t)raw; break;
            default: v.u32 = (uint32_t)raw; break;
        }
        app_driver_set_param(e->param, v, APP_DRIVER_SOURCE_LOCAL);
        httpd_resp_sendstr(req, "ok");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown param");
    return ESP_FAIL;
}

/* Bridge between esp_http_server and the product's settings page, which is
 * written against nothing but this file: it reads the request body through
 * app_webui_read_request() and answers in chunks with app_webui_chunk_begin(),
 * app_webui_send_chunk() and app_webui_chunk_end(). */
static httpd_req_t *s_settings_req = NULL;   /* the request being served */

static esp_err_t settings_page(httpd_req_t *req)
{
    s_settings_req = req;
    return s_settings_handler ? s_settings_handler() : ESP_FAIL;
}

static esp_err_t settings_apply(httpd_req_t *req)
{
    s_settings_req = req;
    if (s_settings_handler == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    return s_settings_handler();
}

/* ── The product's settings page: the small API it is written against ── */

void app_webui_chunk_begin(const char *content_type)
{
    if (s_settings_req == NULL) return;
    httpd_resp_set_type(s_settings_req, content_type);
}

void app_webui_send_chunk(const char *text)
{
    if (s_settings_req == NULL || text == NULL) return;
    httpd_resp_send_chunk(s_settings_req, text, HTTPD_RESP_USE_STRLEN);
}

void app_webui_chunk_end(void)
{
    if (s_settings_req == NULL) return;
    httpd_resp_send_chunk(s_settings_req, NULL, 0);
}

int app_webui_read_request(char *buf, size_t len)
{
    if (s_settings_req == NULL || buf == NULL || len == 0) return -1;
    return httpd_req_recv(s_settings_req, buf, len);
}

static void webui_start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const char *uris[] = { "/", "/api/state", "/api/set" };
    esp_err_t (*handlers[])(httpd_req_t *) = { index_get, state_get, set_post };
    const httpd_method_t methods[] = { HTTP_GET, HTTP_GET, HTTP_POST };
    for (int i = 0; i < 3; i++) {
        httpd_uri_t u = {};
        u.uri = uris[i];
        u.method = methods[i];
        u.handler = handlers[i];
        httpd_register_uri_handler(server, &u);
    }

    /* The product's own settings page: one URI serving both the form (GET) and
     * its submission (POST), so it never knows esp_http_server's types. */
    if (s_settings_handler != NULL) {
        httpd_uri_t page = {};
        page.uri = "/settings";
        page.method = HTTP_GET;
        page.handler = settings_page;
        httpd_register_uri_handler(server, &page);
        httpd_uri_t apply = {};
        apply.uri = "/settings";
        apply.method = HTTP_POST;
        apply.handler = settings_apply;
        httpd_register_uri_handler(server, &apply);
    }
}

esp_err_t app_webui_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[24];
    snprintf(host, sizeof(host), "zerocode-%02x%02x", mac[4], mac[5]);

    /* Shared-network mode: another framework owns Wi-Fi (a station); the
     * server just binds on it. The netif layer and the default loop are not
     * the radio — both are idempotent, and httpd/mdns need them even when the
     * owner is listed after us or (aws_iot) brings the radio up later. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    /* mDNS is the nicest way to reach the settings page, not the only one: the
     * panel's IP (or http://192.168.4.1/ on its own SoftAP before it is set up)
     * works just as well. On this board the mDNS service task ran out of stack
     * during its periodic work (CONFIG_MDNS_TASK_STACK_SIZE is raised in
     * sdkconfig.product); a library that cannot come up must still cost only
     * the hostname, never the boot or the panel's own page. */
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set("ZeroCode device");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS unavailable (%s) — reach the panel by its IP", esp_err_to_name(mdns_err));
    }

    webui_start_server();


    ESP_LOGI(TAG, "Web UI ready — http://%s.local/", host);
    return ESP_OK;
}
