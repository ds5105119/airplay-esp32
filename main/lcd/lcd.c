#include "lcd.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#if CONFIG_LCD_MODE_I2C || CONFIG_LCD_MODE_4BIT

#include "audio_receiver.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hd44780.h"
#include "rtsp_events.h"

static const char *TAG = "lcd";

#ifndef CONFIG_I2C_LCD_SCROLL_RESET_WAIT_MS
#define CONFIG_I2C_LCD_SCROLL_RESET_WAIT_MS 5000
#endif

#if CONFIG_LCD_MODE_I2C
#include "driver/i2c.h"
#endif

typedef struct {
  char title[128];
  int64_t progress_start;
  int64_t progress_current;
  int64_t progress_end;
  int sample_rate;
  double rate;
  int64_t progress_base_us;
  bool has_title;
  bool has_progress;
} lcd_now_playing_t;

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;

#if CONFIG_LCD_MODE_I2C
static i2c_port_t s_i2c_port;
static uint8_t s_i2c_addr;
#endif
static hd44780_t s_lcd;
static lcd_now_playing_t s_now_playing;

static const uint8_t s_char_music_note[8] = {
    0x02, 0x03, 0x02, 0x0E, 0x1E, 0x0C, 0x00, 0x00,
};

static void sanitize_ascii(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }

  size_t w = 0;
  for (size_t r = 0; src[r] != '\0' && w + 1 < dst_size; r++) {
    unsigned char ch = (unsigned char)src[r];
    if (ch >= 0x20 && ch <= 0x7e) {
      dst[w++] = (char)ch;
    } else if (ch == '\t') {
      dst[w++] = ' ';
    } else if ((ch & 0x80) == 0) {
      dst[w++] = '?';
    } else {
      dst[w++] = '?';
      while ((src[r + 1] & 0xC0) == 0x80) {
        r++;
      }
    }
  }
  dst[w] = '\0';
}

#if CONFIG_LCD_MODE_I2C
static esp_err_t lcd_i2c_write(uint8_t data) {
  return i2c_master_write_to_device(s_i2c_port, s_i2c_addr, &data, 1,
                                    pdMS_TO_TICKS(100));
}

static esp_err_t lcd_i2c_probe(uint8_t addr) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd) {
    return ESP_ERR_NO_MEM;
  }

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_stop(cmd);

  esp_err_t err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
  i2c_cmd_link_delete(cmd);
  return err;
}

static int lcd_i2c_scan(uint8_t *out_addrs, size_t out_capacity) {
  int found = 0;
  for (uint8_t addr = 0x03; addr < 0x78; addr++) {
    if (lcd_i2c_probe(addr) == ESP_OK) {
      if (out_addrs && (size_t)found < out_capacity) {
        out_addrs[found] = addr;
      }
      found++;
    }
  }
  return found;
}

static esp_err_t write_lcd_data(const hd44780_t *lcd, uint8_t data) {
  (void)lcd;
  return lcd_i2c_write(data);
}

static esp_err_t lcd_i2c_init(void) {
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = CONFIG_I2C_LCD_SDA_GPIO,
      .scl_io_num = CONFIG_I2C_LCD_SCL_GPIO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 100000,
      .clk_flags = 0,
  };

  esp_err_t err = i2c_param_config(s_i2c_port, &conf);
  if (err == ESP_ERR_INVALID_STATE) {
    return ESP_OK;
  }
  if (err != ESP_OK) {
    return err;
  }

  err = i2c_driver_install(s_i2c_port, conf.mode, 0, 0, 0);
  if (err == ESP_ERR_INVALID_STATE) {
    return ESP_OK;
  }
  return err;
}
#endif

static void lcd_format_title15(char out[16], const char *title, int scroll_off) {
  memset(out, ' ', 15);
  out[15] = '\0';

  if (!title || title[0] == '\0') {
    // default placeholder
    static const char kDefaultTitle[] = "O1";
    memcpy(out, kDefaultTitle, sizeof(kDefaultTitle) - 1);
    return;
  }

  size_t title_len = strlen(title);
  if (title_len <= 15) {
    memcpy(out, title, title_len);
    return;
  }

  // Marquee: "title␠␠title␠␠..." (2-space gap), window is 15 chars.
  const size_t pattern_len = title_len + 2;
  if (pattern_len == 0) {
    return;
  }

  int off = scroll_off % (int)pattern_len;
  if (off < 0) {
    off += (int)pattern_len;
  }

  for (size_t i = 0; i < 15; i++) {
    size_t idx = ((size_t)off + i) % pattern_len;
    out[i] = (idx < title_len) ? title[idx] : ' ';
  }
}

static void clamp_m_ss(uint32_t in_sec, uint8_t *out_min, uint8_t *out_sec) {
  if (!out_min || !out_sec) {
    return;
  }
  if (in_sec > 9 * 60 + 59) {
    in_sec = 9 * 60 + 59;
  }
  *out_min = (uint8_t)(in_sec / 60);
  *out_sec = (uint8_t)(in_sec % 60);
}

static void lcd_format_progress_bar(char out[17], const lcd_now_playing_t *np,
                                    int64_t now_us) {
  // Default placeholder
  memcpy(out, "0:00----|---0:00", 16);
  out[16] = '\0';

  if (!np->has_progress || np->progress_end <= np->progress_start ||
      np->sample_rate <= 0) {
    return;
  }

  int64_t cur = np->progress_current;
  if (np->progress_base_us > 0 && audio_receiver_is_playing()) {
    int64_t elapsed_us = now_us - np->progress_base_us;
    if (elapsed_us > 0) {
      double rate = np->rate;
      if (rate <= 0.01) {
        rate = 1.0;
      }
      cur += (int64_t)((elapsed_us * (double)np->sample_rate * rate) /
                       1000000.0);
    }
  }

  if (cur < np->progress_start) {
    cur = np->progress_start;
  }
  if (cur > np->progress_end) {
    cur = np->progress_end;
  }

  int64_t pos_samples = cur - np->progress_start;
  int64_t dur_samples = np->progress_end - np->progress_start;
  if (pos_samples < 0) {
    pos_samples = 0;
  }
  if (dur_samples <= 0) {
    return;
  }

  uint32_t pos_sec = (uint32_t)(pos_samples / np->sample_rate);
  uint32_t dur_sec = (uint32_t)(dur_samples / np->sample_rate);

  uint8_t pos_m = 0, pos_s = 0, dur_m = 0, dur_s = 0;
  clamp_m_ss(pos_sec, &pos_m, &pos_s);
  clamp_m_ss(dur_sec, &dur_m, &dur_s);

  char left[5];
  char right[5];
  snprintf(left, sizeof(left), "%u:%02u", pos_m, pos_s);
  snprintf(right, sizeof(right), "%u:%02u", dur_m, dur_s);

  char bar[9];
  memset(bar, '-', 8);
  bar[8] = '\0';
  uint32_t bar_pos = (uint32_t)((pos_samples * 7) / dur_samples);
  if (bar_pos > 7) {
    bar_pos = 7;
  }
  bar[bar_pos] = '|';

  memcpy(out + 0, left, 4);
  memcpy(out + 4, bar, 8);
  memcpy(out + 12, right, 4);
}

static void on_rtsp_event(rtsp_event_t event, void *user_data) {
  (void)user_data;
  if (event == RTSP_EVENT_DISCONNECTED) {
    lcd_now_playing_clear();
  }
}

static void lcd_task(void *pvParameters) {
  (void)pvParameters;

  char last_title[128] = {0};
  char title15_prev[16] = {0};
  char line2_prev[17] = {0};

  int scroll_offset = 0;
  int64_t next_scroll_us = 0;
  int64_t reset_wait_until_us = 0;

  while (1) {
    lcd_now_playing_t np = {0};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      np = s_now_playing;
      xSemaphoreGive(s_mutex);
    }

    int64_t now_us = esp_timer_get_time();

    if (strcmp(last_title, np.title) != 0) {
      strlcpy(last_title, np.title, sizeof(last_title));
      scroll_offset = 0;
      // Initial pause before starting marquee scroll
      reset_wait_until_us =
          now_us + (int64_t)CONFIG_I2C_LCD_SCROLL_RESET_WAIT_MS * 1000;
      next_scroll_us = 0;
    }

    char line2[17];
    const char *title = np.has_title ? np.title : "";
    size_t title_len = strlen(title);
    if (title_len > 15) {
      const int pattern_len = (int)(title_len + 2);
      if (pattern_len > 0 && reset_wait_until_us > 0) {
        // Hold initial position during the reset-wait window.
        if (now_us >= reset_wait_until_us) {
          reset_wait_until_us = 0;
          next_scroll_us = now_us + (int64_t)CONFIG_I2C_LCD_SCROLL_MS * 1000;
        } else {
          scroll_offset = 0;
        }
      } else if (pattern_len > 0 && now_us >= next_scroll_us) {
        scroll_offset = (scroll_offset + 1) % pattern_len;
        if (scroll_offset == 0) {
          reset_wait_until_us =
              now_us + (int64_t)CONFIG_I2C_LCD_SCROLL_RESET_WAIT_MS * 1000;
          next_scroll_us = 0;
        } else {
          next_scroll_us = now_us + (int64_t)CONFIG_I2C_LCD_SCROLL_MS * 1000;
        }
      }
    } else {
      scroll_offset = 0;
      reset_wait_until_us = 0;
      next_scroll_us = 0;
    }

    char title15[16];
    lcd_format_title15(title15, title, scroll_offset);

    if (memcmp(title15, title15_prev, sizeof(title15_prev)) != 0) {
      hd44780_gotoxy(&s_lcd, 0, 0);
      hd44780_putc(&s_lcd, 0); // custom char 0: music note
      hd44780_puts(&s_lcd, title15);
      memcpy(title15_prev, title15, sizeof(title15_prev));
    }

    lcd_format_progress_bar(line2, &np, now_us);

    if (strcmp(line2, line2_prev) != 0) {
      hd44780_gotoxy(&s_lcd, 0, 1);
      hd44780_puts(&s_lcd, line2);
      strlcpy(line2_prev, line2, sizeof(line2_prev));
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_I2C_LCD_UPDATE_MS));
  }
}

esp_err_t lcd_init(void) {
  if (s_task) {
    return ESP_OK;
  }

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }

#if CONFIG_LCD_MODE_I2C
  s_i2c_port = (i2c_port_t)CONFIG_I2C_LCD_I2C_PORT;
  s_i2c_addr = (uint8_t)CONFIG_I2C_LCD_I2C_ADDR;

  esp_err_t err = lcd_i2c_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
    return err;
  }

  esp_err_t probe_err = lcd_i2c_probe(s_i2c_addr);
  if (probe_err != ESP_OK) {
    ESP_LOGE(TAG, "I2C probe failed for addr=0x%02x: %s", s_i2c_addr,
             esp_err_to_name(probe_err));

    uint8_t addrs[16];
    int found = lcd_i2c_scan(addrs, sizeof(addrs));
    if (found <= 0) {
      ESP_LOGE(TAG,
               "I2C scan found no devices on port=%d SDA=%d SCL=%d (check "
               "wiring/pullups)",
               (int)s_i2c_port, CONFIG_I2C_LCD_SDA_GPIO, CONFIG_I2C_LCD_SCL_GPIO);
      return ESP_FAIL;
    }

    if (found == 1) {
      ESP_LOGW(TAG, "I2C scan found one device at 0x%02x, using it", addrs[0]);
      s_i2c_addr = addrs[0];
    } else {
      char buf[128];
      size_t pos = 0;
      pos += snprintf(buf + pos, sizeof(buf) - pos, "I2C scan found %d addrs:", found);
      int show = found;
      if (show > (int)(sizeof(addrs) / sizeof(addrs[0]))) {
        show = (int)(sizeof(addrs) / sizeof(addrs[0]));
      }
      for (int i = 0; i < show && pos + 6 < sizeof(buf); i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 0x%02x", addrs[i]);
      }
      ESP_LOGE(TAG, "%s (set CONFIG_I2C_LCD_I2C_ADDR accordingly)", buf);
      return ESP_FAIL;
    }
  }

  s_lcd = (hd44780_t){
      .write_cb = write_lcd_data,
      .font = HD44780_FONT_5X8,
      .lines = 2,
      .pins =
          {
              .rs = 0,
              .e = 2,
              .d4 = 4,
              .d5 = 5,
              .d6 = 6,
              .d7 = 7,
              .bl = 3,
          },
  };
#else
  // Direct 4-bit GPIO connection.
  s_lcd = (hd44780_t){
      .write_cb = NULL,
      .font = HD44780_FONT_5X8,
      .lines = 2,
      .pins =
          {
              .rs = (uint8_t)CONFIG_LCD_4BIT_RS_GPIO,
              .e = (uint8_t)CONFIG_LCD_4BIT_E_GPIO,
              .d4 = (uint8_t)CONFIG_LCD_4BIT_D4_GPIO,
              .d5 = (uint8_t)CONFIG_LCD_4BIT_D5_GPIO,
              .d6 = (uint8_t)CONFIG_LCD_4BIT_D6_GPIO,
              .d7 = (uint8_t)CONFIG_LCD_4BIT_D7_GPIO,
              .bl = HD44780_NOT_USED,
          },
  };
#endif

  esp_err_t err = hd44780_init(&s_lcd);
  if (err != ESP_OK) {
#if CONFIG_LCD_MODE_I2C
    ESP_LOGE(TAG, "LCD init failed (addr=0x%02x): %s", s_i2c_addr,
             esp_err_to_name(err));
#else
    ESP_LOGE(TAG, "LCD init failed (4-bit GPIO): %s", esp_err_to_name(err));
#endif
    return err;
  }

  hd44780_upload_character(&s_lcd, 0, s_char_music_note);

  hd44780_control(&s_lcd, true, false, false);
  hd44780_switch_backlight(&s_lcd, true);
  hd44780_clear(&s_lcd);

  hd44780_gotoxy(&s_lcd, 0, 0);
  hd44780_putc(&s_lcd, 0);
  hd44780_puts(&s_lcd, "O1");
  hd44780_gotoxy(&s_lcd, 0, 1);
  hd44780_puts(&s_lcd, "0:00----|---0:00");

  rtsp_events_register(on_rtsp_event, NULL);

  if (xTaskCreate(lcd_task, "lcd", 4096, NULL, 5, &s_task) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create LCD task");
    s_task = NULL;
    return ESP_ERR_NO_MEM;
  }

#if CONFIG_LCD_MODE_I2C
  ESP_LOGI(TAG, "I2C LCD initialized (SDA=%d SCL=%d addr=0x%02x)",
           CONFIG_I2C_LCD_SDA_GPIO, CONFIG_I2C_LCD_SCL_GPIO, s_i2c_addr);
#else
  ESP_LOGI(TAG,
           "4-bit LCD initialized (RS=%d E=%d D4=%d D5=%d D6=%d D7=%d)",
           CONFIG_LCD_4BIT_RS_GPIO, CONFIG_LCD_4BIT_E_GPIO,
           CONFIG_LCD_4BIT_D4_GPIO, CONFIG_LCD_4BIT_D5_GPIO,
           CONFIG_LCD_4BIT_D6_GPIO, CONFIG_LCD_4BIT_D7_GPIO);
#endif
  return ESP_OK;
}

void lcd_now_playing_set_title(const char *title) {
  if (!s_mutex) {
    return;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  sanitize_ascii(s_now_playing.title, sizeof(s_now_playing.title), title);
  s_now_playing.has_title = s_now_playing.title[0] != '\0';

  xSemaphoreGive(s_mutex);
}

void lcd_now_playing_set_progress(int64_t start, int64_t current, int64_t end,
                                  int sample_rate) {
  if (!s_mutex) {
    return;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  s_now_playing.progress_start = start;
  s_now_playing.progress_current = current;
  s_now_playing.progress_end = end;
  s_now_playing.sample_rate = sample_rate > 0 ? sample_rate : 44100;
  s_now_playing.progress_base_us = esp_timer_get_time();
  s_now_playing.has_progress = (end > start);

  xSemaphoreGive(s_mutex);
}

void lcd_now_playing_set_rate(double rate) {
  if (!s_mutex) {
    return;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  s_now_playing.rate = rate;

  xSemaphoreGive(s_mutex);
}

void lcd_now_playing_clear(void) {
  if (!s_mutex) {
    return;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  memset(&s_now_playing, 0, sizeof(s_now_playing));
  s_now_playing.rate = 1.0;

  xSemaphoreGive(s_mutex);
}

#else

esp_err_t lcd_init(void) {
  return ESP_OK;
}
void lcd_now_playing_set_title(const char *title) {
  (void)title;
}
void lcd_now_playing_set_progress(int64_t start, int64_t current, int64_t end,
                                  int sample_rate) {
  (void)start;
  (void)current;
  (void)end;
  (void)sample_rate;
}
void lcd_now_playing_set_rate(double rate) {
  (void)rate;
}
void lcd_now_playing_clear(void) {
}

#endif
