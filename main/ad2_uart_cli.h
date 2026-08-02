/**
 *  @file    ad2_uart_cli.h
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    02/20/2020
 *
 *  @brief UART command line interface for direct access configuration
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

#ifndef _AD2_UART_CLI_H_
#define _AD2_UART_CLI_H_

#define PROMPT_STRING "AD2IOT # "
#define AD2_HELP_CMD "help"

typedef void (* command_function_t)(const char *string);

typedef struct cli_command {
    char *command;
    char *help_string;
    command_function_t command_fn;
} cli_cmd_t;

typedef struct cli_command_list {
    cli_cmd_t* cmd;
    struct cli_command_list* next;
} cli_cmd_list_t;

void cli_main();
void cli_register_command(cli_cmd_t* cmd);
void cli_task_notify();
void cli_process_command(char* input_string);

/**
 * Route CLI I/O performed by the current task to a connected socket.
 * Calls from all other tasks continue to use the USB UART.
 */
void cli_set_io_socket(int socket_fd);
void cli_clear_io_socket();
bool cli_is_socket_io();
int cli_write_bytes(const char *buffer, size_t length);
int cli_read_bytes(uint8_t *buffer, size_t length, TickType_t timeout);

#endif /* _AD2_UART_CLI_H_ */
