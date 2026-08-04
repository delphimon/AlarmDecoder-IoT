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
#include "usdupdate.h"

 // Disable via config
#if CONFIG_AD2IOT_USDUPDATE

// esp component includes
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <mbedtls/sha256.h>
#include <sys/stat.h>
#include <algorithm>

//#define DEBUG_LUPDATE

#define CONFIG_FIRMWARE_PATH "/sdcard/firmware.bin"

#define USDUPDATE_UPGRADE_CMD   "upgradeusd"
#define USDUPDATE_VERSION_CMD   "versionusd"


// forward decl
void usd_do_version(const char *arg);
void usd_do_update(const char *command);

// OTA Update task
TaskHandle_t usdupdate_task_handle = NULL;

static void usd_status_error(usd_firmware_status *status, const char *error)
{
    strlcpy(status->error, error, sizeof(status->error));
}

static bool usd_read_exact(FILE *file, void *buffer, size_t length)
{
    return length == 0 || fread(buffer, 1, length, file) == length;
}

/**
 * Inspect and integrity-check the SD card image without modifying flash.
 * ESP-IDF performs its full bootloader-compatible validation again in
 * esp_ota_end() before the image can be selected for boot.
 */
bool usd_get_firmware_status(usd_firmware_status *status)
{
    if (!status) {
        return false;
    }
    memset(status, 0, sizeof(*status));
    status->sd_mounted = g_uSD_mounted;
    status->update_in_progress = usdupdate_task_handle != NULL;
    if (!status->sd_mounted) {
        usd_status_error(status, "SD card is not mounted");
        return false;
    }

    struct stat file_info;
    if (stat(CONFIG_FIRMWARE_PATH, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
        usd_status_error(status, "firmware.bin was not found on the SD card");
        return false;
    }
    status->present = true;
    status->size_bytes = (size_t)file_info.st_size;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        usd_status_error(status, "No OTA update partition is available");
        return false;
    }
    if (file_info.st_size <= 0 || (size_t)file_info.st_size > update_partition->size) {
        usd_status_error(status, "Firmware size is invalid for the OTA partition");
        return false;
    }

    FILE *file = fopen(CONFIG_FIRMWARE_PATH, "rb");
    if (!file) {
        usd_status_error(status, "Unable to open firmware.bin");
        return false;
    }

    bool ok = false;
    uint8_t *buffer = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    do {
        esp_image_header_t image_header;
        if (!usd_read_exact(file, &image_header, sizeof(image_header))) {
            usd_status_error(status, "Firmware image header is truncated");
            break;
        }
        if (image_header.magic != ESP_IMAGE_HEADER_MAGIC ||
                image_header.segment_count == 0 ||
                image_header.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
            usd_status_error(status, "Firmware image header is invalid");
            break;
        }
        if (image_header.chip_id != ESP_CHIP_ID_ESP32) {
            usd_status_error(status, "Firmware image is not for an ESP32");
            break;
        }
        mbedtls_sha256_starts(&sha, 0);
        mbedtls_sha256_update(&sha, (const unsigned char *)&image_header,
                              sizeof(image_header));

        buffer = (uint8_t *)malloc(4096);
        if (!buffer) {
            usd_status_error(status, "Not enough memory to inspect firmware.bin");
            break;
        }

        size_t offset = sizeof(image_header);
        uint8_t checksum = 0xEF;
        esp_app_desc_t app_desc;
        memset(&app_desc, 0, sizeof(app_desc));
        bool have_app_desc = false;
        const esp_app_desc_t *installed = NULL;
        size_t checksum_block_length = 0;
        uint8_t checksum_block[16] = {0};
        unsigned char calculated_hash[32] = {0};
        unsigned char expected_hash[32] = {0};

        for (uint8_t segment_index = 0; segment_index < image_header.segment_count; segment_index++) {
            esp_image_segment_header_t segment_header;
            if (!usd_read_exact(file, &segment_header, sizeof(segment_header))) {
                usd_status_error(status, "Firmware segment header is truncated");
                goto validation_done;
            }
            offset += sizeof(segment_header);
            if ((segment_header.data_len & 3) != 0 ||
                    segment_header.data_len > update_partition->size ||
                    offset + segment_header.data_len > status->size_bytes) {
                usd_status_error(status, "Firmware segment length is invalid");
                goto validation_done;
            }
            mbedtls_sha256_update(&sha, (const unsigned char *)&segment_header,
                                  sizeof(segment_header));

            size_t segment_read = 0;
            while (segment_read < segment_header.data_len) {
                const size_t chunk = std::min((size_t)4096,
                                              (size_t)segment_header.data_len - segment_read);
                if (!usd_read_exact(file, buffer, chunk)) {
                    usd_status_error(status, "Firmware segment data is truncated");
                    goto validation_done;
                }
                if (segment_index == 0 && segment_read < sizeof(app_desc)) {
                    const size_t copy = std::min(chunk, sizeof(app_desc) - segment_read);
                    memcpy((uint8_t *)&app_desc + segment_read, buffer, copy);
                    if (segment_read + copy == sizeof(app_desc)) {
                        have_app_desc = true;
                    }
                }
                for (size_t i = 0; i < chunk; i++) {
                    checksum ^= buffer[i];
                }
                mbedtls_sha256_update(&sha, buffer, chunk);
                segment_read += chunk;
                offset += chunk;
            }
        }

        if (!have_app_desc || app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
            usd_status_error(status, "Firmware application metadata is invalid");
            break;
        }
        snprintf(status->version, sizeof(status->version), "%.31s", app_desc.version);
        snprintf(status->project_name, sizeof(status->project_name), "%.31s", app_desc.project_name);
        snprintf(status->build_date, sizeof(status->build_date), "%.15s", app_desc.date);
        snprintf(status->build_time, sizeof(status->build_time), "%.15s", app_desc.time);

        installed = esp_app_get_description();
        if (!installed || strncmp(app_desc.project_name, installed->project_name,
                                  sizeof(app_desc.project_name)) != 0) {
            usd_status_error(status, "Firmware image is for a different application");
            break;
        }

        checksum_block_length = ((offset + 1 + 15) & ~((size_t)15)) - offset;
        if (checksum_block_length == 0 || checksum_block_length > sizeof(checksum_block) ||
                offset + checksum_block_length > status->size_bytes ||
                !usd_read_exact(file, checksum_block, checksum_block_length)) {
            usd_status_error(status, "Firmware checksum block is invalid");
            break;
        }
        offset += checksum_block_length;
        if (checksum_block[checksum_block_length - 1] != checksum) {
            usd_status_error(status, "Firmware checksum does not match");
            break;
        }
        mbedtls_sha256_update(&sha, checksum_block, checksum_block_length);

        mbedtls_sha256_finish(&sha, calculated_hash);
        if (image_header.hash_appended) {
            if (offset + sizeof(expected_hash) != status->size_bytes ||
                    !usd_read_exact(file, expected_hash, sizeof(expected_hash))) {
                usd_status_error(status, "Firmware SHA-256 hash is missing or image has trailing data");
                break;
            }
            if (memcmp(calculated_hash, expected_hash, sizeof(expected_hash)) != 0) {
                usd_status_error(status, "Firmware SHA-256 hash does not match");
                break;
            }
        } else if (offset != status->size_bytes) {
            usd_status_error(status, "Firmware image has unexpected trailing data");
            break;
        }

        status->valid = true;
        status->error[0] = '\0';
        ok = true;
validation_done:
        ;
    } while (false);

    free(buffer);
    mbedtls_sha256_free(&sha);
    fclose(file);
    return ok;
}

/**
 * @brief Firmware update task that preforms the update to the flash
 * from the uSD disk.
 */
static void usd_task_func(void * command)
{
    free(command);
    ad2_printf_host(false, "Starting uSD update from '" CONFIG_FIRMWARE_PATH "'.\r\n");

    usd_firmware_status firmware;
    if (!usd_get_firmware_status(&firmware)) {
        ad2_printf_host(false, "uSD update image is invalid: %s. Update aborted.\r\n",
                        firmware.error[0] ? firmware.error : "validation failed");
        usdupdate_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ad2_printf_host(false, "Validated uSD firmware %s (%u bytes), built %s %s.\r\n",
                    firmware.version, (unsigned)firmware.size_bytes,
                    firmware.build_date, firmware.build_time);

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
    (void)command;
    if (!usd_start_update()) {
        ESP_LOGW(TAG, "Unable to start uSD update (already running or task allocation failed).");
    }
}

bool usd_update_in_progress()
{
    return usdupdate_task_handle != NULL;
}

bool usd_start_update()
{
    if (usdupdate_task_handle != NULL) {
        ESP_LOGW(TAG, "Device is currently updating.");
        return false;
    }
    char *task_command = strdup(USDUPDATE_UPGRADE_CMD);
    if (task_command == NULL ||
            xTaskCreate(&usd_task_func, "AD2 uSD Update", 1024*8, task_command,
                        tskIDLE_PRIORITY+2, &usdupdate_task_handle) != pdPASS) {
        free(task_command);
        usdupdate_task_handle = NULL;
        ESP_LOGE(TAG, "Unable to start uSD update task.");
        return false;
    }
    return true;
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
        "    Report installed firmware and validate /sdcard/firmware.bin\r\n"
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
    usd_firmware_status firmware;
    if (usd_get_firmware_status(&firmware)) {
        ad2_printf_host(false,
                        "SD firmware is valid: version(%s), project(%s), built(%s %s), size(%u bytes)%s.\r\n",
                        firmware.version, firmware.project_name, firmware.build_date,
                        firmware.build_time, (unsigned)firmware.size_bytes,
                        strcmp(firmware.version, ad2_firmware_version()) == 0 ?
                        " [same as installed]" : " [available for upgrade]");
    } else {
        ad2_printf_host(false, "SD firmware is not available for upgrade: %s.\r\n",
                        firmware.error[0] ? firmware.error : "validation failed");
    }
}

#endif /* CONFIG_AD2IOT_USDUPDATE */
