/**
 *  @file    ser2sock.h
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    12/17/2020
 *
 *  @brief ser2sock server daemon
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
#ifndef _SER2SOCKD_H
#define _SER2SOCKD_H
#if CONFIG_AD2IOT_SER2SOCKD

/* Constants that aren't configurable in menuconfig */
#define PORT 10000
#define MAX_CLIENTS 4
#define MAX_FIFO_BUFFERS 30
#define MAXCONNECTIONS MAX_CLIENTS+1

#define SD2D_COMMAND          "ser2sockd"
#define S2SD_SUBCMD_ENABLE    "enable"
#define S2SD_SUBCMD_ACL       "acl"

#ifdef __cplusplus
extern "C" {
#endif

void ser2sockd_register_cmds();
void ser2sockd_init();
void ser2sockd_server_task(void *pvParameters);
void ser2sockd_sendall(uint8_t *buffer, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* CONFIG_AD2IOT_SER2SOCKD */
#endif /* _SER2SOCKD_H */

