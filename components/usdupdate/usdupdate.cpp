/**
 *  @file    usdupdate_util.c
 *  @author  Sean Mathews <coder@f34r.com>
 *  @date    09/18/2020
 *  @version 1.0.3
 *
 *  @brief uSD firmware update support. Flash firmware from file on uSD disk.
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

static const char *TAG = "AD2LUPDATE";

 // AlarmDecoder std includes
#include "alarmdecoder_main.h"

 // Disable via config
#if CONFIG_AD2IOT_USDUPDATE

// esp component includes
#include <esp_ota_ops.h>
#include <esp_partition.h>

//#define DEBUG_LUPDATE

#define CONFIG_FIRMWARE_PATH "/sdcard/firmware.bin"

#define USDUPDATE_UPGRADE_CMD   "upgradeusd"
#define USDUPDATE_VERSION_CMD   "versionusd"


// forward decl
void usd_do_version(const char *arg);
void usd_do_update(const char *command);

// OTA Update task
TaskHandle_t usdupdate_task_handle = NULL;

/**
 * @brief Firmware update task that preforms the update to the flash
 * from the uSD disk.
 */
static void usd_task_func(void * command)
{
    free(command);
    ad2_printf_host(false, "Starting uSD update from '" CONFIG_FIRMWARE_PATH "'.\r\n");

    FILE *f = fopen(CONFIG_FIRMWARE_PATH, "rb");
    if (f == NULL) {
        ad2_printf_host(false, "uSD update image not found; update aborted.\r\n");
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ad2_printf_host(false, "No OTA update partition is available; update aborted.\r\n");
        fclose(f);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        ad2_printf_host(false, "Unable to read update image size; update aborted.\r\n");
        fclose(f);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    long image_size = ftell(f);
    rewind(f);
    if (image_size <= 0 || (size_t)image_size > update_partition->size) {
        ad2_printf_host(false,
                        "Invalid update image size (%ld bytes, slot capacity %u); update aborted.\r\n",
                        image_size, (unsigned)update_partition->size);
        fclose(f);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Start OTA
    esp_err_t result = esp_ota_begin(update_partition, (size_t)image_size, &update_handle);
    if (result != ESP_OK) {
        ad2_printf_host(false, "Unable to start OTA update: %s. Image retained.\r\n", esp_err_to_name(result));
        fclose(f);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    char *buffer = (char *)malloc(4096);
    if (buffer == NULL) {
        ad2_printf_host(false, "Unable to allocate update buffer; image retained.\r\n");
        esp_ota_abort(update_handle);
        fclose(f);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t total_written = 0;
    bool write_ok = true;
    while (true) {
        size_t read = fread(buffer, 1, 4096, f);
        if (read == 0) {
            if (ferror(f)) {
                ad2_printf_host(false, "Error reading update image; image retained.\r\n");
                write_ok = false;
            }
            break;
        }
        result = esp_ota_write(update_handle, buffer, read);
        if (result != ESP_OK) {
            ad2_printf_host(false, "Error writing OTA partition: %s. Image retained.\r\n", esp_err_to_name(result));
            write_ok = false;
            break;
        }
        total_written += read;
    }

    free(buffer);
    fclose(f);

    if (!write_ok || total_written != (size_t)image_size) {
        esp_ota_abort(update_handle);
        ad2_printf_host(false, "Incomplete OTA write (%u of %ld bytes); update aborted.\r\n",
                        (unsigned)total_written, image_size);
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // esp_ota_end validates the complete ESP application image before it can
    // be selected as the next boot partition.
    result = esp_ota_end(update_handle);
    if (result != ESP_OK) {
        ad2_printf_host(false, "Update image validation failed: %s. Image retained.\r\n", esp_err_to_name(result));
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t app_desc;
    result = esp_ota_get_partition_description(update_partition, &app_desc);
    if (result != ESP_OK) {
        ad2_printf_host(false, "Unable to read installed image metadata: %s. Image retained.\r\n", esp_err_to_name(result));
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        ad2_printf_host(false, "Unable to select the updated boot partition: %s. Image retained.\r\n", esp_err_to_name(result));
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (remove(CONFIG_FIRMWARE_PATH) != 0) {
        ESP_LOGW(TAG, "Update installed but unable to remove '%s'", CONFIG_FIRMWARE_PATH);
    }
    ad2_printf_host(false, "Installed %s (%ld bytes) in OTA slot '%s'.\r\n",
                    app_desc.version, image_size, update_partition->label);
    ad2_printf_host(true, "%s Prepare to restart system!", TAG);
    usdupdate_task_handle = NULL;
    hal_restart();
}

/**
 * @brief Initiate and uSD update process
 */
void usd_do_update(const char *command)
{
    if (usdupdate_task_handle != NULL) {
        ESP_LOGW(TAG, "Device is currently updating.");
        return;
    }
    char *task_command = strdup(command ? command : "");
    if (task_command == NULL ||
            xTaskCreate(&usd_task_func, "AD2 uSD Update", 1024*8, task_command,
                        tskIDLE_PRIORITY+2, &usdupdate_task_handle) != pdPASS) {
        free(task_command);
        usdupdate_task_handle = NULL;
        ESP_LOGE(TAG, "Unable to start uSD update task.");
    }
}

/**
 * @brief command list for module
 */
static struct cli_command uSDupdate_cmd_list[] = {
    {
        (char*)USDUPDATE_UPGRADE_CMD,(char*)
        "Usage: upgradeusd\r\n"
        "\r\n"
        "    Preform upgrade from connected uSD flash drive.\r\n"
        , usd_do_update
    },
    {
        (char*)USDUPDATE_VERSION_CMD,(char*)
        "Usage: versionusd\r\n"
        "\r\n"
        "    Report the current version\r\n"
        , usd_do_version
    }
};


/**
 * @brief Register component cli commands.
 */
void usdupdate_register_cmds()
{
    // Register CLI commands
    for (int i = 0; i < ARRAY_SIZE(uSDupdate_cmd_list); i++) {
        cli_register_command(&uSDupdate_cmd_list[i]);
    }
}

/**
 * @brief Init.
 */
void usdupdate_init()
{
     ad2_printf_host(true, "%s: Init done", TAG);
}

/**
 * @brief Show installed and available version
 */
void usd_do_version(const char *arg)
{
    ad2_printf_host(false, "Installed version(%s) build flag (%s).\r\n",
                    ad2_firmware_version(), FIRMWARE_BUILDFLAGS);
}

#endif /* CONFIG_AD2IOT_USDUPDATE */
