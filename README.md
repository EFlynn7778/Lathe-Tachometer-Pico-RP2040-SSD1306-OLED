# Lathe-Tachometer-Pico-RP2040-SSD1306-OLED
RP2040 Based OLED Tachometer for a Lathe, or any other rotating device.


# Lathe Tachometer with Pi Pico

A digital tachometer for lathes or other rotating device using a Raspberry Pi Pico microcontroller and SSD1306 OLED display. 
The system uses a hall effect sensor, or other voltage output digital senssor type to measure rotation speed and displays RPM with configurable settings.

Sensor can be hall,optical, inductive etc.  Output mut be < 3.3v. If your sensor output is > 3.3v use a voltage divider on the output to the Pico input pin.

## Features

- Real-time RPM measurement and display
- 128x64 OLED display with large digit readout
- Configurable settings stored in flash memory:
  - Number of pulses per revolution (1-66)
  - Gear ratio adjustment (0.1-10.0)
  - Decimal point display option
  - Adjustable low-pass filtering (0-10)
- Auto-ranging display (decimal points for low RPM, integers for high RPM)
- Settings menu with timeout
- Non-volatile storage of settings

## Hardware Requirements

- Raspberry Pi Pico
- SSD1306 128x64 OLED Display (I2C)
- Hall Effect Sensor or any other sensor capbable of sending a pulse
- 2 Push Buttons
- Pull-up Resistors for Buttons

## Pin Connections - Change to match your hardware

- **I2C Display:**
  - SDA: GPIO 6
  - SCL: GPIO 7
- **Hall Sensor:** GPIO 29
- **Buttons:**
  - UP: GPIO 1
  - DOWN: GPIO 2

## Usage

### Normal Operation
- The display shows current RPM in large digits
- RPM below 100 can show decimal places if enabled
- Current pulses/rev and gear ratio shown at bottom

### Settings Menu
- **Long Press UP:** Enter menu/cycle through options
- **Long Press DOWN:** Exit menu and save
- **Short Press UP/DOWN:** Adjust selected value
- Menu items:
  1. Pulses per revolution (1-66)
  2. Gear ratio (0.1-10.0)
  3. Show decimal point (Yes/No)
  4. Filter strength (0-10)

## Building

Requires the Raspberry Pi Pico C/C++ SDK. Build using CMake:

Flash the resulting `.uf2` file to the Pico.

## Dependencies

- Raspberry Pi Pico SDK
- SSD1306 OLED Library
