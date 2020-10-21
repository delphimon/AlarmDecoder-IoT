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

#include "st_dev.h"
#include "caps_alarm.h"

static const char *TAG = "CAPS_ALRM";

static int caps_alarm_attr_alarm_str2idx(const char *value)
{
    int index;

    for (index = 0; index < CAP_ENUM_ALARM_ALARM_VALUE_MAX; index++) {
        if (!strcmp(value, caps_helper_alarm.attr_alarm.values[index])) {
            return index;
        }
    }
    return -1;
}

static const char *caps_alarm_get_alarm_value(caps_alarm_data_t *caps_data)
{
    if (!caps_data) {
        ESP_LOGE(TAG, "caps_data is NULL");
        return NULL;
    }
    return caps_data->alarm_value;
}

static void caps_alarm_set_alarm_value(caps_alarm_data_t *caps_data, const char *value)
{
    if (!caps_data) {
        ESP_LOGE(TAG, "caps_data is NULL");
        return;
    }
    if (caps_data->alarm_value) {
        free(caps_data->alarm_value);
    }
    caps_data->alarm_value = strdup(value);
}

static void caps_alarm_attr_alarm_send(caps_alarm_data_t *caps_data)
{
    int sequence_no = -1;

    if (!caps_data || !caps_data->handle) {
        ESP_LOGE(TAG, "fail to get handle");
        return;
    }
    if (!caps_data->alarm_value) {
        ESP_LOGE(TAG, "value is NULL");
        return;
    }

    ST_CAP_SEND_ATTR_STRING(caps_data->handle,
            (char *)caps_helper_alarm.attr_alarm.name,
            caps_data->alarm_value,
            NULL,
            NULL,
            sequence_no);

    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send alarm value");
    else
        ESP_LOGI(TAG, "Sequence number return : %d", sequence_no);

}


static void caps_alarm_cmd_both_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_alarm_data_t *caps_data = (caps_alarm_data_t *)usr_data;
    const char* value = caps_helper_alarm.attr_alarm.values[CAP_ENUM_ALARM_ALARM_VALUE_BOTH];

    ESP_LOGI(TAG, "called [%s] func with num_args:%u", __func__, cmd_data->num_args);

    caps_alarm_set_alarm_value(caps_data, value);
    if (caps_data && caps_data->cmd_both_usr_cb)
        caps_data->cmd_both_usr_cb(caps_data);
    caps_alarm_attr_alarm_send(caps_data);
}

static void caps_alarm_cmd_siren_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_alarm_data_t *caps_data = (caps_alarm_data_t *)usr_data;
    const char* value = caps_helper_alarm.attr_alarm.values[CAP_ENUM_ALARM_ALARM_VALUE_SIREN];

    ESP_LOGI(TAG, "called [%s] func with num_args:%u", __func__, cmd_data->num_args);

    caps_alarm_set_alarm_value(caps_data, value);
    if (caps_data && caps_data->cmd_siren_usr_cb)
        caps_data->cmd_siren_usr_cb(caps_data);
    caps_alarm_attr_alarm_send(caps_data);
}

static void caps_alarm_cmd_off_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_alarm_data_t *caps_data = (caps_alarm_data_t *)usr_data;
    const char* value = caps_helper_alarm.attr_alarm.values[CAP_ENUM_ALARM_ALARM_VALUE_OFF];

    ESP_LOGI(TAG, "called [%s] func with num_args:%u", __func__, cmd_data->num_args);

    caps_alarm_set_alarm_value(caps_data, value);
    if (caps_data && caps_data->cmd_off_usr_cb)
        caps_data->cmd_off_usr_cb(caps_data);
    caps_alarm_attr_alarm_send(caps_data);
}

static void caps_alarm_cmd_strobe_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_alarm_data_t *caps_data = (caps_alarm_data_t *)usr_data;
    const char* value = caps_helper_alarm.attr_alarm.values[CAP_ENUM_ALARM_ALARM_VALUE_STROBE];

    ESP_LOGI(TAG, "called [%s] func with num_args:%u", __func__, cmd_data->num_args);

    caps_alarm_set_alarm_value(caps_data, value);
    if (caps_data && caps_data->cmd_strobe_usr_cb)
        caps_data->cmd_strobe_usr_cb(caps_data);
    caps_alarm_attr_alarm_send(caps_data);
}

static void caps_alarm_init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_alarm_data_t *caps_data = usr_data;
    if (caps_data && caps_data->init_usr_cb)
        caps_data->init_usr_cb(caps_data);
    caps_alarm_attr_alarm_send(caps_data);
}

caps_alarm_data_t *caps_alarm_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_alarm_data_t *caps_data = NULL;
    int err;

    caps_data = malloc(sizeof(caps_alarm_data_t));
    if (!caps_data) {
        ESP_LOGE(TAG, "fail to malloc for caps_alarm_data");
        return NULL;
    }

    memset(caps_data, 0, sizeof(caps_alarm_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;

    caps_data->get_alarm_value = caps_alarm_get_alarm_value;
    caps_data->set_alarm_value = caps_alarm_set_alarm_value;
    caps_data->attr_alarm_str2idx = caps_alarm_attr_alarm_str2idx;
    caps_data->attr_alarm_send = caps_alarm_attr_alarm_send;
    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, caps_helper_alarm.id, caps_alarm_init_cb, caps_data);
    }
    if (caps_data->handle) {
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_alarm.cmd_both.name, caps_alarm_cmd_both_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for both of alarm");
    }
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_alarm.cmd_siren.name, caps_alarm_cmd_siren_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for siren of alarm");
    }
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_alarm.cmd_off.name, caps_alarm_cmd_off_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for off of alarm");
    }
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_alarm.cmd_strobe.name, caps_alarm_cmd_strobe_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for strobe of alarm");
    }
    } else {
        ESP_LOGE(TAG, "fail to init alarm handle\n");
    }

    return caps_data;
}

#endif /* CONFIG_STDK_IOT_CORE */
