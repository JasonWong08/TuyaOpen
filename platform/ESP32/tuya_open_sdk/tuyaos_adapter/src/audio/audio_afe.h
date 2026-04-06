/**
 * @file audio_afe.h
 * @brief Audio Front-End (AFE) processor interface for ESP32 platforms.
 *
 * Provides wakeword detection, noise suppression, and AEC via ESP-SR.
 * On platforms without PSRAM (ESP32-C3/C6), AFE is disabled and the
 * device operates in push-to-talk (manual VAD) mode.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __AUDIO_AFE_H__
#define __AUDIO_AFE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the AFE processor.
 *
 * On PSRAM-equipped platforms, creates the ESP-SR AFE pipeline (AEC, NS,
 * wakeword). On no-PSRAM platforms (CONFIG_SPIRAM not set), returns OPRT_OK
 * immediately -- the device should use manual VAD / push-to-talk instead.
 *
 * @return OPRT_OK on success or when skipped, error code otherwise.
 */
OPERATE_RET audio_afe_processor_init(void);

/**
 * @brief Feed a PCM audio frame to the AFE processor.
 *
 * Called from the I2S read task for every captured frame (~10 ms).
 * When AFE is not initialized (e.g. no-PSRAM platforms), the frame is
 * silently discarded.
 *
 * @param[in] data  Pointer to PCM audio data.
 * @param[in] len   Length of audio data in bytes.
 */
void auio_afe_processor_feed(uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_AFE_H__ */
