/*!
	Project Name: Lathe Tach Pico SSD1306 OLED
*/

// === Libraries ===
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "ssd1306/SSD1306_OLED.hpp"
#include "ssd1306/SSD1306_OLED_font.hpp"

// Screen settings
#define myOLEDwidth  128
#define myOLEDheight 64
#define myScreenSize (myOLEDwidth * (myOLEDheight/8)) // eg 1024 bytes = 128 * 64/8
uint8_t screenBuffer[myScreenSize]; // Define a buffer to cover whole screen  128 * 64/8

// Display timing parameters
#define DISPLAY_UPDATE_INTERVAL 100  // Update display every 100ms (was 250ms)
#define MENU_TIMEOUT 10000          // Exit menu after 10 seconds of inactivity
#define RPM_TIMEOUT_MS 500          // Timeout for zero RPM (was 3000000us = 3000ms)
#define RPM_TIMEOUT_CHECK_MS 100    // Check for timeout every 100ms (was 1000ms)
#define FLASH_PAGE_SIZE   256

// I2C settings
const uint16_t I2C_Speed = 1000;
const uint8_t I2C_GPIO_CLK = 7;
const uint8_t I2C_GPIO_DATA = 6;

// Hall sensor and button settings
const uint8_t HALL_SENSOR_PIN = 12;    // GPIO pin for hall sensor input
const uint8_t BUTTON_UP_PIN = 1;      // GPIO pin for UP button
const uint8_t BUTTON_DOWN_PIN = 2;    // GPIO pin for DOWN button
const uint32_t DEBOUNCE_DELAY = 50;    // Button debounce delay (ms)
const uint32_t LONG_PRESS_TIME = 1000; // Long press detection time (ms)

// Settings storage
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
const uint8_t *flash_target_contents = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

// Settings structure
typedef struct {
    uint32_t magic_number;       // To verify settings are valid
    uint8_t pulses_per_rev;      // Number of pulses per revolution
    float gear_ratio;            // Gear ratio multiplier
    bool show_decimal;           // Display decimal point or not
    uint8_t filter_strength;     // Filter strength (0-10, 0=no filtering)
    float workpiece_diameter;    // Diameter of the workpiece
    bool use_inches;             // true = inches, false = mm
} tach_settings_t;

#define SETTINGS_MAGIC 0xABCD1234 // Magic number to validate settings

// Menu states
enum MenuState {
    MENU_NONE,
    MENU_PULSES,
    MENU_RATIO,
    MENU_DECIMAL,
    MENU_FILTER,
    MENU_DIAMETER,
    MENU_UNITS
};

// Global variables
volatile uint32_t pulse_count = 0;               // Counter for hall sensor pulses
volatile uint64_t last_pulse_time = 0;           // Time of last pulse
volatile uint64_t current_pulse_time = 0;        // Time of current pulse
volatile uint64_t pulse_interval_sum = 0;        // Sum of pulse intervals
volatile uint8_t pulse_intervals_count = 0;      // Count of measured intervals
volatile bool rpm_data_ready = false;            // Flag for new RPM data
volatile float current_rpm = 0.0f;               // Current calculated RPM
volatile float filtered_rpm = 0.0f;              // Filtered RPM value
tach_settings_t settings;                        // Tachometer settings

// Button states
volatile bool button_up_pressed = false;
volatile bool button_down_pressed = false;
volatile uint64_t button_up_press_time = 0;
volatile uint64_t button_down_press_time = 0;
volatile bool button_up_long_press = false;
volatile bool button_down_long_press = false;

// UI state
MenuState current_menu = MENU_NONE;
uint32_t menu_last_activity = 0;

// instantiate an OLED object
SSD1306 myOLED(myOLEDwidth, myOLEDheight);

// GPIO interrupt handler
void gpio_callback(uint gpio, uint32_t events);

// =============== Function prototypes ================
void setup(void);
void process_buttons(void);
void load_settings(void);
void save_settings(void);
void display_rpm(void);
void display_menu(void);
void calculate_rpm(void);

// Calculate surface speed based on RPM and workpiece diameter
float calculate_surface_speed() {
    // If RPM is 0 or very low, return 0 to avoid unnecessary calculations
    if (current_rpm < 0.1f) {
        return 0.0f;
    }
    
    // Calculate surface speed
    // For metric (mm): surface speed in m/min = RPM * diameter * π / 1000
    // For imperial (inches): surface speed in ft/min = RPM * diameter * π / 12
    
    float surface_speed = 0.0f;
    if (settings.use_inches) {
        // Calculate surface speed in feet per minute for inches
        surface_speed = current_rpm * settings.workpiece_diameter * 3.14159f / 12.0f;
    } else {
        // Calculate surface speed in meters per minute for mm
        surface_speed = current_rpm * settings.workpiece_diameter * 3.14159f / 1000.0f;
    }
    
    return surface_speed;
}

// ==================== Main ===================
int main() 
{
    uint32_t last_display_update = 0;
    uint32_t last_timeout_check = 0;
    
    // Initialize everything
    setup();
    
    // Main loop
    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Process button presses
        process_buttons();
        
        // Calculate RPM when new data is available
        if (rpm_data_ready) {
            calculate_rpm();
            rpm_data_ready = false;
        }
        
        // Periodically check for RPM timeout (faster checks for more responsive zero)
        if (current_time - last_timeout_check >= RPM_TIMEOUT_CHECK_MS) {
            calculate_rpm(); // Will reset RPM to zero if no pulses within timeout period
            last_timeout_check = current_time;
        }
        
        // Update display periodically (faster updates for more responsive display)
        if (current_time - last_display_update >= DISPLAY_UPDATE_INTERVAL) {
            myOLED.OLEDclearBuffer();
            
            if (current_menu == MENU_NONE) {
                display_rpm();
            } else {
                display_menu();
                
                // Check for menu timeout
                if (current_time - menu_last_activity >= MENU_TIMEOUT) {
                    current_menu = MENU_NONE;
                    save_settings();
                }
            }
            
            myOLED.OLEDupdate();
            last_display_update = current_time;
        }
        
        // Small delay to avoid hogging CPU
        sleep_ms(5); // Reduced from 10ms for more responsive system
    }
}

// GPIO interrupt handler for hall sensor and buttons
void gpio_callback(uint gpio, uint32_t events) {
    // Process hall sensor interrupts
    if (gpio == HALL_SENSOR_PIN) {
        pulse_count = pulse_count + 1; // Avoid ++ on volatile
        
        // Calculate time between pulses for RPM calculation
        last_pulse_time = current_pulse_time;
        current_pulse_time = time_us_64();
        
        if (last_pulse_time > 0) {
            uint64_t interval = current_pulse_time - last_pulse_time;
            
            // Add to running average calculation
            pulse_interval_sum += interval;
            pulse_intervals_count = pulse_intervals_count + 1; // Avoid ++ on volatile
            
            // Signal that we can calculate RPM after each pulse for faster response
            // This is more responsive than waiting for settings.pulses_per_rev pulses
            rpm_data_ready = true;
        }
    }
    
    // Process button interrupts
    uint64_t current_time = time_us_64();
    if (gpio == BUTTON_UP_PIN) {
        if (events & GPIO_IRQ_EDGE_FALL) {  // Button pressed
            button_up_pressed = true;
            button_up_press_time = current_time;
            button_up_long_press = false;
        } else if (events & GPIO_IRQ_EDGE_RISE) {  // Button released
            button_up_pressed = false;
        }
    } else if (gpio == BUTTON_DOWN_PIN) {
        if (events & GPIO_IRQ_EDGE_FALL) {  // Button pressed
            button_down_pressed = true;
            button_down_press_time = current_time;
            button_down_long_press = false;
        } else if (events & GPIO_IRQ_EDGE_RISE) {  // Button released
            button_down_pressed = false;
        }
    }
}

// ===================== Function Space =====================
void setup() 
{
    // Initialize stdio
    stdio_init_all();
    busy_wait_ms(500);
    
    // Initialize OLED
    while(myOLED.OLEDbegin(SSD1306::SSD1306_ADDR, i2c1, I2C_Speed, I2C_GPIO_DATA, I2C_GPIO_CLK) != DisplayRet::Success)
    {
        printf("Setup ERROR: Failed to initialize OLED!\r\n");
        busy_wait_ms(1500);
    }
    
    if (myOLED.OLEDSetBufferPtr(myOLEDwidth, myOLEDheight, screenBuffer) != DisplayRet::Success)
    {
        printf("Setup ERROR: OLEDSetBufferPtr Failed!\r\n");
        while(1) {
            busy_wait_ms(1000);
        }
    }
    
    // Load settings from flash
    load_settings();
    
    // Initialize hall sensor GPIO as input with pull-up
    gpio_init(HALL_SENSOR_PIN);
    gpio_set_dir(HALL_SENSOR_PIN, GPIO_IN);
    gpio_pull_up(HALL_SENSOR_PIN);
    
    // Initialize buttons as inputs with pull-ups
    gpio_init(BUTTON_UP_PIN);
    gpio_set_dir(BUTTON_UP_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_UP_PIN);
    
    gpio_init(BUTTON_DOWN_PIN);
    gpio_set_dir(BUTTON_DOWN_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_DOWN_PIN);
    
    // Configure GPIO interrupts for hall sensor and buttons
    gpio_set_irq_enabled_with_callback(HALL_SENSOR_PIN, GPIO_IRQ_EDGE_FALL, true, &gpio_callback);
    gpio_set_irq_enabled(BUTTON_UP_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BUTTON_DOWN_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

    // Display welcome message
    myOLED.setFont(pFontWide);
    myOLED.setCursor(0, 0);
    myOLED.print("LATHE TACH");
	myOLED.setFont(pFontDefault);
    myOLED.setCursor(0, 16);
    myOLED.print("Pulses: ");
    myOLED.print(settings.pulses_per_rev);
    myOLED.setCursor(0, 24);
    myOLED.print("Ratio: ");
    myOLED.print(settings.gear_ratio, 2);
    myOLED.OLEDupdate();
    busy_wait_ms(2000);
    myOLED.OLEDclearBuffer();
}

// Process button presses for UI control
void process_buttons() {
    uint64_t current_time = time_us_64();
    uint32_t ms_time = to_ms_since_boot(get_absolute_time());
    
    // Check for long presses
    if (button_up_pressed && !button_up_long_press && 
        (current_time - button_up_press_time >= LONG_PRESS_TIME * 1000)) {
        button_up_long_press = true;
        
        // Long press UP button enters/cycles through menu
        if (current_menu == MENU_NONE) {
            current_menu = MENU_PULSES;
        } else if (current_menu == MENU_PULSES) {
            current_menu = MENU_RATIO;
        } else if (current_menu == MENU_RATIO) {
            current_menu = MENU_DECIMAL;
        } else if (current_menu == MENU_DECIMAL) {
            current_menu = MENU_FILTER;
        } else if (current_menu == MENU_FILTER) {
            current_menu = MENU_DIAMETER;
        } else if (current_menu == MENU_DIAMETER) {
            current_menu = MENU_UNITS;
        } else {
            current_menu = MENU_NONE;
            save_settings();
        }
        
        menu_last_activity = ms_time;
    }
    
    if (button_down_pressed && !button_down_long_press && 
        (current_time - button_down_press_time >= LONG_PRESS_TIME * 1000)) {
        button_down_long_press = true;
        
        // Long press DOWN button exits menu or toggles diameter adjust mode
        if (current_menu != MENU_NONE) {
            current_menu = MENU_NONE;
            save_settings();
        }
    }
    
    // Handle short presses (released after press but before long press)
    static bool button_up_handled = false;
    static bool button_down_handled = false;
    
    if (!button_up_pressed && !button_up_handled) {
        if (current_time - button_up_press_time < LONG_PRESS_TIME * 1000) {
            // Short press UP button - increment value in current menu or adjust diameter
            if (current_menu == MENU_NONE) {
                // Direct diameter adjustment from main screen
                if (settings.use_inches) {
                    settings.workpiece_diameter += 0.125f;  // 1/8" increments
                    if (settings.workpiece_diameter > 12.0f) {  // Max 12 inches
                        settings.workpiece_diameter = 0.125f;
                    }
                } else {
                    settings.workpiece_diameter += 1.0f;  // 1mm increments
                    if (settings.workpiece_diameter > 300.0f) {  // Max 300mm
                        settings.workpiece_diameter = 1.0f;
                    }
                }
                save_settings(); // Save immediately when changing diameter
            } else if (current_menu == MENU_PULSES) {
                settings.pulses_per_rev++;
                if (settings.pulses_per_rev > 66) {  // Set reasonable max
                    settings.pulses_per_rev = 1;
                }
            } else if (current_menu == MENU_RATIO) {
                settings.gear_ratio += 0.1f;
                if (settings.gear_ratio > 10.0f) {  // Set reasonable max
                    settings.gear_ratio = 0.1f;
                }
            } else if (current_menu == MENU_DECIMAL) {
                settings.show_decimal = !settings.show_decimal;
            } else if (current_menu == MENU_FILTER) {
                settings.filter_strength++;
                if (settings.filter_strength > 10) {  // 0-10 range
                    settings.filter_strength = 0;
                }
            } else if (current_menu == MENU_DIAMETER) {
                // Increment by different amounts based on units
                if (settings.use_inches) {
                    settings.workpiece_diameter += 0.125f;  // 1/8" increments
                    if (settings.workpiece_diameter > 12.0f) {  // Max 12 inches
                        settings.workpiece_diameter = 0.125f;
                    }
                } else {
                    settings.workpiece_diameter += 1.0f;  // 1mm increments
                    if (settings.workpiece_diameter > 300.0f) {  // Max 300mm
                        settings.workpiece_diameter = 1.0f;
                    }
                }
            } else if (current_menu == MENU_UNITS) {
                settings.use_inches = !settings.use_inches;
                
                // Convert diameter value when changing units
                if (settings.use_inches) {
                    // Convert mm to inches
                    settings.workpiece_diameter /= 25.4f;
                    // Round to nearest 1/8"
                    settings.workpiece_diameter = roundf(settings.workpiece_diameter * 8.0f) / 8.0f;
                    if (settings.workpiece_diameter < 0.125f) settings.workpiece_diameter = 0.125f;
                } else {
                    // Convert inches to mm
                    settings.workpiece_diameter *= 25.4f;
                    // Round to nearest mm
                    settings.workpiece_diameter = roundf(settings.workpiece_diameter);
                    if (settings.workpiece_diameter < 1.0f) settings.workpiece_diameter = 1.0f;
                }
            }
            
            menu_last_activity = ms_time;
        }
        button_up_handled = true;
    } else if (button_up_pressed) {
        button_up_handled = false;
    }
    
    if (!button_down_pressed && !button_down_handled) {
        if (current_time - button_down_press_time < LONG_PRESS_TIME * 1000) {
            // Short press DOWN button - decrement value in current menu or adjust diameter
            if (current_menu == MENU_NONE) {
                // Direct diameter adjustment from main screen
                if (settings.use_inches) {
                    if (settings.workpiece_diameter <= 0.125f) {
                        settings.workpiece_diameter = 12.0f;  // Max 12 inches
                    } else {
                        settings.workpiece_diameter -= 0.125f;  // 1/8" increments
                    }
                } else {
                    if (settings.workpiece_diameter <= 1.0f) {
                        settings.workpiece_diameter = 300.0f;  // Max 300mm
                    } else {
                        settings.workpiece_diameter -= 1.0f;  // 1mm increments
                    }
                }
                save_settings(); // Save immediately when changing diameter
            } else if (current_menu == MENU_PULSES) {
                if (settings.pulses_per_rev <= 1) {
                    settings.pulses_per_rev = 66;
                } else {
                    settings.pulses_per_rev--;
                }
            } else if (current_menu == MENU_RATIO) {
                settings.gear_ratio -= 0.1f;
                if (settings.gear_ratio < 0.1f) {
                    settings.gear_ratio = 10.0f;
                }
            } else if (current_menu == MENU_DECIMAL) {
                settings.show_decimal = !settings.show_decimal;
            } else if (current_menu == MENU_FILTER) {
                if (settings.filter_strength <= 0) {
                    settings.filter_strength = 10;
                } else {
                    settings.filter_strength--;
                }
            } else if (current_menu == MENU_DIAMETER) {
                // Decrement by different amounts based on units
                if (settings.use_inches) {
                    if (settings.workpiece_diameter <= 0.125f) {
                        settings.workpiece_diameter = 12.0f;  // Max 12 inches
                    } else {
                        settings.workpiece_diameter -= 0.125f;  // 1/8" increments
                    }
                } else {
                    if (settings.workpiece_diameter <= 1.0f) {
                        settings.workpiece_diameter = 300.0f;  // Max 300mm
                    } else {
                        settings.workpiece_diameter -= 1.0f;  // 1mm increments
                    }
                }
            } else if (current_menu == MENU_UNITS) {
                settings.use_inches = !settings.use_inches;
                
                // Convert diameter value when changing units
                if (settings.use_inches) {
                    // Convert mm to inches
                    settings.workpiece_diameter /= 25.4f;
                    // Round to nearest 1/8"
                    settings.workpiece_diameter = roundf(settings.workpiece_diameter * 8.0f) / 8.0f;
                    if (settings.workpiece_diameter < 0.125f) settings.workpiece_diameter = 0.125f;
                } else {
                    // Convert inches to mm
                    settings.workpiece_diameter *= 25.4f;
                    // Round to nearest mm
                    settings.workpiece_diameter = roundf(settings.workpiece_diameter);
                    if (settings.workpiece_diameter < 1.0f) settings.workpiece_diameter = 1.0f;
                }
            }
            
            menu_last_activity = ms_time;
        }
        button_down_handled = true;
    } else if (button_down_pressed) {
        button_down_handled = false;
    }
}

// Calculate RPM based on pulse timing
void calculate_rpm() {
    // Timeout detection - if no pulses for a set period, set RPM to 0
    uint64_t current_time = time_us_64();
    
    // Store previous RPM for rate-of-change detection
    float previous_rpm = current_rpm;
    
    // If no pulses received for timeout period, set RPM to zero
    if (current_time - current_pulse_time > RPM_TIMEOUT_MS * 1000) {
        current_rpm = 0;
        filtered_rpm = 0;

        // Reset for next calculation
        pulse_interval_sum = 0;
        pulse_intervals_count = 0;
        return; // No need to continue calculation if we've timed out
    }
    
    // Process even if we have just one pulse interval (for faster response)
    if (pulse_intervals_count > 0) {
        // Calculate average pulse interval in microseconds
        uint64_t avg_interval = pulse_interval_sum / pulse_intervals_count;

        // Avoid division by zero
        if (avg_interval > 0) {
            // Convert to RPM: 60 seconds * 1,000,000 microseconds / average interval time / pulses per rev
            // Apply gear ratio adjustment
            float new_rpm = (60.0f * 1000000.0f) / avg_interval / settings.pulses_per_rev * settings.gear_ratio;
            
            // Detect rapid deceleration (RPM dropping quickly)
            bool rapid_deceleration = (previous_rpm > 10.0f && new_rpm < previous_rpm * 0.7f);
            
            // Apply low-pass filter if enabled and not rapidly decelerating
            if (settings.filter_strength > 0 && !rapid_deceleration) {
                // Calculate filter coefficient (0-1 range)
                // Higher filter_strength = more filtering (smoother, slower response)
                float filter_alpha = settings.filter_strength / 10.0f;
                
                if (filtered_rpm == 0) {
                    // Initialize filter with first reading
                    filtered_rpm = new_rpm;
                } else {
                    // Apply exponential moving average filter
                    // filtered = prev * alpha + new * (1-alpha)
                    filtered_rpm = (filtered_rpm * filter_alpha) + (new_rpm * (1.0f - filter_alpha));
                }
                
                // Use filtered value
                current_rpm = filtered_rpm;
            } else {
                // No filtering or rapid deceleration - respond quickly
                current_rpm = new_rpm;
                filtered_rpm = new_rpm;
            }
        }
        
        // Reset for next calculation
        pulse_interval_sum = 0;
        pulse_intervals_count = 0;
    }
}

// Display the current RPM
void display_rpm() {
    // Use large segment display font for RPM display
    myOLED.setFont(pFontSixteenSeg);
    myOLED.setInvertFont(false);
    
    // Display RPM value
    if (current_rpm < 10000) {
        char buffer[10];
        
        if (settings.show_decimal && current_rpm < 100) {
            // Format with one decimal place for low RPMs
            sprintf(buffer, "%.1f", current_rpm);
        } else {
            // Format as integer for higher RPMs
            sprintf(buffer, "%d", (int)current_rpm);
        }
        
        // Calculate the width of the text to position it properly
        // For pFontSixteenSeg, each character is approximately 32 pixels wide
        // OLED width is 128 pixels
        int char_width = 32;
        int text_width = strlen(buffer) * char_width;
        int x_position = 0;
        
        // Right justify if less than 1000, otherwise left justify
        if (current_rpm < 1000) {
            // Right justify with some margin
            x_position = myOLEDwidth - text_width - 10;
            if (x_position < 0) x_position = 0;
        }
        
        // Set cursor position and display the value
        myOLED.setCursor(x_position, 0);
        myOLED.print(buffer);
    } else {
        // If over 10000, just display "HIGH"
        myOLED.setFont(pFontDefault);
        myOLED.setCursor(40, 20);
        myOLED.print("HIGH RPM");
    }
    
    // Show "RPM" label
    myOLED.setFont(pFontDefault);
    
    // Show surface speed in small font at bottom
    myOLED.setCursor(0, 56);
    
    // Calculate and display surface speed
    float surface_speed = calculate_surface_speed();
    
    // Show diameter and surface speed
    myOLED.print("D:");
    
    if (settings.use_inches) {
        myOLED.print(settings.workpiece_diameter);
        myOLED.print("\" ");
        
        // Surface speed in ft/min for inch units
        myOLED.print("SFM:");
        if (surface_speed < 10) {
            myOLED.print(surface_speed, 1); // One decimal place for small values
        } else {
            myOLED.print((int)surface_speed); // Integer for larger values
        }
    } else {
        myOLED.print(settings.workpiece_diameter);
        myOLED.print("mm ");
        
        // Surface speed in m/min for metric units
        myOLED.print("m/min:");
        if (surface_speed < 10) {
            myOLED.print(surface_speed, 1); // One decimal place for small values
        } else {
            myOLED.print((int)surface_speed); // Integer for larger values
        }
    }
}

// Display the settings menu
void display_menu() {
    myOLED.setFont(pFontDefault);
    myOLED.setCursor(0, 0);
    myOLED.print("SETTINGS");
    
    switch (current_menu) {
        case MENU_PULSES:
            myOLED.setCursor(0, 16);
            myOLED.print("> Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("  Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("  Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("  Filter: ");
            myOLED.print(settings.filter_strength);
            break;
            
        case MENU_RATIO:
            myOLED.setCursor(0, 16);
            myOLED.print("  Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("> Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("  Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("  Filter: ");
            myOLED.print(settings.filter_strength);
            break;
            
        case MENU_DECIMAL:
            myOLED.setCursor(0, 16);
            myOLED.print("  Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("  Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("> Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("  Filter: ");
            myOLED.print(settings.filter_strength);
            break;
            
        case MENU_FILTER:
            myOLED.setCursor(0, 16);
            myOLED.print("  Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("  Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("  Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("> Filter: ");
            myOLED.print(settings.filter_strength);
            break;
            
        case MENU_DIAMETER:
            myOLED.setCursor(0, 16);
            myOLED.print("  Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("  Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("  Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("> Diameter: ");
            if (settings.use_inches) {
                myOLED.print(settings.workpiece_diameter);
                myOLED.print("\"");
            } else {
                myOLED.print(settings.workpiece_diameter);
                myOLED.print("mm");
            }
            break;

        case MENU_UNITS:
            myOLED.setCursor(0, 16);
            myOLED.print("  Pulses per rev: ");
            myOLED.print(settings.pulses_per_rev);
            myOLED.setCursor(0, 24);
            myOLED.print("  Gear ratio: ");
            myOLED.print(settings.gear_ratio, 1);
            myOLED.setCursor(0, 32);
            myOLED.print("  Show decimal: ");
            myOLED.print(settings.show_decimal ? "Yes" : "No");
            myOLED.setCursor(0, 40);
            myOLED.print("> Units: ");
            myOLED.print(settings.use_inches ? "Inches" : "mm");
            break;
            
        default:
            break;
    }
    
    // Instructions at the bottom
    myOLED.setCursor(0, 56);
    myOLED.print("Short: +/- Long: Next/Exit");
}

// Load settings from flash
void load_settings() {
    tach_settings_t *flash_settings = (tach_settings_t *)flash_target_contents;
    
    // Check if settings in flash are valid using magic number
    if (flash_settings->magic_number == SETTINGS_MAGIC) {
        settings = *flash_settings;
    } else {
        // Use defaults
        settings.magic_number = SETTINGS_MAGIC;
        settings.pulses_per_rev = 1;
        settings.gear_ratio = 1.0f;
        settings.show_decimal = true;
        settings.filter_strength = 3; // Default medium filtering
        settings.workpiece_diameter = 25.0f; // Default 25mm (about 1 inch)
        settings.use_inches = false; // Default to metric
        
        // Save default settings
        save_settings();
    }
}

// Save settings to flash
void save_settings() {
    // Data must be a multiple of 256 bytes for flash operations
    uint8_t data[FLASH_PAGE_SIZE];
    
    // Copy settings to buffer
    memcpy(data, &settings, sizeof(settings));
    
    // Fill rest with 0xFF (erased flash state)
    for (size_t i = sizeof(settings); i < FLASH_PAGE_SIZE; ++i) {
        data[i] = 0xFF;
    }
    
    // Disable interrupts for flash operation
    uint32_t ints = save_and_disable_interrupts();
    
    // Erase flash sector
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    
    // Program flash page
    flash_range_program(FLASH_TARGET_OFFSET, data, FLASH_PAGE_SIZE);
    
    // Restore interrupts
    restore_interrupts(ints);
}