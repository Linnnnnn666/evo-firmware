// SSD1306 OLED 驱动（128x64, I2C）——烧录工作台 HUD
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool oled_init(void);
void oled_clear(void);
void oled_show(void);                     // 把缓冲刷到屏幕
void oled_text(uint8_t x, uint8_t y, const char *s);   // 6x8 字符，x=列(0..20) y=行(0..7)
void oled_bar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t pct);
void oled_big(uint8_t y, const char *s);  // 16x16 数字/大写字母（状态大字）
void oled_cn(uint8_t x, uint8_t y, const char *s);  // 中文 16x16（UTF-8），y=页(0..6 偶数)
void oled_cn_line(uint8_t y, const char *s);        // 中文行（左对齐），y=页(0..6 偶数)

#ifdef __cplusplus
}
#endif
