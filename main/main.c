#include <stdio.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <errno.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_camera.h"
#include "utils/nvs_storage.h"
#include "driver/gpio.h"

// support IDF 5.x
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

// AI-Thinker PIN Map
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1 //software reset will be performed
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27

#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22
#define CAM_PIN_DATA1 4 //闪光灯和DATA1共用
#define IO0 11 //IO0

//NAMESPACE
#define NVS_NAMESPACE "Picture_Count"

//KEY_DATA
#define NVS_PICTURE_COUNT_KEY "picture_count"

static const char *TAG = "Camera:";
static sdmmc_card_t *card;

static camera_config_t camera_config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
        .frame_size = FRAMESIZE_SVGA,    //QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

        .jpeg_quality = 5, //0-63, for OV series camera sensors, lower number means higher quality
        .fb_location = CAMERA_FB_IN_DRAM,
        .fb_count = 1,       //When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
        .grab_mode = CAMERA_GRAB_LATEST
};

//初始化相机
static esp_err_t init_camera(void) {
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}


// 初始化 SD 卡
static esp_err_t init_sd_card() {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card VFAT filesystem. Error: %d", ret);
    }

    ESP_LOGI(TAG, "init sd_card successed");

    return ret;
}

// 拍摄并保存图像
esp_err_t capture_and_save_image() {
    /*//手动白平衡
    sensor_t *sensor = esp_camera_sensor_get();
    sensor->set_wb_mode(sensor, 0);  // 设置白平衡模式为手动模式（0 代表手动模式）
    sensor->set_awb_gain(sensor, 0); // 禁用自动白平衡增益
    sensor->set_aec_value(sensor, (150 << 16) | (100 << 8) | 150); //sensor, R, G, B*/

    char picture_name[29];
    sprintf(picture_name, "/sdcard/%d.jpg", read_data_from_nvs(NVS_NAMESPACE, NVS_PICTURE_COUNT_KEY));

    ESP_LOGI(TAG, "Picture Count: %s", picture_name);

    // 配置GPIO为输出模式
    gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << CAM_PIN_DATA1),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,

    };
    gpio_config(&io_conf);

    gpio_set_level(CAM_PIN_DATA1, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    camera_fb_t *pic = esp_camera_fb_get();
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CAM_PIN_DATA1, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); //没测试，感觉可有可无

    init_sd_card(); //DATA1使用完必须初始化sd卡，否则无法访问

    if (!pic) {
        ESP_LOGE(TAG, "Camera Capture Failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Picture taken! Its size was: %zu bytes", pic->len);

    FILE *file = fopen(picture_name, "wb");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing. Error: %s", strerror(errno));
        esp_camera_fb_return(pic);
        return ESP_FAIL;
    }

    size_t bytes_written = fwrite(pic->buf, 1, pic->len, file);
    if (bytes_written != pic->len) {
        ESP_LOGE(TAG, "Write failed");
        fclose(file);
        esp_camera_fb_return(pic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Write successed");

    fclose(file);
    esp_camera_fb_return(pic);

    write_data_to_nvs(read_data_from_nvs(NVS_NAMESPACE, NVS_PICTURE_COUNT_KEY) + 1,
                      NVS_NAMESPACE, NVS_PICTURE_COUNT_KEY);

    return ESP_OK;
}

void app_main(void) {
    if (ESP_OK != init_camera() || ESP_OK != init_nvs()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    //write_data_to_nvs(0, NVS_NAMESPACE, NVS_PICTURE_COUNT_KEY); //初始化照片名字
    ESP_LOGI(TAG, "Taking picture...");
    capture_and_save_image();

    vTaskDelay(pdMS_TO_TICKS(100));

    // 配置GPIO为输出模式
    gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << CAM_PIN_DATA1),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,

    };
    gpio_config(&io_conf);
    gpio_set_level(CAM_PIN_DATA1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(CAM_PIN_DATA1, 0);

}
