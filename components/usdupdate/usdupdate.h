/**
 *  @file    usdupdate_util.h
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    09/18/2020
 *
 *  @brief uSD firmware update support
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

#ifndef _USDUPDATE_UTIL_H
#define _USDUPDATE_UTIL_H

#include <stddef.h>

struct usd_firmware_status {
    bool sd_mounted;
    bool present;
    bool valid;
    bool update_in_progress;
    size_t size_bytes;
    char version[32];
    char project_name[32];
    char build_date[16];
    char build_time[16];
    char error[96];
};

void usd_do_update(const char *arg);
bool usd_start_update();
bool usd_update_in_progress();
bool usd_get_firmware_status(usd_firmware_status *status);
void usdupdate_register_cmds();
void usdupdate_init();

#endif /* _USDUPDATE_UTIL_H */

