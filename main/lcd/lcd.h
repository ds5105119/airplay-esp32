#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t lcd_init(void);

void lcd_now_playing_set_title(const char *title);
void lcd_now_playing_set_progress(int64_t start, int64_t current, int64_t end,
                                  int sample_rate);
void lcd_now_playing_set_rate(double rate);
void lcd_now_playing_clear(void);

