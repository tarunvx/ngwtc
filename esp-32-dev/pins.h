#ifndef PINS_H
#define PINS_H

// ============================================================
//  GPIO map — ESP32 WROOM-32 (Smart Water Tank Controller)
//  Avoid strapping pins (0, 2, 12, 15) for inputs.
//  Input-only pins: 34..39 (no internal pull-ups — use external 10k).
// ============================================================

// Tank floats (active LOW, internal pull-up)
#define PIN_FLOAT_25     32
#define PIN_FLOAT_50     33
#define PIN_FLOAT_75     25
#define PIN_FLOAT_100    26

// Flow sensor (YF-S201) — ISR
#define PIN_FLOW         27

// CT clamp (current sense) ADC, input-only
#define PIN_CT_ADC       35

// Solenoid feedback micro-switches (active LOW, pull-up)
#define PIN_FB_ON        14
#define PIN_FB_OFF       13

// Solenoid relay drives (active HIGH, opto-isolated relays)
#define PIN_RELAY_ON     16
#define PIN_RELAY_OFF    17

// DHT22 (single-wire)
#define PIN_DHT22         4

// Buttons (active LOW, pull-up)
#define PIN_BTN_1        18
#define PIN_BTN_2        19
#define PIN_BTN_3         0   // on-board BOOT button — internal pull-up OK
#define PIN_BTN_4        15   // strapping pin — must be HIGH at boot (pull-up does this)

// Buzzer (active HIGH)
#define PIN_BUZZER       23

// NeoPixel data
#define PIN_NEOPIXEL      5
#define NEO_COUNT         9

// I2C (OLED)
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22

#endif



/*

The G prefix on the ESP32 silkscreen just means GPIO.
So G5 = GPIO5, G18 = GPIO18, etc. The numbers in pins.h are GPIO numbers — they map 1:1 to the Gxx labels on the board.

Mapping table (current pins.h)
Function	Macro	GPIO	Board label	Notes
Float 25%	[PIN_FLOAT_25]pins.h )	32	G32	Input + pull-up
Float 50%	[PIN_FLOAT_50]pins.h )	33	G33	Input + pull-up
Float 75%	[PIN_FLOAT_75]pins.h )	25	G25	Input + pull-up
Float 100%	[PIN_FLOAT_100]pins.h )	26	G26	Input + pull-up
Flow sensor (YF-S201)	[PIN_FLOW]pins.h )	27	G27	ISR, pull-up
CT clamp ADC	[PIN_CT_ADC]pins.h )	35	G35 / VN-area	Input-only, ADC1_CH7
Feedback ON µ-switch	[PIN_FB_ON]pins.h )	14	G14	Input + pull-up
Feedback OFF µ-switch	[PIN_FB_OFF]pins.h )	13	G13	Input + pull-up
Relay ON	[PIN_RELAY_ON]pins.h )	16	G16 (RX2)	Output
Relay OFF	[PIN_RELAY_OFF]pins.h )	17	G17 (TX2)	Output
DHT22	[PIN_DHT22]pins.h )	4	G4	1-wire
Button 1	[PIN_BTN_1]pins.h )	18	G18	Input + pull-up
Button 2	[PIN_BTN_2]pins.h )	19	G19	Input + pull-up
Button 3	[PIN_BTN_3]pins.h )	34	G34	Input-only, ext. 10 k pull-up
Button 4	[PIN_BTN_4]pins.h )	39	G39 / VN	Input-only, ext. 10 k pull-up
Buzzer	[PIN_BUZZER]pins.h )	23	G23	Output
NeoPixel data	[PIN_NEOPIXEL]pins.h )	5	G5	Output (boot-strapping; HIGH at boot is fine for WS2812)
I²C SDA (OLED)	[PIN_I2C_SDA]pins.h )	21	G21	I²C bus
I²C SCL (OLED)	[PIN_I2C_SCL]pins.h )	22	G22	I²C bus
Power / fixed pins on the dev board (not in pins.h, just reference)
Label	Meaning
VIN / 5V	5 V input from buck #1 (via USB regulator bypass)
3V3	3.3 V LDO output (~500 mA max — don't power relays)
GND	Ground (use star grounding)
EN	Reset (active LOW)
CLK, SD0, SD1, CMD	Internal SPI flash — do not use
G6–G11	Connected to flash — do not use
TX0 (G1), RX0 (G3)	USB serial — keep free for logs
Strapping / boot pins (avoid for inputs that may be LOW at boot)
GPIO	Constraint
G0	Must be HIGH at boot (held LOW = flash mode)
G2	Must be LOW or floating at boot
G12	Must be LOW at boot (selects flash voltage)
G15	Must be HIGH at boot (suppresses boot log)

Quick visual (typical 30-pin DevKit-V1)

            ┌─────────────┐
        EN  │             │ G23  ← Buzzer
       G36  │             │ G22  ← I2C SCL
       G39  │             │ TX0
       G34  │ ← PRESSURE  │ RX0
       G35  │ ← CT_ADC    │ G21  ← I2C SDA
       G32  │ ← FLOAT_25  │ G19  ← BTN2
       G33  │ ← FLOAT_50  │ G18  ← BTN1
       G25  │ ← FLOAT_75  │ G5   ← NEOPIXEL
       G26  │ ← FLOAT_100 │ G17  ← RELAY_OFF
       G27  │ ← FLOW      │ G16  ← RELAY_ON
       G14  │ ← FB_ON     │ G4   ← DHT22
       G12  │ (strap)     │ G0   ← BTN3 (strap/BOOT) 
       G13  │ ← FB_OFF    │ G2   (strap/LED)
       GND  │             │ G15  ← BTN4 (strap)
       VIN  │             │ 3V3
            └─────────────┘

*/