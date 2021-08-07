/**
 *  @file    ad2_utils.h
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    02/20/2020
 *
 *  @brief AD2IOT common utils shared between main and components
 *
 *  @copyright Copyright (C) 2020 Nu Tech Software Solutions, Inc.
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
#ifndef _AD2_UTILS_H
#define _AD2_UTILS_H

// required
#include "cJSON.h"

// Helper to find array storage size.
#define ARRAY_SIZE(x) (int)(sizeof(x)/sizeof(x[0]))

// Debugging NVS
//#define DEBUG_NVS

#ifdef __cplusplus
extern "C" {
#endif

// Communication with AD2* device / host

void ad2_arm_away(int codeId, int vpartId);
void ad2_arm_stay(int codeId, int vpartId);
void ad2_disarm(int codeId, int vpartId);
void ad2_chime_toggle(int codeId, int vpartId);
void ad2_fire_alarm(int codeId, int vpartId);
void ad2_panic_alarm(int codeId, int vpartId);
void ad2_aux_alarm(int codeId, int vpartId);
void ad2_exit_now(int vpartId);
void ad2_send(std::string &buf);
AD2VirtualPartitionState *ad2_get_partition_state(int vpartId);
cJSON *ad2_get_partition_state_json(AD2VirtualPartitionState *);
void ad2_printf_host(const char *format, ...);
void ad2_snprintf_host(const char *fmt, size_t size, ...);
char ad2_network_mode(std::string &args);
char ad2_log_mode();
void ad2_set_log_mode(char m);

// string utils

std::string ad2_string_printf(const char *fmt, ...);
std::string ad2_string_vaprintf(const char *fmt, va_list args);
std::string ad2_string_vasnprintf(const char *fmt, size_t size, va_list args);
int ad2_copy_nth_arg(std::string &dest, char* src, int n, bool remaining = false);
void ad2_tokenize(std::string const &str, const char* delim, std::vector<std::string> &out);
std::string ad2_to_string(int n);
std::string ad2_make_bearer_auth_header(const std::string& apikey);
std::string ad2_make_basic_auth_header(const std::string& user, const std::string& password);
std::string ad2_urlencode(const std::string str);
void ad2_genUUID(uint8_t n, std::string& ret);
int ad2_query_key_value(std::string &qry_str, const char *key, std::string &val);
void ad2_lcase(std::string &str);
void ad2_ucase(std::string &str);
bool ad2_replace_all(std::string& inStr, const char *findStr, const char *replaceStr);
void ad2_ltrim(std::string &s);
void ad2_rtrim(std::string &s);
void ad2_trim(std::string &s);
void ad2_remove_ws(std::string &s);

/**
 *  @brief ACL parser and test class
 *
 *  @details Simple parser for ACL strings and testing for matches given
 *  an IP string or the uint32_t value of an IP.
 *     Example ACL String: '192.168.0.0/24, 192.168.1.1-192.168.1.2'
 */
class ad2_acl_check
{
public:
    int add(std::string& acl);
    bool find(std::string ip);
    bool find(uint32_t ip);
    void clear()
    {
        allowed_networks.clear();
    };

    enum {
        ACL_FORMAT_OK = 0,
        ACL_ERR_BADFORMAT_CIDR = -1,
        ACL_ERR_BADFORMAT_IP = -2
    };

private:
    // @brief parse ip address string to uint32_t value.
    bool _ipaddr(std::string& ip, uint32_t& addr)
    {
        int a, b, c, d;
        if (std::sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
            return false;
        } else {
            addr = a << 24;
            addr |= b << 16;
            addr |= c << 8;
            addr |= d;
        };
        return true;
    };
    // @brief list of allowed network start and end addresses.
    vector<pair<int,int>> allowed_networks;
};

// NV Storage utilities

void ad2_get_nv_arg(const char *key, std::string &value);
void ad2_set_nv_arg(const char *key, const char *value);
void ad2_set_nv_slot_key_int(const char *key, int slot, const char *suffix, int value);
void ad2_get_nv_slot_key_int(const char *key, int slot, const char *suffix, int *value);
void ad2_set_nv_slot_key_string(const char *key, int slot, const char *suffix, const char *value);
void ad2_get_nv_slot_key_string(const char *key, int slot, const char *suffix, std::string &value);

#ifdef __cplusplus
}
#endif

#endif /* _AD2_UTILS_H */

