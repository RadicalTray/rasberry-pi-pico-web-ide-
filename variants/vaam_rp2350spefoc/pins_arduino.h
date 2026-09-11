#pragma once
#define PICO_RP2350A 1
// Pin definitions for RP2350 SimpleFOC + SPE

// Motor
#define PIN_MOTOR_LU    (20u)  // Motor Lower U
#define PIN_MOTOR_UU    (21u)  // Motor Upper U
#define PIN_MOTOR_LV    (22u)  // Motor Lower V
#define PIN_MOTOR_UV    (23u)  // Motor Upper V
#define PIN_MOTOR_LW    (24u)  // Motor Lower W
#define PIN_MOTOR_UW    (25u)  // Motor Upper W

// FET Driver
#define PIN_FET_EN    (12u)  // FET Lower U

// ADC
#define PIN_ADC_U      (27u)  // ADC U
#define PIN_ADC_V      (28u)  // ADC V
#define PIN_ADC_W      (29u)  // ADC W
#define PIN_ADC_PWS    (26u)  // ADC Supplied Voltage
// Encoder

#define PIN_ENCODER_CS (1u)  // Encoder Lower A
// other encoder pins are shared with SPI fet driver pins

// LED GPIO
#define PIN_LED        (0u)

// Serial
//#define PIN_SERIAL1_TX (12u)
//#define PIN_SERIAL1_RX (13u)

// SPI (probably unused idk, SPIETH uses 5 as CS anyway)
// #define PIN_SPI0_MISO  (4u)
// #define PIN_SPI0_MOSI  (3u)
// #define PIN_SPI0_SCK   (2u)
// #define PIN_SPI0_SS    (5u)

// SPI ETH
#define PIN_SPI0_MISO  (16u)
#define PIN_SPI0_MOSI  (19u)
#define PIN_SPI0_SCK   (18u)
#define PIN_SPI0_SS    ( 5u)
#define PIN_ETH_SS     (17u)
#define PIN_ETH_RST    (15u)
#define PIN_ETH_IRQ    (14u)

// Wire
#define PIN_WIRE0_SDA  (20u)     // pico pin must be this pair
#define PIN_WIRE0_SCL  (21u)     //..
#define PIN_WIRE1_SDA  (22u)
#define PIN_WIRE1_SCL  (23u)

// LCD - EXT
#define PIN_LCD_CS     (7u)      // LCD Chip Select
#define PIN_LCD_DC     (8u)      // LCD Data/Command
#define PIN_LCD_RES    (9u)      // LCD Reset
#define PIN_LCD_BLK     (6u)      // LCD Backlight

#define SERIAL_HOWMANY (0u)
#define SPI_HOWMANY    (2u)
#define WIRE_HOWMANY   (1u)
#define PIN_SERIAL1_TX -1
#define PIN_SERIAL1_RX -1
#define PIN_SERIAL2_TX -1
#define PIN_SERIAL2_RX -1

#include "../common.h"
