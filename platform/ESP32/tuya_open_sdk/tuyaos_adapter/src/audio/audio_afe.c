/**
 * @file audio_afe.c
 * @brief Audio Front-End (AFE) processor for ESP32 platforms.
 *
 * This module wraps the ESP-SR AFE engine. On PSRAM-equipped boards the full
 * pipeline (AEC, NS, wakeword) is created; on boards without PSRAM (such as
 * ESP32-C3 and ESP32-C6) the initialization is skipped so that the device
 * can operate in push-to-talk / manual-VAD mode without crashing.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "sdkconfig.h"
#include "audio_afe.h"
#include "esp_log.h"

static const char *TAG = "audio_afe";

/* ------------------------------------------------------------------ */
/*  Platform gate: PSRAM is mandatory for ESP-SR AFE                  */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_SPIRAM

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

typedef struct {
    bool                      is_init;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t        *afe_data;
} AUDIO_AFE_PROCESSOR_T;

static AUDIO_AFE_PROCESSOR_T sg_afe_proce = {0};

static OPERATE_RET __esp_afe_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGW(TAG, "Failed to load SR models - AFE disabled");
        return OPRT_COM_ERROR;
    }

    sg_afe_proce.afe_iface = &ESP_AFE_SR_HANDLE;

    afe_config_t *afe_config =
        (afe_config_t *)sg_afe_proce.afe_iface->create_config("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return OPRT_COM_ERROR;
    }

    afe_config->memory_alloc_mode  = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->afe_perferred_core = 0;

    sg_afe_proce.afe_data = sg_afe_proce.afe_iface->create_from_config(afe_config);
    if (sg_afe_proce.afe_data == NULL) {
        ESP_LOGE(TAG, "afe create_from_config failed (OOM?)");
        return OPRT_COM_ERROR;
    }

    sg_afe_proce.is_init = true;
    ESP_LOGI(TAG, "AFE processor initialized (PSRAM mode)");
    return OPRT_OK;
}

OPERATE_RET audio_afe_processor_init(void)
{
    return __esp_afe_init();
}

void auio_afe_processor_feed(uint8_t *data, int len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (!sg_afe_proce.is_init) {
        return;
    }
    sg_afe_proce.afe_iface->feed(sg_afe_proce.afe_data, (int16_t *)data);
}

#else /* !CONFIG_SPIRAM -- no PSRAM platforms (ESP32-C3, C6, etc.) */

OPERATE_RET audio_afe_processor_init(void)
{
    ESP_LOGW(TAG, "No PSRAM - AFE processor skipped (use manual VAD / push-to-talk mode)");
    return OPRT_OK;
}

void auio_afe_processor_feed(uint8_t *data, int len)
{
    (void)data;
    (void)len;
}

#endif /* CONFIG_SPIRAM */
