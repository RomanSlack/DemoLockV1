/* 🔒 智能锁项目 - ESP32-S3 固件 🚀
 * 
 * 🌟 功能说明：
 * - Wi-Fi接入点模式 🛜
 * - HTTP服务器提供挑战-响应认证 🔐
 * - 使用可寻址LED指示锁状态 💡
 */

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "led_strip.h"
#include "sdkconfig.h"

/* 🏷️ 日志标签 */
static const char *TAG = "lock_app";

/* 🚥 LED配置 - GPIO 48 */
#define LED_STRIP_GPIO 48

static led_strip_handle_t led_strip = NULL;
static char pre_shared_key[32] = "DEFAULT_KEY";
static char current_challenge[64];
static bool lock_is_open = false;

/* 🎨 LED条初始化配置 */
static void configure_led_strip(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = 1,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    if (led_strip) {
        ESP_LOGI(TAG, "🎉 LED初始化成功 GPIO %d", LED_STRIP_GPIO);
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
    }
}

/* 🎨 设置LED颜色 */
static void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip) {
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, r, g, b));
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    }
}

/* 🎲 生成挑战令牌处理程序 */
static esp_err_t get_challenge_handler(httpd_req_t *req) {
    uint32_t rand_val = esp_random();
    snprintf(current_challenge, sizeof(current_challenge), "%u", (unsigned)rand_val);
    ESP_LOGI(TAG, "🎲 生成新挑战: %s", current_challenge);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, current_challenge);
    return ESP_OK;
}

/* 🔑 验证响应处理程序 */
static esp_err_t post_response_handler(httpd_req_t *req) {
    char resp_buf[64];
    int total_len = req->content_len;
    if (total_len >= sizeof(resp_buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "响应太长");
        return ESP_FAIL;
    }

    int recv_len = httpd_req_recv(req, resp_buf, total_len);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "未收到响应数据");
        return ESP_FAIL;
    }
    resp_buf[recv_len] = '\0';

    char expected[128];
    snprintf(expected, sizeof(expected), "%s%s", current_challenge, pre_shared_key);

    if (strcmp(resp_buf, expected) == 0) {
        lock_is_open = true;
        ESP_LOGI(TAG, "🟢 解锁成功！LED设为绿色");
        set_led_color(0, 255, 0);
        httpd_resp_sendstr(req, "Unlocked");
    } else {
        ESP_LOGW(TAG, "🔵 无效令牌 - LED闪烁蓝色");
        set_led_color(0, 0, 255);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid token");
        vTaskDelay(pdMS_TO_TICKS(4000));
        lock_is_open = false;
        ESP_LOGI(TAG, "🔴 重新锁定 - LED设为红色");
        set_led_color(255, 0, 0);
    }

    return ESP_OK;
}

/* 📱 主页处理程序 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    size_t index_html_size = (index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

/* 🌐 启动Web服务器 */
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/", .method = HTTP_GET, .handler = root_get_handler
        });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/challenge", .method = HTTP_GET, .handler = get_challenge_handler
        });
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/response", .method = HTTP_POST, .handler = post_response_handler
        });
    }
    return server;
}

/* 📡 初始化Wi-Fi接入点 */
static void wifi_init_softap(void) {
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "LockAP",
            .ssid_len = strlen("LockAP"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "🛜 Wi-Fi AP已启动. SSID=%s, 密码=%s",
             wifi_config.ap.ssid, wifi_config.ap.password);
}

/* 🚀 主程序入口 */
void app_main(void) {
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 初始化网络组件 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 配置LED并设置初始颜色为红色 */
    configure_led_strip();
    ESP_LOGI(TAG, "🔴 启动时设置LED为红色（已锁定）");
    set_led_color(255, 0, 0);

    /* 初始化Wi-Fi接入点 */
    wifi_init_softap();

    /* 启动HTTP服务器 */
    httpd_handle_t server = start_webserver();
    if (server) {
        ESP_LOGI(TAG, "🌐 HTTP服务器运行中. 连接到'LockAP'并访问 http://192.168.4.1/");
    } else {
        ESP_LOGE(TAG, "❌ HTTP服务器启动失败");
    }
}
