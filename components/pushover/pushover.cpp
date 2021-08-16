/**
*  @file    pushover.cpp
*  @author  Sean Mathews <coder@f34r.com>
*  @date    01/06/2021
*  @version 1.0.0
*
*  @brief Simple commands to post to api.pushover.net
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
#if CONFIG_AD2IOT_PUSHOVER_CLIENT
static const char *TAG = "PUSHOVER";

// AlarmDecoder std includes
#include "alarmdecoder_main.h"

// specific includes
#include "pushover.h"

//#define DEBUG_PUSHOVER
#define AD2_DEFAULT_PUSHOVER_SLOT 0

/* Constants that aren't configurable in menuconfig */
#define PUSHOVER_API_VERSION "1"
const char PUSHOVER_URL[] = "https://api.pushover.net/" PUSHOVER_API_VERSION "/messages.json";

#define PUSHOVER_COMMAND        "pushover"
#define PUSHOVER_PREFIX         "po"
#define PUSHOVER_TOKEN_CFGKEY   "apptoken"
#define PUSHOVER_USERKEY_CFGKEY "userkey"
#define PUSHOVER_SAS_CFGKEY     "switch"

#define MAX_SEARCH_KEYS 9

// NV storage sub key values for virtual search switch
#define SK_NOTIFY_SLOT       "N"
#define SK_DEFAULT_STATE     "D"
#define SK_AUTO_RESET        "R"
#define SK_TYPE_LIST         "T"
#define SK_PREFILTER_REGEX   "P"
#define SK_OPEN_REGEX_LIST   "O"
#define SK_CLOSED_REGEX_LIST "C"
#define SK_FAULT_REGEX_LIST  "F"
#define SK_OPEN_OUTPUT_FMT   "o"
#define SK_CLOSED_OUTPUT_FMT "c"
#define SK_FAULT_OUTPUT_FMT  "f"

static std::vector<AD2EventSearch *> pushover_AD2EventSearches;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4,1,0)
/**
 * @brief Root cert for api.pushover.net
 *   The PEM file was extracted from the output of this command:
 *  openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null
 *  The CA root cert is the last cert given in the chain of certs.
 *  To embed it in the app binary, the PEM file is named
 *  in the component.mk COMPONENT_EMBED_TXTFILES variable.
 */
extern const uint8_t pushover_root_pem_start[] asm("_binary_pushover_root_pem_start");
extern const uint8_t pushover_root_pem_end[]   asm("_binary_pushover_root_pem_end");
#endif


#ifdef __cplusplus
extern "C" {
#endif

// forward decl

/**
 * @brief class that will be stored in the sendQ for each request.
 */
class request_message
{
public:
    request_message()
    {
        config_client = (esp_http_client_config_t *)calloc(1, sizeof(esp_http_client_config_t));
    }
    ~request_message()
    {
        if (config_client) {
            free(config_client);
        }
    }

    // client_config used the the http_sendQ
    esp_http_client_config_t* config_client;

    // Application specific
    std::string token;
    std::string userkey;
    std::string message;
    std::string post;
    std::string results;
};

/**
 * @brief ad2_http_sendQ callback before esp_http_client_perform() is called.
 * Add headers content etc.
 *
 */
void _sendQ_ready_handler(esp_http_client_handle_t client, esp_http_client_config_t *config)
{
    esp_err_t err;
    // if perform failed this can be NULL
    if (client) {
        request_message *r = (request_message*) config->user_data;

        // Pushover message API
        //   https://pushover.net/api
        r->post = std::string("token=" + ad2_urlencode(r->token) + "&user=" + ad2_urlencode(r->userkey) + \
                              "&message=" + ad2_urlencode(r->message));

        // does not copy data just a pointer so we have to maintain memory.
        err = esp_http_client_set_post_field(client, r->post.c_str(), r->post.length());

    }
}

/**
 * @brief ad2_http_sendQ callback when esp_http_client_perform() is finished.
 * Preform any final cleanup here.
 *
 *  @param [in]esp_err_t res - Results of esp_http_client_preform()
 *  @param [in]evt esp_http_client_event_t
 *  @param [in]config esp_http_client_config_t *
 */
void _sendQ_done_handler(esp_err_t res, esp_http_client_handle_t client, esp_http_client_config_t *config)
{
    request_message *r = (request_message*) config->user_data;

    ESP_LOGI(TAG, "perform results = %d HTTP Status = %d, response length = %d response = '%s'", res,
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client), r->results.c_str());

    // free message_data class will also delete the client_config in the distructor.
    delete r;
}

/**
 * @brief esp_http_client event callback.
 *
 * HTTP_EVENT_ON_DATA to capture server response message.
 * Also use for diagnostics of response headers etc.
 *
 *  @param [in]evt esp_http_client_event_t *
 */
esp_err_t _pushover_http_event_handler(esp_http_client_event_t *evt)
{
    request_message *r;
    size_t len;

    switch(evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
#endif
        break;
    case HTTP_EVENT_ERROR:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
#endif
        break;
    case HTTP_EVENT_ON_CONNECTED:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
#endif
        break;
    case HTTP_EVENT_HEADERS_SENT:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_HEADERS_SENT");
#endif
        break;
    case HTTP_EVENT_ON_FINISH:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
#endif
        break;
    case HTTP_EVENT_DISCONNECTED:
#if defined(DEBUG_PUSHOVER)
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
#endif
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len) {
            // Save the results into our message_data structure.
            r = (request_message *)evt->user_data;
            // Limit size.
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
            len = MIN(1024, evt->data_len);
            r->results = std::string((char*)evt->data, len);
        }
        break;
    }

    return ESP_OK;
}

/**
 * @brief SmartSwitch match callback.
 * Called when the current message matches a AD2EventSearch test.
 *
 * @param [in]msg std::string new version string.
 * @param [in]s nullptr
 * @param [in]arg nullptr.
 *
 * @note No full queue handler
 */
void on_search_match_cb_po(std::string *msg, AD2VirtualPartitionState *s, void *arg)
{
    AD2EventSearch *es = (AD2EventSearch *)arg;
    ESP_LOGI(TAG, "ON_SEARCH_MATCH_CB: '%s' -> '%s' notify slot #%02i", msg->c_str(), es->out_message.c_str(), es->INT_ARG);

    // Container to store details needed for delivery.
    request_message *r = new request_message();

    // load our settings for this event type.
    // es->INT_ARG is the notifiaction slot for this notification.
    std::string nvkey;

    // save the user key
    nvkey = std::string(PUSHOVER_PREFIX) + std::string(PUSHOVER_USERKEY_CFGKEY);
    ad2_get_nv_slot_key_string(nvkey.c_str(), es->INT_ARG, nullptr, r->userkey);

    // save the token
    nvkey = std::string(PUSHOVER_PREFIX) + std::string(PUSHOVER_TOKEN_CFGKEY);
    ad2_get_nv_slot_key_string(nvkey.c_str(), es->INT_ARG, nullptr, r->token);

    // save the message
    r->message = es->out_message;

    // Settings specific for http_client_config
    r->config_client->url = PUSHOVER_URL;
    // set request type
    r->config_client->method = HTTP_METHOD_POST;

    // optional define an internal event handler
    r->config_client->event_handler = _pushover_http_event_handler;

    // required save internal class to user_data to be used in callback.
    r->config_client->user_data = (void *)r; // Definition of grok.. see grok.

    // Fails, but not needed to work.
    // config_client->use_global_ca_store = true;

    // Add client config to the http_sendQ for processing.
    bool res = ad2_add_http_sendQ(r->config_client, _sendQ_ready_handler, _sendQ_done_handler);
    if (res) {
        ESP_LOGI(TAG,"Adding HTTP request to ad2_add_http_sendQ");
    } else {
        ESP_LOGE(TAG,"Error adding HTTP request to ad2_add_http_sendQ.");
        // destroy storage class if we fail to add to the sendQ
        delete r;
    }
}

/**
 * Command support values.
 */
enum {
    PUSHOVER_TOKEN_CFGKEY_ID = 0,
    PUSHOVER_USERKEY_CFGKEY_ID,
    PUSHOVER_SAS_CFGKEY_ID
};
char * PUSHOVER_SETTINGS [] = {
    (char*)PUSHOVER_TOKEN_CFGKEY,
    (char*)PUSHOVER_USERKEY_CFGKEY,
    (char*)PUSHOVER_SAS_CFGKEY,
    0 // EOF
};

/**
 * Component generic command event processing
 *  command: [COMMAND] [SUB] <id> <arg>
 * ex.
 *   [COMMAND] [SUB] 0 arg...
 */
static void _cli_cmd_pushover_event_generic(std::string &subcmd, char *string)
{
    int slot = -1;
    std::string buf;

    // get the sub command value validation
    ad2_copy_nth_arg(subcmd, string, 1);
    ad2_lcase(subcmd);

    // build the NV storage key concat prefix to sub command. Take human readable and make it less so.
    std::string key = std::string(PUSHOVER_PREFIX) + subcmd.c_str();
    if (ad2_copy_nth_arg(buf, string, 2) >= 0) {
        slot = strtol(buf.c_str(), NULL, 10);
    }
    if (slot >= 0) {
        if (ad2_copy_nth_arg(buf, string, 3) >= 0) {
            ad2_set_nv_slot_key_string(key.c_str(), slot, nullptr, buf.c_str());
            ad2_printf_host("Setting '%s' value finished.\r\n", subcmd.c_str());
        } else {
            buf = "";
            ad2_get_nv_slot_key_string(key.c_str(), slot, nullptr, buf);
            ad2_printf_host("Current slot #%02i '%s' value '%s'\r\n", slot, subcmd.c_str(), buf.length() ? buf.c_str() : "EMPTY");
        }
    } else {
        ad2_printf_host("Missing <slot>\r\n");
    }
}

/**
 * component SmartSwitch command event processing
 *  command: [COMMAND] [SUB] <id> <setting> <arg1> <arg2>
 * ex. Switch #0 [N]otification slot 0
 *   [COMMAND] [SUB] 0 N 0
 */
static void _cli_cmd_pushover_smart_alert_switch(std::string &subcmd, char *instring)
{
    int i;
    int slot = -1;
    std::string buf;
    std::string arg1;
    std::string arg2;
    std::string tmpsz;

    // get the sub command value validation
    std::string key = std::string(PUSHOVER_PREFIX) + subcmd.c_str();

    if (ad2_copy_nth_arg(buf, instring, 2) >= 0) {
        slot = std::atoi (buf.c_str());
    }
    // add the slot# to the key max 99 slots.
    if (slot > 0 && slot < 100) {

        if (ad2_copy_nth_arg(buf, instring, 3) >= 0) {

            // sub key suffix.
            std::string sk = ad2_string_printf("%c", buf[0]);

            /* setting */
            switch(buf[0]) {
            case SK_NOTIFY_SLOT[0]: // Notification slot
                ad2_copy_nth_arg(arg1, instring, 4);
                i = std::atoi (arg1.c_str());
                tmpsz = i < 0 ? "Clearing" : "Setting";
                ad2_set_nv_slot_key_int(key.c_str(), slot, sk.c_str(), i);
                ad2_printf_host("%s smartswitch #%i to use notification settings from slot #%i.\r\n", tmpsz.c_str(), slot, i);
                break;
            case SK_DEFAULT_STATE[0]: // Default state
                ad2_copy_nth_arg(arg1, instring, 4);
                i = std::atoi (arg1.c_str());
                ad2_set_nv_slot_key_int(key.c_str(), slot, sk.c_str(), i);
                ad2_printf_host("Setting smartswitch #%i to use default state '%s' %i.\r\n", slot, AD2Parse.state_str[i].c_str(), i);
                break;
            case SK_AUTO_RESET[0]: // Auto Reset
                ad2_copy_nth_arg(arg1, instring, 4);
                i = std::atoi (arg1.c_str());
                ad2_set_nv_slot_key_int(key.c_str(), slot, sk.c_str(), i);
                ad2_printf_host("Setting smartswitch #%i auto reset value %i.\r\n", slot, i);
                break;
            case SK_TYPE_LIST[0]: // Message type filter list
                // consume the arge and to EOL
                ad2_copy_nth_arg(arg1, instring, 4, true);
                ad2_set_nv_slot_key_string(key.c_str(), slot, sk.c_str(), arg1.c_str());
                ad2_printf_host("Setting smartswitch #%i message type filter list to '%s'.\r\n", slot, arg1.c_str());
                break;
            case SK_PREFILTER_REGEX[0]: // Pre filter REGEX
                // consume the arge and to EOL
                ad2_copy_nth_arg(arg1, instring, 4, true);
                ad2_set_nv_slot_key_string(key.c_str(), slot, sk.c_str(), arg1.c_str());
                ad2_printf_host("Setting smartswitch #%i pre filter regex to '%s'.\r\n", slot, arg1.c_str());
                break;

            case SK_OPEN_REGEX_LIST[0]: // Open state REGEX list editor
            case SK_CLOSED_REGEX_LIST[0]: // Closed state REGEX list editor
            case SK_FAULT_REGEX_LIST[0]: // Fault state REGEX list editor
                // Add index to file name to track N number of elements.
                ad2_copy_nth_arg(arg1, instring, 4);
                i = std::atoi (arg1.c_str());
                if ( i > 0 && i < MAX_SEARCH_KEYS) {
                    // consume the arge and to EOL
                    ad2_copy_nth_arg(arg2, instring, 5, true);
                    std::string op = arg2.length() > 0 ? "Setting" : "Clearing";
                    sk+=ad2_string_printf("%02i", i);
                    if (buf[0] == SK_OPEN_REGEX_LIST[0]) {
                        tmpsz = "OPEN";
                    }
                    if (buf[0] == SK_CLOSED_REGEX_LIST[0]) {
                        tmpsz = "CLOSED";
                    }
                    if (buf[0] == SK_FAULT_REGEX_LIST[0]) {
                        tmpsz = "FAULT";
                    }
                    ad2_set_nv_slot_key_string(key.c_str(), slot, sk.c_str(), arg2.c_str());
                    ad2_printf_host("%s smartswitch #%i REGEX filter #%02i for state '%s' to '%s'.\r\n", op.c_str(), slot, i, tmpsz.c_str(), arg2.c_str());
                } else {
                    ad2_printf_host("Error invalid index %i. Valid values are 1-8.\r\n", slot, i);
                }
                break;

            case SK_OPEN_OUTPUT_FMT[0]: // Open state output format string
            case SK_CLOSED_OUTPUT_FMT[0]: // Closed state output format string
            case SK_FAULT_OUTPUT_FMT[0]: // Fault state output format string
                // consume the arge and to EOL
                ad2_copy_nth_arg(arg1, instring, 4, true);
                if (buf[0] == SK_OPEN_OUTPUT_FMT[0]) {
                    tmpsz = "OPEN";
                }
                if (buf[0] == SK_CLOSED_OUTPUT_FMT[0]) {
                    tmpsz = "CLOSED";
                }
                if (buf[0] == SK_FAULT_OUTPUT_FMT[0]) {
                    tmpsz = "FAULT";
                }
                ad2_set_nv_slot_key_string(key.c_str(), slot, sk.c_str(), arg1.c_str());
                ad2_printf_host("Setting smartswitch #%i output format string for '%s' state to '%s'.\r\n", slot, tmpsz.c_str(), arg1.c_str());
                break;

            default:
                ESP_LOGW(TAG, "Unknown setting '%c' ignored.", buf[0]);
                return;
            }
        } else {
            // query show contents.
            int i;
            std::string out;
            std::string args = SK_NOTIFY_SLOT
                               SK_DEFAULT_STATE
                               SK_AUTO_RESET
                               SK_TYPE_LIST
                               SK_PREFILTER_REGEX
                               SK_OPEN_REGEX_LIST
                               SK_CLOSED_REGEX_LIST
                               SK_FAULT_REGEX_LIST
                               SK_OPEN_OUTPUT_FMT
                               SK_CLOSED_OUTPUT_FMT
                               SK_FAULT_OUTPUT_FMT;

            ad2_printf_host("Pushover SmartSwitch #%i report\r\n", slot);
            // sub key suffix.
            std::string sk;
            for(char& c : args) {
                std::string sk = ad2_string_printf("%c", c);
                switch(c) {
                case SK_NOTIFY_SLOT[0]:
                    i = 0; // Default 0
                    ad2_get_nv_slot_key_int(key.c_str(), slot, sk.c_str(), &i);
                    ad2_printf_host("# Set notification slot [%c] to #%i.\r\n", c, i);
                    ad2_printf_host("%s %s %i %c %i\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, i);
                    break;
                case SK_DEFAULT_STATE[0]:
                    i = 0; // Default CLOSED
                    ad2_get_nv_slot_key_int(key.c_str(), slot, sk.c_str(), &i);
                    ad2_printf_host("# Set default virtual switch state [%c] to '%s'(%i)\r\n", c, AD2Parse.state_str[i].c_str(), i);
                    ad2_printf_host("%s %s %i %c %i\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, i);
                    break;
                case SK_AUTO_RESET[0]:
                    i = 0; // Defaut 0 or disabled
                    ad2_get_nv_slot_key_int(key.c_str(), slot, sk.c_str(), &i);
                    ad2_printf_host("# Set auto reset time in ms [%c] to '%s'\r\n", c, (i > 0) ? ad2_to_string(i).c_str() : "DISABLED");
                    ad2_printf_host("%s %s %i %c %i\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, i);
                    break;
                case SK_TYPE_LIST[0]:
                    out = "";
                    ad2_get_nv_slot_key_string(key.c_str(), slot, sk.c_str(), out);
                    ad2_printf_host("# Set message type list [%c]\r\n", c);
                    if (out.length()) {
                        ad2_printf_host("%s %s %i %c %s\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, out.c_str());
                    } else {
                        ad2_printf_host("# disabled\r\n");
                    }
                    break;
                case SK_PREFILTER_REGEX[0]:
                    out = "";
                    ad2_get_nv_slot_key_string(key.c_str(), slot, sk.c_str(), out);
                    if (out.length()) {
                        ad2_printf_host("# Set pre filter REGEX [%c]\r\n", c);
                        ad2_printf_host("%s %s %i %c %s\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, out.c_str());
                    }
                    break;
                case SK_OPEN_REGEX_LIST[0]:
                case SK_CLOSED_REGEX_LIST[0]:
                case SK_FAULT_REGEX_LIST[0]:
                    // * Find all sub keys and show info.
                    if (c == SK_OPEN_REGEX_LIST[0]) {
                        tmpsz = "OPEN";
                    }
                    if (c == SK_CLOSED_REGEX_LIST[0]) {
                        tmpsz = "CLOSED";
                    }
                    if (c == SK_FAULT_REGEX_LIST[0]) {
                        tmpsz = "FAULT";
                    }
                    for ( i = 1; i < MAX_SEARCH_KEYS; i++ ) {
                        out = "";
                        std::string tsk = sk + ad2_string_printf("%02i", i);
                        ad2_get_nv_slot_key_string(key.c_str(), slot, tsk.c_str(), out);
                        if (out.length()) {
                            ad2_printf_host("# Set '%s' state REGEX Filter [%c] #%02i.\r\n", tmpsz.c_str(), c, i);
                            ad2_printf_host("%s %s %i %c %i %s\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, i, out.c_str());
                        }
                    }
                    break;
                case SK_OPEN_OUTPUT_FMT[0]:
                case SK_CLOSED_OUTPUT_FMT[0]:
                case SK_FAULT_OUTPUT_FMT[0]:
                    if (c == SK_OPEN_OUTPUT_FMT[0]) {
                        tmpsz = "OPEN";
                    }
                    if (c == SK_CLOSED_OUTPUT_FMT[0]) {
                        tmpsz = "CLOSED";
                    }
                    if (c == SK_FAULT_OUTPUT_FMT[0]) {
                        tmpsz = "FAULT";
                    }
                    out = "";
                    ad2_get_nv_slot_key_string(key.c_str(), slot, sk.c_str(), out);
                    if (out.length()) {
                        ad2_printf_host("# Set output format string for '%s' state [%c].\r\n", tmpsz.c_str(), c);
                        ad2_printf_host("%s %s %i %c %s\r\n", PUSHOVER_COMMAND, PUSHOVER_SAS_CFGKEY, slot, c, out.c_str());
                    }
                    break;
                }
            }
        }
    } else {
        ad2_printf_host("Missing or invalid <slot> 1-99\r\n");
        // TODO: DUMP when slot is 0 or 100
    }
}

/**
 * Component command router
 */
static void _cli_cmd_pushover_command_router(char *string)
{
    int i;
    std::string subcmd;

    // get the sub command value validation
    ad2_copy_nth_arg(subcmd, string, 1);
    ad2_lcase(subcmd);

    for(i = 0;; ++i) {
        if (PUSHOVER_SETTINGS[i] == 0) {
            ad2_printf_host("What?\r\n");
            break;
        }
        if(subcmd.compare(PUSHOVER_SETTINGS[i]) == 0) {
            switch(i) {
            case PUSHOVER_TOKEN_CFGKEY_ID:   // 'apptoken' sub command
            case PUSHOVER_USERKEY_CFGKEY_ID: // 'userkey' sub command
                _cli_cmd_pushover_event_generic(subcmd, string);
                break;
            case PUSHOVER_SAS_CFGKEY_ID:
                _cli_cmd_pushover_smart_alert_switch(subcmd, string);
                break;
            }
            // all done
            break;
        }
    }
}

/**
 * @brief command list for component
 *
 */
static struct cli_command pushover_cmd_list[] = {
    {
        // ### Pushover.net notification component
        (char*)PUSHOVER_COMMAND,(char*)
        "#### Configuration for Pushover.net notification\r\n"
        "- Sets the ```Application Token/Key``` for a given notification slot. Multiple slots allow for multiple pushover accounts or applications.\r\n"
        "  - ```" PUSHOVER_COMMAND " " PUSHOVER_TOKEN_CFGKEY " {slot} {hash}```\r\n"
        "    - {slot}: [N]\r\n"
        "      - For default use 0.\r\n"
        "    - {hash}: Pushover.net ```User Auth Token```.\r\n"
        "  - Example: ```" PUSHOVER_COMMAND " " PUSHOVER_TOKEN_CFGKEY " 0 aabbccdd112233..```\r\n"
        "- Sets the 'User Key' for a given notification slot.\r\n"
        "  - ```" PUSHOVER_COMMAND " " PUSHOVER_USERKEY_CFGKEY " {slot} {hash}```\r\n"
        "    - {slot}: [N]\r\n"
        "      - For default use 0.\r\n"
        "    - {hash}: Pushover ```User Key```.\r\n"
        "  - Example: ```" PUSHOVER_COMMAND " " PUSHOVER_USERKEY_CFGKEY " 0 aabbccdd112233..```\r\n"
        "- Define a smart virtual switch that will track and alert alarm panel state changes using user configurable filter and formatting rules.\r\n"
        "  - ```" PUSHOVER_COMMAND " " PUSHOVER_SAS_CFGKEY " {slot} {setting} {arg1} [arg2]```\r\n"
        "    - {slot}\r\n"
        "      - 1-99 : Supports multiple virtual smart alert switches.\r\n"
        "    - {setting}\r\n"
        "      - [N] Notification slot\r\n"
        "        -  Notification settings slot to use for sending this events.\r\n"
        "      - [D] Default state\r\n"
        "        - {arg1}: [0]CLOSE(OFF) [1]OPEN(ON)\r\n"
        "      - [R] AUTO Reset.\r\n"
        "        - {arg1}:  time in ms 0 to disable\r\n"
        "      - [T] Message type filter.\r\n"
        "        - {arg1}: Message type list seperated by ',' or empty to disables filter.\r\n"
        "          - Message Types: [ALPHA,LRR,REL,EXP,RFX,AUI,KPM,KPE,CRC,VER,ERR,EVENT]\r\n"
        "            - For EVENT type the message will be generated by the API and not the AD2\r\n"
        "      - [P] Pre filter REGEX or empty to disable.\r\n"
        "      - [O] Open(ON) state regex search string list management.\r\n"
        "        - {arg1}: Index # 1-8\r\n"
        "        - {arg2}: Regex string for this slot or empty string  to clear\r\n"
        "      - [C] Close(OFF) state regex search string list management.\r\n"
        "        - {arg1}: Index # 1-8\r\n"
        "        - {arg2}: Regex string for this slot or empty string to clear\r\n"
        "      - [F] Fault state regex search string list management.\r\n"
        "        - {arg1}: Index # 1-8\r\n"
        "        - {arg2}: Regex string for this slot or empty  string to clear\r\n"
        "      - [o] Open output format string.\r\n"
        "      - [c] Close output format string.\r\n"
        "      - [f] Fault output format string.\r\n\r\n", _cli_cmd_pushover_command_router
    },
};

/**
 * Register component cli commands
 */
void pushover_register_cmds()
{
    // Register pushover CLI commands
    for (int i = 0; i < ARRAY_SIZE(pushover_cmd_list); i++) {
        cli_register_command(&pushover_cmd_list[i]);
    }
}

/**
 * Initialize component
 */
void pushover_init()
{
    // Register search based virtual switches with the AlarmDecoderParser.
    std::string key = std::string(PUSHOVER_PREFIX) + std::string(PUSHOVER_SAS_CFGKEY);
    int subscribers = 0;
    for (int i = 1; i < 99; i++) {
        int notification_slot = -1;
        ad2_get_nv_slot_key_int(key.c_str(), i, SK_NOTIFY_SLOT, &notification_slot);
        if (notification_slot >= 0) {
            AD2EventSearch *es1 = new AD2EventSearch(AD2_STATE_CLOSED, 0);

            // Save the notification slot. That is all we need to complete the task.
            es1->INT_ARG = notification_slot;
            es1->PTR_ARG = nullptr;

            // We at least need some output format or skip
            ad2_get_nv_slot_key_string(key.c_str(), i, SK_OPEN_OUTPUT_FMT, es1->OPEN_OUTPUT_FORMAT);
            ad2_get_nv_slot_key_string(key.c_str(), i, SK_CLOSED_OUTPUT_FMT, es1->CLOSED_OUTPUT_FORMAT);
            ad2_get_nv_slot_key_string(key.c_str(), i, SK_FAULT_OUTPUT_FMT, es1->FAULT_OUTPUT_FORMAT);
            if ( es1->OPEN_OUTPUT_FORMAT.length()
                    || es1->CLOSED_OUTPUT_FORMAT.length()
                    || es1->FAULT_OUTPUT_FORMAT.length() ) {

                std::string notify_types_sz = "";
                std::vector<std::string> notify_types_v;
                ad2_get_nv_slot_key_string(key.c_str(), i, SK_TYPE_LIST, notify_types_sz);
                ad2_tokenize(notify_types_sz, ", ", notify_types_v);
                for (auto &sztype : notify_types_v) {
                    ad2_trim(sztype);
                    auto x = AD2Parse.message_type_id.find(sztype);
                    if(x != std::end(AD2Parse.message_type_id)) {
                        ad2_message_t mt = (ad2_message_t)AD2Parse.message_type_id.at(sztype);
                        es1->PRE_FILTER_MESAGE_TYPE.push_back(mt);
                    }
                }
                ad2_get_nv_slot_key_string(key.c_str(), i, SK_PREFILTER_REGEX, es1->PRE_FILTER_REGEX);

                // Load all regex search patterns for OPEN,CLOSE and FAULT sub keys.
                std::string regex_sk_list = SK_FAULT_REGEX_LIST SK_CLOSED_REGEX_LIST SK_OPEN_REGEX_LIST;
                for(char& c : regex_sk_list) {
                    std::string sk = ad2_string_printf("%c", c);
                    for ( int a = 1; a < MAX_SEARCH_KEYS; a++) {
                        std::string out = "";
                        std::string tsk = sk + ad2_string_printf("%02i", a);
                        ad2_get_nv_slot_key_string(key.c_str(), i, tsk.c_str(), out);
                        if ( out.length()) {
                            if (c == SK_OPEN_REGEX_LIST[0]) {
                                es1->OPEN_REGEX_LIST.push_back(out);
                            }
                            if (c == SK_CLOSED_REGEX_LIST[0]) {
                                es1->CLOSED_REGEX_LIST.push_back(out);
                            }
                            if (c == SK_FAULT_REGEX_LIST[0]) {
                                es1->FAULT_REGEX_LIST.push_back(out);
                            }
                        }
                    }
                }

                // Save the search to a list for management.
                pushover_AD2EventSearches.push_back(es1);

                // subscribe the AD2EventSearch virtual switch callback.
                AD2Parse.subscribeTo(on_search_match_cb_po, es1);

                // keep track of how many for user feedback.
                subscribers++;
            }
        }
    }

    ESP_LOGI(TAG, "Found and configured %i virtual switches.", subscribers);

}

/**
 * component memory cleanup
 */
void pushover_free()
{
}

#ifdef __cplusplus
} // extern "C"
#endif
#endif /*  CONFIG_AD2IOT_PUSHOVER_CLIENT */

