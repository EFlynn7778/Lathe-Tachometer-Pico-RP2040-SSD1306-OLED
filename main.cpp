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
#define DISPLAY_UPDATE_INTERVAL 250 // Update display every 250ms
#define MENU_TIMEOUT 10000          // Exit menu after 10 seconds of inactivity

// I2C settings
const uint16_t I2C_Speed = 1000;
const uint8_t I2C_GPIO_CLK = 7;
const uint8_t I2C_GPIO_DATA = 6;

// Hall sensor and button settings
const uint8_t HALL_SENSOR_PIN = 29;    // GPIO pin for hall sensor input
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
} tach_settings_t;

#define SETTINGS_MAGIC 0xABCD1234 // Magic number to validate settings

// Menu states
enum MenuState {
    MENU_NONE,
    MENU_PULSES,
    MENU_RATIO,
    MENU_DECIMAL,
    MENU_FILTER
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

// ==================== Main ===================
int main() 
{
    uint32_t last_display_update = 0;
    
    // Initialize everything
    setup();
    
    // Main loop
    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Process button presses
        process_buttons();
        
        // Calculate RPM periodically
        if (rpm_data_ready) {
            calculate_rpm();
            rpm_data_ready = false;
        }
        
        // Update display periodically
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
        sleep_ms(10);
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
            
            // When we have enough samples, signal that we can calculate RPM
            if (pulse_intervals_count >= settings.pulses_per_rev) {
                rpm_data_ready = true;
            }
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
        } else {
            current_menu = MENU_NONE;
            save_settings();
        }
        
        menu_last_activity = ms_time;
    }
    
    if (button_down_pressed && !button_down_long_press && 
        (current_time - button_down_press_time >= LONG_PRESS_TIME * 1000)) {
        button_down_long_press = true;
        
        // Long press DOWN button exits menu
        if (current_menu != MENU_NONE) {
            current_menu = MENU_NONE;
            save_settings();
        }
        
        menu_last_activity = ms_time;
    }
    
    // Handle short presses (released after press but before long press)
    static bool button_up_handled = false;
    static bool button_down_handled = false;
    
    if (!button_up_pressed && !button_up_handled) {
        if (current_time - button_up_press_time < LONG_PRESS_TIME * 1000) {
            // Short press UP button - increment value in current menu
            if (current_menu == MENU_PULSES) {
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
            }
            
            menu_last_activity = ms_time;
        }
        button_up_handled = true;
    } else if (button_up_pressed) {
        button_up_handled = false;
    }
    
    if (!button_down_pressed && !button_down_handled) {
        if (current_time - button_down_press_time < LONG_PRESS_TIME * 1000) {
            // Short press DOWN button - decrement value in current menu
            if (current_menu == MENU_PULSES) {
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
    if (pulse_intervals_count > 0) {
        // Calculate average pulse interval in microseconds
        uint64_t avg_interval = pulse_interval_sum / pulse_intervals_count;
        
        // Avoid division by zero
        if (avg_interval > 0) {
            // Convert to RPM: 60 seconds * 1,000,000 microseconds / average interval time / pulses per rev
            // Apply gear ratio adjustment
            float new_rpm = (60.0f * 1000000.0f) / avg_interval / settings.pulses_per_rev * settings.gear_ratio;
            
            // Apply low-pass filter if enabled
            if (settings.filter_strength > 0) {
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
                // No filtering
                current_rpm = new_rpm;
                filtered_rpm = new_rpm;
            }
        }
        
        // Reset for next calculation
        pulse_interval_sum = 0;
        pulse_intervals_count = 0;
        
        // Timeout detection - if no pulses for 3 seconds, set RPM to 0
        uint64_t current_time = time_us_64();
        if (current_time - current_pulse_time > 3000000) {
            current_rpm = 0;
            filtered_rpm = 0;
        }
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
    myOLED.setFont(pFontWide);
    myOLED.setCursor(90, 50);
    myOLED.print("RPM");
    
    // Show pulse count in small font at bottom
	myOLED.setFont(pFontDefault);
    myOLED.setCursor(0, 56);
    myOLED.print("P:");
    myOLED.print(settings.pulses_per_rev);
    myOLED.print(" R:");
    myOLED.print(settings.gear_ratio, 1);
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