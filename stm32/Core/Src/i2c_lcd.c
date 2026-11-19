#include "i2c_lcd.h"

#define LCD_ADDR (0x27 << 1)   // try 0x3F<<1 if the LCD stays blank

static I2C_HandleTypeDef *lcd_i2c;

void lcd_send_cmd(char cmd) {
    uint8_t data_u = (cmd & 0xF0);
    uint8_t data_l = ((cmd << 4) & 0xF0);
    uint8_t data_t[4] = {
        (uint8_t)(data_u | 0x0C), (uint8_t)(data_u | 0x08),
        (uint8_t)(data_l | 0x0C), (uint8_t)(data_l | 0x08)
    };
    HAL_I2C_Master_Transmit(lcd_i2c, LCD_ADDR, data_t, 4, 100);
}

void lcd_send_data(char data) {
    uint8_t data_u = (data & 0xF0);
    uint8_t data_l = ((data << 4) & 0xF0);
    uint8_t data_t[4] = {
        (uint8_t)(data_u | 0x0D), (uint8_t)(data_u | 0x09),
        (uint8_t)(data_l | 0x0D), (uint8_t)(data_l | 0x09)
    };
    HAL_I2C_Master_Transmit(lcd_i2c, LCD_ADDR, data_t, 4, 100);
}

void lcd_clear(void) {
    lcd_send_cmd(0x01);
    HAL_Delay(2);
}

void lcd_put_cur(int row, int col) {
    switch (row) {
        case 0: col |= 0x80; break;
        case 1: col |= 0xC0; break;
    }
    lcd_send_cmd(col);
}

void lcd_init(I2C_HandleTypeDef *hi2c) {
    lcd_i2c = hi2c;
    HAL_Delay(50);
    lcd_send_cmd(0x30); HAL_Delay(5);
    lcd_send_cmd(0x30); HAL_Delay(1);
    lcd_send_cmd(0x30); HAL_Delay(10);
    lcd_send_cmd(0x20); HAL_Delay(10);  // switch to 4-bit mode

    lcd_send_cmd(0x28); HAL_Delay(1);   // 2-line, 5x8 font
    lcd_send_cmd(0x08); HAL_Delay(1);   // display off
    lcd_send_cmd(0x01); HAL_Delay(2);   // clear
    lcd_send_cmd(0x06); HAL_Delay(1);   // entry mode
    lcd_send_cmd(0x0C); HAL_Delay(1);   // display on, cursor off
}

void lcd_send_string(char *str) {
    while (*str) lcd_send_data(*str++);
}/*
 * i2c_lcd.c
 *
 *  Created on: Sep 22, 2026
 *      Author: Jyoba
 */


