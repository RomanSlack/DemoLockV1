/*
 * 🔒 Smart Lock Project - ESP32-S3 Firmware 🚀
 *
 * This firmware implements a smart lock system with the following features:
 *  - Operates in Wi-Fi Access Point (AP) mode.
 *  - Provides an HTTP server that implements challenge-response authentication.
 *  - Uses an addressable LED to indicate the lock status.
 *
 * The design leverages ESP-IDF components including Wi-Fi, HTTP server, and LED control via RMT.
 * Detailed error checking is performed using the ESP_ERROR_CHECK macro to ensure system robustness.
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

/* 🏷️ Global Log Tag for debugging messages */
static const char *TAG = "lock_app";

/* 🚥 LED configuration - using GPIO 48 for the addressable LED */
#define LED_STRIP_GPIO 48

/* Global handle for the LED strip device */
static led_strip_handle_t led_strip = NULL;

/* Pre-shared key for challenge-response authentication.
 * In a production environment, this key should be securely provisioned.
 */
static char pre_shared_key[32] = "DEFAULT_KEY";

/* Buffer to store the current challenge token */
static char current_challenge[64];

/* Boolean flag representing the lock state:
 * true  -> Unlocked
 * false -> Locked
 */
static bool lock_is_open = false;

/**
 * @brief Configures and initializes the LED strip.
 *
 * This function sets up the LED strip hardware by specifying the GPIO pin used
 * and the number of LEDs on the strip. It configures the RMT peripheral to drive
 * the LED with a specified resolution. On success, the LED strip is cleared to
 * ensure that no residual data is displayed.
 */
static void configure_led_strip(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = 1,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz resolution for precise timing control
        .flags.with_dma = false,
    };

    // Initialize the LED strip device using the RMT peripheral
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    if (led_strip) {
        ESP_LOGI(TAG, "🎉 LED initialization successful on GPIO %d", LED_STRIP_GPIO);
        // Clear the LED strip to ensure all LEDs are off at startup
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
    }
}

/**
 * @brief Sets the color of the LED.
 *
 * This function updates the color of the first LED (index 0) on the LED strip
 * by setting its red, green, and blue intensity values. The changes are then
 * applied by refreshing the LED strip.
 *
 * @param r Red intensity (0-255)
 * @param g Green intensity (0-255)
 * @param b Blue intensity (0-255)
 */
static void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip) {
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, r, g, b));
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    }
}

/**
 * @brief HTTP GET handler to generate and return a challenge token.
 *
 * This handler generates a pseudo-random challenge token using the ESP32's
 * hardware random number generator (esp_random). The generated challenge is stored
 * in a global buffer and then returned to the client as a plain text response.
 *
 * @param req Pointer to the HTTP request object.
 *
 * @return esp_err_t ESP_OK on success, or an appropriate error code on failure.
 */
static esp_err_t get_challenge_handler(httpd_req_t *req) {
    uint32_t rand_val = esp_random();
    snprintf(current_challenge, sizeof(current_challenge), "%u", (unsigned)rand_val);
    ESP_LOGI(TAG, "🎲 New challenge generated: %s", current_challenge);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, current_challenge);
    return ESP_OK;
}

/**
 * @brief HTTP POST handler for processing the client's response token.
 *
 * This function handles the authentication response from the client. It reads
 * the response token from the HTTP request and verifies it against the expected
 * token, which is a concatenation of the current challenge and the pre-shared key.
 *
 * On successful verification:
 *   - The lock state is updated to unlocked.
 *   - The LED is set to green to indicate an unlocked state.
 *   - A success message is sent back to the client.
 *
 * On failure:
 *   - The LED flashes blue to indicate an invalid token.
 *   - An HTTP error (401 Unauthorized) is sent to the client.
 *   - After a short delay, the lock is re-engaged and the LED is set to red.
 *
 * @param req Pointer to the HTTP request object.
 *
 * @return esp_err_t ESP_OK on success, or an appropriate error code on failure.
 */
static esp_err_t post_response_handler(httpd_req_t *req) {
    char resp_buf[64];
    int total_len = req->content_len;

    // Validate that the received data does not exceed the buffer size
    if (total_len >= sizeof(resp_buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Response too long");
        return ESP_FAIL;
    }

    // Receive the response token from the client
    int recv_len = httpd_req_recv(req, resp_buf, total_len);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No response data received");
        return ESP_FAIL;
    }
    resp_buf[recv_len] = '\0'; // Null-terminate the received string

    // Construct the expected token by concatenating the challenge and the pre-shared key
    char expected[128];
    snprintf(expected, sizeof(expected), "%s%s", current_challenge, pre_shared_key);

    // Verify the response token
    if (strcmp(resp_buf, expected) == 0) {
        lock_is_open = true;
        ESP_LOGI(TAG, "🟢 Unlock successful! LED set to green");
        set_led_color(0, 255, 0);
        httpd_resp_sendstr(req, "Unlocked");
    } else {
        ESP_LOGW(TAG, "🔵 Invalid token - LED flashing blue");
        set_led_color(0, 0, 255);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid token");

        // Wait for 4 seconds to allow the blue LED indication to be visible
        vTaskDelay(pdMS_TO_TICKS(4000));

        // Re-lock the system after the delay
        lock_is_open = false;
        ESP_LOGI(TAG, "🔴 Relocking - LED set to red");
        set_led_color(255, 0, 0);
    }

    return ESP_OK;
}

/**
 * @brief HTTP GET handler for serving the root web page.
 *
 * This handler serves the index.html file embedded in the firmware binary.
 * The start and end symbols (index_html_start and index_html_end) mark the
 * location of the HTML file data in the binary.
 *
 * @param req Pointer to the HTTP request object.
 *
 * @return esp_err_t ESP_OK on success.
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    // External symbols generated by the linker, representing the HTML file in flash
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    size_t index_html_size = (index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

/**
 * @brief Initializes and starts the HTTP server.
 *
 * This function configures the HTTP server with default settings, registers URI
 * handlers for the root page, challenge token generation, and authentication response,
 * and then starts the server.
 *
 * @return httpd_handle_t Handle to the HTTP server instance, or NULL if server startup fails.
 */
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handler for serving the main web page
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/", .method = HTTP_GET, .handler = root_get_handler
        });
        // Register URI handler for generating the challenge token
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/challenge", .method = HTTP_GET, .handler = get_challenge_handler
        });
        // Register URI handler for processing the response token
        httpd_register_uri_handler(server, &(httpd_uri_t){
            .uri = "/response", .method = HTTP_POST, .handler = post_response_handler
        });
    }
    return server;
}

/**
 * @brief Initializes and starts the Wi-Fi Access Point (AP) mode.
 *
 * This function sets up the ESP32 as a Wi-Fi Access Point with the following parameters:
 *  - SSID: "LockAP"
 *  - Channel: 1
 *  - Password: "12345678"
 *  - Maximum number of simultaneous connections: 4
 *  - Authentication mode: WPA/WPA2 PSK
 *
 * Upon successful configuration, the Access Point is started and becomes available
 * for client connections.
 */
static void wifi_init_softap(void) {
    // Create the default Wi-Fi AP network interface
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

    // Set the device to operate in AP mode and apply the configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "🛜 Wi-Fi AP started. SSID=%s, Password=%s",
             wifi_config.ap.ssid, wifi_config.ap.password);
}

/**
 * @brief Main application entry point.
 *
 * This function performs the following initialization steps:
 *  1. Initializes Non-Volatile Storage (NVS) to support system configurations.
 *  2. Sets up network components including the default Wi-Fi Access Point and event loop.
 *  3. Configures the LED strip and sets its initial color to red, indicating that the lock is engaged.
 *  4. Initializes the Wi-Fi Access Point to allow client connections.
 *  5. Starts the HTTP server to handle incoming web requests.
 */
void app_main(void) {
    /* Initialize NVS flash storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // If necessary, erase and reinitialize NVS
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize network components: ESP-NETIF and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Configure the LED strip and set the initial LED color to red (locked state) */
    configure_led_strip();
    ESP_LOGI(TAG, "🔴 Setting LED to red on startup (locked)");
    set_led_color(255, 0, 0);

    /* Initialize the Wi-Fi Access Point for client connections */
    wifi_init_softap();

    /* Start the HTTP server to handle incoming requests */
    httpd_handle_t server = start_webserver();
    if (server) {
        ESP_LOGI(TAG, "🌐 HTTP Server running. Connect to 'LockAP' and visit http://192.168.4.1/");
    } else {
        ESP_LOGE(TAG, "❌ HTTP Server failed to start");
    }
}