/**
*  @file    ser2sock.cpp
*  @author  Sean Mathews <coder@f34r.com>
*  @date    12/17/2020
*  @version 1.0.0
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
// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <atomic>

// Disable via sdkconfig
#if CONFIG_AD2IOT_SER2SOCKD
static const char *TAG = "SER2SOCKD";

// AlarmDecoder std includes
#include "alarmdecoder_main.h"

// esp component includes
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "esp_system.h"

// specific includes
#include "ser2sock.h"

/* Constants that aren't configurable in menuconfig */
//#define S2SD_DEBUG
#define PORT 10000
#define MAX_CLIENTS 4
#define MAX_FIFO_BUFFERS 30
#define MAXCONNECTIONS (MAX_CLIENTS+1)

#define SD2D_COMMAND          "ser2sockd"
#define S2SD_SUBCMD_ENABLE    "enable"
#define S2SD_SUBCMD_ACL       "acl"

#define S2SD_CONFIG_SECTION "ser2sockd"

// forward decl
void ser2sockd_server_task(void *pvParameters);

// types and structs
enum FD_TYPES {
    NA, LISTEN_SOCKET = 1, CLIENT_SOCKET
} fd_types;

typedef struct {
    int size, in, out, avail;
    void **table;
} fifo;

typedef struct {
    size_t size;
    size_t offset;
    unsigned char buffer[];
} fifo_buffer;

typedef struct {
    /* flags */
    int inuse;
    int fd_type;

    /* the fd */
    int fd;

    /* Set by a producer when a client must be closed by the server task. */
    bool disconnect_requested;

    /* the buffer */
    fifo send_buffer;
} FDs;

// fifo buffer stuff
static void _fifo_init(fifo *f, int size);
static int _fifo_empty(fifo *f);
static void* _fifo_make_buffer(void *in_buffer, size_t len);
static int _fifo_add(fifo *f, void *next);
static void* _fifo_peek(fifo *f);
static void* _fifo_get(fifo *f);
static void _fifo_clear(fifo *f);
static int _cleanup_fd(int n);
static void ser2sockd_shutdown(void);

int listen_backlog = 10;

FDs my_fds[MAXCONNECTIONS];

/* our listen socket */
int listen_sock = -1;
struct sockaddr_in serv_addr;

/* Protects connection state and the per-client producer/consumer FIFOs. */
static SemaphoreHandle_t fds_mutex = NULL;
static std::atomic_bool ser2sockd_shutting_down(false);
static bool shutdown_handler_registered = false;

/* ACL control */
ad2_acl_check ser2sock_acl;

/**
 * ser2sock command list and enum.
 */
char * S2SD_SUBCMD [] = {
    (char*)S2SD_SUBCMD_ENABLE,
    (char*)S2SD_SUBCMD_ACL,
    0 // EOF
};

enum {
    S2SD_SUBCMD_ENABLE_ID = 0,
    S2SD_SUBCMD_ACL_ID,
};

/**
 * ser2sockd generic command event processing
 *  command: [COMMAND] <id> <arg>
 * ex.
 *   [COMMAND] 0 arg...
 */
static void _cli_cmd_ser2sockd_event(const char *string)
{

    // key value validation
    std::string cmd;
    ad2_copy_nth_arg(cmd, string, 0);
    ad2_lcase(cmd);

    if(cmd.compare(SD2D_COMMAND) != 0) {
        ad2_printf_host(false, "What?\r\n");
        return;;
    }

    // key value validation
    std::string subcmd;
    ad2_copy_nth_arg(subcmd, string, 1);
    ad2_lcase(subcmd);

    int i;
    bool en;
    for(i = 0;; ++i) {
        if (S2SD_SUBCMD[i] == 0) {
            ad2_printf_host(false, "What?\r\n");
            break;
        }
        if(subcmd.compare(S2SD_SUBCMD[i]) == 0) {
            std::string arg;
            std::string acl;
            switch(i) {
            /**
             * Enable/Disable ser2sock daemon.
             */
            case S2SD_SUBCMD_ENABLE_ID:
                if (ad2_copy_nth_arg(arg, string, 2) >= 0) {
                    ad2_set_config_key_bool(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ENABLE, (arg[0] == 'Y' || arg[0] ==  'y'));
                    ad2_printf_host(false, "Success setting value. Restart required to take effect.\r\n");
                }

                // show contents of this slot
                en = false;
                ad2_get_config_key_bool(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ENABLE, &en);
                ad2_printf_host(false, "ser2sock daemon is '%s'.\r\n", (en ? "Enabled" : "Disabled"));
                break;
            /**
             * ser2sock daemon IP/CIDR ACL list.
             */
            case S2SD_SUBCMD_ACL_ID:
                // If no arg then return ACL list
                if (ad2_copy_nth_arg(arg, string, 2, true) >= 0) {
                    ser2sock_acl.clear();
                    int res = ser2sock_acl.add(arg);
                    if (res == ser2sock_acl.ACL_FORMAT_OK) {
                        ad2_set_config_key_string(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ACL, arg.c_str());
                    } else {
                        ad2_printf_host(false, "Error parsing ACL string. Check ACL format. Not saved.\r\n");
                    }
                }
                // show contents of this slot set default to allow all
                acl = "0.0.0.0/0";
                ad2_get_config_key_string(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ACL, acl);
                ad2_printf_host(false, "ser2sockd 'acl' set to '%s'.\r\n", acl.c_str());
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
static struct cli_command ser2sockd_cmd_list[] = {
    {
        (char*)SD2D_COMMAND,(char*)
        "Usage: ser2sockd <command> [arg]\r\n"
        "\r\n"
        "    Configuration tool for ser2sock server\r\n"
        "Commands:\r\n"
        "    enable [Y|N]            Set or get enable flag\r\n"
        "    acl [aclString|-]       Set or get ACL CIDR CSV list use - to delete\r\n"
        "Examples:\r\n"
        "    ```ser2sockd enable Y```\r\n"
        "    ```ser2sockd acl 192.168.0.0/28,192.168.1.0-192.168.1.10,192.168.3.4```\r\n"
        , _cli_cmd_ser2sockd_event
    }
};

/**
 * @brief Register componet cli commands.
 */
void ser2sockd_register_cmds()
{
    // Register ser2sock CLI commands
    for (int i = 0; i < ARRAY_SIZE(ser2sockd_cmd_list); i++) {
        cli_register_command(&ser2sockd_cmd_list[i]);
    }
}

/**
 * @brief Initialize the ser2sock daemon
 */
void ser2sockd_init(void)
{
    if (fds_mutex == NULL) {
        fds_mutex = xSemaphoreCreateMutex();
        if (fds_mutex == NULL) {
            ESP_LOGE(TAG, "Unable to create connection-state mutex");
            return;
        }
    }

    if (!shutdown_handler_registered) {
        esp_err_t shutdown_handler_result = esp_register_shutdown_handler(ser2sockd_shutdown);
        if (shutdown_handler_result == ESP_OK) {
            shutdown_handler_registered = true;
        } else {
            ESP_LOGW(TAG, "Unable to register shutdown handler: %s",
                     esp_err_to_name(shutdown_handler_result));
        }
    }

    // load and parse ACL if set or set default to allow all.
    std::string acl = "0.0.0.0/0";
    ad2_get_config_key_string(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ACL, acl);
    if (acl.length()) {
        int res = ser2sock_acl.add(acl);
        if (res != ser2sock_acl.ACL_FORMAT_OK) {
            ESP_LOGW(TAG, "ACL parse error %i for '%s'", res, acl.c_str());
        }
    }

    int x;
    for (x = 0; x < MAXCONNECTIONS; x++) {
        my_fds[x].inuse = false;
        my_fds[x].fd = -1;
        my_fds[x].fd_type = NA;
        my_fds[x].disconnect_requested = false;
        _fifo_init(&my_fds[x].send_buffer, MAX_FIFO_BUFFERS);
        if (my_fds[x].send_buffer.table == NULL) {
            ESP_LOGE(TAG, "Unable to allocate send queue for slot %d", x);
            for (int y = 0; y < x; y++) {
                free(my_fds[y].send_buffer.table);
                my_fds[y].send_buffer.table = NULL;
            }
            return;
        }
    }

    bool en = false;
    ad2_get_config_key_bool(S2SD_CONFIG_SECTION, S2SD_SUBCMD_ENABLE, &en);

    // nothing more needs to be done once commands are set if not enabled.
    if (!en) {
        ad2_printf_host(true, "%s: Client disabled", TAG);
        return;
    }

    // ser2sockd worker thread
    // 20210815SM: 1284 bytes stack free after first connection.
    ad2_printf_host(true, "%s: Init done, daemon starting.", TAG);
    xTaskCreate(&ser2sockd_server_task, "AD2 ser2sockd", 1024*4, NULL, tskIDLE_PRIORITY+1, NULL);

}


//<Fifo Buffer>
/*
 init queue allocate memory
 */
static void _fifo_init(fifo *f, int size)
{
    f->avail = 0;
    f->in = 0;
    f->out = 0;
    f->size = size;
    f->table = (void**) malloc(f->size * sizeof(void*));
}

/*
 fifo empty if queue = 1 else 0
 */
static int _fifo_empty(fifo *f)
{
    return (f->avail == 0);
}

/*
 remove all stored pending data
 */
static void _fifo_clear(fifo *f)
{
    void *p;
    while (!_fifo_empty(f)) {
        p = _fifo_get(f);
        if (p) {
            free(p);
        }
    }
}

/*
 allocate a fifo_buffer and fill it with the supplied in_buffer and len.
 if len is 0 it will be calculated using strlen() so NULL chars in the
 stream will be excluded and thus not true binary. For binary and to include
 NULL values in the stream len must be > 0.
 note:
	this has a specific type where all the other fifo low level routines
	are all void *.
 */
static void* _fifo_make_buffer(void *in_buffer, size_t len)
{
    fifo_buffer* out_buffer;

    // Calculate the buffer size as a string not including the null terminator
    if (!len) {
        len = strlen((const char *)in_buffer);
    }
    out_buffer = (fifo_buffer *)malloc(sizeof(fifo_buffer)+len);
    if (!out_buffer) {
        return NULL;
    }
    memcpy(out_buffer->buffer, in_buffer, len);
    out_buffer->size = len;
    out_buffer->offset = 0;
    return (void*) out_buffer;
}

/*
 insert an element
 this must be already allocated with malloc or strdup
 */
static int _fifo_add(fifo *f, void *next)
{
    if (!f->table || !next || f->avail == f->size) {
        return (0);
    } else {
        f->table[f->in] = next;
        f->avail++;
        f->in = (f->in + 1) % f->size;
        return (1);
    }
}

/*
 return the next element without removing it
 */
static void* _fifo_peek(fifo *f)
{
    if (f->avail > 0) {
        return f->table[f->out];
    }
    return NULL;
}

/*
 return next element
 */
static void* _fifo_get(fifo *f)
{
    void* get;
    if (f->avail > 0) {
        get = f->table[f->out];
        f->out = (f->out + 1) % f->size;
        f->avail--;
        return (get);
    }
    return 0;
}
//</Fifo Buffer>

/*
 Cleanup an entry in the fd array and do any fd_type specific cleanup
 */
static int _cleanup_fd(int n)
{
    int fd = -1;

    if (n < 0 || n >= MAXCONNECTIONS || fds_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
        /* Mark the slot unused before closing so producers stop queueing. */
        if (my_fds[n].inuse) {
            fd = my_fds[n].fd;
            my_fds[n].fd = -1;
            my_fds[n].inuse = false;
            my_fds[n].fd_type = NA;
            my_fds[n].disconnect_requested = false;
            _fifo_clear(&my_fds[n].send_buffer);
        }
        xSemaphoreGive(fds_mutex);
    }

    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (fd == listen_sock) {
            listen_sock = -1;
        }
    }
    return true;
}

/*
 Close all sockets before esp_restart() resets the network stack. This gives
 connected Home Assistant clients an immediate close/RST instead of leaving a
 half-open TCP session behind.
 */
static void ser2sockd_shutdown(void)
{
    ser2sockd_shutting_down = true;
    for (int n = 0; n < MAXCONNECTIONS; n++) {
        _cleanup_fd(n);
    }
}

/*
 ser2sockd_sendall
 adds a buffer to every connected socket fd ie multiplexes
 */
void ser2sockd_sendall(uint8_t *buffer, size_t len)
{
    uint32_t disconnect_slots = 0;

    if (!buffer || !len || fds_mutex == NULL || ser2sockd_shutting_down) {
        return;
    }

    /*
     Adding anything to the fifo must be allocated so it can be free'd later
     Not very efficient but we have plenty of mem with as few connections as we
     will use. If we needed many more I would need to re-factor this code
     */
    if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
        for (int n = 0; n < MAXCONNECTIONS; n++) {
            if (my_fds[n].inuse && my_fds[n].fd_type == CLIENT_SOCKET &&
                    !my_fds[n].disconnect_requested) {
                void *tempbuffer = _fifo_make_buffer(buffer, len);
                if (!tempbuffer || !_fifo_add(&my_fds[n].send_buffer, tempbuffer)) {
                    free(tempbuffer);
                    my_fds[n].disconnect_requested = true;
                    disconnect_slots |= (1U << n);
                }
            }
        }
        xSemaphoreGive(fds_mutex);
    }

    for (int n = 0; n < MAXCONNECTIONS; n++) {
        if (disconnect_slots & (1U << n)) {
            ESP_LOGW(TAG, "Closing slow client in slot %d: send queue exhausted", n);
        }
    }
}

/*
 add all of our fd to our r,w and e fd sets
*/
static void _build_fdsets(fd_set *read_fdset, fd_set *write_fdset, fd_set *except_fdset)
{
    int n;

    /* add all sockets to our fdset */
    FD_ZERO(read_fdset);
    FD_ZERO(write_fdset);
    FD_ZERO(except_fdset);
    if (fds_mutex == NULL || xSemaphoreTake(fds_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (n = 0; n < MAXCONNECTIONS; n++) {
        if (my_fds[n].inuse && !my_fds[n].disconnect_requested) {
            FD_SET(my_fds[n].fd, read_fdset);
            FD_SET(my_fds[n].fd, except_fdset);
            if (my_fds[n].fd_type == CLIENT_SOCKET &&
                    !_fifo_empty(&my_fds[n].send_buffer)) {
                FD_SET(my_fds[n].fd, write_fdset);
            }
        }
    }
    xSemaphoreGive(fds_mutex);
}

/*
 poll any exception fd's return TRUE if we did some work
 */
static bool _poll_exception_fdset(fd_set *except_fdset)
{
    bool did_work = false;

    for (int n = 0; n < MAXCONNECTIONS; n++) {
        int fd = -1;
        int fd_type = NA;

        if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
            if (my_fds[n].inuse) {
                fd = my_fds[n].fd;
                fd_type = my_fds[n].fd_type;
            }
            xSemaphoreGive(fds_mutex);
        }

        if (fd >= 0 && fd_type == CLIENT_SOCKET && FD_ISSET(fd, except_fdset)) {
            did_work = true;
            ESP_LOGE(TAG, "Exception occurred on socket fd slot %i closing the socket. %s",n, strerror(errno));
            _cleanup_fd(n);
        }
    }
    return did_work;
}

/*
 Makes a fd non blocking
 */
static void _set_non_blocking(int fd)
{
    int res = 1;
    if (ioctl(fd, FIONBIO, &res) < 0) {
        ESP_LOGE(TAG, "Error setting FIONBIO");
    }
}

/*
 Add a fd to our array so we can poll it in our state machien loop
 */
static int _add_fd(int fd, int fd_type)
{
    int results = -1;
    struct linger solinger;

    if (fds_mutex == NULL || ser2sockd_shutting_down) {
        return -1;
    }

    _set_non_blocking(fd);

    solinger.l_onoff = true;
    solinger.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &solinger, sizeof(solinger));

    if (fd_type == CLIENT_SOCKET) {
        int ret;
        int keep_alive = 1;
        ret = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int));
        if (ret < 0) {
            ESP_LOGE(TAG, "socket set keep-alive failed %d", errno);
        }

        int idle = 10;
        ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(int));
        if (ret < 0) {
            ESP_LOGE(TAG, "socket set keep-idle failed %d", errno);
        }

        int interval = 5;
        ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(int));
        if (ret < 0) {
            ESP_LOGE(TAG, "socket set keep-interval failed %d", errno);
        }

        int maxpkt = 3;
        ret = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(int));
        if (ret < 0) {
            ESP_LOGE(TAG, "socket set keep-count failed %d", errno);
        }
    }

    if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
        for (int x = 0; x < MAXCONNECTIONS; x++) {
            if (my_fds[x].inuse == false) {
                _fifo_clear(&my_fds[x].send_buffer);
                my_fds[x].disconnect_requested = false;
                my_fds[x].inuse = true;
                my_fds[x].fd_type = fd_type;
                my_fds[x].fd = fd;
                results = x;
                break;
            }
        }
        xSemaphoreGive(fds_mutex);
    }

    return results;
}

/* Close clients that a producer marked after queue allocation/overflow failure. */
static void _cleanup_requested_fds(void)
{
    uint32_t disconnect_slots = 0;

    if (fds_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
        for (int x = 0; x < MAXCONNECTIONS; x++) {
            if (my_fds[x].inuse && my_fds[x].disconnect_requested) {
                disconnect_slots |= (1U << x);
            }
        }
        xSemaphoreGive(fds_mutex);
    }

    for (int x = 0; x < MAXCONNECTIONS; x++) {
        if (disconnect_slots & (1U << x)) {
            _cleanup_fd(x);
        }
    }
}

/*
  poll any read fd's return true if we did do some work
 */
static bool _poll_read_fdset(fd_set *read_fdset)
{
    int received, newsockfd, added_slot;
    bool did_work = false;
    char buffer[1024] = {0};

    /* check every socket to find the one that needs read */
    for (int n = 0; n < MAXCONNECTIONS; n++) {
        int fd = -1;
        int fd_type = NA;

        if (xSemaphoreTake(fds_mutex, portMAX_DELAY) == pdTRUE) {
            if (my_fds[n].inuse) {
                fd = my_fds[n].fd;
                fd_type = my_fds[n].fd_type;
            }
            xSemaphoreGive(fds_mutex);
        }

        /* check read fd */
        if (fd >= 0 && FD_ISSET(fd,read_fdset)) {
            /*  if this is a listening socket then we accept on it and
             * get a new client socket
             */
            if (fd_type == LISTEN_SOCKET) {
                /* clear our state vars */
                newsockfd = -1;
                {
                    socklen_t addr_len;
                    struct sockaddr_storage peer_addr = {};
                    addr_len = sizeof(peer_addr);
                    newsockfd = accept(fd, (struct sockaddr *) &peer_addr, &addr_len);
                }
                if (newsockfd != -1) {
                    /* reset our added id to a bad state */
                    added_slot = -2;

                    // Convert client address to string for ACL testing.
                    std::string IP;
                    hal_get_socket_client_ip(newsockfd, IP);

                    /* ACL test */
                    if (!ser2sock_acl.find(IP)) {
                        struct linger lo = { 1, 0 };
                        setsockopt(newsockfd, SOL_SOCKET, SO_LINGER, &lo, sizeof(lo));
                        close(newsockfd);
                        ESP_LOGW(TAG, "Rejecting client connection from '%s'", IP.c_str());
                    } else {
                        added_slot = _add_fd(newsockfd, CLIENT_SOCKET);
                        if (added_slot >= 0) {
#if defined(S2SD_DEBUG)
                            ESP_LOGI(TAG, "Socket connected slot %i from %s", added_slot, IP.c_str());
#endif
                            did_work = true;
                        } else {
#if defined(S2SD_DEBUG)
                            ESP_LOGE(TAG,"add slot error %i", added_slot);
#endif
                            close(newsockfd);
                            if(added_slot == -1) {
                                ESP_LOGW(TAG, "Socket refused. Max connections.");
                            }
                        }
                    }
#if defined(S2SD_DEBUG)
                } else {
                    ESP_LOGW(TAG,"accept errno: %i '%s' %i", errno, strerror(errno), fd);
#endif
                }
            } else if (fd_type == CLIENT_SOCKET) {
                errno = 0;
                {
                    received = recv(fd, (void *)buffer, sizeof(buffer), 0);
                }
                if (received == 0) {
#if defined(S2SD_DEBUG)
                    ESP_LOGI(TAG, "Closing socket fd slot %i errno: %i '%s'", n,
                             errno, strerror(errno));
#endif
                    _cleanup_fd(n);
                } else {
                    if (received < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                            continue;
                        }
#if defined(S2SD_DEBUG)
                        ESP_LOGI(TAG,
                                 "Closing socket errno: %i '%s'",
                                 errno, strerror(errno));
#endif
                        _cleanup_fd(n);
                    } else {
                        did_work = true;
                        // FIXME: Need to keep it clean and not call back into main()
#if defined(S2SD_DEBUG)
                        ESP_LOGI(TAG,"fd(%i) slot(%i) sending %i bytes to the AD2*", fd, n, received);
#endif
                        // FIXME: overide to send raw pointer and not buffer.
                        std::string tmp(buffer, received);
                        ad2_send(tmp);
                    }
                }
            }
        } /* end FD_ISSET() */
    }

    return did_work;
}

/*
  poll all write fd's return TRUE if we did do some work
 */
static bool _poll_write_fdset(fd_set *write_fdset)
{
    bool did_work = false;

    /* check every socket to find the one that needs write */
    for (int n = 0; n < MAXCONNECTIONS; n++) {
        bool close_client = false;
        int send_errno = 0;

        if (fds_mutex == NULL || xSemaphoreTake(fds_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (my_fds[n].inuse && my_fds[n].fd_type == CLIENT_SOCKET &&
                FD_ISSET(my_fds[n].fd, write_fdset)) {
            fifo_buffer *tempbuffer = (fifo_buffer *)_fifo_peek(
                                          &my_fds[n].send_buffer);
            if (tempbuffer) {
                size_t remaining = tempbuffer->size - tempbuffer->offset;
                ssize_t written = send(my_fds[n].fd,
                                       tempbuffer->buffer + tempbuffer->offset,
                                       remaining, 0);
                if (written > 0) {
                    tempbuffer->offset += (size_t)written;
                    did_work = true;
                    if (tempbuffer->offset == tempbuffer->size) {
                        free(_fifo_get(&my_fds[n].send_buffer));
                    }
                } else if (written == 0) {
                    close_client = true;
                    send_errno = ECONNRESET;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close_client = true;
                    send_errno = errno;
                }
            }
        }
        xSemaphoreGive(fds_mutex);

        if (close_client) {
            ESP_LOGW(TAG, "Closing socket fd slot %d after send error: %d '%s'",
                     n, send_errno, strerror(send_errno));
            _cleanup_fd(n);
        }
    }

    return did_work;
}

/**
 * @brief ser2sock server task
 *
 * @param [in]pvParameters currently not used NULL.
 */
void ser2sockd_server_task(void *pvParameters)
{
#if CONFIG_LWIP_IPV6
    int addr_family = AF_INET6;
    struct sockaddr_in6 dest_addr;
#else
    int addr_family = AF_INET;
    struct sockaddr_in dest_addr;
#endif
    int n;
    bool bOptionTrue = true;
    bool did_work = false;
    fd_set read_fdset, write_fdset, except_fdset;
    struct timeval wait;

#if defined(S2SD_DEBUG)
    ESP_LOGI(TAG, "%s waiting for network layer to start.", TAG);
#endif
    while (!ser2sockd_shutting_down) {
        if (!hal_get_netif_started()) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "Network layer is OK. %s client starting.", TAG);


#if defined(S2SD_DEBUG)
    ESP_LOGI(TAG, "%s waiting for network IP layer to start.", TAG);
#endif
    for (; !ser2sockd_shutting_down;) {
        if (hal_get_network_connected()) {
#if defined(S2SD_DEBUG)
            ESP_LOGI(TAG, "Network IP layer is OK. %s daemon service starting.", TAG);
#endif
#if CONFIG_LWIP_IPV6
            // IPv6 socket will listen on both IPv4 and IPv6 at the same time.
            bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
            dest_addr.sin6_family = AF_INET6;
            dest_addr.sin6_port = htons(PORT);
            listen_sock = socket(addr_family, SOCK_STREAM, IPPROTO_IPV6);
#else
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(PORT);
            listen_sock = socket(addr_family, SOCK_STREAM, IPPROTO_IP);
#endif
            if (listen_sock < 0) {
                ESP_LOGE(TAG, "ser2sock server unable to create socket: errno %d", errno);
                vTaskDelete(NULL);
                return;
            }

            setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &bOptionTrue,
                       sizeof(bOptionTrue));

            struct linger solinger;
            solinger.l_onoff = true;
            solinger.l_linger = 0;
            setsockopt(listen_sock, SOL_SOCKET, SO_LINGER, &solinger, sizeof(solinger));
#if defined(S2SD_DEBUG)
            ESP_LOGI(TAG, "ser2sock server socket created %i", listen_sock);
#endif
            int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            if (err != 0) {
                ESP_LOGE(TAG, "ser2sock server socket unable to bind: errno %d", errno);
                ESP_LOGE(TAG, "ser2sock server IPPROTO: %d", addr_family);
                goto CLEAN_UP;
            }
#if defined(S2SD_DEBUG)
            ESP_LOGI(TAG, "ser2sock server socket bound, port %d", PORT);
#endif
            err = listen(listen_sock, listen_backlog);
            if (err != 0) {
                ESP_LOGE(TAG, "ser2sock server error occurred during listen: errno %d", errno);
                goto CLEAN_UP;
            }

            if (_add_fd(listen_sock, LISTEN_SOCKET) < 0) {
                ESP_LOGE(TAG, "Unable to reserve a slot for the listening socket");
                goto CLEAN_UP;
            }

            while (!ser2sockd_shutting_down) {
                /* reset our loop state var(s) for this iteration */
                did_work = false;

                /* honor overflow/allocation failures requested by producers */
                _cleanup_requested_fds();

                /* build our fd sets */
                _build_fdsets(&read_fdset, &write_fdset, &except_fdset);

                /* lets not block our select and bail after 20us */
                wait.tv_sec = 0;
                wait.tv_usec = 20;

                /* see if any of the fd's need attention */
                n = select(FD_SETSIZE, &read_fdset, &write_fdset, &except_fdset, &wait);
                if (n == -1) {
                    ESP_LOGE(TAG, "An error occurred during select() errno: %i '%s'", errno, strerror(errno));
                    continue;
                }

                /* A producer may have requested cleanup while select ran. */
                _cleanup_requested_fds();
                /* poll our exception fdset */
                _poll_exception_fdset(&except_fdset);

                /* poll our read fdset */
                did_work |= _poll_read_fdset(&read_fdset);

                /* poll our write fdset */
                did_work |= _poll_write_fdset(&write_fdset);

                /* if we did not do anything then sleep a little predict
                next go round will be idle too
                */
                if (!did_work) {
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                }
                /* if network goes away then we are done */
                if (!hal_get_network_connected()) {
                    goto CLEAN_UP;
                }
#if defined(AD2_STACK_REPORT)
#define EXTRA_INFO_EVERY 1000
                static int extra_info = EXTRA_INFO_EVERY;
                if(!--extra_info) {
                    extra_info = EXTRA_INFO_EVERY;
                    ESP_LOGI(TAG, "ser2sockd stack free %d", uxTaskGetStackHighWaterMark(NULL));
                }
#endif
            }
CLEAN_UP:
            for (n = 0; n < MAXCONNECTIONS; n++) {
                _cleanup_fd(n);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

#endif /*  CONFIG_AD2IOT_SER2SOCKD */
