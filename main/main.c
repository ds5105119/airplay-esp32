#include "audio_output.h"
#include "audio_receiver.h"
#include "dns_server.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "lcd.h"
#include "nvs_flash.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "web_server.h"
#include "wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#ifdef CONFIG_SQUEEZEAMP
#include "squeezeamp.h"
#endif

static const char *TAG = "main";

// AP mode IP address (192.168.4.1 in network byte order)
#define AP_IP_ADDR 0x0104A8C0
#define BOOT_BUTTON_GPIO GPIO_NUM_0

static bool s_airplay_started = false;

static void start_airplay_services(void) {
  if (s_airplay_started) {
    return;
  }
  s_airplay_started = true;

  ESP_LOGI(TAG, "Starting AirPlay services...");

  esp_err_t err = ptp_clock_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
    s_airplay_started = false;
    return;
  }

  ESP_ERROR_CHECK(hap_init());
  ESP_ERROR_CHECK(audio_receiver_init());
  ESP_ERROR_CHECK(audio_output_init());
  audio_output_start();
  mdns_airplay_init();
  ESP_ERROR_CHECK(rtsp_server_start());

  ESP_LOGI(TAG, "AirPlay ready");
}

static void boot_button_task(void *pvParameters) {
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);

  bool was_pressed = false;
  while (1) {
    bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    if (pressed && !was_pressed) {
      vTaskDelay(pdMS_TO_TICKS(60));
      if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
        ESP_LOGI(TAG, "BOOT pressed: opening settings AP");
        wifi_settings_ap_open();
      }
    }
    was_pressed = pressed;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void wifi_monitor_task(void *pvParameters) {
  bool was_connected = wifi_is_connected();
  bool ap_enabled = wifi_settings_ap_is_enabled();
  bool dns_running = false;
  if (ap_enabled) {
    dns_server_start(AP_IP_ADDR);
    dns_running = true;
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool new_ap_enabled = wifi_settings_ap_is_enabled();
    if (new_ap_enabled != ap_enabled) {
      ap_enabled = new_ap_enabled;
      if (ap_enabled && !dns_running) {
        dns_server_start(AP_IP_ADDR);
        dns_running = true;
      } else if (!ap_enabled && dns_running) {
        dns_server_stop();
        dns_running = false;
      }
    }

    bool connected = wifi_is_connected();
    if (connected == was_connected) {
      continue;
    }

    if (connected) {
      ESP_LOGI(TAG, "WiFi connected");
      start_airplay_services();
    } else {
      ESP_LOGW(TAG, "WiFi disconnected");
    }

    was_connected = connected;
  }
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
  led_init();
  esp_err_t lcd_err = lcd_init();
  if (lcd_err != ESP_OK) {
    ESP_LOGW(TAG, "LCD init failed: %s", esp_err_to_name(lcd_err));
  }

#ifdef CONFIG_SQUEEZEAMP
  esp_err_t err = ESP_OK;
  err = squeezeamp_init();
  if (ESP_OK != err) {
    ESP_LOGE(TAG, "Failed to initialize SqueezeAMP: %s", esp_err_to_name(err));
  };
#endif

  // Start WiFi (APSTA mode: AP for config, STA for connection)
  wifi_init_apsta("O1", "OpenAirplay");

  // Wait for initial connection if credentials exist
  bool connected = false;
  if (settings_has_wifi_credentials()) {
    connected = wifi_wait_connected(30000);
  }

  if (!connected) {
    ESP_LOGI(TAG, "Connect to 'O1' -> http://192.168.4.1");
  }

  // Start services
  web_server_start(80);
  xTaskCreate(wifi_monitor_task, "wifi_mon", 4096, NULL, 5, NULL);
  xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 5, NULL);

  if (connected) {
    start_airplay_services();
  }
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
