/**
 *  @file    ota_util.h
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    09/18/2020
 *
 *  @brief OTA update support
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

#ifndef _OTA_UTIL_H
#define _OTA_UTIL_H
#if CONFIG_AD2IOT_OTAUPDATE
void ota_do_update(const char *arg);
void ota_check_for_update();
bool ota_update_in_progress();
const char *ota_get_available_version();
void ota_register_cmds();
void ota_init();
#endif
#endif /* _OTA_UTIL_H */

