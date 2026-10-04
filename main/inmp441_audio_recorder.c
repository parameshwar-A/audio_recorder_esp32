/*
 * INMP441 Touch-Triggered 1-Second Audio WAV Recorder for ESP32 WROOM
 *
 * Hardware Features:
 * - INMP441 I2S Microphone (16 kHz, 16-bit signed PCM mono)
 * - Touch Trigger on GPIO 32 (Touch CH 9) & GPIO 4 (Touch CH 0)
 * - Button Trigger on GPIO 0 (on-board BOOT button) as zero-wire fallback
 * - Visual Indicator LED on GPIO 16 (Solid ON during 1.0s recording, 3 blinks when saved)
 * - Lossless binary packet transmission over USB Serial to companion Python script
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/touch_sens.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "AUDIO_RECORDER";

/*
 * ====================================================================
 * HARDWARE PIN CONFIGURATION (ESP32 WROOM)
 * ====================================================================
 * INMP441 SCK (Clock)        --> GPIO 26
 * INMP441 WS  (Word Select)  --> GPIO 25
 * INMP441 SD  (Serial Data)  --> GPIO 33
 * INMP441 L/R (Channel)      --> GND (Left Channel)
 * INMP441 VDD (Power)        --> 3V3 (Do NOT connect to 5V!)
 * INMP441 GND (Ground)       --> GND
 *
 * Primary Touch Sensor       --> GPIO 32 (Touch Channel 9)
 * Secondary Touch Sensor     --> GPIO 4  (Touch Channel 0)
 * On-Board BOOT Button       --> GPIO 0  (Active LOW fallback)
 * Visual Indicator LED       --> GPIO 16 (External LED - Solid ON while recording)
 * ====================================================================
 */
#define I2S_SCK_PIN             GPIO_NUM_26
#define I2S_WS_PIN              GPIO_NUM_25
#define I2S_SD_PIN              GPIO_NUM_33

#define PRIMARY_TOUCH_GPIO      GPIO_NUM_32    // Touch Channel 9 (GPIO 32)
#define SECONDARY_TOUCH_GPIO    GPIO_NUM_4     // Touch Channel 0 (GPIO 4)
#define BOOT_BUTTON_PIN         GPIO_NUM_0     // Built-in BOOT button (pull-up)
#define INDICATOR_LED_PIN       GPIO_NUM_16    // LED on GPIO 16 (clean, no touch noise)
#define BUILTIN_LED_PIN         GPIO_NUM_2     // Also mirrors to GPIO 2

/* Audio Parameters */
#define SAMPLE_RATE_HZ          16000
#define RECORD_SECONDS          1
#define TOTAL_SAMPLES           (SAMPLE_RATE_HZ * RECORD_SECONDS)  // Exactly 16,000 samples
#define DMA_CHUNK_SAMPLES       512

/* Binary Protocol Magic Markers */
#define PACKET_MAGIC_START      0x55AA55AA
#define PACKET_MAGIC_END        0xAA55AA55

/* Binary Packet Header (20 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic_start;   // PACKET_MAGIC_START (0x55AA55AA)
    uint32_t sample_rate;   // 16000
    uint32_t sample_count;  // 16000
    uint16_t channels;      // 1 (mono)
    uint16_t bits_per_sample;// 16
    uint32_t checksum;      // 32-bit sum of audio payload
} audio_packet_header_t;

static i2s_chan_handle_t rx_chan = NULL;
static touch_sensor_handle_t touch_sens = NULL;
static touch_channel_handle_t chan32_handle = NULL;
static touch_channel_handle_t chan4_handle = NULL;
static uint32_t baseline32 = 1000;
static uint32_t baseline4  = 1000;
static int touch_confirm_32 = 0;
static int touch_confirm_4  = 0;

/* LED Controls */
static void led_init(void)
{
    gpio_reset_pin(INDICATOR_LED_PIN);
    gpio_set_direction(INDICATOR_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(INDICATOR_LED_PIN, 0);

    gpio_reset_pin(BUILTIN_LED_PIN);
    gpio_set_direction(BUILTIN_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BUILTIN_LED_PIN, 0);
}

static void led_set(int level)
{
    gpio_set_level(INDICATOR_LED_PIN, level);
    gpio_set_level(BUILTIN_LED_PIN, level);
}

static void led_blink(int count, int delay_ms)
{
    for (int i = 0; i < count; i++) {
        led_set(1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        led_set(0);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* Button & Touch Configuration */
static void button_init(void)
{
    gpio_reset_pin(BOOT_BUTTON_PIN);
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en(BOOT_BUTTON_PIN);
}

static void touch_sensor_setup(void)
{
    ESP_LOGI(TAG, "Configuring touch sensors (GPIO 32 and GPIO 4)...");

    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7)
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    if (touch_sensor_new_controller(&sens_cfg, &touch_sens) != ESP_OK) {
        ESP_LOGW(TAG, "Touch controller init failed; BOOT button fallback active.");
        return;
    }

    touch_channel_config_t chan_cfg = {
        .abs_active_thresh = {500},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
        .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
    };

    // Channel 9 = GPIO 32
    touch_sensor_new_channel(touch_sens, 9, &chan_cfg, &chan32_handle);
    // Channel 0 = GPIO 4
    touch_sensor_new_channel(touch_sens, 0, &chan_cfg, &chan4_handle);

    // Software filter (mandatory on ESP32 Touch V1)
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    touch_sensor_config_filter(touch_sens, &filter_cfg);

    // Stabilization scan
    touch_sensor_enable(touch_sens);
    for (int i = 0; i < 5; i++) {
        touch_sensor_trigger_oneshot_scanning(touch_sens, 2000);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (chan32_handle) {
        touch_channel_read_data(chan32_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &baseline32);
        if (baseline32 == 0) baseline32 = 1000;
    }
    if (chan4_handle) {
        touch_channel_read_data(chan4_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &baseline4);
        if (baseline4 == 0) baseline4 = 1000;
    }

    touch_sensor_start_continuous_scanning(touch_sens);
    ESP_LOGI(TAG, "Touch active: GPIO 32 (base %"PRIu32"), GPIO 4 (base %"PRIu32")", baseline32, baseline4);
}

/* I2S Microphone Initialization */
static esp_err_t i2s_inmp441_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S for INMP441...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
    return ESP_OK;
}

/* Check if trigger was activated */
static bool is_triggered(void)
{
    // 1. Check capacitive touch on GPIO 32 (CH 9)
    if (chan32_handle) {
        uint32_t val32 = 0;
        if (touch_channel_read_data(chan32_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &val32) == ESP_OK) {
            uint32_t thresh32 = (uint32_t)(baseline32 * 0.60f);
            if (val32 > 0 && val32 < thresh32) {
                touch_confirm_32++;
                if (touch_confirm_32 >= 2) {
                    touch_confirm_32 = 0;
                    return true;
                }
            } else {
                touch_confirm_32 = 0;
                if (val32 > 0) baseline32 = (baseline32 * 31 + val32) / 32;
            }
        }
    }

    // 2. Check capacitive touch on GPIO 4 (CH 0)
    if (chan4_handle) {
        uint32_t val4 = 0;
        if (touch_channel_read_data(chan4_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &val4) == ESP_OK) {
            uint32_t thresh4 = (uint32_t)(baseline4 * 0.60f);
            if (val4 > 0 && val4 < thresh4) {
                touch_confirm_4++;
                if (touch_confirm_4 >= 2) {
                    touch_confirm_4 = 0;
                    return true;
                }
            } else {
                touch_confirm_4 = 0;
                if (val4 > 0) baseline4 = (baseline4 * 31 + val4) / 32;
            }
        }
    }

    // 3. Check BOOT button (active LOW)
    if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20)); // Debounce
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            while (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return true;
        }
    }

    return false;
}

/* Main Recording Task */
static void recorder_task(void *pvParameters)
{
    int32_t *dma_buffer = (int32_t *)malloc(DMA_CHUNK_SAMPLES * sizeof(int32_t));
    int16_t *audio_buffer = (int16_t *)malloc(TOTAL_SAMPLES * sizeof(int16_t));

    if (dma_buffer == NULL || audio_buffer == NULL) {
        ESP_LOGE(TAG, "Fatal: failed to allocate audio buffer!");
        vTaskDelete(NULL);
        return;
    }

    fprintf(stderr, "\n============================================================\n");
    fprintf(stderr, " ESP32 INMP441 1-SECOND AUDIO RECORDER READY\n");
    fprintf(stderr, " -> Touch GPIO 32 or GPIO 4 OR Press BOOT button to record\n");
    fprintf(stderr, " -> LED (GPIO 16) turns SOLID ON during 1.0s recording\n");
    fprintf(stderr, "============================================================\n\n");

    // Double blink to indicate readiness
    led_blink(2, 100);

    while (1) {
        // Wait for trigger
        if (!is_triggered()) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        fprintf(stderr, "[START] Recording 1 second of audio... Speak now!\n");

        // 1. Drain any stale DMA audio buffers accumulated while idle
        size_t bytes_read = 0;
        for (int flush_i = 0; flush_i < 6; flush_i++) {
            if (i2s_channel_read(rx_chan, dma_buffer, DMA_CHUNK_SAMPLES * sizeof(int32_t), &bytes_read, 0) != ESP_OK || bytes_read == 0) {
                break;
            }
        }

        // 2. Turn ON LED solidly to signal recording
        led_set(1);

        // 3. Record exactly TOTAL_SAMPLES (16,000 samples = 1.000s)
        size_t collected = 0;
        int64_t dc_sum = 0;

        while (collected < TOTAL_SAMPLES) {
            size_t to_read = TOTAL_SAMPLES - collected;
            if (to_read > DMA_CHUNK_SAMPLES) {
                to_read = DMA_CHUNK_SAMPLES;
            }

            esp_err_t ret = i2s_channel_read(rx_chan, dma_buffer, to_read * sizeof(int32_t), &bytes_read, portMAX_DELAY);
            if (ret != ESP_OK || bytes_read == 0) {
                continue;
            }

            size_t count = bytes_read / sizeof(int32_t);
            for (size_t i = 0; i < count && collected < TOTAL_SAMPLES; i++) {
                // Shift 24-bit to 16-bit range with strict clamping to prevent overflow wrap
                int32_t sample_32 = dma_buffer[i] >> 14;
                if (sample_32 > 32767) sample_32 = 32767;
                if (sample_32 < -32768) sample_32 = -32768;
                int16_t sample_16 = (int16_t)sample_32;

                audio_buffer[collected++] = sample_16;
                dc_sum += sample_16;
            }
        }

        // 4. Recording done -> Turn OFF LED
        led_set(0);
        fprintf(stderr, "[DONE] 1 second recorded (%d samples). Processing & transmitting...\n", TOTAL_SAMPLES);

        // 5. Remove DC offset with clamping
        int32_t dc_offset = (int32_t)(dc_sum / (int64_t)TOTAL_SAMPLES);
        uint32_t checksum = 0;
        uint8_t *byte_ptr = (uint8_t *)audio_buffer;
        size_t total_bytes = TOTAL_SAMPLES * sizeof(int16_t);

        for (size_t i = 0; i < TOTAL_SAMPLES; i++) {
            int32_t corrected = (int32_t)audio_buffer[i] - dc_offset;
            if (corrected > 32767) corrected = 32767;
            if (corrected < -32768) corrected = -32768;
            audio_buffer[i] = (int16_t)corrected;
        }

        for (size_t i = 0; i < total_bytes; i++) {
            checksum += byte_ptr[i];
        }

        // 6. Send Binary Packet Header
        audio_packet_header_t header = {
            .magic_start = PACKET_MAGIC_START,
            .sample_rate = SAMPLE_RATE_HZ,
            .sample_count = TOTAL_SAMPLES,
            .channels = 1,
            .bits_per_sample = 16,
            .checksum = checksum,
        };

        fwrite(&header, 1, sizeof(header), stdout);
        fwrite(audio_buffer, 1, total_bytes, stdout);
        uint32_t end_magic = PACKET_MAGIC_END;
        fwrite(&end_magic, 1, sizeof(end_magic), stdout);
        fflush(stdout);

        // 7. Success confirmation blink (3 quick blinks)
        led_blink(3, 50);

        fprintf(stderr, "[SUCCESS] Transmitted %zu audio bytes. Ready for next touch!\n\n", total_bytes);
        vTaskDelay(pdMS_TO_TICKS(300)); // Debounce
    }

    free(dma_buffer);
    free(audio_buffer);
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Configure UART stdout for raw binary mode (disable CRLF 0x0A->0x0D0x0A conversion)
    uart_vfs_dev_port_set_tx_line_endings(0, ESP_LINE_ENDINGS_LF);

    ESP_LOGI(TAG, "Starting ESP32 INMP441 Touch Audio Recorder...");

    led_init();
    button_init();
    touch_sensor_setup();
    ESP_ERROR_CHECK(i2s_inmp441_init());

    // Create recording task on Core 1 with 4096 words stack
    xTaskCreatePinnedToCore(recorder_task, "recorder_task", 4096, NULL, 5, NULL, 1);
}
