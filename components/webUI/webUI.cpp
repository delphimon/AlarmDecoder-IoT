/**
 *  @file    webUI.cpp
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    07/17/2021
 *
 *  @brief WEB server for user interface to alarm system
 *
 *  @copyright Copyright (C) 2021 Nu Tech Software Solutions, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Disable via sdkconfig
#if CONFIG_AD2IOT_WEBSERVER_UI
static const char *TAG = "WEBUI";

// AlarmDecoder std includes
#include <algorithm>
#include <ctime>
#include "alarmdecoder_main.h"

// esp component includes
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "usdupdate.h"

// specific includes

/* Constants that aren't configurable in menuconfig */
//#define DEBUG_WEBUI
#define PORT 10000
#define MAX_CLIENTS 4
#define WEBUI_TLS_MAX_CLIENTS 2
#define MAX_FIFO_BUFFERS 30
#define MAXCONNECTIONS MAX_CLIENTS+1
#define WEBUI_DOC_ROOT         "/www"
#define WEBUI_COMMAND          "webui"
#define WEBUI_SUBCMD_ENABLE    "enable"
#define WEBUI_SUBCMD_ACL       "acl"
#define WEBUI_SUBCMD_SSL       "ssl"
#define WEBUI_SUBCMD_SSLCERT   "sslcert"
#define WEBUI_SUBCMD_SSLKEY    "sslkey"
#define WEBUI_SUBCMD_USER      "user"
#define WEBUI_SUBCMD_PASSWORD  "password"

#define WEBUI_CONFIG_SECTION  "webui"

#define WEBUI_DEFAULT_ACL "127.0.0.1"
#define WEBUI_HISTORY_SIZE 64
#define WEBUI_WS_MAX_PAYLOAD 256
#define WEBUI_CONFIG_MAX_BYTES (64 * 1024)
#define WEBUI_CONFIG_LINE_MAX 1024
#define WEBUI_DEFAULT_SSL_CERT "certs/fullchain.pem"
#define WEBUI_DEFAULT_SSL_KEY  "certs/privkey.pem"
#define WEBUI_AUTH_USER_MAX 32
#define WEBUI_AUTH_PASSWORD_MIN 12
#define WEBUI_AUTH_PASSWORD_MAX 64
#define WEBUI_AUTH_HEADER_MAX 160
#define WEBUI_COOKIE_HEADER_MAX 512
#define WEBUI_SESSION_COOKIE "AD2IOT_SESSION"

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (255)

/* Component-scoped helper avoids colliding with FatFS/system headers. */
#define WEBUI_MIN(x, y) (((x) < (y)) ? (x) : (y))

// Global handle to httpd server
httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
httpd_handle_t server = nullptr;
static bool webui_tls_enabled = false;
static bool webui_server_uses_tls = false;
static std::string webui_tls_cert_setting = WEBUI_DEFAULT_SSL_CERT;
static std::string webui_tls_key_setting = WEBUI_DEFAULT_SSL_KEY;
static unsigned webui_tls_sessions = 0;
static size_t webui_tls_start_free_heap = 0;
static std::string webui_basic_authorization;
static std::string webui_session_token;
static std::string webui_session_cookie_http;
static std::string webui_session_cookie_https;

/* ACL control */
ad2_acl_check webui_acl;

/**
 * WebUI command list and enum.
 */
char * WEBUI_SUBCMD [] = {
    (char*)WEBUI_SUBCMD_ENABLE,
    (char*)WEBUI_SUBCMD_ACL,
    (char*)WEBUI_SUBCMD_SSL,
    (char*)WEBUI_SUBCMD_SSLCERT,
    (char*)WEBUI_SUBCMD_SSLKEY,
    (char*)WEBUI_SUBCMD_USER,
    (char*)WEBUI_SUBCMD_PASSWORD,
    0 // EOF
};

enum {
    WEBUI_SUBCMD_ENABLE_ID = 0,
    WEBUI_SUBCMD_ACL_ID,
    WEBUI_SUBCMD_SSL_ID,
    WEBUI_SUBCMD_SSLCERT_ID,
    WEBUI_SUBCMD_SSLKEY_ID,
    WEBUI_SUBCMD_USER_ID,
    WEBUI_SUBCMD_PASSWORD_ID,
};

/**
 * @brief websocket session storage structure.
 */
struct ws_session_storage {
    int partID;
    int codeID;
    bool authenticated;
    bool synced;
};

/**
 * A bounded, reboot-scoped activity log.  Fixed-size fields keep memory use
 * predictable on the ESP32 and avoid retaining panel protocol messages.
 */
struct webui_history_entry {
    uint64_t sequence;
    uint64_t uptime_ms;
    int partition;
    int zone;
    char event[24];
    char alpha[64];
};

static webui_history_entry webui_history[WEBUI_HISTORY_SIZE];
static size_t webui_history_head = 0;
static size_t webui_history_count = 0;
static uint64_t webui_history_sequence = 0;
static SemaphoreHandle_t webui_history_mutex = nullptr;

// C++

// Include template engine from
//   https://github.com/full-stack-ex/tiny-template-engine-arduino
// Currently not functional with esp-idf development platform only Arduino so some mods were needed.
#include "TinyTemplateEngine.h"
#include "TinyTemplateEngineFileReader.h"

static bool has_file_extension(const char *filename, const char *extension)
{
    size_t filename_len = strlen(filename);
    size_t extension_len = strlen(extension);
    return filename_len >= extension_len &&
           strcasecmp(filename + filename_len - extension_len, extension) == 0;
}

static cJSON *webui_state_json(AD2PartitionState *s, const char *event)
{
    cJSON *root = ad2_get_partition_state_json(s);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(hal_uptime_us() / 1000));
    if (s) {
        cJSON_AddNumberToObject(root, "partition", s->partition);
        cJSON_AddNumberToObject(root, "zone", s->zone);
    }
    cJSON_AddItemToObject(root, "zone_alerts", ad2_get_partition_zone_alerts_json(s));
    return root;
}

static void webui_add_history(AD2PartitionState *s, int event_id)
{
    if (!s || !webui_history_mutex) {
        return;
    }

    if (xSemaphoreTake(webui_history_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        webui_history_entry &entry = webui_history[webui_history_head];
        entry.sequence = ++webui_history_sequence;
        entry.uptime_ms = hal_uptime_us() / 1000;
        entry.partition = s->partition;
        entry.zone = s->zone;
        strlcpy(entry.event, AD2Parse.event_str[event_id].c_str(), sizeof(entry.event));
        strlcpy(entry.alpha, s->last_alpha_message.c_str(), sizeof(entry.alpha));

        webui_history_head = (webui_history_head + 1) % WEBUI_HISTORY_SIZE;
        if (webui_history_count < WEBUI_HISTORY_SIZE) {
            webui_history_count++;
        }
        xSemaphoreGive(webui_history_mutex);
    }
}

static cJSON *webui_history_json(size_t limit, int partition)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    cJSON_AddStringToObject(root, "event", "HISTORY");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(hal_uptime_us() / 1000));
    cJSON_AddItemToObject(root, "items", items);

    if (!webui_history_mutex ||
            xSemaphoreTake(webui_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return root;
    }

    size_t wanted = WEBUI_MIN(limit, (size_t)WEBUI_HISTORY_SIZE);
    size_t added = 0;
    for (size_t offset = 0; offset < webui_history_count && added < wanted; offset++) {
        size_t index = (webui_history_head + WEBUI_HISTORY_SIZE - 1 - offset) % WEBUI_HISTORY_SIZE;
        const webui_history_entry &entry = webui_history[index];
        if (partition >= 0 && entry.partition != partition) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "sequence", (double)entry.sequence);
        cJSON_AddNumberToObject(item, "uptime_ms", (double)entry.uptime_ms);
        cJSON_AddNumberToObject(item, "partition", entry.partition);
        cJSON_AddNumberToObject(item, "zone", entry.zone);
        cJSON_AddStringToObject(item, "event", entry.event);
        cJSON_AddStringToObject(item, "alpha", entry.alpha);
        cJSON_AddItemToArray(items, item);
        added++;
    }
    xSemaphoreGive(webui_history_mutex);
    return root;
}

/**
 * generate uptime string
 */
void uptimeString(std::string &tstring)
{
    // 64bit milliseconds since boot
    uint64_t ms = hal_uptime_us() / 1000;
    // seconds
    uint32_t s = ms / 1000;
    // days
    uint32_t d = s / 86400;
    s %= 86400;
    // hours
    uint32_t h = s / 3600;
    s %= 3600;
    // minutes
    uint32_t m = s / 60;
    s %= 60;
    char fbuff[255];
    snprintf(fbuff, sizeof(fbuff), "%04lud:%02luh:%02lum:%02lus", d, h, m, s);
    tstring = fbuff;
}

/**
 * @brief Given a file name set the request session response type.
 *
 * @param [in]httpd_req_t *req
 * @param [in]const char *filename
 *
 * @return esp_err_t
 *
 */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    /* Limited set of types hard coded here */
    if (has_file_extension(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (has_file_extension(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (has_file_extension(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (has_file_extension(filename, ".json")) {
        return httpd_resp_set_type(req, "application/json");
    } else if (has_file_extension(filename, ".jpeg") || has_file_extension(filename, ".jpg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (has_file_extension(filename, ".png")) {
        return httpd_resp_set_type(req, "image/png");
    } else if (has_file_extension(filename, ".svg")) {
        return httpd_resp_set_type(req, "image/svg+xml");
    } else if (has_file_extension(filename, ".gz")) {
        return httpd_resp_set_type(req, "application/x-gzip");
    } else if (has_file_extension(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/**
 * @brief Copies the full path into destination buffer and returns
 * pointer to relative path after the base_path.
 *
 * @param [in]char *dest
 * @param [in]const char *base_path
 * @param [in]const char *uri
 * @param [in]size_t destsize
 *
 * @return stat const char*
 *
 */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = WEBUI_MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = WEBUI_MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/**
 * @brief Free session context memory.
 *
 * @param [in]void *ctx.
 *
 */
void free_ws_session_storage(void *ctx)
{
    // sanity check.
    if (ctx) {
        free(ctx);
    }
}

static bool webui_request_allowed(httpd_req_t *req)
{
    std::string ip;
    hal_get_socket_client_ip(httpd_req_to_sockfd(req), ip);
    if (webui_acl.find(ip)) {
        return true;
    }
    ESP_LOGW(TAG, "Rejecting request from '%s'", ip.c_str());
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Access denied by Web UI ACL");
    return false;
}

static bool webui_secure_equal(const std::string &left, const std::string &right)
{
    if (left.length() != right.length()) {
        return false;
    }
    unsigned char difference = 0;
    for (size_t index = 0; index < left.length(); index++) {
        difference |= (unsigned char)(left[index] ^ right[index]);
    }
    return difference == 0;
}

static bool webui_valid_credentials(const std::string &user, const std::string &password)
{
    if (user.empty() || user.length() > WEBUI_AUTH_USER_MAX ||
            password.length() < WEBUI_AUTH_PASSWORD_MIN ||
            password.length() > WEBUI_AUTH_PASSWORD_MAX) {
        return false;
    }
    for (char value : user) {
        const unsigned char byte = (unsigned char)value;
        if (byte < 0x21 || byte > 0x7e || value == ':') {
            return false;
        }
    }
    for (char value : password) {
        const unsigned char byte = (unsigned char)value;
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool webui_load_credentials()
{
    std::string user;
    std::string password;
    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_USER, user);
    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_PASSWORD, password);
    if (!webui_valid_credentials(user, password)) {
        return false;
    }
    webui_basic_authorization = "Basic " + ad2_make_basic_auth_string(user, password);

    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    char token[(sizeof(random_bytes) * 2) + 1];
    for (size_t index = 0; index < sizeof(random_bytes); index++) {
        snprintf(token + (index * 2), 3, "%02x", random_bytes[index]);
    }
    token[sizeof(token) - 1] = '\0';
    webui_session_token = token;
    webui_session_cookie_http = std::string(WEBUI_SESSION_COOKIE) + "=" + webui_session_token +
                                "; Path=/; HttpOnly; SameSite=Strict";
    webui_session_cookie_https = webui_session_cookie_http + "; Secure";
    return true;
}

static bool webui_get_header(httpd_req_t *req, const char *name, size_t maximum,
                             std::string &value)
{
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    if (!length || length > maximum) {
        return false;
    }
    std::vector<char> buffer(length + 1, 0);
    if (httpd_req_get_hdr_value_str(req, name, buffer.data(), buffer.size()) != ESP_OK) {
        return false;
    }
    value.assign(buffer.data(), length);
    return true;
}

static bool webui_session_cookie_valid(httpd_req_t *req)
{
    std::string cookies;
    if (!webui_get_header(req, "Cookie", WEBUI_COOKIE_HEADER_MAX, cookies)) {
        return false;
    }
    const std::string marker = std::string(WEBUI_SESSION_COOKIE) + "=";
    size_t cursor = 0;
    while (cursor < cookies.length()) {
        while (cursor < cookies.length() && (cookies[cursor] == ' ' || cookies[cursor] == ';')) {
            cursor++;
        }
        size_t end = cookies.find(';', cursor);
        if (end == std::string::npos) {
            end = cookies.length();
        }
        const std::string item = cookies.substr(cursor, end - cursor);
        if (item.rfind(marker, 0) == 0 &&
                webui_secure_equal(item.substr(marker.length()), webui_session_token)) {
            return true;
        }
        cursor = end + 1;
    }
    return false;
}

static void webui_set_session_cookie(httpd_req_t *req)
{
    // ESP-IDF stores the header value pointer until the response body begins,
    // so this must refer to process-lifetime storage rather than a local string.
    const std::string &cookie = webui_server_uses_tls ?
                                webui_session_cookie_https : webui_session_cookie_http;
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
}

static bool webui_authenticate_request(httpd_req_t *req)
{
    if (webui_session_cookie_valid(req)) {
        return true;
    }

    std::string authorization;
    if (webui_get_header(req, "Authorization", WEBUI_AUTH_HEADER_MAX, authorization) &&
            webui_secure_equal(authorization, webui_basic_authorization)) {
        webui_set_session_cookie(req);
        return true;
    }

    std::string ip;
    hal_get_socket_client_ip(httpd_req_to_sockfd(req), ip);
    ESP_LOGW(TAG, "Rejecting unauthenticated request from '%s'", ip.c_str());
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AD2IoT\", charset=\"UTF-8\"");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return false;
}

static bool webui_authorize_request(httpd_req_t *req)
{
    return webui_request_allowed(req) && webui_authenticate_request(req);
}

static bool webui_origin_allowed(httpd_req_t *req)
{
    std::string origin;
    if (!webui_get_header(req, "Origin", 192, origin)) {
        // Non-browser clients commonly omit Origin. Browser WebSocket and fetch
        // requests include it, so reject any cross-origin browser request below.
        return true;
    }
    std::string host;
    if (!webui_get_header(req, "Host", 128, host)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing request host");
        return false;
    }
    const std::string expected = std::string(webui_server_uses_tls ? "https://" : "http://") + host;
    if (webui_secure_equal(origin, expected)) {
        return true;
    }
    ESP_LOGW(TAG, "Rejecting cross-origin Web UI request");
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-origin request denied");
    return false;
}

#if CONFIG_HTTPD_WS_SUPPORT
/**
 * @brief Send current alarm state to web socket connection. Lookup
 * ws_session_storage for this connection from user_ctx
 *
 * @param [in]void * socket fd for this connection cast as void *.
 *
 */
static void ws_alarmstate_async_send(void *arg)
{
    int wsfd = (int)arg;
    if (wsfd) {
        if (server && hal_get_network_connected()) {
            // lookup session context from socket id.
            struct ws_session_storage *sess = (ws_session_storage *)httpd_sess_get_ctx(server, wsfd);
            if (sess) {
                // get the partition state based upon the partition ID on the AD2IoT firmware.
                AD2PartitionState *s = ad2_get_partition_state(sess->partID);
                if (s) {
                    cJSON *root = webui_state_json(s, "SYNC");
                    char *sys_info = cJSON_PrintUnformatted(root);
                    if (sys_info) {
                        httpd_ws_frame_t ws_pkt;
                        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                        ws_pkt.payload = (uint8_t*)sys_info;
                        ws_pkt.len = strlen(sys_info);
                        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                        httpd_ws_send_frame_async(server, wsfd, &ws_pkt);
                        cJSON_free(sys_info);
                    }
                    cJSON_Delete(root);
                }
            }
        }
    }
}

static esp_err_t webui_ws_send_text(httpd_req_t *req, const char *text, size_t length)
{
    httpd_ws_frame_t response;
    memset(&response, 0, sizeof(response));
    response.type = HTTPD_WS_TYPE_TEXT;
    response.payload = (uint8_t *)text;
    response.len = length;
    return httpd_ws_send_frame(req, &response);
}

static esp_err_t webui_ws_send_text(httpd_req_t *req, const std::string &text)
{
    return webui_ws_send_text(req, text.c_str(), text.length());
}

static esp_err_t webui_ws_error(httpd_req_t *req, const char *message)
{
    return webui_ws_send_text(req, std::string("!ERROR:") + message);
}

/**
 * @brief HTTP GET websocket handler for api access.
 *
 * @param [in]httpd_req_t *
 *
 * @return esp_err_t
 *
 */
esp_err_t ad2ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!webui_authorize_request(req) || !webui_origin_allowed(req)) {
            return ESP_FAIL;
        }
        req->sess_ctx = calloc(1, sizeof(ws_session_storage));
        req->free_ctx = free_ws_session_storage;
        if (!req->sess_ctx) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Unable to allocate WebSocket session");
        }
        ((ws_session_storage *)req->sess_ctx)->authenticated = true;
        return ESP_OK;
    }

    if (!webui_request_allowed(req)) {
        return ESP_FAIL;
    }
    ws_session_storage *session = (ws_session_storage *)req->sess_ctx;
    if (!session || !session->authenticated) {
        return webui_ws_error(req, "Authenticated session required");
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        return ret;
    }

    if (ws_pkt.len == 0 || ws_pkt.len > WEBUI_WS_MAX_PAYLOAD) {
        return webui_ws_error(req, "Invalid frame length");
    }

    uint8_t rx_buf[WEBUI_WS_MAX_PAYLOAD + 1] = { 0 };
    ws_pkt.payload = rx_buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, WEBUI_WS_MAX_PAYLOAD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame payload failed with %d", ret);
        return ret;
    }
    rx_buf[ws_pkt.len] = '\0';

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        std::string message((char *)rx_buf, ws_pkt.len);

        // Register and return current state.
        std::string key_sync = "!SYNC:";
        if(message.rfind(key_sync, 0) == 0) {
            std::string args = message.substr(key_sync.length());
            std::vector<std::string> args_v;
            ad2_tokenize(args, ",", args_v);
            if (args_v.size() != 2) {
                return webui_ws_error(req, "SYNC requires partition and code slots");
            }
            char *end = nullptr;
            long partID = strtol(args_v[0].c_str(), &end, 10);
            if (*end != '\0' || partID < 0 || partID > AD2_MAX_PARTITION) {
                return webui_ws_error(req, "Invalid partition slot");
            }
            long codeID = strtol(args_v[1].c_str(), &end, 10);
            if (*end != '\0' || codeID < 0 || codeID > AD2_MAX_CODE) {
                return webui_ws_error(req, "Invalid code slot");
            }

            session->codeID = (int)codeID;
            session->partID = (int)partID;
            session->synced = true;

            // trigger an async send using httpd_queue_work
            return httpd_queue_work(req->handle, ws_alarmstate_async_send, (void *)httpd_req_to_sockfd(req));
        }

        std::string key_ping = "!PING:";
        if(message.rfind(key_ping, 0) == 0) {
            // send back a !PONG reply
            return webui_ws_send_text(req, "!PONG:00000000");
        }

        std::string key_history = "!HISTORY:";
        if (message.rfind(key_history, 0) == 0) {
            long limit = strtol(message.substr(key_history.length()).c_str(), nullptr, 10);
            if (limit < 1 || limit > WEBUI_HISTORY_SIZE) {
                limit = WEBUI_HISTORY_SIZE;
            }
            cJSON *root = webui_history_json((size_t)limit, -1);
            char *history = cJSON_PrintUnformatted(root);
            esp_err_t result;
            if (history) {
                result = webui_ws_send_text(req, history, strlen(history));
            } else {
                const char *empty_history = "{\"event\":\"HISTORY\",\"items\":[]}";
                result = webui_ws_send_text(req, empty_history, strlen(empty_history));
            }
            cJSON_free(history);
            cJSON_Delete(root);
            return result;
        }

        std::string key_send = "!SEND:";
        if(message.rfind(key_send, 0) == 0) {
            if (!session->synced) {
                return webui_ws_error(req, "SYNC is required before commands");
            }
            std::string sendbuf = message.substr(key_send.length());
            int codeID = session->codeID;
            int partID = session->partID;

            if (sendbuf == "<DISARM>") {
                ad2_disarm(codeID, partID);
            } else if (sendbuf == "<STAY>") {
                ad2_arm_stay(codeID, partID);
            } else if (sendbuf == "<AWAY>") {
                ad2_arm_away(codeID, partID);
            } else if (sendbuf == "<EXIT>") {
                ad2_exit_now(partID);
            } else if (sendbuf == "<CHIME>") {
                ad2_chime_toggle(codeID, partID);
            } else if (sendbuf == "<AUX_ALARM>") {
                ad2_aux_alarm(partID);
            } else if (sendbuf == "<PANIC_ALARM>") {
                ad2_panic_alarm(partID);
            } else if (sendbuf == "<FIRE_ALARM>") {
                ad2_fire_alarm(partID);
            } else if (sendbuf.rfind("<BYPASS>", 0) == 0) {
                std::string zone_text = sendbuf.substr(8);
                char *end = nullptr;
                long zone = strtol(zone_text.c_str(), &end, 10);
                if (*end != '\0' || zone < 1 || zone > AD2_MAX_ZONES) {
                    return webui_ws_error(req, "Invalid bypass zone");
                }
                ad2_bypass_zone(codeID, partID, (uint8_t)zone);
            } else if (sendbuf.rfind("<KEYS>", 0) == 0) {
                if (!ad2_keypad_send(sendbuf.substr(6), partID)) {
                    return webui_ws_error(req, "Invalid keypad input or partition");
                }
            } else {
                ESP_LOGW(TAG, "Unknown websocket command '%s'", sendbuf.c_str());
                return webui_ws_error(req, "Unknown command");
            }
            return ESP_OK;
        }

        return webui_ws_error(req, "Unknown request");
    }
    return webui_ws_error(req, "Text frames only");
}
#endif

static int webui_query_int(httpd_req_t *req, const char *name, int default_value)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (!query_len || query_len > 128) {
        return default_value;
    }
    char query[129] = { 0 };
    char value[16] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, name, value, sizeof(value)) != ESP_OK) {
        return default_value;
    }
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    return (*end == '\0') ? (int)parsed : default_value;
}

static bool webui_query_value(httpd_req_t *req, const char *name, std::string &value)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (!query_len || query_len > 128) {
        return false;
    }
    char query[129] = { 0 };
    char result[32] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, name, result, sizeof(result)) != ESP_OK) {
        return false;
    }
    value = result;
    return true;
}

static esp_err_t webui_send_json_response(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to encode JSON");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

static const char *webui_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "Power-on reset";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Software panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    case ESP_RST_UNKNOWN:
    default: return "Unknown";
    }
}

static void webui_tls_session_callback(esp_https_server_user_cb_arg_t *arg)
{
    if (!arg) {
        return;
    }
    if (arg->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) {
        webui_tls_sessions++;
    } else if (arg->user_cb_state == HTTPD_SSL_USER_CB_SESS_CLOSE && webui_tls_sessions > 0) {
        webui_tls_sessions--;
    }
    ESP_LOGI(TAG, "TLS session %s (%u/%u): heap=%u, minimum=%u, largest=%u",
             arg->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE ? "opened" : "closed",
             webui_tls_sessions, WEBUI_TLS_MAX_CLIENTS,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/** Read-only current state API: GET /api/state?partition=0 */
static esp_err_t webui_state_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    int partID = webui_query_int(req, "partition", 0);
    if (partID < 0 || partID > AD2_MAX_PARTITION) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid partition slot");
    }
    AD2PartitionState *s = ad2_get_partition_state(partID);
    return webui_send_json_response(req, webui_state_json(s, "SYNC"));
}

/** Reboot-scoped activity API: GET /api/history?limit=64&partition=1 */
static esp_err_t webui_history_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    int limit = webui_query_int(req, "limit", WEBUI_HISTORY_SIZE);
    int partition = webui_query_int(req, "partition", -1);
    if (limit < 1 || limit > WEBUI_HISTORY_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Limit must be between 1 and 64");
    }
    return webui_send_json_response(req, webui_history_json((size_t)limit, partition));
}

static void webui_add_file_status(cJSON *parent, const char *name, const char *path)
{
    struct stat info;
    cJSON *file = cJSON_CreateObject();
    const bool present = stat(path, &info) == 0 && S_ISREG(info.st_mode);
    cJSON_AddBoolToObject(file, "present", present);
    cJSON_AddNumberToObject(file, "size_bytes", present ? (double)info.st_size : 0);
    cJSON_AddItemToObject(parent, name, file);
}

/** Build, network, storage, and runtime status: GET /api/system */
static esp_err_t webui_system_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "firmware_version", ad2_firmware_version());
    cJSON_AddStringToObject(root, "build_flags", FIRMWARE_BUILDFLAGS);
    cJSON_AddStringToObject(root, "build_date", app ? app->date : "Unknown");
    cJSON_AddStringToObject(root, "build_time", app ? app->time : "Unknown");
    cJSON_AddStringToObject(root, "idf_version", app ? app->idf_ver : "Unknown");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(hal_uptime_us() / 1000));

    std::string network_args;
    const char network_mode = ad2_get_network_mode(network_args);
    const char *network_mode_name = network_mode == 'W' ? "Wireless" :
                                    network_mode == 'E' ? "Ethernet" : "Disabled";
    std::string local_ip = "Unavailable";
    hal_get_socket_local_ip(httpd_req_to_sockfd(req), local_ip);
    cJSON *network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "mode", network_mode_name);
    cJSON_AddStringToObject(network, "ip_address", local_ip.c_str());
    cJSON_AddBoolToObject(network, "connected", hal_get_network_connected());
    cJSON_AddBoolToObject(network, "time_synchronized", hal_time_is_synchronized());
    cJSON_AddNumberToObject(network, "unix_time", (double)time(nullptr));
    cJSON_AddStringToObject(network, "web_protocol", webui_server_uses_tls ? "HTTPS" : "HTTP");
    cJSON_AddNumberToObject(network, "web_port", webui_server_uses_tls ? 443 : 80);
    cJSON_AddItemToObject(root, "network", network);

    cJSON *storage = cJSON_CreateObject();
    cJSON *sd = cJSON_CreateObject();
    cJSON_AddBoolToObject(sd, "mounted", g_uSD_mounted);
    uint64_t sd_total = 0;
    uint64_t sd_free = 0;
    const bool sd_info_ok = g_uSD_mounted &&
                            esp_vfs_fat_info("/" AD2_USD_MOUNT_POINT, &sd_total, &sd_free) == ESP_OK;
    cJSON_AddNumberToObject(sd, "total_bytes", sd_info_ok ? (double)sd_total : 0);
    cJSON_AddNumberToObject(sd, "free_bytes", sd_info_ok ? (double)sd_free : 0);
    webui_add_file_status(sd, "config", "/" AD2_USD_MOUNT_POINT AD2_CONFIG_FILE);
    webui_add_file_status(sd, "diagnostic_log", AD2_SD_LOG_PATH);
    webui_add_file_status(sd, "diagnostic_log_rotated", AD2_SD_LOG_OLD_PATH);
    bool sd_log_enabled = false;
    bool sd_log_active = false;
    uint32_t sd_log_dropped = 0;
    uint32_t sd_log_write_errors = 0;
    ad2_get_sd_logging_status(&sd_log_enabled, &sd_log_active,
                              &sd_log_dropped, &sd_log_write_errors);
    cJSON_AddBoolToObject(sd, "logging_enabled", sd_log_enabled);
    cJSON_AddBoolToObject(sd, "logging_active", sd_log_active);
    cJSON_AddNumberToObject(sd, "logging_dropped", sd_log_dropped);
    cJSON_AddNumberToObject(sd, "logging_write_errors", sd_log_write_errors);
    cJSON_AddItemToObject(storage, "sd_card", sd);

    cJSON *spiffs = cJSON_CreateObject();
    size_t spiffs_total = 0;
    size_t spiffs_used = 0;
    const bool spiffs_ok = esp_spiffs_info(AD2_SPIFFS_MOUNT_POINT, &spiffs_total, &spiffs_used) == ESP_OK;
    cJSON_AddBoolToObject(spiffs, "mounted", spiffs_ok);
    cJSON_AddNumberToObject(spiffs, "total_bytes", spiffs_ok ? (double)spiffs_total : 0);
    cJSON_AddNumberToObject(spiffs, "used_bytes", spiffs_ok ? (double)spiffs_used : 0);
    webui_add_file_status(spiffs, "config", "/" AD2_SPIFFS_MOUNT_POINT AD2_CONFIG_FILE);
    cJSON_AddItemToObject(storage, "spiffs", spiffs);
    cJSON_AddStringToObject(storage, "active_config_source", ad2_config_uses_sd() ? "SD card" : "SPIFFS");
    cJSON_AddItemToObject(root, "storage", storage);

    cJSON *memory = cJSON_CreateObject();
    cJSON_AddNumberToObject(memory, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(memory, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(memory, "largest_free_block_bytes",
                            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(memory, "internal_free_heap_bytes",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(memory, "tls_start_free_heap_bytes", webui_tls_start_free_heap);
    cJSON_AddItemToObject(root, "memory", memory);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *device = cJSON_CreateObject();
    std::string uuid;
    ad2_genUUID(0, uuid);
    cJSON_AddStringToObject(device, "uuid", uuid.c_str());
    cJSON_AddNumberToObject(device, "cpu_cores", chip.cores);
    cJSON_AddNumberToObject(device, "cpu_revision", chip.revision);
    cJSON_AddStringToObject(device, "last_reset_reason", webui_reset_reason_name(esp_reset_reason()));
    cJSON_AddStringToObject(device, "alarmdecoder_source", g_ad2_mode == 'S' ? "Network socket" :
                                                        g_ad2_mode == 'C' ? "Serial" : "Unavailable");
    cJSON_AddNumberToObject(device, "tls_sessions", webui_tls_sessions);
    cJSON_AddNumberToObject(device, "tls_session_limit",
                            webui_server_uses_tls ? WEBUI_TLS_MAX_CLIENTS : 0);
    cJSON_AddItemToObject(root, "device", device);

    return webui_send_json_response(req, root);
}

/** SD-card firmware availability and integrity: GET /api/firmware */
static esp_err_t webui_firmware_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed_version", ad2_firmware_version());
#if CONFIG_AD2IOT_USDUPDATE
    usd_firmware_status status;
    usd_get_firmware_status(&status);
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddBoolToObject(root, "sd_mounted", status.sd_mounted);
    cJSON_AddBoolToObject(root, "present", status.present);
    cJSON_AddBoolToObject(root, "valid", status.valid);
    cJSON_AddBoolToObject(root, "integrity_valid", status.integrity_valid);
    cJSON_AddBoolToObject(root, "version_policy_valid", status.version_policy_valid);
    cJSON_AddBoolToObject(root, "newer_version", status.newer_version);
    cJSON_AddBoolToObject(root, "same_version", status.same_version);
    cJSON_AddBoolToObject(root, "downgrade", status.downgrade);
    cJSON_AddBoolToObject(root, "update_in_progress", status.update_in_progress);
    cJSON_AddNumberToObject(root, "size_bytes", (double)status.size_bytes);
    cJSON_AddStringToObject(root, "version", status.version);
    cJSON_AddStringToObject(root, "project_name", status.project_name);
    cJSON_AddStringToObject(root, "build_date", status.build_date);
    cJSON_AddStringToObject(root, "build_time", status.build_time);
    cJSON_AddStringToObject(root, "error", status.error);
    cJSON_AddBoolToObject(root, "upgrade_available", status.valid && status.newer_version);
#else
    cJSON_AddBoolToObject(root, "supported", false);
    cJSON_AddStringToObject(root, "error", "SD firmware updates are disabled in this build");
#endif
    return webui_send_json_response(req, root);
}

static void webui_delayed_restart(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    hal_restart();
    vTaskDelete(NULL);
}

static esp_err_t webui_action_response(httpd_req_t *req, const char *action, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "accepted", true);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", message);
    httpd_resp_set_status(req, "202 Accepted");
    return webui_send_json_response(req, root);
}

/** Explicit maintenance action with a custom-header CSRF guard: POST /api/action */
static esp_err_t webui_action_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    if (!webui_origin_allowed(req)) {
        return ESP_OK;
    }
    char guard[16] = {0};
    const size_t guard_length = httpd_req_get_hdr_value_len(req, "X-AD2IoT-Action");
    if (guard_length == 0 || guard_length >= sizeof(guard) ||
            httpd_req_get_hdr_value_str(req, "X-AD2IoT-Action", guard, sizeof(guard)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing maintenance-action guard");
    }
    if (req->content_len < 2 || req->content_len > 96) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action request");
    }
    char body[97] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to read action request");
        }
        received += (size_t)count;
    }
    cJSON *json = cJSON_ParseWithLength(body, received);
    cJSON *action_item = json ? cJSON_GetObjectItemCaseSensitive(json, "action") : NULL;
    if (!cJSON_IsString(action_item) || !action_item->valuestring ||
            strcmp(guard, action_item->valuestring) != 0) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid maintenance action");
    }
    std::string action = action_item->valuestring;
    cJSON_Delete(json);

    if (action == "restart") {
        if (xTaskCreate(&webui_delayed_restart, "Web UI restart", 2048, NULL,
                        tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Unable to schedule restart");
        }
        return webui_action_response(req, "restart", "Device restart scheduled");
    }
    if (action == "upgradeusd") {
#if CONFIG_AD2IOT_USDUPDATE
        usd_firmware_status firmware;
        if (!usd_get_firmware_status(&firmware)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       firmware.error[0] ? firmware.error : "SD firmware is invalid");
        }
        if (!usd_start_update()) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "A firmware update is already running or could not start");
        }
        return webui_action_response(req, "upgradeusd",
                                     "SD firmware installation started; the device will restart after validation");
#else
        return httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED,
                                   "SD firmware updates are disabled in this build");
#endif
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown maintenance action");
}

static bool webui_sensitive_key(const std::string &section, const std::string &key)
{
    if (section == "code") {
        return true;
    }
    static const char *markers[] = {
        "password", "passwd", "secret", "token", "apikey", "api_key",
        "userkey", "authkey", "private_key", "credential", "sid"
    };
    for (const char *marker : markers) {
        if (key.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static void webui_redact_inline_value(std::string &line, const char *name)
{
    std::string lowered = line;
    ad2_lcase(lowered);
    std::string marker = std::string(name) + "=";
    size_t search_from = 0;
    while (true) {
        const size_t start = lowered.find(marker, search_from);
        if (start == std::string::npos) {
            break;
        }
        const size_t value_start = start + marker.length();
        size_t value_end = line.find_first_of("& \t\r\n", value_start);
        if (value_end == std::string::npos) {
            value_end = line.length();
        }
        line.replace(value_start, value_end - value_start, "[redacted]");
        lowered = line;
        ad2_lcase(lowered);
        search_from = value_start + strlen("[redacted]");
    }
}

static void webui_redact_config(std::string &contents)
{
    std::string section;
    std::vector<std::string> sensitive_values;
    size_t cursor = 0;
    while (cursor < contents.length()) {
        size_t end = contents.find('\n', cursor);
        const bool has_newline = end != std::string::npos;
        if (!has_newline) {
            end = contents.length();
        }
        std::string line = contents.substr(cursor, end - cursor);
        std::string parsed = line;
        ad2_trim(parsed);
        while (!parsed.empty() && (parsed[0] == '#' || parsed[0] == ';')) {
            parsed.erase(0, 1);
            ad2_trim(parsed);
        }
        if (parsed.length() > 2 && parsed.front() == '[' && parsed.back() == ']') {
            section = parsed.substr(1, parsed.length() - 2);
            ad2_lcase(section);
            ad2_trim(section);
        } else {
            const size_t equals = parsed.find('=');
            if (equals != std::string::npos) {
                std::string key = parsed.substr(0, equals);
                ad2_lcase(key);
                ad2_trim(key);
                if (webui_sensitive_key(section, key)) {
                    std::string value = parsed.substr(equals + 1);
                    ad2_trim(value);
                    // Scrub a configured secret wherever it is reused under a
                    // less descriptive key. Very short values are skipped to
                    // avoid making the diagnostic snapshot unreadable.
                    if (value.length() >= 4) {
                        sensitive_values.push_back(value);
                    }
                    const size_t original_equals = line.find('=');
                    if (original_equals != std::string::npos) {
                        line.erase(original_equals + 1);
                        line += " [redacted]";
                    }
                }
            }
        }

        webui_redact_inline_value(line, "password");
        webui_redact_inline_value(line, "token");
        std::string lowered = line;
        ad2_lcase(lowered);
        const size_t scheme = lowered.find("://");
        const size_t at = scheme == std::string::npos ? std::string::npos : line.find('@', scheme + 3);
        if (at != std::string::npos) {
            line.replace(scheme + 3, at - (scheme + 3), "[redacted]");
        }
        contents.replace(cursor, end - cursor, line);
        cursor += line.length() + (has_newline ? 1 : 0);
    }

    for (const std::string &value : sensitive_values) {
        size_t match = 0;
        while ((match = contents.find(value, match)) != std::string::npos) {
            contents.replace(match, value.length(), "[redacted]");
            match += strlen("[redacted]");
        }
    }
}

static int webui_read_config_line(FILE *file, std::string &line)
{
    char chunk[256];
    line.clear();
    while (fgets(chunk, sizeof(chunk), file)) {
        const size_t count = strlen(chunk);
        line.append(chunk, count);
        if (line.length() > WEBUI_CONFIG_LINE_MAX) {
            return -1;
        }
        if ((count > 0 && chunk[count - 1] == '\n') || count < sizeof(chunk) - 1) {
            return 1;
        }
    }
    if (ferror(file)) {
        return -1;
    }
    return line.empty() ? 0 : 1;
}

static void webui_collect_sensitive_config_value(const std::string &line,
        std::string &section, std::vector<std::string> &values)
{
    std::string parsed = line;
    ad2_trim(parsed);
    while (!parsed.empty() && (parsed[0] == '#' || parsed[0] == ';')) {
        parsed.erase(0, 1);
        ad2_trim(parsed);
    }
    if (parsed.length() > 2 && parsed.front() == '[' && parsed.back() == ']') {
        section = parsed.substr(1, parsed.length() - 2);
        ad2_lcase(section);
        ad2_trim(section);
        return;
    }
    const size_t equals = parsed.find('=');
    if (equals == std::string::npos) {
        return;
    }
    std::string key = parsed.substr(0, equals);
    ad2_lcase(key);
    ad2_trim(key);
    if (!webui_sensitive_key(section, key)) {
        return;
    }
    std::string value = parsed.substr(equals + 1);
    ad2_trim(value);
    if (value.length() >= 4 &&
            std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static void webui_scrub_sensitive_values(std::string &line,
        const std::vector<std::string> &values)
{
    for (const std::string &value : values) {
        size_t match = 0;
        while ((match = line.find(value, match)) != std::string::npos) {
            line.replace(match, value.length(), "[redacted]");
            match += strlen("[redacted]");
        }
    }
}

static bool webui_read_file(const char *path, std::string &contents)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || info.st_size > WEBUI_CONFIG_MAX_BYTES) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    contents.clear();
    contents.reserve((size_t)info.st_size);
    char chunk[512];
    size_t count;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        contents.append(chunk, count);
    }
    const bool ok = !ferror(file);
    fclose(file);
    return ok;
}

static esp_err_t webui_send_redacted_config_file(httpd_req_t *req, const char *path,
        const char *source_name)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || info.st_size > WEBUI_CONFIG_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "Configuration is not available");
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "Configuration is not available");
    }

    std::vector<std::string> sensitive_values;
    sensitive_values.reserve(16);
    std::string section;
    std::string line;
    int line_status;
    while ((line_status = webui_read_config_line(file, line)) > 0) {
        webui_collect_sensitive_config_value(line, section, sensitive_values);
    }
    if (line_status < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Configuration contains an oversized or unreadable line");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Config-Source", source_name);
    while ((line_status = webui_read_config_line(file, line)) > 0) {
        webui_redact_config(line);
        webui_scrub_sensitive_values(line, sensitive_values);
        const esp_err_t result = httpd_resp_send_chunk(req, line.c_str(), line.length());
        if (result != ESP_OK) {
            fclose(file);
            return result;
        }
    }
    fclose(file);
    if (line_status < 0) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static bool webui_resolve_sd_path(const std::string &setting, std::string &path)
{
    std::string candidate = setting;
    ad2_trim(candidate);
    if (candidate.empty() || candidate.find("..") != std::string::npos ||
            candidate.find('\\') != std::string::npos) {
        return false;
    }
    if (candidate[0] == '/') {
        if (candidate.rfind("/" AD2_USD_MOUNT_POINT "/", 0) != 0) {
            return false;
        }
        path = candidate;
    } else {
        if (candidate.rfind(AD2_USD_MOUNT_POINT "/", 0) == 0) {
            candidate.erase(0, strlen(AD2_USD_MOUNT_POINT) + 1);
        }
        path = "/" AD2_USD_MOUNT_POINT "/" + candidate;
    }
    return true;
}

static bool webui_load_tls_material(std::string &certificate, std::string &private_key)
{
    std::string cert_path;
    std::string key_path;
    if (!g_uSD_mounted ||
            !webui_resolve_sd_path(webui_tls_cert_setting, cert_path) ||
            !webui_resolve_sd_path(webui_tls_key_setting, key_path)) {
        ESP_LOGE(TAG, "HTTPS requires valid certificate paths beneath /" AD2_USD_MOUNT_POINT);
        return false;
    }
    if (!webui_read_file(cert_path.c_str(), certificate) ||
            certificate.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
        ESP_LOGE(TAG, "Unable to load PEM certificate chain '%s'", cert_path.c_str());
        return false;
    }
    if (!webui_read_file(key_path.c_str(), private_key) ||
            (private_key.find("-----BEGIN PRIVATE KEY-----") == std::string::npos &&
             private_key.find("-----BEGIN RSA PRIVATE KEY-----") == std::string::npos &&
             private_key.find("-----BEGIN EC PRIVATE KEY-----") == std::string::npos)) {
        ESP_LOGE(TAG, "Unable to load PEM private key '%s'", key_path.c_str());
        return false;
    }
    return true;
}

/** Redacted configuration text: GET /api/config?source=active|spiffs|sd */
static esp_err_t webui_config_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    std::string source;
    if (!webui_query_value(req, "source", source)) {
        source = "active";
    }
    ad2_lcase(source);
    const char *config_path = nullptr;
    const char *source_name = nullptr;
    if (source == "active") {
        const bool using_sd = ad2_config_uses_sd();
        config_path = using_sd ? "/" AD2_USD_MOUNT_POINT AD2_CONFIG_FILE :
                      "/" AD2_SPIFFS_MOUNT_POINT AD2_CONFIG_FILE;
        source_name = using_sd ? "SD card (active boot source)" :
                      "SPIFFS (active boot source)";
    } else if (source == "spiffs") {
        config_path = "/" AD2_SPIFFS_MOUNT_POINT AD2_CONFIG_FILE;
        source_name = "SPIFFS file";
    } else if (source == "sd") {
        if (!g_uSD_mounted) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                       "Configuration is not available");
        }
        config_path = "/" AD2_USD_MOUNT_POINT AD2_CONFIG_FILE;
        source_name = "SD card file";
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown configuration source");
    }
    return webui_send_redacted_config_file(req, config_path, source_name);
}

/** Bounded reboot-scoped device log: GET /api/logs?limit=64 */
static esp_err_t webui_logs_handler(httpd_req_t *req)
{
    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    int limit = webui_query_int(req, "limit", 64);
    if (limit < 1 || limit > 64) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Limit must be between 1 and 64");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(hal_uptime_us() / 1000));
    cJSON *items = ad2_get_recent_logs_json((size_t)limit);
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            std::string redacted = text->valuestring;
            webui_redact_config(redacted);
            cJSON_SetValuestring(text, redacted.c_str());
        }
    }
    cJSON_AddItemToObject(root, "items", items);
    return webui_send_json_response(req, root);
}

/**
 * @brief HTTP GET handler for downloading files from uSD card.
 *
 * @param [in]httpd_req_t *
 *
 * @return esp_err_t
 *
 */
esp_err_t file_get_handler(httpd_req_t *req)
{

    if (!webui_authorize_request(req)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; connect-src 'self' ws: wss:; "
                       "img-src 'self' data:; object-src 'none'; frame-ancestors 'none'");

    // state: send raw file or process as template
    bool apply_template = false;
    bool apply_gzip = false;

    struct stat file_stat;

    // extract the full file path using AD2_USD_MOUNT_POINT as the root.
    char temppath[FILE_PATH_MAX];
    const char *filename = get_path_from_uri(temppath,  "/" AD2_USD_MOUNT_POINT WEBUI_DOC_ROOT,
                           req->uri, sizeof(temppath));
    if (!filename) {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL; // close socket
    }

    // Never allow the filesystem to resolve a request outside WEBUI_DOC_ROOT.
    // Backslashes are rejected as well so the same rule remains safe if the
    // storage implementation changes.
    if (strstr(filename, "..") || strchr(filename, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    // copy string into something more flexible.
    std::string filepath(temppath);

    // Switch / with /index.html
    if (filepath.back() == '/') {
        filepath += "index.html";
    }
    const std::string content_type_path = filepath;

    // Special case check for pre compressed files.
    std::string gzfile = filepath + ".gz";
    if (stat(gzfile.c_str(), &file_stat) == 0) {
        // Add content encoding so the client knows how to pre process the stream.
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        // change file adding gz so we spool out the pre compressed file.
        filepath = gzfile;
        apply_gzip = true;
    } else {
        // Check if _NOT_ exists swap for 404.html
        if (stat(filepath.c_str(), &file_stat) != 0) {
            // not found check if we have a custom 404.html file in our document root.
            filepath = "/" AD2_USD_MOUNT_POINT WEBUI_DOC_ROOT "/404.html";
            if (stat(filepath.c_str(), &file_stat) != 0) {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found.<br>Connect a uSD card with a FAT32 partition and the html content in the root directory before the device starts.");
                return ESP_FAIL; // close socket
            }
            // set status and continue processing 404.html file
            httpd_resp_set_status(req, "404 Not found");
        } else {
            // Check if a flag file with the same name with .tpl extension exist.
            std::string tplfile = filepath+".tpl";
            if (!apply_gzip && stat(tplfile.c_str(), &file_stat) == 0) {
                apply_template = true; //FIXME disable for testing.
            }
        }
    }

    // set the content type based upon the extension.
    set_content_type_from_file(req, content_type_path.c_str());

    // inform the client we prefer they disconnect when the page is delivered.
    httpd_resp_set_hdr(req, "Connection", "close");

    // Open the file and spool it to the client.
    FILE *f = fopen(filepath.c_str(), "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL; // close socket
    }

    // return state
    int result = ESP_OK;

    // FIXME: add function will use it more than 1 time.
    // apply template if set
    if (apply_template) {
        int sockfd = httpd_req_to_sockfd(req);

        // build standard template values FIXME: function dynamic.
        // Version MACRO ${0}
        std::string szVersion = "1.0";

        // Time MACRO ${1}
        std::string szTime;
        uptimeString(szTime);

        // socket Local address string MACRO ${2}
        std::string szLocalIP;
        hal_get_socket_local_ip(sockfd, szLocalIP);

        // socket Client address string MACRO ${3}
        std::string szClientIP;
        hal_get_socket_client_ip(sockfd, szClientIP);

        // Request protocol MACRO ${4}
        std::string szProt = "HTTP"; // (req->isSecure() ? "HTTPS" : "HTTP");

        // UUID MACRO ${5}
        std::string szUUID;
        ad2_genUUID(0x0, szUUID);

        const char* values[] = {
            szVersion.c_str(), // match ${0}
            szTime.c_str(),    // match ${1}
            szLocalIP.c_str(), // match ${2}
            szClientIP.c_str(),// match ${3}
            szProt.c_str(),    // match ${4}
            szUUID.c_str(),    // match ${5}
            0 // guard
        };

        // init template engine
        TinyTemplateEngineFileReader reader(f);
        reader.keepLineEnds(true);
        TinyTemplateEngine engine(reader);

        // process and send
        engine.start(values);

        // Send content
        int len = 0;
        do {
            len = 0;
            const char* line = engine.nextLine();
            if (line) {
                len = strlen(line);
                if (len && httpd_resp_send_chunk(req, line, len) != ESP_OK) {
                    result = ESP_FAIL;
                    break;
                }
            }
            // Yield to other tasks.
            vTaskDelay(1);
        } while (len);
        engine.end();
    } else {
        /* Read file in chunks (relaxes any constraint due to large file sizes)
        * and send HTTP response in chunked encoding */
        char   chunk[1024];
        size_t chunksize;
        do {
            chunksize = fread(chunk, 1, sizeof(chunk), f);
            if (chunksize && httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                result = ESP_FAIL;
                break;
            }
        } while (chunksize != 0);
    }

    // all done sending the file so close it.
    fclose(f);

    // FIXME: close socket
    if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "error calling httpd_resp_send_chunk with NULL to close.");
        result = ESP_FAIL; // close socket.
    }
    // finally force the socket to be closed.
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));

    return result;
}

/**
 * @brief Generic callback for all AlarmDecoder API event subscriptions.
 *
 * @param [in]msg std::string panel message.
 * @param [in]s AD2PartitionState *.
 * @param [in]arg cast as int for event type (ON_ARM,,,).
 *
 */
void webui_on_state_change(std::string *msg, AD2PartitionState *s, void *arg)
{
    webui_add_history(s, (int)arg);
#if CONFIG_HTTPD_WS_SUPPORT
#if defined(DEBUG_WEBUI)
    ESP_LOGI(TAG, "webui_on_state_change partition(%i) event(%s) message('%s')", s->partition, AD2Parse.event_str[(int)arg].c_str(), msg->c_str());
#endif
    size_t fds = server_config.max_open_sockets;
    int client_fds[fds];
    if (server && hal_get_network_connected()) {
        httpd_get_client_list(server, &fds, client_fds);
        for (int i=0; i<fds; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                struct ws_session_storage *sess = (ws_session_storage *)httpd_sess_get_ctx(server, client_fds[i]);
                if (sess) {
                    // get the partition state based upon the partition requested.
                    AD2PartitionState *temps = ad2_get_partition_state(sess->partID);
                    if (temps && s->partition == temps->partition) {
                        cJSON *root = webui_state_json(s, AD2Parse.event_str[(int)arg].c_str());
                        char *sys_info = cJSON_PrintUnformatted(root);
                        if (sys_info) {
                            httpd_ws_frame_t ws_pkt;
                            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                            ws_pkt.payload = (uint8_t*)sys_info;
                            ws_pkt.len = strlen(sys_info);
                            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                            httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
                            cJSON_free(sys_info);
                        }
                        cJSON_Delete(root);
                    }
                }
            }
        }
    }
#endif
}

/**
 * @brief webui server task
 *
 * @param [in]pvParameters currently not used NULL.
 */
void webui_server_task(void *pvParameters)
{
    esp_err_t err;
#if defined(FTPD_DEBUG)
     ESP_LOGI(TAG, "%s waiting for network layer to start.", TAG);
#endif
    while (1) {
        if (!hal_get_netif_started()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "Network layer OK. %s daemon service starting.", TAG);

    // Configure the web server and handlers.
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.lru_purge_enable = true;
    server_config.max_open_sockets = webui_tls_enabled ? WEBUI_TLS_MAX_CLIENTS : MAX_CLIENTS;
    server_config.max_uri_handlers = 10;
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t ad2ws_server = {
        .uri       = "/ad2ws",
        .method    = HTTP_GET,
        .handler   = ad2ws_handler,
        .user_ctx  = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
    };
#endif
    httpd_uri_t state_api = {
        .uri       = "/api/state",
        .method    = HTTP_GET,
        .handler   = webui_state_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t history_api = {
        .uri       = "/api/history",
        .method    = HTTP_GET,
        .handler   = webui_history_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t system_api = {
        .uri       = "/api/system",
        .method    = HTTP_GET,
        .handler   = webui_system_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t config_api = {
        .uri       = "/api/config",
        .method    = HTTP_GET,
        .handler   = webui_config_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t logs_api = {
        .uri       = "/api/logs",
        .method    = HTTP_GET,
        .handler   = webui_logs_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t firmware_api = {
        .uri       = "/api/firmware",
        .method    = HTTP_GET,
        .handler   = webui_firmware_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t action_api = {
        .uri       = "/api/action",
        .method    = HTTP_POST,
        .handler   = webui_action_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };
    httpd_uri_t file_server = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = file_get_handler,
        .user_ctx  = NULL,
#if CONFIG_HTTPD_WS_SUPPORT
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr
#endif
    };

    for (;;) {
        if (hal_get_network_connected() && server==nullptr) {

            if (webui_tls_enabled) {
                std::string certificate;
                std::string private_key;
                if (!webui_load_tls_material(certificate, private_key)) {
            vTaskDelay(pdMS_TO_TICKS(10000));
                    continue;
                }
                httpd_ssl_config_t tls_config = HTTPD_SSL_CONFIG_DEFAULT();
                tls_config.httpd = server_config;
                tls_config.httpd.stack_size = 10240;
                tls_config.servercert = (const uint8_t *)certificate.c_str();
                tls_config.servercert_len = certificate.length() + 1;
                tls_config.prvtkey_pem = (const uint8_t *)private_key.c_str();
                tls_config.prvtkey_len = private_key.length() + 1;
                tls_config.user_cb = webui_tls_session_callback;
                ESP_LOGI(TAG, "Starting HTTPS with %u client slots: heap=%u, minimum=%u, largest=%u",
                         WEBUI_TLS_MAX_CLIENTS,
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                err = httpd_ssl_start(&server, &tls_config);
            } else {
                err = httpd_start(&server, &server_config);
            }

            // Register handlers after the HTTP or HTTPS listener starts.
            if (err == ESP_OK) {
                webui_server_uses_tls = webui_tls_enabled;
                webui_tls_start_free_heap = webui_server_uses_tls ? esp_get_free_heap_size() : 0;
                ESP_LOGI(TAG, "Web UI listening with %s: heap=%u, minimum=%u, largest=%u",
                         webui_server_uses_tls ? "HTTPS" : "HTTP",
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                // Set URI handlers
#if CONFIG_HTTPD_WS_SUPPORT
                httpd_register_uri_handler(server, &ad2ws_server);
#endif
                httpd_register_uri_handler(server, &state_api);
                httpd_register_uri_handler(server, &history_api);
                httpd_register_uri_handler(server, &system_api);
                httpd_register_uri_handler(server, &config_api);
                httpd_register_uri_handler(server, &logs_api);
                httpd_register_uri_handler(server, &firmware_api);
                httpd_register_uri_handler(server, &action_api);
                httpd_register_uri_handler(server, &file_server);
            } else {
                // error long 10s sleep.
                ESP_LOGW(TAG, "Error starting %s server [%s]",
                         webui_tls_enabled ? "HTTPS" : "HTTP", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10000));
            }
        } else {
            // network down
            if (!hal_get_network_connected() && server!=nullptr) {
                taskENTER_CRITICAL(&spinlock);
                httpd_handle_t ts = server;
                const bool stop_tls = webui_server_uses_tls;
                server = nullptr;
                webui_server_uses_tls = false;
                webui_tls_sessions = 0;
                webui_tls_start_free_heap = 0;
                taskEXIT_CRITICAL(&spinlock);
                err = stop_tls ? httpd_ssl_stop(ts) : httpd_stop(ts);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Error stopping Web UI server [%s]", esp_err_to_name(err));
                }
            }
            // short 1s sleep
        vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    vTaskDelete(NULL);
}

/**
 * WebUI generic command event processing
 *  command: [COMMAND] <id> <arg>
 * ex.
 *   [COMMAND] 0 arg...
 */
static void _cli_cmd_webui_event(const char *string)
{

    // key value validation
    std::string cmd;
    ad2_copy_nth_arg(cmd, string, 0);
    ad2_lcase(cmd);

    if(cmd.compare(WEBUI_COMMAND) != 0) {
        ad2_printf_host(false, "What?\r\n");
        return;;
    }

    // key value validation
    std::string subcmd;
    ad2_copy_nth_arg(subcmd, string, 1);
    ad2_lcase(subcmd);

    int i;
    for(i = 0;; ++i) {
        if (WEBUI_SUBCMD[i] == 0) {
            ad2_printf_host(false, "What?\r\n");
            break;
        }
        if(subcmd.compare(WEBUI_SUBCMD[i]) == 0) {
            std::string arg;
            std::string acl;
            switch(i) {
            /**
             * Enable/Disable WebUI daemon.
             */
            case WEBUI_SUBCMD_ENABLE_ID:
                if (ad2_copy_nth_arg(arg, string, 2) >= 0) {
                    const bool requested = (arg[0] == 'Y' || arg[0] == 'y');
                    std::string configured_user;
                    std::string configured_password;
                    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_USER, configured_user);
                    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_PASSWORD,
                                              configured_password);
                    if (requested && !webui_valid_credentials(configured_user, configured_password)) {
                        ad2_printf_host(false, "Set a valid Web UI user and password before enabling.\r\n");
                    } else {
                        ad2_set_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ENABLE, requested);
                        ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
                    }
                }

                {
                    // show contents of this slot
                    bool en = false;
                    ad2_get_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ENABLE, &en);
                    ad2_printf_host(false, "WebUI daemon is '%s'.\r\n", (en ? "Enabled" : "Disabled"));
                }
                break;
            /**
             * WebUI daemon IP/CIDR ACL list.
             */
            case WEBUI_SUBCMD_ACL_ID:
                // If no arg then return ACL list
                if (ad2_copy_nth_arg(arg, string, 2, true) >= 0) {
                    webui_acl.clear();
                    int res = webui_acl.add(arg);
                    if (res == webui_acl.ACL_FORMAT_OK) {
                        ad2_set_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ACL, arg.c_str());
                    } else {
                        ad2_printf_host(false, "Error parsing ACL string. Check ACL format. Not saved.\r\n");
                    }
                }
                // show contents of this slot set default to allow all
                acl = WEBUI_DEFAULT_ACL;
                ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ACL, acl);
                ad2_printf_host(false, WEBUI_COMMAND " 'acl' set to '%s'.\r\n", acl.c_str());
                break;
            case WEBUI_SUBCMD_SSL_ID:
                if (ad2_copy_nth_arg(arg, string, 2) >= 0) {
                    ad2_set_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_SSL,
                                            (arg[0] == 'Y' || arg[0] == 'y'));
                    ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
                }
                {
                    bool ssl = false;
                    ad2_get_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_SSL, &ssl);
                    ad2_printf_host(false, "WebUI HTTPS is '%s'.\r\n", ssl ? "Enabled" : "Disabled");
                }
                break;
            case WEBUI_SUBCMD_SSLCERT_ID:
            case WEBUI_SUBCMD_SSLKEY_ID:
                {
                    const char *key = i == WEBUI_SUBCMD_SSLCERT_ID ? WEBUI_SUBCMD_SSLCERT : WEBUI_SUBCMD_SSLKEY;
                    const char *default_path = i == WEBUI_SUBCMD_SSLCERT_ID ?
                                               WEBUI_DEFAULT_SSL_CERT : WEBUI_DEFAULT_SSL_KEY;
                    if (ad2_copy_nth_arg(arg, string, 2, true) >= 0) {
                        if (arg == "-") {
                            arg = default_path;
                        }
                        std::string resolved;
                        if (!webui_resolve_sd_path(arg, resolved)) {
                            ad2_printf_host(false, "Path must remain beneath /" AD2_USD_MOUNT_POINT ". Not saved.\r\n");
                            break;
                        }
                        ad2_set_config_key_string(WEBUI_CONFIG_SECTION, key, arg.c_str());
                        ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
                    }
                    std::string saved = default_path;
                    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, key, saved);
                    ad2_printf_host(false, "WebUI '%s' path is '%s'.\r\n", key, saved.c_str());
                }
                break;
            case WEBUI_SUBCMD_USER_ID:
                if (ad2_copy_nth_arg(arg, string, 2) >= 0) {
                    if (arg == "-") {
                        arg.clear();
                    }
                    if (!arg.empty() &&
                            !webui_valid_credentials(arg, std::string(WEBUI_AUTH_PASSWORD_MIN, 'x'))) {
                        ad2_printf_host(false, "User must be 1-32 printable characters without spaces or ':'. Not saved.\r\n");
                    } else {
                        ad2_set_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_USER, arg.c_str());
                        ad2_printf_host(false, "Web UI user updated. Restart required to take effect.\r\n");
                    }
                }
                {
                    std::string configured_user;
                    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_USER, configured_user);
                    ad2_printf_host(false, "Web UI user is '%s'.\r\n",
                                    configured_user.empty() ? "Not configured" : "Configured");
                }
                break;
            case WEBUI_SUBCMD_PASSWORD_ID:
                if (ad2_copy_nth_arg(arg, string, 2, true) >= 0) {
                    if (arg == "-") {
                        arg.clear();
                    }
                    if (!arg.empty() && !webui_valid_credentials("user", arg)) {
                        ad2_printf_host(false, "Password must be 12-64 printable characters. Not saved.\r\n");
                    } else {
                        ad2_set_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_PASSWORD, arg.c_str());
                        ad2_printf_host(false, "Web UI password updated. Restart required to take effect.\r\n");
                    }
                }
                {
                    std::string configured_password;
                    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_PASSWORD,
                                              configured_password);
                    ad2_printf_host(false, "Web UI password is '%s'.\r\n",
                                    configured_password.empty() ? "Not configured" : "Configured");
                }
                break;
            default:
                break;
            }
            break;
        }
    }
}

/**
 * @brief command list for module
 */
static struct cli_command webui_cmd_list[] = {
    {
        (char*)WEBUI_COMMAND,(char*)
        "Usage: webui <command> [arg]\r\n"
        "\r\n"
        "    Configuration tool for WebUI server\r\n"
        "Commands:\r\n"
        "    enable [Y|N]            Set or get enable flag\r\n"
        "    acl [aclString|-]       Set or get ACL CIDR CSV list\r\n"
        "                            use - to delete\r\n"
        "    ssl [Y|N]               Enable HTTPS on port 443\r\n"
        "    sslcert [path|-]        PEM full-chain path beneath /sdcard\r\n"
        "    sslkey [path|-]         PEM private-key path beneath /sdcard\r\n"
        "                            use - to restore default paths\r\n"
        "    user [name|-]           Set user; use - to clear\r\n"
        "    password [value|-]      Set 12-64 character password; use - to clear\r\n"
        "Examples:\r\n"
        "    ```webui acl 192.168.0.0/28,192.168.1.0-192.168.1.10,192.168.3.4```\r\n"
        "    ```webui sslcert certs/fullchain.pem```\r\n"
        "    ```webui sslkey certs/privkey.pem```\r\n"
        "    ```webui user alarmadmin```\r\n"
        "    ```webui password use-a-long-unique-password```\r\n"
        "    ```webui ssl Y```\r\n"
        "    ```webui enable Y```\r\n"
        , _cli_cmd_webui_event
    }
};

/**
 * @brief Register componet cli commands.
 */
void webui_register_cmds()
{
    // Register webui CLI commands
    for (int i = 0; i < ARRAY_SIZE(webui_cmd_list); i++) {
        cli_register_command(&webui_cmd_list[i]);
    }
}

/**
 * @brief AD2IoT Component webUI init
 *
 */
void webui_init(void)
{
    bool en = false;
    ad2_get_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ENABLE, &en);

    // nothing more needs to be done once commands are set if not enabled.
    if (!en) {
        ad2_printf_host(true, "%s: daemon disabled.", TAG);
        return;
    }

    if (!webui_load_credentials()) {
        ESP_LOGE(TAG, "Web UI credentials are missing or invalid; refusing to start");
        ad2_printf_host(true, "%s: set a 1-32 character user and 12-64 character password before enabling.",
                        TAG);
        return;
    }

    ad2_get_config_key_bool(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_SSL, &webui_tls_enabled);
    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_SSLCERT, webui_tls_cert_setting);
    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_SSLKEY, webui_tls_key_setting);

    // Load and parse the ACL. Missing configuration is loopback-only.
    std::string acl = WEBUI_DEFAULT_ACL;

    ad2_get_config_key_string(WEBUI_CONFIG_SECTION, WEBUI_SUBCMD_ACL, acl);
    if (acl.empty()) {
        ESP_LOGE(TAG, "Web UI ACL is empty; refusing to start");
        return;
    }
    int acl_result = webui_acl.add(acl);
    if (acl_result != webui_acl.ACL_FORMAT_OK) {
        ESP_LOGE(TAG, "ACL parse error %i for '%s'; refusing to start", acl_result, acl.c_str());
        return;
    }

    webui_history_mutex = xSemaphoreCreateMutex();
    if (!webui_history_mutex) {
        ESP_LOGE(TAG, "Unable to allocate activity history mutex");
        return;
    }

    // Subscribe to AlarmDecoder events
    AD2Parse.subscribeTo(ON_ARM, webui_on_state_change, (void *)ON_ARM);
    AD2Parse.subscribeTo(ON_DISARM, webui_on_state_change, (void *)ON_DISARM);
    AD2Parse.subscribeTo(ON_CHIME_CHANGE, webui_on_state_change, (void *)ON_CHIME_CHANGE);
    AD2Parse.subscribeTo(ON_BEEPS_CHANGE, webui_on_state_change, (void *)ON_BEEPS_CHANGE);
    AD2Parse.subscribeTo(ON_FIRE_CHANGE, webui_on_state_change, (void *)ON_FIRE_CHANGE);
    AD2Parse.subscribeTo(ON_POWER_CHANGE, webui_on_state_change, (void *)ON_POWER_CHANGE);
    AD2Parse.subscribeTo(ON_READY_CHANGE, webui_on_state_change, (void *)ON_READY_CHANGE);
    AD2Parse.subscribeTo(ON_LOW_BATTERY, webui_on_state_change, (void *)ON_LOW_BATTERY);
    AD2Parse.subscribeTo(ON_ALARM_CHANGE, webui_on_state_change, (void *)ON_ALARM_CHANGE);
    AD2Parse.subscribeTo(ON_ZONE_BYPASSED_CHANGE, webui_on_state_change, (void *)ON_ZONE_BYPASSED_CHANGE);
    AD2Parse.subscribeTo(ON_EXIT_CHANGE, webui_on_state_change, (void *)ON_EXIT_CHANGE);
    AD2Parse.subscribeTo(ON_PROGRAMMING_CHANGE, webui_on_state_change, (void *)ON_PROGRAMMING_CHANGE);
    AD2Parse.subscribeTo(ON_ALPHA_MESSAGE, webui_on_state_change, (void *)ON_ALPHA_MESSAGE);
    AD2Parse.subscribeTo(ON_PANIC, webui_on_state_change, (void *)ON_PANIC);
    AD2Parse.subscribeTo(ON_LRR, webui_on_state_change, (void *)ON_LRR);
    // Subscribe to ON_ZONE_CHANGE events
    AD2Parse.subscribeTo(ON_ZONE_CHANGE, webui_on_state_change, (void *)ON_ZONE_CHANGE);

    ad2_printf_host(true, "%s: Init done, daemon starting.", TAG);
    xTaskCreate(&webui_server_task, "AD2 webUI", 1024*5, NULL, tskIDLE_PRIORITY+1, NULL);
}

#endif /*  CONFIG_AD2IOT_WEBSERVER_UI */
