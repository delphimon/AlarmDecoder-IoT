/**
 * @file network_cli.cpp
 * @brief Authenticated TCP transport for the AD2IoT command line interface.
 */
#include "alarmdecoder_main.h"

#if CONFIG_AD2IOT_NETWORK_CLI

#include <errno.h>
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "network_cli.h"

static const char *TAG = "NETCLI";

#define NETWORK_CLI_COMMAND          "netcli"
#define NETWORK_CLI_EXIT_COMMAND     "exit"
#define NETWORK_CLI_QUIT_COMMAND     "quit"
#define NETWORK_CLI_CONFIG_SECTION   "netcli"
#define NETWORK_CLI_KEY_ENABLE       "enable"
#define NETWORK_CLI_KEY_PORT         "port"
#define NETWORK_CLI_KEY_ACL          "acl"
#define NETWORK_CLI_KEY_PASSWORD     "password"
#define NETWORK_CLI_DEFAULT_PORT     2323
#define NETWORK_CLI_MIN_PASSWORD     8
#define NETWORK_CLI_MAX_PASSWORD     128

static ad2_acl_check network_cli_acl;
static bool network_cli_disconnect_requested = false;

enum telnet_command {
    TELNET_SE = 240,
    TELNET_SB = 250,
    TELNET_WILL = 251,
    TELNET_WONT = 252,
    TELNET_DO = 253,
    TELNET_DONT = 254,
    TELNET_IAC = 255
};

enum telnet_option {
    TELNET_ECHO = 1,
    TELNET_SUPPRESS_GO_AHEAD = 3
};

static bool _secure_compare(const std::string &left, const std::string &right)
{
    size_t max_length = left.length() > right.length() ? left.length() : right.length();
    unsigned int difference = left.length() ^ right.length();
    for (size_t i = 0; i < max_length; i++) {
        unsigned char l = i < left.length() ? left[i] : 0;
        unsigned char r = i < right.length() ? right[i] : 0;
        difference |= l ^ r;
    }
    return difference == 0;
}

static bool _send_all(int socket_fd, const char *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int result = send(socket_fd, buffer + sent, length - sent, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += result;
    }
    return true;
}

static bool _send_string(int socket_fd, const char *message)
{
    return _send_all(socket_fd, message, strlen(message));
}

static void _enable_telnet_mode(int socket_fd, bool &telnet_mode)
{
    if (telnet_mode) {
        return;
    }
    telnet_mode = true;

    // Advertise server-side echo so Telnet clients disable local echo. Raw
    // TCP clients never receive these bytes because Telnet mode is detected
    // from an incoming IAC command first.
    const unsigned char negotiation[] = {
        TELNET_IAC, TELNET_WILL, TELNET_ECHO,
        TELNET_IAC, TELNET_WILL, TELNET_SUPPRESS_GO_AHEAD,
        TELNET_IAC, TELNET_DO, TELNET_SUPPRESS_GO_AHEAD
    };
    _send_all(socket_fd, reinterpret_cast<const char *>(negotiation), sizeof(negotiation));
}

static bool _detect_telnet_client(int socket_fd, bool &telnet_mode)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);
    struct timeval wait = {};
    wait.tv_usec = 200000;

    int ready = select(socket_fd + 1, &read_fds, NULL, NULL, &wait);
    if (ready <= 0) {
        return false;
    }

    unsigned char first_byte = 0;
    int result = recv(socket_fd, &first_byte, 1, MSG_PEEK);
    if (result == 1 && first_byte == TELNET_IAC) {
        _enable_telnet_mode(socket_fd, telnet_mode);
        return true;
    }
    return false;
}

static bool _consume_telnet_command(int socket_fd, bool &telnet_mode)
{
    _enable_telnet_mode(socket_fd, telnet_mode);

    unsigned char command = 0;
    int result = recv(socket_fd, &command, 1, 0);
    if (result <= 0) {
        return false;
    }

    if (command == TELNET_IAC) {
        return true;
    }
    if (command == TELNET_SB) {
        unsigned char previous = 0;
        unsigned char current = 0;
        do {
            result = recv(socket_fd, &current, 1, 0);
            if (result <= 0) {
                return false;
            }
            if (previous == TELNET_IAC && current == TELNET_SE) {
                break;
            }
            previous = current;
        } while (hal_get_network_connected());
        return true;
    }
    if (command == TELNET_WILL || command == TELNET_WONT ||
            command == TELNET_DO || command == TELNET_DONT) {
        unsigned char option = 0;
        return recv(socket_fd, &option, 1, 0) == 1;
    }
    return true;
}

static int _read_line(int socket_fd, std::string &line, size_t max_length, bool echo,
                      bool &telnet_mode)
{
    line.clear();

    while (hal_get_network_connected()) {
        unsigned char ch;
        int result = recv(socket_fd, &ch, 1, 0);
        if (result == 0) {
            return 0;
        }
        if (result < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }

        if (ch == TELNET_IAC) {
            if (!_consume_telnet_command(socket_fd, telnet_mode)) {
                return -1;
            }
            continue;
        }

        if (ch == '\n') {
            // Ignore the LF half of CRLF and empty input lines.
            if (line.empty()) {
                continue;
            }
            if (echo) {
                _send_string(socket_fd, "\r\n");
            }
            return 1;
        }
        if (ch == '\r') {
            if (echo) {
                _send_string(socket_fd, "\r\n");
            }
            return 1;
        }

        if (ch == 0x03) {
            line.clear();
            if (echo) {
                _send_string(socket_fd, "^C\r\n");
            }
            return 1;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (!line.empty()) {
                line.pop_back();
                if (echo) {
                    _send_string(socket_fd, "\b \b");
                }
            }
            continue;
        }
        if (ch < ' ' || ch > '~') {
            continue;
        }
        if (line.length() >= max_length) {
            _send_string(socket_fd, "\r\nInput line too long.\r\n");
            return -2;
        }
        line.push_back(ch);
        if (echo) {
            _send_all(socket_fd, reinterpret_cast<const char *>(&ch), 1);
        }
    }
    return -1;
}

static bool _authenticate(int socket_fd, const std::string &password, bool &telnet_mode)
{
    _detect_telnet_client(socket_fd, telnet_mode);
    _send_string(socket_fd, "AD2IoT network CLI\r\n");
    for (int attempt = 0; attempt < 3; attempt++) {
        _send_string(socket_fd, "Password: ");
        std::string supplied;
        int result = _read_line(socket_fd, supplied, NETWORK_CLI_MAX_PASSWORD, false, telnet_mode);
        _send_string(socket_fd, "\r\n");
        if (result <= 0) {
            return false;
        }
        if (_secure_compare(supplied, password)) {
            return true;
        }
        _send_string(socket_fd, "Authentication failed.\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

static void _handle_client(int socket_fd, const std::string &password)
{
    struct timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    bool telnet_mode = false;
    if (!_authenticate(socket_fd, password, telnet_mode)) {
        return;
    }

    network_cli_disconnect_requested = false;
    cli_set_io_socket(socket_fd);
    ad2_printf_host(false, "Authenticated. Type 'help' for commands.\r\n" PROMPT_STRING);

    while (hal_get_network_connected()) {
        std::string command;
        int result = _read_line(socket_fd, command, MAX_UART_CMD_SIZE - 1, true, telnet_mode);
        if (result <= 0) {
            break;
        }
        if (result == 1 && !command.empty()) {
            cli_process_command(&command[0]);
        }
        if (network_cli_disconnect_requested) {
            break;
        }
        ad2_printf_host(false, PROMPT_STRING);
    }
    cli_clear_io_socket();
}

static void _cli_cmd_network_cli_exit(const char *string)
{
    if (!cli_is_socket_io()) {
        ad2_printf_host(false, "This command only closes a network CLI session.\r\n");
        return;
    }
    ad2_printf_host(false, "Goodbye.\r\n");
    network_cli_disconnect_requested = true;
}

static void network_cli_server_task(void *pvParameters)
{
    int port = NETWORK_CLI_DEFAULT_PORT;
    ad2_get_config_key_int(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PORT, &port);
    if (port < 1 || port > 65535) {
        port = NETWORK_CLI_DEFAULT_PORT;
    }

    std::string password;
    ad2_get_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PASSWORD, password);

    while (true) {
        while (!hal_get_network_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_socket < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int reuse = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);

        if (bind(listen_socket, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0 ||
                listen(listen_socket, 1) != 0) {
            ESP_LOGE(TAG, "Unable to listen on TCP port %d: errno %d", port, errno);
            close(listen_socket);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int flags = fcntl(listen_socket, F_GETFL, 0);
        fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
        ESP_LOGI(TAG, "Listening on TCP port %d", port);

        while (hal_get_network_connected()) {
            struct sockaddr_storage peer_address = {};
            socklen_t peer_length = sizeof(peer_address);
            int client_socket = accept(listen_socket,
                                       reinterpret_cast<struct sockaddr *>(&peer_address),
                                       &peer_length);
            if (client_socket < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ESP_LOGW(TAG, "Accept failed: errno %d", errno);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            std::string client_ip;
            hal_get_socket_client_ip(client_socket, client_ip);
            if (!network_cli_acl.find(client_ip)) {
                ESP_LOGW(TAG, "Rejected connection from '%s'", client_ip.c_str());
                close(client_socket);
                continue;
            }

            ESP_LOGI(TAG, "Connection from '%s'", client_ip.c_str());
            _handle_client(client_socket, password);
            shutdown(client_socket, SHUT_RDWR);
            close(client_socket);
        }

        close(listen_socket);
    }
}

static void _cli_cmd_network_cli_event(const char *string)
{
    std::string subcommand;
    ad2_copy_nth_arg(subcommand, string, 1);
    ad2_lcase(subcommand);

    std::string argument;
    if (subcommand == NETWORK_CLI_KEY_ENABLE) {
        if (ad2_copy_nth_arg(argument, string, 2) >= 0) {
            bool enable = argument[0] == 'Y' || argument[0] == 'y';
            ad2_set_config_key_bool(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ENABLE, enable);
            ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
        }
        bool enabled = false;
        ad2_get_config_key_bool(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ENABLE, &enabled);
        ad2_printf_host(false, "Network CLI is '%s'.\r\n", enabled ? "Enabled" : "Disabled");
    } else if (subcommand == NETWORK_CLI_KEY_PORT) {
        if (ad2_copy_nth_arg(argument, string, 2) >= 0) {
            int port = std::atoi(argument.c_str());
            if (port < 1 || port > 65535) {
                ad2_printf_host(false, "Port must be between 1 and 65535.\r\n");
                return;
            }
            ad2_set_config_key_int(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PORT, port);
            ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
        }
        int port = NETWORK_CLI_DEFAULT_PORT;
        ad2_get_config_key_int(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PORT, &port);
        ad2_printf_host(false, "Network CLI port is '%d'.\r\n", port);
    } else if (subcommand == NETWORK_CLI_KEY_ACL) {
        if (ad2_copy_nth_arg(argument, string, 2, true) >= 0) {
            ad2_acl_check candidate;
            if (candidate.add(argument) != candidate.ACL_FORMAT_OK) {
                ad2_printf_host(false, "Error parsing ACL string. Not saved.\r\n");
                return;
            }
            ad2_set_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ACL, argument.c_str());
            ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
        }
        std::string acl = "127.0.0.1";
        ad2_get_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ACL, acl);
        ad2_printf_host(false, "Network CLI ACL is '%s'.\r\n", acl.c_str());
    } else if (subcommand == NETWORK_CLI_KEY_PASSWORD) {
        if (ad2_copy_nth_arg(argument, string, 2, true) < 0) {
            std::string password;
            ad2_get_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PASSWORD, password);
            ad2_printf_host(false, "Network CLI password is %s.\r\n", password.empty() ? "not set" : "set");
            return;
        }
        if (argument == "-") {
            ad2_set_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PASSWORD, "", -1, NULL, true);
            ad2_printf_host(false, "Network CLI password cleared.\r\n");
        } else if (argument.length() < NETWORK_CLI_MIN_PASSWORD) {
            ad2_printf_host(false, "Password must contain at least %d characters.\r\n", NETWORK_CLI_MIN_PASSWORD);
        } else if (argument.length() > NETWORK_CLI_MAX_PASSWORD) {
            ad2_printf_host(false, "Password must contain at most %d characters.\r\n", NETWORK_CLI_MAX_PASSWORD);
        } else {
            ad2_set_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PASSWORD, argument.c_str());
            ad2_printf_host(false, "Network CLI password updated. Restart required to take effect.\r\n");
        }
    } else {
        ad2_printf_host(false, "What?\r\n");
    }
}

static struct cli_command network_cli_commands[] = {
    {
        (char *)NETWORK_CLI_COMMAND, (char *)
        "Usage: netcli <command> [arg]\r\n"
        "\r\n"
        "    Configure the authenticated TCP command-line server.\r\n"
        "Commands:\r\n"
        "    enable [Y|N]            Set or get the enable flag\r\n"
        "    port [1-65535]          Set or get the TCP port (default 2323)\r\n"
        "    acl [aclString]         Set or get the allowed IP/CIDR list\r\n"
        "    password [value|-]      Set, clear, or report password status\r\n"
        "Examples:\r\n"
        "    netcli password use-a-long-unique-password\r\n"
        "    netcli acl 192.168.1.0/24\r\n"
        "    netcli enable Y\r\n",
        _cli_cmd_network_cli_event
    },
    {
        (char *)NETWORK_CLI_EXIT_COMMAND, (char *)
        "Usage: exit\r\n"
        "    Close the current network CLI connection.\r\n",
        _cli_cmd_network_cli_exit
    },
    {
        (char *)NETWORK_CLI_QUIT_COMMAND, (char *)
        "Usage: quit\r\n"
        "    Alias for exit; close the current network CLI connection.\r\n",
        _cli_cmd_network_cli_exit
    }
};

void network_cli_register_cmds()
{
    for (int i = 0; i < ARRAY_SIZE(network_cli_commands); i++) {
        cli_register_command(&network_cli_commands[i]);
    }
}

void network_cli_init()
{
    bool enabled = false;
    ad2_get_config_key_bool(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ENABLE, &enabled);
    if (!enabled) {
        ad2_printf_host(true, "%s: Server disabled", TAG);
        return;
    }

    std::string password;
    ad2_get_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_PASSWORD, password);
    if (password.length() < NETWORK_CLI_MIN_PASSWORD) {
        ESP_LOGE(TAG, "Server not started: configure a password of at least %d characters",
                 NETWORK_CLI_MIN_PASSWORD);
        return;
    }

    std::string acl = "127.0.0.1";
    ad2_get_config_key_string(NETWORK_CLI_CONFIG_SECTION, NETWORK_CLI_KEY_ACL, acl);
    if (!acl.empty() && network_cli_acl.add(acl) != network_cli_acl.ACL_FORMAT_OK) {
        ESP_LOGE(TAG, "Server not started: invalid ACL '%s'", acl.c_str());
        return;
    }

    xTaskCreate(network_cli_server_task, "AD2 network CLI", 1024 * 6, NULL,
                tskIDLE_PRIORITY + 1, NULL);
}

#endif /* CONFIG_AD2IOT_NETWORK_CLI */
