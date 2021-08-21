/* ***************************************************************************
 *
 * Copyright 2019-2020 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

// Disable componet via sdkconfig
#if CONFIG_STDK_IOT_CORE
static const char *TAG = "CAPS_PSRC";

#include "st_dev.h"
#include "caps_powerSource.h"

static int caps_powerSource_attr_powerSource_str2idx(const char *value)
{
    int index;

    for (index = 0; index < CAP_ENUM_POWERSOURCE_POWERSOURCE_VALUE_MAX; index++) {
        if (!strcmp(value, caps_helper_powerSource.attr_powerSource.values[index])) {
            return index;
        }
    }
    return -1;
}

static const char *caps_powerSource_get_powerSource_value(caps_powerSource_data_t *caps_data)
{
    if (!caps_data) {
        ESP_LOGE(TAG, "caps_data is NULL");
        return NULL;
    }
    return caps_data->powerSource_value;
}

static void caps_powerSource_set_powerSource_value(caps_powerSource_data_t *caps_data, const char *value)
{
    if (!caps_data) {
        ESP_LOGE( TAG, "caps_data is NULL");
        return;
    }
    if (caps_data->powerSource_value) {
        free(caps_data->powerSource_value);
    }
    caps_data->powerSource_value = strdup(value);
}

static void caps_powerSource_attr_powerSource_send(caps_powerSource_data_t *caps_data)
{
    int sequence_no = -1;

    if (!caps_data || !caps_data->handle) {
        ESP_LOGE(TAG, "fail to get handle");
        return;
    }
    if (!caps_data->powerSource_value) {
        ESP_LOGE(TAG, "value is NULL");
        return;
    }

    ST_CAP_SEND_ATTR_STRING(caps_data->handle,
            (char *)caps_helper_powerSource.attr_powerSource.name,
            caps_data->powerSource_value,
            NULL,
            NULL,
            sequence_no);

    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send powerSource value");
    else
        ESP_LOGI(TAG, "Sequence number return : %d", sequence_no);

}


static void caps_powerSource_init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_powerSource_data_t *caps_data = usr_data;
    if (caps_data && caps_data->init_usr_cb)
        caps_data->init_usr_cb(caps_data);
    caps_powerSource_attr_powerSource_send(caps_data);
}

caps_powerSource_data_t *caps_powerSource_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_powerSource_data_t *caps_data = NULL;

    caps_data = malloc(sizeof(caps_powerSource_data_t));
    if (!caps_data) {
        ESP_LOGE(TAG, "fail to malloc for caps_powerSource_data");
        return NULL;
    }

    memset(caps_data, 0, sizeof(caps_powerSource_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;

    caps_data->get_powerSource_value = caps_powerSource_get_powerSource_value;
    caps_data->set_powerSource_value = caps_powerSource_set_powerSource_value;
    caps_data->attr_powerSource_str2idx = caps_powerSource_attr_powerSource_str2idx;
    caps_data->attr_powerSource_send = caps_powerSource_attr_powerSource_send;
    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, caps_helper_powerSource.id, caps_powerSource_init_cb, caps_data);
    }
    if (!caps_data->handle) {
        ESP_LOGE(TAG, "fail to init powerSource handle");
    }

    return caps_data;
}

#endif /* CONFIG_STDK_IOT_CORE */
