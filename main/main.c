#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/rtc_io.h"

static const char *TAG = "ARCADE";


// ============================================================================
// HARDWARE PIN DEFINITIONS (SEEED STUDIO XIAO ESP32-C6)
// ============================================================================
#define PIN_NUM_MOSI    GPIO_NUM_21 // D3  (SDA)
#define PIN_NUM_CLK     GPIO_NUM_22 // D4  (SCL)
#define PIN_NUM_CS      GPIO_NUM_20 // D9  (CS)
#define PIN_NUM_DC      GPIO_NUM_19 // D8  (DC)
#define PIN_NUM_RST     GPIO_NUM_18 // D10 (RST)
// #define PIN_NUM_BCKL GPIO_NUM_23  // No backlight pin on this board
#define PIN_BUZZER      GPIO_NUM_23 // D5 (Piezo buzzer - LEDC PWM)

#define BTN_RIGHT       GPIO_NUM_0  // D0
#define BTN_LEFT        GPIO_NUM_1  // D1
#define BTN_ROTATE      GPIO_NUM_2  // D2
#define BTN_DROP        GPIO_NUM_16 // D6

#define PIN_TFT_PWR     GPIO_NUM_17 // D7 - MOSFET gate: HIGH=TFT power ON, LOW=TFT power OFF

// Inactivity timeout before entering deep sleep (5 minutes)
#define INACTIVITY_TIMEOUT_US   300000000ULL

// ============================================================================
// DISPLAY CONFIGURATION
// ============================================================================
#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   320

// RGB565 Colors
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_DARKGRAY  0x2104
#define COLOR_LIGHTGRAY 0x8410
#define COLOR_CYAN      0x07FF
#define COLOR_BLUE      0x001F
#define COLOR_ORANGE    0xFD20
#define COLOR_YELLOW    0xFFE0
#define COLOR_GREEN     0x07E0
#define COLOR_PURPLE    0x780F
#define COLOR_RED       0xF800
#define COLOR_MAGENTA   0xF81F

static spi_device_handle_t spi;

typedef enum {
    STATE_MENU = 0,
    STATE_TETRIS,
    STATE_INVADERS,
    STATE_BREAKOUT,
    STATE_PONG,
    STATE_SNAKE,
    STATE_FLAPPY,
    STATE_RACER
} GameMode;

static GameMode current_game = STATE_MENU;

// ============================================================================
// NON-BLOCKING INPUT & EVENT SYSTEM
// ============================================================================
typedef struct {
    bool left_pressed;
    bool right_pressed;
    bool rotate_short_click;
    bool drop_short_click;
    bool rotate_long_press;
    bool drop_long_press;
} ButtonEvents;

#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_0 // GPIO0 (D0 / BTN_RIGHT)

static adc_oneshot_unit_handle_t battery_adc_handle = NULL;

static inline bool is_right_button_pressed(void) {
    if (!battery_adc_handle) return false;
    int raw = 4095;
    if (adc_oneshot_read(battery_adc_handle, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
        // When pressed, D0 is shorted to GND (raw ADC near 0, typically < 100).
        // When unpressed, D0 is held at VBAT/2 (~1.5V - 2.1V, raw ADC ~1800 - 2600).
        // Threshold of 600 (~0.48V) cleanly discriminates even with battery near empty.
        return (raw < 600);
    }
    return false;
}

static ButtonEvents get_button_events(void) {
    ButtonEvents ev = {0};

    ev.left_pressed  = (gpio_get_level(BTN_LEFT) == 0);
    ev.right_pressed = is_right_button_pressed();

    // ROTATE timing tracking
    static int64_t rot_start = 0;
    static bool rot_was_down = false;
    static bool rot_long_fired = false;

    if (gpio_get_level(BTN_ROTATE) == 0) {
        if (!rot_was_down) {
            rot_was_down = true;
            rot_start = esp_timer_get_time();
            rot_long_fired = false;
        } else if (!rot_long_fired && (esp_timer_get_time() - rot_start) >= 1000000ULL) {
            rot_long_fired = true;
            ev.rotate_long_press = true;
        }
    } else {
        if (rot_was_down) {
            if (!rot_long_fired) ev.rotate_short_click = true;
            rot_was_down = false;
        }
    }

    // DROP timing tracking
    static int64_t drop_start = 0;
    static bool drop_was_down = false;
    static bool drop_long_fired = false;

    if (gpio_get_level(BTN_DROP) == 0) {
        if (!drop_was_down) {
            drop_was_down = true;
            drop_start = esp_timer_get_time();
            drop_long_fired = false;
        } else if (!drop_long_fired && (esp_timer_get_time() - drop_start) >= 1000000ULL) {
            drop_long_fired = true;
            ev.drop_long_press = true;
        }
    } else {
        if (drop_was_down) {
            if (!drop_long_fired) ev.drop_short_click = true;
            drop_was_down = false;
        }
    }

    return ev;
}

// ============================================================================
// AUDIO / BUZZER DRIVER (LEDC PWM on D5 / GPIO23)
// ============================================================================
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_10_BIT
#define LEDC_DUTY_50            512

static void buzzer_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_BUZZER,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void sound_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, freq_hz);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY_50);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void sfx_move(void)        { sound_tone(440, 12); }
static void sfx_rotate(void)      { sound_tone(750, 18); }
static void sfx_drop(void)        { sound_tone(220, 25); }
static void sfx_shoot(void)       { sound_tone(950, 20); }
static void sfx_hit(void)         { sound_tone(350, 30); sound_tone(150, 30); }
static void sfx_menu_select(void) { sound_tone(523, 35); sound_tone(659, 35); }
static void sfx_powerdown(void)   { sound_tone(600, 60); sound_tone(400, 60); sound_tone(250, 120); }

static void sfx_line_clear(void) {
    sound_tone(523, 35); sound_tone(659, 35); sound_tone(784, 35); sound_tone(1046, 50);
}

static void sfx_game_over(void) {
    sound_tone(440, 90); sound_tone(349, 90); sound_tone(293, 90); sound_tone(220, 200);
}

// ============================================================================
// ST7789 TFT DISPLAY DRIVER
// ============================================================================
static void st7789_cmd(uint8_t cmd) {
    gpio_set_level(PIN_NUM_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(spi, &t);
}

static void st7789_data(const uint8_t *data, int len) {
    if (len == 0) return;
    gpio_set_level(PIN_NUM_DC, 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(spi, &t);
}

static void st7789_data_u8(uint8_t d) { st7789_data(&d, 1); }

static void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    st7789_cmd(0x2A);
    uint8_t data_x[] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    st7789_data(data_x, 4);

    st7789_cmd(0x2B);
    uint8_t data_y[] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    st7789_data(data_y, 4);

    st7789_cmd(0x2C);
}

static uint8_t *frame_buffer = NULL;

static void init_framebuffer(void) {
    if (!frame_buffer) {
        frame_buffer = (uint8_t *)heap_caps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (frame_buffer) {
            memset(frame_buffer, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
        }
    }
}

static void fb_present(void) {
    if (!frame_buffer) return;
    st7789_set_window(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
    gpio_set_level(PIN_NUM_DC, 1);

    int total_bytes = SCREEN_WIDTH * SCREEN_HEIGHT * 2;
    int offset = 0;
    #define DMA_CHUNK_SIZE 4096

    while (total_bytes > 0) {
        int len = (total_bytes > DMA_CHUNK_SIZE) ? DMA_CHUNK_SIZE : total_bytes;
        spi_transaction_t t = {
            .length = len * 8,
            .tx_buffer = frame_buffer + offset
        };
        spi_device_polling_transmit(spi, &t);
        offset += len;
        total_bytes -= len;
    }
}

// ============================================================================
// POWER MANAGEMENT - MOSFET-GATED TFT DEEP SLEEP
// ============================================================================
static void enter_power_down_deep_sleep(void) {
    sfx_powerdown();

    // 1. ST7789 software shutdown sequence
    st7789_cmd(0x28); // DISPOFF
    vTaskDelay(pdMS_TO_TICKS(50));
    st7789_cmd(0x10); // SLPIN
    vTaskDelay(pdMS_TO_TICKS(120));

    // 2. Kill TFT power via MOSFET (D7 LOW).
    //    With VCC cut, there is NO backfeed path - no need to touch SPI pins.
    gpio_set_level(PIN_TFT_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_hold_en((gpio_num_t)PIN_TFT_PWR); // Hold MOSFET off during sleep

    // 3. Hold RST LOW as belt-and-suspenders (ST7789 in hardware reset)
    gpio_set_level(PIN_NUM_RST, 0);
    gpio_hold_en((gpio_num_t)PIN_NUM_RST);

    // 4. Ensure LP-domain pull-ups on wakeup buttons survive deep sleep
    // Note: BTN_RIGHT (GPIO0) shares the battery divider. Do NOT enable internal pullup on it!
    gpio_pullup_dis(BTN_RIGHT);
    gpio_pulldown_dis(BTN_RIGHT);
    rtc_gpio_pullup_dis(BTN_RIGHT);
    rtc_gpio_pulldown_dis(BTN_RIGHT);
    gpio_sleep_set_pull_mode(BTN_RIGHT, GPIO_FLOATING);

    // Pull-ups only for buttons that do not share the battery divider
    gpio_pullup_en((gpio_num_t)BTN_LEFT);
    gpio_pullup_en((gpio_num_t)BTN_ROTATE);

    // 5. Wait for DROP to be released, then wait for wakeup buttons to be
    //    clearly HIGH. Wakeup is level-triggered LOW — any LOW pin at sleep
    //    entry causes an immediate spurious wakeup.
    while (gpio_get_level(BTN_DROP)   == 0) vTaskDelay(pdMS_TO_TICKS(20));
    while (gpio_get_level(BTN_LEFT)   == 0 ||
           gpio_get_level(BTN_ROTATE) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(500)); // Extra debounce — hand fully away

    // 6. Arm wakeup on BTN_LEFT and BTN_ROTATE and enter deep sleep
    // Note: Do NOT put BTN_RIGHT in wakeup mask, because ESP-IDF sleep driver
    // automatically turns on internal pull-up and pad hold on wakeup pins!
    uint64_t mask = (1ULL << BTN_LEFT) | (1ULL << BTN_ROTATE);
    esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

static void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (!frame_buffer) return;
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;

    uint8_t ch = (uint8_t)(color >> 8);
    uint8_t cl = (uint8_t)(color & 0xFF);

    for (int r = 0; r < h; r++) {
        int row_offset = ((y + r) * SCREEN_WIDTH + x) * 2;
        for (int c = 0; c < w; c++) {
            frame_buffer[row_offset + c * 2]     = ch;
            frame_buffer[row_offset + c * 2 + 1] = cl;
        }
    }
}


static void draw_bitmap(int x, int y, int w, int h, const uint8_t *bmp, uint16_t color) {
    for (int r = 0; r < h; r++) {
        uint8_t line = bmp[r];
        for (int c = 0; c < w; c++) {
            if (line & (1 << (7 - c))) {
                st7789_fill_rect(x + c * 2, y + r * 2, 2, 2, color);
            }
        }
    }
}

static void st7789_init(void) {
    init_framebuffer();

    // TFT power is already ON (PIN_TFT_PWR driven HIGH in app_main before we get here)
    // Just assert hardware reset to ensure a clean ST7789 startup state
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    st7789_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(120));
    st7789_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));

    st7789_cmd(0x3A); st7789_data_u8(0x55);
    st7789_cmd(0x36); st7789_data_u8(0xC0); // 240x320 Portrait (180 deg flipped)
    st7789_cmd(0x21); // Display Inversion ON
    st7789_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    st7789_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));

    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    fb_present();
}


// ============================================================================
// 5x7 ASCII FONT RENDERER
// ============================================================================
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // Space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}  // Z
};

static void draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg, uint8_t scale) {
    if (c < ' ' || c > 'Z') c = ' ';
    uint8_t idx = c - ' ';

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = font5x7[idx][col];
        for (uint8_t row = 0; row < 7; row++) {
            uint16_t px_color = (line & (1 << row)) ? color : bg;
            st7789_fill_rect(x + col * scale, y + row * scale, scale, scale, px_color);
        }
    }
    st7789_fill_rect(x + 5 * scale, y, scale, 7 * scale, bg);
}

static void draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
    while (*str) {
        draw_char(x, y, *str, color, bg, scale);
        x += 6 * scale;
        str++;
    }
}

static void draw_centered_string(uint16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t scale) {
    int len = strlen(str);
    int width = len * 6 * scale;
    int x = (SCREEN_WIDTH - width) / 2;
    if (x < 0) x = 0;
    draw_string(x, y, str, color, bg, scale);
}

// Modern Retro Arcade Modal Dialog for Game Over / Victory / Crashes
static void draw_game_over_modal(const char *title, uint32_t score, int level, uint16_t accent_color) {
    int mw = 210;
    int mh = 140;
    int mx = (SCREEN_WIDTH - mw) / 2; // 15
    int my = (SCREEN_HEIGHT - mh) / 2; // 90

    // 3D Drop Shadow
    st7789_fill_rect(mx + 6, my + 6, mw, mh, 0x1082); // Dark Charcoal shadow

    // Outer Box Fill & Double Border Frame
    st7789_fill_rect(mx, my, mw, mh, COLOR_BLACK);
    st7789_fill_rect(mx, my, mw, 3, accent_color);
    st7789_fill_rect(mx, my + mh - 3, mw, 3, accent_color);
    st7789_fill_rect(mx, my, 3, mh, accent_color);
    st7789_fill_rect(mx + mw - 3, my, 3, mh, accent_color);

    // Inner Fine Border Line
    st7789_fill_rect(mx + 5, my + 5, mw - 10, 1, accent_color);
    st7789_fill_rect(mx + 5, my + mh - 6, mw - 10, 1, accent_color);
    st7789_fill_rect(mx + 5, my + 5, 1, mh - 10, accent_color);
    st7789_fill_rect(mx + mw - 6, my + 5, 1, mh - 10, accent_color);

    // Header Title (Centered)
    draw_centered_string(my + 15, title, accent_color, COLOR_BLACK, 2);

    // Divider Line
    st7789_fill_rect(mx + 15, my + 42, mw - 30, 2, accent_color);

    // Score Info
    if (score > 0) {
        char score_str[32];
        snprintf(score_str, sizeof(score_str), "SCORE: %05lu", (unsigned long)score);
        draw_centered_string(my + 54, score_str, COLOR_WHITE, COLOR_BLACK, 1);
    }

    if (level > 0) {
        char lvl_str[32];
        snprintf(lvl_str, sizeof(lvl_str), "REACHED STAGE %d", level);
        draw_centered_string(my + 72, lvl_str, COLOR_YELLOW, COLOR_BLACK, 1);
    }

    // Action Prompt Box (Bottom of Modal)
    st7789_fill_rect(mx + 20, my + mh - 32, mw - 40, 20, 0x2104); // Dark accent box
    draw_centered_string(my + mh - 26, "PRESS ROTATE", COLOR_GREEN, 0x2104, 1);

    fb_present();
}

// ============================================================================
// BATTERY MONITORING SUBSYSTEM (ADC on BTN_RIGHT / D0 / GPIO0 / ADC1_CH0)
// Multiplexed with BTN_RIGHT via R1=180k (to VBAT) and R2=180k (to GND)
// ============================================================================
#define BATTERY_DIVIDER_RATIO   0.5000f       // 180k / (180k + 180k) = 0.5000

static adc_cali_handle_t battery_cali_handle = NULL;
static float battery_cached_voltage = 3.90f;   // Start with reasonable default ~75%
static int battery_cached_percent = 75;
static int64_t battery_last_read_time = 0;

static void battery_monitor_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &battery_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %d", err);
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12, // Measure up to ~3.3V
    };
    adc_oneshot_config_channel(battery_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);

    // Try curve fitting calibration (standard for ESP32-C6)
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &battery_cali_handle) != ESP_OK) {
        battery_cali_handle = NULL;
    }
}

static float update_battery_reading(void) {
    if (!battery_adc_handle) return battery_cached_voltage;

    int raw_sum = 0;
    int samples = 8;
    for (int i = 0; i < samples; i++) {
        int r = 0;
        if (adc_oneshot_read(battery_adc_handle, BATTERY_ADC_CHANNEL, &r) == ESP_OK) {
            raw_sum += r;
        }
    }
    int raw_avg = raw_sum / samples;

    float pin_v = 0.0f;
    if (battery_cali_handle) {
        int mv = 0;
        adc_cali_raw_to_voltage(battery_cali_handle, raw_avg, &mv);
        pin_v = mv / 1000.0f;
    } else {
        pin_v = (raw_avg / 4095.0f) * 3.3f;
    }

    // If button is currently pressed (pin pulled near 0V), keep previous cached value
    if (pin_v < 0.4f) {
        return battery_cached_voltage;
    }

    // Reconstruct VBAT from divider ratio: V_pin = VBAT * (R2 / (R1 + R2))
    float vbat = pin_v / BATTERY_DIVIDER_RATIO;
    ESP_LOGI("BATT_DIAG", "ADC Raw: %d, Pin Volts: %.3f V, Reconstructed VBAT: %.3f V", raw_avg, pin_v, vbat);
    if (vbat > 4.35f) vbat = 4.35f;
    if (vbat < 2.80f) vbat = 2.80f;

    // Exponential moving average filter for smooth, stable indicator
    battery_cached_voltage = (battery_cached_voltage * 0.75f) + (vbat * 0.25f);

    // Standard 3.7V LiPo discharge curve mapped from reconstructed vbat
    int pct = 0;
    if (vbat >= 4.20f) pct = 100;
    else if (vbat <= 3.00f) pct = 0;
    else pct = (int)((vbat - 3.00f) / 1.20f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    battery_cached_percent = pct;

    return battery_cached_voltage;
}

static void check_battery_periodic(void) {
    int64_t now = esp_timer_get_time();
    if (now - battery_last_read_time >= 2000000ULL || battery_last_read_time == 0) { // Read every 2s
        battery_last_read_time = now;
        update_battery_reading();
    }
}

// Persistent Battery Gauge (18x9 px body + 2x5 px terminal tip)
static void draw_battery_indicator(uint16_t x, uint16_t y) {
    // Battery Shell (18x9)
    st7789_fill_rect(x, y, 18, 9, COLOR_WHITE);
    st7789_fill_rect(x + 1, y + 1, 16, 7, COLOR_BLACK);
    // Positive terminal nipple on right (+ terminal)
    st7789_fill_rect(x + 18, y + 2, 2, 5, COLOR_WHITE);

    // Charge level in 3 distinct segment bars (up to 14px internal width)
    // 0 bars: empty/critical, 1 bar: low (<35%), 2 bars: medium (35-70%), 3 bars: full (>70%)
    int bars = 0;
    uint16_t bar_color = COLOR_GREEN;
    if (battery_cached_percent > 70) {
        bars = 3;
        bar_color = COLOR_GREEN;
    } else if (battery_cached_percent > 35) {
        bars = 2;
        bar_color = COLOR_YELLOW;
    } else if (battery_cached_percent > 10) {
        bars = 1;
        bar_color = COLOR_RED;
    } else {
        bars = 0; // Blinking empty red outline
        static int blink = 0;
        blink++;
        if ((blink % 20) < 10) {
            st7789_fill_rect(x + 2, y + 2, 3, 5, COLOR_RED);
        }
        return;
    }

    if (bars >= 1) st7789_fill_rect(x + 2,  y + 2, 4, 5, bar_color);
    if (bars >= 2) st7789_fill_rect(x + 7,  y + 2, 4, 5, bar_color);
    if (bars >= 3) st7789_fill_rect(x + 12, y + 2, 4, 5, bar_color);
}

// Blocks while a modal is displayed until ROTATE is pressed, DROP is long-pressed (sleep),
// or the inactivity timeout (5 min) expires (deep sleep).
static void wait_modal_confirm_or_timeout(void) {
    int64_t modal_start = esp_timer_get_time();
    int64_t drop_hold_start = 0;
    bool drop_held = false;

    while (1) {
        // Periodic battery voltage update while modal is sitting on screen
        check_battery_periodic();

        // 1. Check user confirm (ROTATE button pressed -> LOW)
        if (gpio_get_level(BTN_ROTATE) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            // Wait for release to avoid instant double click
            while (gpio_get_level(BTN_ROTATE) == 0) vTaskDelay(pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(100));
            return;
        }

        // 2. Immediate manual power-off: Holding DROP for > 1 sec in modal
        if (gpio_get_level(BTN_DROP) == 0) {
            if (!drop_held) {
                drop_held = true;
                drop_hold_start = esp_timer_get_time();
            } else if ((esp_timer_get_time() - drop_hold_start) >= 1000000ULL) {
                enter_power_down_deep_sleep();
            }
        } else {
            drop_held = false;
        }

        // 3. Auto-shutdown: inactivity timeout reached in modal -> deep sleep
        if ((esp_timer_get_time() - modal_start) >= INACTIVITY_TIMEOUT_US) {
            enter_power_down_deep_sleep();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ============================================================================
// GAME 1: TETRIS
// ============================================================================
#define BOARD_COLS      10
#define BOARD_ROWS      20
#define BLOCK_SIZE      11
#define CELL_STRIDE     12
#define BOARD_OFFSET_X  5
#define BOARD_OFFSET_Y  40

static uint16_t tetris_board[BOARD_ROWS][BOARD_COLS] = {0};

typedef struct {
    int shape[4][4];
    uint16_t color;
} Tetromino;

static const Tetromino PIECES[7] = {
    { .shape = {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, .color = COLOR_CYAN },
    { .shape = {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_BLUE },
    { .shape = {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_ORANGE },
    { .shape = {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_YELLOW },
    { .shape = {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_GREEN },
    { .shape = {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_PURPLE },
    { .shape = {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, .color = COLOR_RED }
};

static Tetromino current_piece;
static int piece_x = 3, piece_y = 0;
static Tetromino next_piece;
static uint32_t t_score = 0, t_level = 1, t_lines = 0;
static bool t_game_over = false;

static void draw_block(int col, int row, uint16_t color) {
    uint16_t px = BOARD_OFFSET_X + col * CELL_STRIDE;
    uint16_t py = BOARD_OFFSET_Y + row * CELL_STRIDE;

    if (color == COLOR_BLACK) {
        st7789_fill_rect(px, py, CELL_STRIDE, CELL_STRIDE, COLOR_BLACK);
        st7789_fill_rect(px + BLOCK_SIZE, py, 1, CELL_STRIDE, COLOR_DARKGRAY);
        st7789_fill_rect(px, py + BLOCK_SIZE, CELL_STRIDE, 1, COLOR_DARKGRAY);
    } else {
        st7789_fill_rect(px, py, BLOCK_SIZE, BLOCK_SIZE, color);
        st7789_fill_rect(px, py, BLOCK_SIZE, 1, COLOR_WHITE);
        st7789_fill_rect(px, py, 1, BLOCK_SIZE, COLOR_WHITE);
    }
}

static void render_tetris_board(void) {
    for (int r = 0; r < BOARD_ROWS; r++) {
        for (int c = 0; c < BOARD_COLS; c++) {
            draw_block(c, r, tetris_board[r][c]);
        }
    }
}

static void draw_current_piece(bool erase) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (current_piece.shape[r][c]) {
                int br = piece_y + r;
                int bc = piece_x + c;
                if (br >= 0 && br < BOARD_ROWS && bc >= 0 && bc < BOARD_COLS) {
                    draw_block(bc, br, erase ? COLOR_BLACK : current_piece.color);
                }
            }
        }
    }
}

static void draw_next_piece_preview(void) {
    uint16_t preview_x = 150, preview_y = 55;
    st7789_fill_rect(preview_x, preview_y, 50, 45, COLOR_BLACK);
    st7789_fill_rect(preview_x - 3, preview_y - 3, 56, 51, COLOR_DARKGRAY);
    st7789_fill_rect(preview_x - 1, preview_y - 1, 52, 47, COLOR_BLACK);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (next_piece.shape[r][c]) {
                uint16_t px = preview_x + c * 10;
                uint16_t py = preview_y + r * 10;
                st7789_fill_rect(px, py, 9, 9, next_piece.color);
            }
        }
    }
}

static void render_tetris_sidebar(void) {
    draw_string(25, 10, "TETRIS", COLOR_CYAN, COLOR_BLACK, 2);
    draw_battery_indicator(185, 13);
    draw_string(140, 40, "NEXT:", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    draw_next_piece_preview();

    char buf[16];
    draw_string(140, 125, "SCORE:", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%05lu", (unsigned long)t_score);
    draw_string(140, 138, buf, COLOR_YELLOW, COLOR_BLACK, 1);

    draw_string(140, 163, "LEVEL:", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%02lu", (unsigned long)t_level);
    draw_string(140, 176, buf, COLOR_GREEN, COLOR_BLACK, 1);

    draw_string(140, 201, "LINES:", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    snprintf(buf, sizeof(buf), "%03lu", (unsigned long)t_lines);
    draw_string(140, 214, buf, COLOR_ORANGE, COLOR_BLACK, 1);

    draw_string(153, 250, "ROTATE", COLOR_CYAN, COLOR_BLACK, 1);
    draw_string(138, 265, "LEFT  RIGHT", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    draw_string(160, 280, "DROP", COLOR_ORANGE, COLOR_BLACK, 1);
}

static bool check_tetris_collision(const Tetromino *p, int px, int py) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (p->shape[r][c]) {
                int target_r = py + r;
                int target_c = px + c;
                if (target_c < 0 || target_c >= BOARD_COLS || target_r >= BOARD_ROWS) return true;
                if (target_r >= 0 && tetris_board[target_r][target_c] != COLOR_BLACK) return true;
            }
        }
    }
    return false;
}

static void rotate_tetris_piece(Tetromino *p) {
    Tetromino temp = *p;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            p->shape[c][3 - r] = temp.shape[r][c];
        }
    }
}

static void spawn_tetris_piece(void) {
    current_piece = next_piece;
    next_piece = PIECES[esp_random() % 7];
    piece_x = 3; piece_y = 0;
    draw_next_piece_preview();
    if (check_tetris_collision(&current_piece, piece_x, piece_y)) t_game_over = true;
}

static void lock_tetris_piece(void) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (current_piece.shape[r][c]) {
                int br = piece_y + r, bc = piece_x + c;
                if (br >= 0 && br < BOARD_ROWS && bc >= 0 && bc < BOARD_COLS) {
                    tetris_board[br][bc] = current_piece.color;
                }
            }
        }
    }
    int cleared = 0;
    for (int r = BOARD_ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < BOARD_COLS; c++) {
            if (tetris_board[r][c] == COLOR_BLACK) { full = false; break; }
        }
        if (full) {
            cleared++;
            for (int shift_r = r; shift_r > 0; shift_r--) {
                for (int c = 0; c < BOARD_COLS; c++) tetris_board[shift_r][c] = tetris_board[shift_r - 1][c];
            }
            for (int c = 0; c < BOARD_COLS; c++) tetris_board[0][c] = COLOR_BLACK;
            r++;
        }
    }
    if (cleared > 0) {
        t_lines += cleared;
        static const uint32_t line_scores[5] = {0, 100, 300, 500, 800};
        t_score += line_scores[cleared] * t_level;
        t_level = 1 + (t_lines / 10);
        sfx_line_clear();
        render_tetris_board();
        render_tetris_sidebar();
    }
    spawn_tetris_piece();
}

static void reset_tetris_game(void) {
    memset(tetris_board, 0, sizeof(tetris_board));
    t_score = 0; t_level = 1; t_lines = 0; t_game_over = false;
    next_piece = PIECES[esp_random() % 7];
    spawn_tetris_piece();
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(3, 40, 2, 242, COLOR_WHITE);
    st7789_fill_rect(125, 40, 2, 242, COLOR_WHITE);
    st7789_fill_rect(3, 280, 124, 2, COLOR_WHITE);
    st7789_fill_rect(132, 40, 1, 240, COLOR_DARKGRAY);
    render_tetris_board();
    render_tetris_sidebar();
}

// ============================================================================
// GAME 2: SPACE INVADERS (WITH CLASSIC PIXEL ART ALIEN SPRITES & WEAPONS)
// ============================================================================
#define INV_ROWS 4
#define INV_COLS 6
#define INV_COUNT (INV_ROWS * INV_COLS)

// Classic Alien Bitmaps (8x8 pixels)
static const uint8_t ALIEN_SQUID[8] = {
    0b00011000, 0b00111100, 0b01111110, 0b11011011,
    0b11111111, 0b00100100, 0b01011010, 0b10100101
};
static const uint8_t ALIEN_CRAB[8] = {
    0b00100100, 0b00011000, 0b01111110, 0b11011011,
    0b11111111, 0b10111101, 0b10100101, 0b00100100
};
static const uint8_t ALIEN_JELLY[8] = {
    0b01111110, 0b11111111, 0b11011011, 0b11111111,
    0b01111110, 0b00100100, 0b01011010, 0b01000010
};
static const uint8_t ALIEN_SKULL[8] = { // Elite Commander Alien
    0b00111100, 0b01111110, 0b11011011, 0b11111111,
    0b01111110, 0b01011010, 0b10000001, 0b11000011
};
static const uint8_t ALIEN_BAT[8] = { // Fast Winged Alien
    0b10000001, 0b11000011, 0b11111111, 0b11011011,
    0b01111110, 0b00111100, 0b01000010, 0b10000001
};
static const uint8_t ALIEN_EXPLOSION[8] = {
    0b10000001, 0b01000010, 0b00100100, 0b00011000,
    0b00011000, 0b00100100, 0b01000010, 0b10000001
};


typedef struct {
    int x, y;
    bool alive;
    uint16_t color;
    const uint8_t *bmp;
} Invader;

static Invader invaders[INV_COUNT];
static int inv_dir = 1;
static int ship_x = 105;
static int p_bullet_x = -1, p_bullet_y = -1;
static int e_bullet_x = -1, e_bullet_y = -1;
static uint32_t inv_score = 0;
static int inv_lives = 3;
static bool inv_game_over = false;
static int inv_level = 1;
static const char *inv_formation_name = "CLASSIC SQUAD";

static void setup_space_invaders_wave(int level) {
    inv_dir = 1; ship_x = 105;
    p_bullet_x = -1; p_bullet_y = -1; e_bullet_x = -1; e_bullet_y = -1;

    // Start height scales slightly with level (aliens start lower, but clamped)
    int start_y = 44 + ((level - 1) % 4) * 4;

    // 6 Distinct Formations cycling through waves:
    // Wave 1: Classic Standard (24 aliens)
    // Wave 2: Flying V-Formation (Apex vanguard lead by Commander)
    // Wave 3: Pincer Flankers (Split wing columns)
    // Wave 4: Diamond Fortress (Reinforced core)
    // Wave 5: Staggered Checkerboard (Difficult interleaved formation)
    // Wave 6: Heavy Dreadnought (High-density front lines)
    int pattern = (level - 1) % 6;

    // Rich palette rotating across waves
    static const uint16_t wave_colors[6][3] = {
        { COLOR_CYAN,    COLOR_GREEN,   COLOR_YELLOW  }, // Wave 1: Retro Neon
        { COLOR_MAGENTA, COLOR_ORANGE,  COLOR_RED     }, // Wave 2: Crimson Assault
        { COLOR_YELLOW,  COLOR_CYAN,    COLOR_GREEN   }, // Wave 3: Emerald Flank
        { COLOR_WHITE,   COLOR_MAGENTA, COLOR_CYAN    }, // Wave 4: Cosmic Diamond
        { COLOR_GREEN,   COLOR_YELLOW,  COLOR_ORANGE  }, // Wave 5: Toxic Swarm
        { COLOR_RED,     COLOR_MAGENTA, COLOR_WHITE   }  // Wave 6: Dread Armada
    };

    uint16_t c_top = wave_colors[pattern][0];
    uint16_t c_mid = wave_colors[pattern][1];
    uint16_t c_bot = wave_colors[pattern][2];

    switch (pattern) {
        case 0: inv_formation_name = "CLASSIC SQUAD"; break;
        case 1: inv_formation_name = "V-FORMATION"; break;
        case 2: inv_formation_name = "PINCER FLANK"; break;
        case 3: inv_formation_name = "DIAMOND MATRIX"; break;
        case 4: inv_formation_name = "CHECKER SWARM"; break;
        default: inv_formation_name = "DREAD ARMADA"; break;
    }

    for (int r = 0; r < INV_ROWS; r++) {
        for (int c = 0; c < INV_COLS; c++) {
            int idx = r * INV_COLS + c;
            invaders[idx].x = 12 + c * 35;
            invaders[idx].y = start_y + r * 24;
            invaders[idx].alive = true;

            // Pattern-specific layout & density modifications
            if (pattern == 1) {
                // V-Formation: Center points forward/down, wings sweep outward
                int v_shift = abs(c - 2) * 8;
                invaders[idx].y += (16 - v_shift);
                if (r == 0 && (c == 2 || c == 3)) {
                    invaders[idx].bmp = ALIEN_SKULL;
                    invaders[idx].color = COLOR_WHITE;
                } else if (r <= 1) {
                    invaders[idx].bmp = ALIEN_BAT;
                    invaders[idx].color = c_top;
                } else {
                    invaders[idx].bmp = ALIEN_CRAB;
                    invaders[idx].color = c_mid;
                }
            } else if (pattern == 2) {
                // Pincer Flankers: Empty center in top rows, heavy on left & right wings
                if (r < 2 && (c == 2 || c == 3)) {
                    invaders[idx].alive = false; // Carve out open center funnel
                }
                if (c == 0 || c == 5) {
                    invaders[idx].bmp = ALIEN_BAT;
                    invaders[idx].color = c_top;
                } else if (r == 0) {
                    invaders[idx].bmp = ALIEN_SKULL;
                    invaders[idx].color = c_mid;
                } else {
                    invaders[idx].bmp = ALIEN_JELLY;
                    invaders[idx].color = c_bot;
                }
            } else if (pattern == 3) {
                // Diamond Fortress: Outer corners omitted, diamond cluster
                if ((r == 0 && (c == 0 || c == 5)) || (r == 3 && (c == 0 || c == 5))) {
                    invaders[idx].alive = false;
                }
                if (r == 1 && (c == 2 || c == 3)) {
                    invaders[idx].bmp = ALIEN_SKULL; // Center core bosses
                    invaders[idx].color = COLOR_YELLOW;
                } else if (r <= 1) {
                    invaders[idx].bmp = ALIEN_SQUID;
                    invaders[idx].color = c_top;
                } else {
                    invaders[idx].bmp = ALIEN_CRAB;
                    invaders[idx].color = c_mid;
                }
            } else if (pattern == 4) {
                // Checkerboard Swarm: Alternating staggered arrangement with bats and squids
                if ((r + c) % 2 == 1 && r < 3) {
                    invaders[idx].x += 6;
                    invaders[idx].bmp = ALIEN_BAT;
                    invaders[idx].color = c_top;
                } else {
                    invaders[idx].bmp = (r == 0) ? ALIEN_SKULL : ALIEN_JELLY;
                    invaders[idx].color = (r == 0) ? c_mid : c_bot;
                }
            } else {
                // Dread Armada: Front-line heavily armored crabs & skulls
                if (r == 0) {
                    invaders[idx].bmp = ALIEN_SKULL;
                    invaders[idx].color = c_top;
                } else if (r == 1) {
                    invaders[idx].bmp = ALIEN_BAT;
                    invaders[idx].color = c_mid;
                } else {
                    invaders[idx].bmp = (c % 2 == 0) ? ALIEN_CRAB : ALIEN_JELLY;
                    invaders[idx].color = c_bot;
                }
            }
        }
    }
}

static void reset_space_invaders(void) {
    inv_score = 0; inv_lives = 3; inv_game_over = false; inv_level = 1;
    setup_space_invaders_wave(1);
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(10, 10, "SPACE INVADERS", COLOR_GREEN, COLOR_BLACK, 2);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_GREEN);
}

static void render_space_invaders(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "SCR:%04lu W:%d L:%d", (unsigned long)inv_score, inv_level, inv_lives);
    draw_string(10, 35, buf, COLOR_WHITE, COLOR_BLACK, 1);

    // Draw Pixel Art Invaders (double-size 16x16)
    for (int i = 0; i < INV_COUNT; i++) {
        if (invaders[i].alive) {
            draw_bitmap(invaders[i].x, invaders[i].y, 8, 8, invaders[i].bmp, invaders[i].color);
        }
    }

    // Draw Defender Ship
    st7789_fill_rect(ship_x, 285, 24, 8, COLOR_GREEN);
    st7789_fill_rect(ship_x + 9, 280, 6, 5, COLOR_GREEN);
    st7789_fill_rect(ship_x + 11, 277, 2, 3, COLOR_WHITE);
}


// ============================================================================
// GAME 3: BREAKOUT ("THE BETTY")
// ============================================================================
#define BRK_ROWS 5
#define BRK_COLS 7
#define BRK_COUNT (BRK_ROWS * BRK_COLS)

typedef struct {
    int x, y, w, h;
    bool alive;
    uint16_t color;
    int hits_left; // 1 = normal, 2 = silver 2-hit brick
} Brick;

static Brick bricks[BRK_COUNT];
static float ball_x = 120, ball_y = 200, ball_vx = 7.0f, ball_vy = -8.0f;
static int paddle_x = 96, paddle_w = 48;
static uint32_t brk_score = 0;
static int brk_lives = 3;
static bool brk_game_over = false;
static float brk_speed_mult = 1.0f; // ROTATE=faster, DROP=slower
static int brk_level = 1;

static void setup_breakout_level(int level) {
    ball_x = 120; ball_y = 200;

    // Speed scales slightly with level progression
    float lvl_speed = 1.0f + (level - 1) * 0.08f;
    if (lvl_speed > 1.8f) lvl_speed = 1.8f;
    ball_vx = 4.5f * brk_speed_mult * lvl_speed;
    ball_vy = -5.0f * brk_speed_mult * lvl_speed;

    // Paddle narrows slightly on higher levels (min 28px)
    paddle_w = 48 - (level - 1) * 2;
    if (paddle_w < 28) paddle_w = 28;
    paddle_x = (SCREEN_WIDTH - paddle_w) / 2;

    static const uint16_t row_colors[6] = {
        COLOR_RED, COLOR_ORANGE, COLOR_YELLOW, COLOR_GREEN, COLOR_CYAN, COLOR_MAGENTA
    };

    int layout = (level - 1) % 10; // 10 distinct level layouts

    for (int r = 0; r < BRK_ROWS; r++) {
        for (int c = 0; c < BRK_COLS; c++) {
            int idx = r * BRK_COLS + c;
            bricks[idx].x = 5 + c * 33;
            bricks[idx].y = 50 + r * 14;
            bricks[idx].w = 30;
            bricks[idx].h = 10;
            bricks[idx].alive = true;
            bricks[idx].color = row_colors[r % 6];
            bricks[idx].hits_left = 1;

            switch (layout) {
                case 0: // Level 1: Standard 5-row Rainbow
                    break;

                case 1: // Level 2: Checkerboard
                    if ((r + c) % 2 != 0) bricks[idx].alive = false;
                    break;

                case 2: // Level 3: Pyramid / Diamond
                    if (c < (2 - r / 2) || c > (4 + r / 2)) bricks[idx].alive = false;
                    break;

                case 3: // Level 4: Silver 2-Hit Top Row
                    if (r == 0) { bricks[idx].color = COLOR_WHITE; bricks[idx].hits_left = 2; }
                    break;

                case 4: // Level 5: Alien Invader Pattern
                    if ((r == 0 && (c == 0 || c == 6)) || (r == 4 && c % 2 == 1)) bricks[idx].alive = false;
                    if (r == 1 || r == 2) bricks[idx].color = COLOR_CYAN;
                    break;

                case 5: // Level 6: Vertical Stripes
                    bricks[idx].color = row_colors[c % 6];
                    break;

                case 6: // Level 7: Ring Vault (center hollow, Silver core)
                    if (r >= 1 && r <= 3 && c >= 2 && c <= 4) {
                        if (r == 2 && c == 3) { bricks[idx].color = COLOR_WHITE; bricks[idx].hits_left = 2; }
                        else bricks[idx].alive = false;
                    }
                    break;

                case 7: // Level 8: Low Wall (starts at Y=70)
                    bricks[idx].y = 70 + r * 14;
                    break;

                case 8: // Level 9: Staggered Columns
                    if ((c % 2 == 1 && r == 0) || (c % 2 == 0 && r == 4)) bricks[idx].alive = false;
                    break;

                case 9: // Level 10: THE OMEGA VAULT (Silver 2-hit top 2 rows)
                    if (r <= 1) { bricks[idx].color = COLOR_WHITE; bricks[idx].hits_left = 2; }
                    break;
            }
        }
    }
}

static void reset_breakout(void) {
    brk_score = 0; brk_lives = 3; brk_game_over = false; brk_level = 1;
    setup_breakout_level(1);
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(25, 10, "BREAKOUT", COLOR_ORANGE, COLOR_BLACK, 2);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_ORANGE);
}


// Sub-pixel anti-aliased ball renderer (provides smooth fractional-pixel vector motion)
static void draw_subpixel_ball(float bx, float by, float old_bx, float old_by) {
    int n_x = (int)floorf(bx);
    int n_y = (int)floorf(by);

    float fx = bx - (float)n_x;
    float fy = by - (float)n_y;

    for (int r = 0; r < 7; r++) {
        for (int c = 0; c < 7; c++) {
            float weight = 1.0f;
            if (c == 0) weight *= (1.0f - fx);
            else if (c == 6) weight *= fx;

            if (r == 0) weight *= (1.0f - fy);
            else if (r == 6) weight *= fy;

            uint8_t val = (uint8_t)(weight * 255.0f);
            if (val > 25) {
                uint8_t r5 = (val >> 3);
                uint8_t g6 = (val >> 2);
                uint8_t b5 = (val >> 3);
                uint16_t color = (r5 << 11) | (g6 << 5) | b5;
                st7789_fill_rect(n_x + c, n_y + r, 1, 1, color);
            } else {
                st7789_fill_rect(n_x + c, n_y + r, 1, 1, COLOR_BLACK);
            }
        }
    }
}


// ============================================================================
// GAME 4: CLASSIC PONG / TENNIS
// ============================================================================
static float p_paddle_y = 130, c_paddle_y = 130;
static int p_paddle_h = 44, c_paddle_h = 44;
static float pong_bx = 120, pong_by = 160, pong_vx = 2.4f, pong_vy = 1.5f;
static int player_score = 0, comp_score = 0;
static bool pong_game_over = false;
static float pong_speed_mult = 1.0f; // Adjustable with LEFT/RIGHT buttons

// Draw super-fat 1970s arcade Pong digital score number (22x32 px, 5px thick segments)
static void draw_retro_squarish_num(int x, int y, int num, uint16_t color) {
    st7789_fill_rect(x, y, 24, 34, COLOR_BLACK);
    if (num < 0 || num > 9) return;

    // 7-segment masks for 0..9
    static const uint8_t segs[10] = {
        0b00111111, // 0: A B C D E F
        0b00000110, // 1: B C
        0b01011011, // 2: A B D E G
        0b01001111, // 3: A B C D G
        0b01100110, // 4: B C F G
        0b01101101, // 5: A C D F G
        0b01111101, // 6: A C D E F G
        0b00000111, // 7: A B C
        0b01111111, // 8: A B C D E F G
        0b01101111  // 9: A B C D F G
    };
    uint8_t mask = segs[num];

    if (mask & 0x01) st7789_fill_rect(x + 2, y, 18, 5, color);          // Seg A (top)
    if (mask & 0x02) st7789_fill_rect(x + 17, y + 2, 5, 14, color);     // Seg B (top right)
    if (mask & 0x04) st7789_fill_rect(x + 17, y + 16, 5, 14, color);    // Seg C (bot right)
    if (mask & 0x08) st7789_fill_rect(x + 2, y + 27, 18, 5, color);     // Seg D (bot)
    if (mask & 0x10) st7789_fill_rect(x, y + 16, 5, 14, color);        // Seg E (bot left)
    if (mask & 0x20) st7789_fill_rect(x, y + 2, 5, 14, color);         // Seg F (top left)
    if (mask & 0x40) st7789_fill_rect(x + 2, y + 13, 18, 5, color);     // Seg G (mid)
}

static void reset_pong_game(void) {
    p_paddle_y = 130; c_paddle_y = 130;
    pong_bx = 120; pong_by = 160;
    pong_vx = (esp_random() % 2 == 0) ? 4.8f * pong_speed_mult : -4.8f * pong_speed_mult;
    pong_vy = (((esp_random() % 100) / 35.0f) - 1.4f) * pong_speed_mult;
    player_score = 0; comp_score = 0; pong_game_over = false;

    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_WHITE);
    st7789_fill_rect(0, 312, SCREEN_WIDTH, 2, COLOR_WHITE);

    // Draw retro court net line
    for (int y = 40; y < 310; y += 16) {
        st7789_fill_rect(119, y, 2, 8, COLOR_DARKGRAY);
    }
    draw_retro_squarish_num(65, 45, player_score, COLOR_GREEN);
    draw_retro_squarish_num(153, 45, comp_score, COLOR_CYAN);
}

// ============================================================================
// GAME 5: SNAKE
// ============================================================================
typedef struct { int x, y; } Point;
static Point snake[200];
static int snake_len = 4, snake_dir = 1; // 0=L, 1=R, 2=U, 3=D
static int snake_input_dir = 1;
static int food_x = 10, food_y = 10;
static int snake_score = 0;
static bool snake_game_over = false;

static void spawn_food(void) {
    food_x = (esp_random() % 18) + 1;
    food_y = (esp_random() % 20) + 1;
}

static void reset_snake_game(void) {
    snake_len = 4; snake_dir = 1; snake_input_dir = 1; snake_score = 0; snake_game_over = false;
    for (int i = 0; i < snake_len; i++) {
        snake[i].x = 8 - i; snake[i].y = 10;
    }
    spawn_food();
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_GREEN);
    draw_string(25, 10, "SNAKE", COLOR_GREEN, COLOR_BLACK, 2);
}

// ============================================================================
// GAME 6: FLAPPY BIRD (BALANCED & ACCESSIBLE ENGINE)
// ============================================================================
static float flappy_y = 150.0f, flappy_vy = 0.0f;
static float pipe_x[2] = {240, 410};
static int pipe_gap_y[2] = {100, 140};
static int flappy_score = 0;
static int flappy_level = 1;
static bool flappy_game_over = false;

static void reset_flappy_game(void) {
    flappy_y = 150.0f; flappy_vy = 0.0f; flappy_score = 0; flappy_level = 1; flappy_game_over = false;
    pipe_x[0] = 240; pipe_gap_y[0] = (esp_random() % 100) + 65;
    pipe_x[1] = 410; pipe_gap_y[1] = (esp_random() % 100) + 65;
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_YELLOW);
    draw_string(20, 10, "FLAPPY BIRD", COLOR_YELLOW, COLOR_BLACK, 2);
}

// ============================================================================
// GAME 7: RETRO RACING / HIGHWAY DODGER
// ============================================================================
// GAME 7: ARCADE RACER (OUTRUN-STYLE PSEUDO-3D ENGINE)
// ============================================================================
#define RACER_MAX_TRAFFIC 4

typedef struct {
    float z;          // Perspective depth (0.05 = horizon, 1.0 = player level)
    int lane;         // -1 = Left, 0 = Center, 1 = Right
    uint16_t color;   // Car color
    float speed_mod;  // Relative traffic speed (0.3 to 0.7)
    int type;         // 0 = Car, 1 = Truck, 2 = Oil Slick, 3 = Gold Star
    bool active;
} TrafficObj;

static TrafficObj traffic[RACER_MAX_TRAFFIC];
static uint32_t racer_score = 0;
static int racer_cars_passed = 0;
static int racer_lives = 3;
static bool racer_game_over = false;
static float player_x = 0.0f;        // -1.0 (Left curb) to +1.0 (Right curb)
static float racer_speed_mph = 0.0f; // 0 to 140 MPH
static float track_pos = 0.0f;       // Track distance
static int spinout_timer = 0;        // Oil slick spinout counter

static void reset_racer_game(void) {
    player_x = 0.0f; racer_score = 0; racer_cars_passed = 0;
    racer_lives = 3; racer_game_over = false; racer_speed_mph = 0.0f;
    track_pos = 0.0f; spinout_timer = 0;

    static const uint16_t traffic_cols[4] = { COLOR_YELLOW, COLOR_CYAN, COLOR_ORANGE, COLOR_WHITE };

    for (int i = 0; i < RACER_MAX_TRAFFIC; i++) {
        traffic[i].active = true;
        traffic[i].z = 0.15f + i * 0.22f;
        traffic[i].lane = (i % 3) - 1;
        traffic[i].color = traffic_cols[i % 4];
        traffic[i].speed_mod = 0.35f + (esp_random() % 25) / 100.0f;
        traffic[i].type = (i == 3) ? 3 : ((i == 2) ? 2 : (i % 2)); // 0=Car, 1=Truck, 2=Oil, 3=Star
    }

    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(15, 10, "RETRO RACER", COLOR_CYAN, COLOR_BLACK, 2);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_CYAN);
}

// Draw Detailed Player FPV Sports Car (32x24 px) with Turbo Flames & Steering Tilt
static void draw_racer_player_car(int x, int y, int tilt, bool turbo) {
    // Slicks / Wide Rear Tires
    st7789_fill_rect(x - 3, y + 10, 6, 12, COLOR_DARKGRAY);
    st7789_fill_rect(x + 29, y + 10, 6, 12, COLOR_DARKGRAY);

    // Main Red Sports Body
    st7789_fill_rect(x + 4, y + 4, 24, 16, COLOR_RED);
    st7789_fill_rect(x + 2, y + 12, 28, 8, COLOR_RED);

    // Rear Windshield (Cyan glass)
    st7789_fill_rect(x + 8, y + 6, 16, 6, COLOR_CYAN);

    // Rear Spoiler
    st7789_fill_rect(x + 1, y, 30, 3, COLOR_RED);
    st7789_fill_rect(x + 3, y + 3, 3, 3, COLOR_BLACK);
    st7789_fill_rect(x + 26, y + 3, 3, 3, COLOR_BLACK);

    // Taillights
    st7789_fill_rect(x + 4, y + 14, 6, 3, COLOR_YELLOW);
    st7789_fill_rect(x + 22, y + 14, 6, 3, COLOR_YELLOW);

    // Exhaust & License Plate
    st7789_fill_rect(x + 12, y + 16, 8, 3, COLOR_DARKGRAY);

    // Steering Bank Tilt Indicator Lights
    if (tilt < 0) st7789_fill_rect(x + 2, y + 18, 4, 4, COLOR_YELLOW);
    else if (tilt > 0) st7789_fill_rect(x + 26, y + 18, 4, 4, COLOR_YELLOW);

    // Turbo Exhaust Flames!
    if (turbo) {
        st7789_fill_rect(x + 5, y + 19, 5, 5, COLOR_ORANGE);
        st7789_fill_rect(x + 6, y + 23, 3, 4, COLOR_YELLOW);
        st7789_fill_rect(x + 22, y + 19, 5, 5, COLOR_ORANGE);
        st7789_fill_rect(x + 23, y + 23, 3, 4, COLOR_YELLOW);
    }
}

// Draw Perspective-Scaled Traffic Car / Truck / Oil Slick / Bonus Star
static void draw_scaled_traffic_obj(int center_x, int screen_y, float z, uint16_t color, int type) {
    int w = (int)(6.0f + z * 28.0f);
    int h = (int)(4.0f + z * 16.0f);
    if (w < 4) w = 4;
    if (h < 3) h = 3;
    int x = center_x - w / 2;

    if (type == 0) { // Traffic Car
        st7789_fill_rect(x, screen_y, w, h, color);
        st7789_fill_rect(x + w / 4, screen_y + h / 4, w / 2, h / 3, COLOR_BLUE);
        st7789_fill_rect(x + 1, screen_y + h - 2, (w / 4 > 2 ? w / 4 : 2), 2, COLOR_RED);
        st7789_fill_rect(x + w - (w / 4 > 2 ? w / 4 : 2) - 1, screen_y + h - 2, (w / 4 > 2 ? w / 4 : 2), 2, COLOR_RED);
    } else if (type == 1) { // Semi Truck
        int truck_h = h + (int)(z * 8.0f);
        st7789_fill_rect(x, screen_y - (int)(z * 4.0f), w, truck_h, COLOR_WHITE);
        st7789_fill_rect(x + 2, screen_y, w - 4, h / 2, COLOR_DARKGRAY);
        st7789_fill_rect(x + 1, screen_y + truck_h - 2, w - 2, 2, COLOR_RED);
    } else if (type == 2) { // Oil Slick
        st7789_fill_rect(x, screen_y, w + 4, h / 2 + 2, COLOR_DARKGRAY);
        st7789_fill_rect(x + 2, screen_y + 1, w, h / 2, COLOR_BLACK);
    } else if (type == 3) { // Bonus Gold Star
        st7789_fill_rect(x + w / 4, screen_y, w / 2 + 1, h + 2, COLOR_YELLOW);
        st7789_fill_rect(x, screen_y + h / 4, w + 2, h / 2 + 1, COLOR_ORANGE);
    }
}


// ============================================================================
// ARCADE LAUNCHER MENU
// ============================================================================
static int menu_selected = 0;

static void update_arcade_menu_items(void) {
    uint16_t colors[7];
    for (int i = 0; i < 7; i++) colors[i] = (menu_selected == i) ? COLOR_YELLOW : COLOR_LIGHTGRAY;

    draw_string(15, 65,  (menu_selected == 0) ? "> 1. TETRIS"   : "  1. TETRIS",   colors[0], COLOR_BLACK, 2);
    draw_string(15, 93,  (menu_selected == 1) ? "> 2. INVADERS" : "  2. INVADERS", colors[1], COLOR_BLACK, 2);
    draw_string(15, 121, (menu_selected == 2) ? "> 3. BREAKOUT" : "  3. BREAKOUT", colors[2], COLOR_BLACK, 2);
    draw_string(15, 149, (menu_selected == 3) ? "> 4. TENNIS"   : "  4. TENNIS",   colors[3], COLOR_BLACK, 2);
    draw_string(15, 177, (menu_selected == 4) ? "> 5. SNAKE"    : "  5. SNAKE",    colors[4], COLOR_BLACK, 2);
    draw_string(15, 205, (menu_selected == 5) ? "> 6. FLAPPY"   : "  6. FLAPPY",   colors[5], COLOR_BLACK, 2);
    draw_string(15, 233, (menu_selected == 6) ? "> 7. RACER"    : "  7. RACER",    colors[6], COLOR_BLACK, 2);

    // Persistent battery indicator on menu header
    draw_battery_indicator(212, 13);

    fb_present();
}

static void draw_arcade_menu(void) {
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(15, 10, "ARCADE CONSOLE", COLOR_CYAN, COLOR_BLACK, 2);
    draw_battery_indicator(212, 13);
    st7789_fill_rect(10, 32, 220, 3, COLOR_MAGENTA);

    draw_string(25, 45, "SELECT GAME:", COLOR_WHITE, COLOR_BLACK, 1);
    update_arcade_menu_items();

    st7789_fill_rect(10, 260, 220, 2, COLOR_DARKGRAY);
    draw_string(20, 270, "ROTATE/DROP: UP/DOWN", COLOR_LIGHTGRAY, COLOR_BLACK, 1);
    draw_string(20, 288, "LEFT/RIGHT: SELECT", COLOR_GREEN, COLOR_BLACK, 1);
    draw_string(20, 304, "HOLD DROP: POWER OFF", COLOR_ORANGE, COLOR_BLACK, 1);
    fb_present();
}



// ============================================================================
// MAIN APPLICATION LOOP & EVENT ROUTER
// ============================================================================
void app_main(void) {
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Starting XIAO Arcade Console (Wake cause: %d)...", wake_cause);

    // Release pin holds set during previous deep sleep (TFT_PWR, RST, and wakeup buttons)
    gpio_hold_dis((gpio_num_t)PIN_TFT_PWR);
    gpio_hold_dis((gpio_num_t)PIN_NUM_RST);
    gpio_hold_dis((gpio_num_t)BTN_RIGHT);
    gpio_hold_dis((gpio_num_t)BTN_LEFT);
    gpio_hold_dis((gpio_num_t)BTN_ROTATE);

    // Configure button inputs: LEFT, ROTATE, DROP with pull-ups
    gpio_config_t btn_config = {
        .pin_bit_mask = (1ULL << BTN_LEFT) | (1ULL << BTN_ROTATE) | (1ULL << BTN_DROP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_config);

    // Configure BTN_RIGHT (D0 / GPIO0) as INPUT with NO pull-up or pull-down
    gpio_config_t right_btn_config = {
        .pin_bit_mask = (1ULL << BTN_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&right_btn_config);
    gpio_pullup_dis(BTN_RIGHT);
    rtc_gpio_pullup_dis(BTN_RIGHT);
    rtc_gpio_pulldown_dis(BTN_RIGHT);
    rtc_gpio_hold_dis(BTN_RIGHT);

    // Initialize Battery ADC Monitor (on D0 / BTN_RIGHT)
    battery_monitor_init();
    gpio_input_enable(BTN_RIGHT); // Re-enable digital input buffer disabled by ADC init
    update_battery_reading();

    // Configure TFT power MOSFET pin (D7) and DC/RST as outputs
    gpio_config_t out_config = {
        .pin_bit_mask = (1ULL << PIN_TFT_PWR) | (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_RST),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&out_config);

    // Power ON the TFT via MOSFET
    gpio_set_level(PIN_TFT_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(150)); // Wait for TFT VCC rail to stabilize

    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_WIDTH * SCREEN_HEIGHT * 2
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi));

    buzzer_init(); // Buzzer on D5 (GPIO23)
    st7789_init();

    current_game = STATE_MENU;
    draw_arcade_menu();

    int64_t last_drop_time = esp_timer_get_time();
    int64_t last_activity_time = esp_timer_get_time(); // 5-minute inactivity timer tracker
    int64_t last_menu_batt_redraw = esp_timer_get_time();

    while (1) {
        // Periodic battery voltage update
        check_battery_periodic();

        // Read non-blocking input events
        ButtonEvents ev = get_button_events();

        // Activity check: Reset 5-min inactivity timer on any button interaction
        if (ev.left_pressed || ev.right_pressed || ev.rotate_short_click || ev.drop_short_click ||
            gpio_get_level(BTN_ROTATE) == 0 || gpio_get_level(BTN_DROP) == 0) {
            last_activity_time = esp_timer_get_time();
        }

        // AUTO-SHUTDOWN: 5 minutes of inactivity -> Deep Sleep
        if ((esp_timer_get_time() - last_activity_time) >= INACTIVITY_TIMEOUT_US) {
            enter_power_down_deep_sleep();
        }

        // 1. GLOBAL LONG PRESS DROP (> 1 sec) -> POWER OFF DEEP SLEEP
        if (ev.drop_long_press) {
            enter_power_down_deep_sleep();
        }

        // 2. GLOBAL LONG PRESS ROTATE (> 1 sec) -> RETURN TO MENU
        if (current_game != STATE_MENU && ev.rotate_long_press) {
            sfx_menu_select();
            current_game = STATE_MENU;
            draw_arcade_menu();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- STATE 0: ARCADE MENU ---
        if (current_game == STATE_MENU) {
            // ROTATE = UP, DROP = DOWN, LEFT/RIGHT = LAUNCH GAME
            if (ev.rotate_short_click) {
                menu_selected = (menu_selected + 6) % 7;
                sfx_move();
                update_arcade_menu_items(); // Zero flicker!
                vTaskDelay(pdMS_TO_TICKS(180));
            } else if (ev.drop_short_click) {
                menu_selected = (menu_selected + 1) % 7;
                sfx_move();
                update_arcade_menu_items(); // Zero flicker!
                vTaskDelay(pdMS_TO_TICKS(180));
            } else if (ev.left_pressed || ev.right_pressed) {
                sfx_menu_select();
                if (menu_selected == 0) { current_game = STATE_TETRIS; reset_tetris_game(); }
                else if (menu_selected == 1) { current_game = STATE_INVADERS; reset_space_invaders(); }
                else if (menu_selected == 2) { current_game = STATE_BREAKOUT; reset_breakout(); }
                else if (menu_selected == 3) { current_game = STATE_PONG; reset_pong_game(); }
                else if (menu_selected == 4) { current_game = STATE_SNAKE; reset_snake_game(); }
                else if (menu_selected == 5) { current_game = STATE_FLAPPY; reset_flappy_game(); }
                else { current_game = STATE_RACER; reset_racer_game(); }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }



        // --- STATE 1: TETRIS ---
        if (current_game == STATE_TETRIS) {
            if (t_game_over) {
                draw_game_over_modal("GAME OVER!", t_score, t_level, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_tetris_game();
                last_activity_time = esp_timer_get_time();
                last_drop_time = esp_timer_get_time();
                continue;
            }

            if (ev.left_pressed) {
                draw_current_piece(true);
                if (!check_tetris_collision(&current_piece, piece_x - 1, piece_y)) { piece_x--; sfx_move(); }
                draw_current_piece(false); vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (ev.right_pressed) {
                draw_current_piece(true);
                if (!check_tetris_collision(&current_piece, piece_x + 1, piece_y)) { piece_x++; sfx_move(); }
                draw_current_piece(false); vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (ev.rotate_short_click) {
                draw_current_piece(true);
                Tetromino rot = current_piece; rotate_tetris_piece(&rot);
                if (!check_tetris_collision(&rot, piece_x, piece_y)) { current_piece = rot; sfx_rotate(); }
                draw_current_piece(false);
            }
            if (ev.drop_short_click) {
                draw_current_piece(true);
                while (!check_tetris_collision(&current_piece, piece_x, piece_y + 1)) piece_y++;
                draw_current_piece(false); sfx_drop(); lock_tetris_piece();
                last_drop_time = esp_timer_get_time();
            }

            int drop_interval_ms = 800 - (t_level * 50); if (drop_interval_ms < 100) drop_interval_ms = 100;
            int64_t now = esp_timer_get_time();
            if ((now - last_drop_time) / 1000 >= drop_interval_ms) {
                draw_current_piece(true);
                if (!check_tetris_collision(&current_piece, piece_x, piece_y + 1)) { piece_y++; draw_current_piece(false); }
                else { draw_current_piece(false); lock_tetris_piece(); }
                last_drop_time = now;
            }

            // Periodic battery indicator update in Tetris sidebar
            static int64_t last_tetris_batt_time = 0;
            if (now - last_tetris_batt_time > 1000000ULL) {
                last_tetris_batt_time = now;
                draw_battery_indicator(185, 13);
            }

            fb_present();
        }

        // --- STATE 2: SPACE INVADERS (60 FPS DOUBLE BUFFERED ZERO-FLICKER) ---
        else if (current_game == STATE_INVADERS) {
            if (inv_game_over) {
                draw_game_over_modal("GAME OVER!", inv_score, inv_level, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_space_invaders();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // Clear RAM Framebuffer for 100% flicker-free rendering
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Header & Clean Non-Overlapping HUD
            draw_string(10, 10, "INVADERS", COLOR_GREEN, COLOR_BLACK, 2);
            char inv_buf[32];
            snprintf(inv_buf, sizeof(inv_buf), "SCR:%04lu L:%d", (unsigned long)inv_score, inv_lives);
            draw_string(115, 12, inv_buf, COLOR_WHITE, COLOR_BLACK, 1);
            draw_battery_indicator(214, 11);
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_GREEN);

            // Smooth & Fast Defender Ship Movement
            if (ev.left_pressed && ship_x > 5) {
                ship_x -= 10;
            }
            if (ev.right_pressed && ship_x < 210) {
                ship_x += 10;
            }

            // Draw Defender Ship into RAM
            st7789_fill_rect(ship_x, 285, 24, 8, COLOR_GREEN);
            st7789_fill_rect(ship_x + 9, 280, 6, 5, COLOR_GREEN);
            st7789_fill_rect(ship_x + 11, 277, 2, 3, COLOR_WHITE);

            // Fire Laser on ROTATE or DROP short click
            if ((ev.rotate_short_click || ev.drop_short_click) && p_bullet_y < 0) {
                p_bullet_x = ship_x + 11;
                p_bullet_y = 270;
                sfx_shoot();
            }

            // --- SUB-STEPPED LASER PHYSICS & ACCURATE HIT DETECTION ---
            if (p_bullet_y >= 0) {
                int laser_sub_steps = 4;
                int step_dist = 9;

                for (int s = 0; s < laser_sub_steps; s++) {
                    p_bullet_y -= step_dist;

                    if (p_bullet_y < 40) {
                        p_bullet_y = -1;
                        break;
                    }

                    // Check hit against every active invader
                    bool hit = false;
                    for (int i = 0; i < INV_COUNT; i++) {
                        if (invaders[i].alive) {
                            if (p_bullet_x + 2 >= invaders[i].x && p_bullet_x <= invaders[i].x + 16 &&
                                p_bullet_y + 8 >= invaders[i].y && p_bullet_y <= invaders[i].y + 16) {
                                invaders[i].alive = false;

                                // Draw Explosion Pop Animation
                                draw_bitmap(invaders[i].x, invaders[i].y, 8, 8, ALIEN_EXPLOSION, COLOR_WHITE);
                                fb_present();
                                vTaskDelay(pdMS_TO_TICKS(40));

                                p_bullet_y = -1;
                                inv_score += 20;
                                sfx_hit();
                                hit = true;
                                break;
                            }
                        }
                    }
                    if (hit) break;

                    // Draw yellow laser into RAM
                    st7789_fill_rect(p_bullet_x, p_bullet_y, 2, 8, COLOR_YELLOW);
                }
            }

            // Enemy Missile Firing (frequency increases with wave level)
            int fire_chance = 16 - inv_level * 2;
            if (fire_chance < 4) fire_chance = 4;
            if (e_bullet_y < 0 && (esp_random() % fire_chance == 0)) {
                int col = esp_random() % INV_COLS;
                for (int r = INV_ROWS - 1; r >= 0; r--) {
                    int idx = r * INV_COLS + col;
                    if (invaders[idx].alive) {
                        e_bullet_x = invaders[idx].x + 8;
                        e_bullet_y = invaders[idx].y + 16;
                        break;
                    }
                }
            }

            // Update Enemy Missile
            if (e_bullet_y >= 0) {
                e_bullet_y += 7;
                if (e_bullet_y > 310) e_bullet_y = -1;
                else st7789_fill_rect(e_bullet_x, e_bullet_y, 2, 6, COLOR_RED);

                // Hit Player Ship
                if (e_bullet_y >= 275 && e_bullet_y <= 293 && e_bullet_x >= ship_x && e_bullet_x <= ship_x + 24) {
                    e_bullet_y = -1;
                    inv_lives--;
                    sfx_game_over();
                    if (inv_lives <= 0) inv_game_over = true;
                }
            }

            // Move Invader Matrix (speed increases with wave level)
            int move_threshold = 6 - (inv_level - 1) / 2;
            if (move_threshold < 1) move_threshold = 1;

            static int move_timer = 0;
            if (++move_timer >= move_threshold) {
                move_timer = 0;
                bool hit_wall = false;
                for (int i = 0; i < INV_COUNT; i++) {
                    if (invaders[i].alive) {
                        if ((inv_dir == 1 && invaders[i].x >= 215) || (inv_dir == -1 && invaders[i].x <= 5)) {
                            hit_wall = true; break;
                        }
                    }
                }
                if (hit_wall) {
                    inv_dir = -inv_dir;
                    for (int i = 0; i < INV_COUNT; i++) {
                        invaders[i].y += 8;
                        if (invaders[i].alive && invaders[i].y >= 265) inv_game_over = true;
                    }
                } else {
                    for (int i = 0; i < INV_COUNT; i++) invaders[i].x += inv_dir * 4;
                }
            }

            // Victory Check: All Invaders Destroyed -> Advance Wave!
            bool inv_any_alive = false;
            for (int i = 0; i < INV_COUNT; i++) {
                if (invaders[i].alive) { inv_any_alive = true; break; }
            }
            if (!inv_any_alive) {
                inv_score += 500 * inv_level;
                if (inv_lives < 5) inv_lives++;

                // Wave Clear Banner
                st7789_fill_rect(15, 115, 210, 85, COLOR_DARKGRAY);
                st7789_fill_rect(17, 117, 206, 81, COLOR_BLACK);
                char win_buf[32];
                snprintf(win_buf, sizeof(win_buf), "WAVE %d CLEARED!", inv_level);
                draw_string(25, 125, win_buf, COLOR_YELLOW, COLOR_BLACK, 2);
                snprintf(win_buf, sizeof(win_buf), "+%d PTS  L:%d", 500 * inv_level, inv_lives);
                draw_string(30, 150, win_buf, COLOR_GREEN, COLOR_BLACK, 1);
                draw_string(30, 170, inv_formation_name, COLOR_CYAN, COLOR_BLACK, 1);
                fb_present();
                sfx_line_clear();
                vTaskDelay(pdMS_TO_TICKS(1500));

                inv_level++;
                setup_space_invaders_wave(inv_level);
                continue;
            }

            // Draw Pixel Art Invaders into RAM
            for (int i = 0; i < INV_COUNT; i++) {
                if (invaders[i].alive) {
                    draw_bitmap(invaders[i].x, invaders[i].y, 8, 8, invaders[i].bmp, invaders[i].color);
                }
            }

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // --- STATE 3: BREAKOUT (60 FPS DOUBLE BUFFERED ZERO-FLICKER) ---
        else if (current_game == STATE_BREAKOUT) {
            if (brk_game_over) {
                draw_game_over_modal("GAME OVER!", brk_score, brk_level, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_breakout();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // Clear RAM Framebuffer
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Header & Clean Non-Overlapping HUD
            draw_string(10, 10, "BREAKOUT", COLOR_CYAN, COLOR_BLACK, 2);
            char brk_buf[32];
            snprintf(brk_buf, sizeof(brk_buf), "S:%04lu L:%d LVL:%d", (unsigned long)brk_score, brk_lives, brk_level);
            draw_string(105, 12, brk_buf, COLOR_WHITE, COLOR_BLACK, 1);
            // Speed indicator
            char brk_spd[10];
            int bs_int = (int)(brk_speed_mult * 10.0f + 0.5f);
            if (bs_int % 10 == 0) snprintf(brk_spd, sizeof(brk_spd), "x%d", bs_int / 10);
            else snprintf(brk_spd, sizeof(brk_spd), "x%.2g", brk_speed_mult);
            draw_string(186, 12, brk_spd, COLOR_YELLOW, COLOR_BLACK, 1);
            draw_battery_indicator(214, 11);
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_CYAN);

            // ROTATE short = faster, DROP short = slower (immediate effect)
            if (ev.rotate_short_click) {
                float old = brk_speed_mult;
                brk_speed_mult += 0.25f;
                if (brk_speed_mult > 4.0f) brk_speed_mult = 4.0f;
                float s = brk_speed_mult / old;
                ball_vx *= s; ball_vy *= s;
                sfx_move();
            }
            if (ev.drop_short_click) {
                float old = brk_speed_mult;
                brk_speed_mult -= 0.25f;
                if (brk_speed_mult < 0.25f) brk_speed_mult = 0.25f;
                float s = brk_speed_mult / old;
                ball_vx *= s; ball_vy *= s;
                sfx_move();
            }

            // Smooth & Strictly Clamped Paddle Movement
            if (ev.left_pressed) {
                paddle_x -= 14;
            }
            if (ev.right_pressed) {
                paddle_x += 14;
            }
            if (paddle_x < 5) paddle_x = 5;
            if (paddle_x > SCREEN_WIDTH - paddle_w - 5) paddle_x = SCREEN_WIDTH - paddle_w - 5;

            // Draw Bricks into RAM
            bool any_alive = false;
            for (int i = 0; i < BRK_COUNT; i++) {
                if (bricks[i].alive) {
                    any_alive = true;
                    st7789_fill_rect(bricks[i].x, bricks[i].y, bricks[i].w, bricks[i].h, bricks[i].color);
                }
            }

            // Victory Check: All Bricks Destroyed -> Advance Level!
            if (!any_alive) {
                brk_score += 1000 * brk_level;
                if (brk_lives < 5) brk_lives++;

                // Level Clear Banner
                st7789_fill_rect(15, 120, 210, 75, COLOR_DARKGRAY);
                st7789_fill_rect(17, 122, 206, 71, COLOR_BLACK);
                char win_buf[32];
                snprintf(win_buf, sizeof(win_buf), "LEVEL %d CLEARED!", brk_level);
                draw_string(25, 135, win_buf, COLOR_YELLOW, COLOR_BLACK, 2);
                snprintf(win_buf, sizeof(win_buf), "+%d PTS  L:%d", 1000 * brk_level, brk_lives);
                draw_string(30, 165, win_buf, COLOR_GREEN, COLOR_BLACK, 1);
                fb_present();
                sfx_line_clear();
                vTaskDelay(pdMS_TO_TICKS(1500));

                brk_level++;
                setup_breakout_level(brk_level);
                continue;
            }

            // Sub-step Physics Loop
            float old_bx = ball_x;
            float old_by = ball_y;
            int sub_steps = 6;
            float step_vx = ball_vx / (float)sub_steps;
            float step_vy = ball_vy / (float)sub_steps;

            for (int s = 0; s < sub_steps; s++) {
                ball_x += step_vx;
                ball_y += step_vy;

                // Wall Collisions
                if (ball_x <= 2.0f) { ball_x = 2.0f; ball_vx = fabsf(ball_vx); step_vx = fabsf(step_vx); sfx_move(); }
                if (ball_x >= SCREEN_WIDTH - 8.0f) { ball_x = SCREEN_WIDTH - 8.0f; ball_vx = -fabsf(ball_vx); step_vx = -fabsf(step_vx); sfx_move(); }
                if (ball_y <= 40.0f) { ball_y = 40.0f; ball_vy = fabsf(ball_vy); step_vy = fabsf(step_vy); sfx_move(); }

                // Paddle Collision
                if (ball_vy > 0 && ball_y + 6.0f >= 280.0f && ball_y <= 288.0f &&
                    ball_x + 6.0f >= (float)paddle_x && ball_x <= (float)(paddle_x + paddle_w)) {
                    ball_y = 274.0f;
                    ball_vy = -fabsf(ball_vy);
                    step_vy = -fabsf(step_vy);
                    float hit_offset = (ball_x + 3.0f) - ((float)paddle_x + (float)paddle_w / 2.0f);
                    ball_vx = hit_offset * 0.35f;
                    step_vx = ball_vx / (float)sub_steps;
                    sfx_rotate();
                }

                // Brick Collisions (Handles 2-Hit Silver Bricks)
                for (int i = 0; i < BRK_COUNT; i++) {
                    if (bricks[i].alive) {
                        if (ball_x + 6.0f >= (float)bricks[i].x && ball_x <= (float)(bricks[i].x + bricks[i].w) &&
                            ball_y + 6.0f >= (float)bricks[i].y && ball_y <= (float)(bricks[i].y + bricks[i].h)) {
                            if (bricks[i].hits_left > 1) {
                                bricks[i].hits_left--;
                                static const uint16_t row_colors[6] = { COLOR_RED, COLOR_ORANGE, COLOR_YELLOW, COLOR_GREEN, COLOR_CYAN, COLOR_MAGENTA };
                                bricks[i].color = row_colors[i / BRK_COLS % 6];
                                sfx_rotate();
                            } else {
                                bricks[i].alive = false;
                                brk_score += 20 * brk_level;
                                sfx_rotate();
                            }
                            ball_vy = -ball_vy;
                            step_vy = -step_vy;
                            break;
                        }
                    }
                }

                // Out of Bottom Bounds Reset (Ball Missed Paddle)
                if (ball_y >= 305.0f) {
                    brk_lives--;
                    sfx_hit();
                    if (brk_lives <= 0) {
                        brk_game_over = true;
                    } else {
                        ball_x = paddle_x + paddle_w / 2 - 3;
                        ball_y = 200.0f;
                        float lvl_speed = 1.0f + (brk_level - 1) * 0.08f;
                        if (lvl_speed > 1.8f) lvl_speed = 1.8f;
                        ball_vx = ((esp_random() % 2 == 0) ? 4.5f : -4.5f) * brk_speed_mult * lvl_speed;
                        ball_vy = -5.0f * brk_speed_mult * lvl_speed;
                    }
                    break;
                }
            }

            // Draw Anti-Aliased Sub-pixel Ball into RAM
            draw_subpixel_ball(ball_x, ball_y, old_bx, old_by);

            // Draw Paddle into RAM
            st7789_fill_rect(paddle_x, 280, paddle_w, 8, COLOR_CYAN);

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }





        // --- STATE 4: PONG / TENNIS (60 FPS DOUBLE BUFFERED ZERO-FLICKER) ---
        else if (current_game == STATE_PONG) {
            if (pong_game_over) {
                if (player_score >= 9) draw_game_over_modal("YOU WIN!", player_score, 0, COLOR_GREEN);
                else draw_game_over_modal("COMP WINS!", comp_score, 0, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_pong_game();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // --- PHYSICS & INPUT ---

            // LEFT/RIGHT = Decrease/Increase ball speed (0.5x - 4.0x in 0.25 steps)
            // Speed change takes effect IMMEDIATELY on the live ball
            if (ev.left_pressed) {
                float old_mult = pong_speed_mult;
                pong_speed_mult -= 0.25f;
                if (pong_speed_mult < 0.5f) pong_speed_mult = 0.5f;
                float scale = pong_speed_mult / old_mult;
                pong_vx *= scale;
                pong_vy *= scale;
                sfx_move();
            }
            if (ev.right_pressed) {
                float old_mult = pong_speed_mult;
                pong_speed_mult += 0.25f;
                if (pong_speed_mult > 4.0f) pong_speed_mult = 4.0f;
                float scale = pong_speed_mult / old_mult;
                pong_vx *= scale;
                pong_vy *= scale;
                sfx_move();
            }

            // Player Paddle Controls (ROTATE = UP, DROP = DOWN)
            if (gpio_get_level(BTN_ROTATE) == 0 && p_paddle_y > 35.0f) {
                p_paddle_y -= 7.0f;
                if (p_paddle_y < 35.0f) p_paddle_y = 35.0f;
            }
            if (gpio_get_level(BTN_DROP) == 0 && p_paddle_y < 312 - p_paddle_h - 2) {
                p_paddle_y += 7.0f;
                if (p_paddle_y > 312 - p_paddle_h - 2) p_paddle_y = 312 - p_paddle_h - 2;
            }

            // Computer AI Paddle Tracking
            float target_y = pong_by - (c_paddle_h / 2.0f);
            if (c_paddle_y < target_y - 4) c_paddle_y += 1.8f;
            else if (c_paddle_y > target_y + 4) c_paddle_y -= 1.8f;
            if (c_paddle_y < 35.0f) c_paddle_y = 35.0f;
            if (c_paddle_y > 312 - c_paddle_h - 2) c_paddle_y = 312 - c_paddle_h - 2;

            // Sub-step Physics Loop
            float old_bx = pong_bx;
            float old_by = pong_by;
            int sub_steps = 4;
            float step_vx = pong_vx / (float)sub_steps;
            float step_vy = pong_vy / (float)sub_steps;

            for (int s = 0; s < sub_steps; s++) {
                pong_bx += step_vx;
                pong_by += step_vy;

                // Bounce off Top/Bottom walls
                if (pong_by <= 36.0f) { pong_by = 36.0f; pong_vy = fabsf(pong_vy); step_vy = fabsf(step_vy); sfx_move(); }
                if (pong_by >= 304.0f) { pong_by = 304.0f; pong_vy = -fabsf(pong_vy); step_vy = -fabsf(step_vy); sfx_move(); }

                // Player Paddle Bounce
                if (pong_vx < 0 && pong_bx <= 16.0f && pong_bx >= 8.0f &&
                    pong_by + 6.0f >= p_paddle_y && pong_by <= p_paddle_y + p_paddle_h) {
                    pong_bx = 16.0f;
                    pong_vx = fabsf(pong_vx) + 0.24f;
                    step_vx = pong_vx / (float)sub_steps;
                    float hit_offset = (pong_by + 3.0f) - (p_paddle_y + p_paddle_h / 2.0f);
                    pong_vy = hit_offset * 0.18f;
                    step_vy = pong_vy / (float)sub_steps;
                    sfx_rotate();
                }

                // Computer Paddle Bounce
                if (pong_vx > 0 && pong_bx + 6.0f >= 224.0f && pong_bx <= 232.0f &&
                    pong_by + 6.0f >= c_paddle_y && pong_by <= c_paddle_y + c_paddle_h) {
                    pong_bx = 218.0f;
                    pong_vx = -fabsf(pong_vx) - 0.24f;
                    step_vx = pong_vx / (float)sub_steps;
                    float hit_offset = (pong_by + 3.0f) - (c_paddle_y + c_paddle_h / 2.0f);
                    pong_vy = hit_offset * 0.18f;
                    step_vy = pong_vy / (float)sub_steps;
                    sfx_rotate();
                }

                // Point Scored: Computer Misses -> Player Point
                if (pong_bx >= 236.0f) {
                    player_score++;
                    sfx_rotate();
                    if (player_score >= 9) pong_game_over = true;
                    else { pong_bx = 120; pong_by = 160; pong_vx = -4.8f * pong_speed_mult; pong_vy = 2.4f * pong_speed_mult; }
                    break;
                }

                // Point Scored: Player Misses -> Computer Point
                if (pong_bx <= 4.0f) {
                    comp_score++;
                    sfx_hit();
                    if (comp_score >= 9) pong_game_over = true;
                    else { pong_bx = 120; pong_by = 160; pong_vx = 4.8f * pong_speed_mult; pong_vy = -2.4f * pong_speed_mult; }
                    break;
                }
            }

            // --- FULL FRAME REDRAW INTO RAM BUFFER (zero ball trails) ---
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Court borders
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_WHITE);
            st7789_fill_rect(0, 314, SCREEN_WIDTH, 2, COLOR_WHITE);

            // Dotted centre net
            for (int y = 36; y < 314; y += 16) {
                st7789_fill_rect(119, y, 2, 8, COLOR_DARKGRAY);
            }

            // HUD: title left, speed indicator right
            draw_string(10, 10, "TENNIS", COLOR_WHITE, COLOR_BLACK, 2);
            char spd_buf[12];
            // Format speed as integer if whole number, else one decimal
            int spd_int = (int)(pong_speed_mult * 10.0f + 0.5f);
            if (spd_int % 10 == 0)
                snprintf(spd_buf, sizeof(spd_buf), "SPDx%d", spd_int / 10);
            else
                snprintf(spd_buf, sizeof(spd_buf), "SPD%.1fx", pong_speed_mult);
            draw_string(148, 12, spd_buf, COLOR_YELLOW, COLOR_BLACK, 1);
            draw_battery_indicator(214, 11);
            draw_retro_squarish_num(65, 45, player_score, COLOR_GREEN);
            draw_retro_squarish_num(153, 45, comp_score, COLOR_CYAN);

            // Paddles
            st7789_fill_rect(10,  (int)p_paddle_y, 6, p_paddle_h, COLOR_GREEN);
            st7789_fill_rect(224, (int)c_paddle_y, 6, c_paddle_h, COLOR_CYAN);

            // Ball (solid white square — no trail possible with full redraw)
            st7789_fill_rect((int)pong_bx, (int)pong_by, 6, 6, COLOR_WHITE);

            // Push complete frame
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }


        else if (current_game == STATE_SNAKE) {
            if (snake_game_over) {
                draw_game_over_modal("GAME OVER!", snake_score, 0, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_snake_game();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // Direction Input (D-Pad): latch short clicks and levels so taps during ticks are never lost
            bool req_left  = ev.left_pressed;
            bool req_right = ev.right_pressed;
            bool req_up    = (ev.rotate_short_click || gpio_get_level(BTN_ROTATE) == 0);
            bool req_down  = (ev.drop_short_click   || gpio_get_level(BTN_DROP) == 0);

            if (req_left && snake_dir != 1) snake_input_dir = 0;
            else if (req_right && snake_dir != 0) snake_input_dir = 1;
            else if (req_up && snake_dir != 3) snake_input_dir = 2;
            else if (req_down && snake_dir != 2) snake_input_dir = 3;

            snake_dir = snake_input_dir;

            // Erase Tail
            Point tail = snake[snake_len - 1];
            st7789_fill_rect(tail.x * 12, 40 + tail.y * 12, 11, 11, COLOR_BLACK);

            // Advance Body
            for (int i = snake_len - 1; i > 0; i--) snake[i] = snake[i - 1];

            // Move Head
            if (snake_dir == 0) snake[0].x--;
            else if (snake_dir == 1) snake[0].x++;
            else if (snake_dir == 2) snake[0].y--;
            else if (snake_dir == 3) snake[0].y++;

            // Wall Collisions
            if (snake[0].x < 0 || snake[0].x >= 20 || snake[0].y < 0 || snake[0].y >= 22) {
                snake_game_over = true;
            }

            // Self Collisions
            for (int i = 1; i < snake_len; i++) {
                if (snake[0].x == snake[i].x && snake[0].y == snake[i].y) snake_game_over = true;
            }

            // Eat Food
            if (snake[0].x == food_x && snake[0].y == food_y) {
                snake_score += 10;
                if (snake_len < 195) snake_len++;
                sfx_line_clear();
                spawn_food();
            }

            // Draw Snake
            for (int i = 0; i < snake_len; i++) {
                uint16_t c = (i == 0) ? COLOR_GREEN : COLOR_CYAN;
                st7789_fill_rect(snake[i].x * 12, 40 + snake[i].y * 12, 11, 11, c);
            }

            // Draw Food
            st7789_fill_rect(food_x * 12 + 1, 40 + food_y * 12 + 1, 9, 9, COLOR_RED);

            // Draw Score and Battery Indicator
            char buf[32];
            snprintf(buf, sizeof(buf), "SCORE:%04d", snake_score);
            draw_string(115, 10, buf, COLOR_WHITE, COLOR_BLACK, 1);
            draw_battery_indicator(214, 9);

            fb_present();

            vTaskDelay(pdMS_TO_TICKS(110));
            continue;
        }

        // --- STATE 6: FLAPPY BIRD (BALANCED ARCHITECTURE) ---
        else if (current_game == STATE_FLAPPY) {
            if (flappy_game_over) {
                draw_game_over_modal("GAME OVER!", flappy_score, flappy_level, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_flappy_game();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // Clear RAM Framebuffer for 100% flicker-free rendering
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Level & Gap scaling
            flappy_level = 1 + (flappy_score / 5);
            int gap_h = 95 - (flappy_level - 1) * 3;
            if (gap_h < 75) gap_h = 75; // Minimum 75px gap

            float pipe_speed = 2.2f + (flappy_level - 1) * 0.15f;
            if (pipe_speed > 3.2f) pipe_speed = 3.2f;

            // Flap Impulse (Smooth & responsive)
            if (ev.rotate_short_click || ev.drop_short_click) {
                flappy_vy = -4.2f;
                sfx_shoot();
            }

            flappy_vy += 0.28f; // Gentle Gravity
            flappy_y += flappy_vy;

            // Ceiling & Ground Boundary Collisions
            if (flappy_y < 35 || flappy_y > 300) flappy_game_over = true;

            // Scroll Pipes
            for (int p = 0; p < 2; p++) {
                pipe_x[p] -= pipe_speed;

                if (pipe_x[p] < -30) {
                    pipe_x[p] = 310;
                    pipe_gap_y[p] = (esp_random() % (250 - gap_h - 60)) + 55;
                    flappy_score++;
                    sfx_line_clear();
                }

                // Draw Pipe Top & Bottom cleanly into RAM
                if (pipe_x[p] < 240 && pipe_x[p] + 28 > 0) {
                    int px = (int)pipe_x[p];
                    int pw = 26;
                    if (px < 0) { pw += px; px = 0; }
                    st7789_fill_rect(px, 35, pw, pipe_gap_y[p] - 35, COLOR_GREEN);
                    st7789_fill_rect(px, pipe_gap_y[p] + gap_h, pw, 315 - (pipe_gap_y[p] + gap_h), COLOR_GREEN);
                }

                // Forgiving Collision check with Bird (Bird X=50..66, Y=flappy_y+2 .. flappy_y+10)
                if (pipe_x[p] <= 64 && pipe_x[p] + 26 >= 52) {
                    if (flappy_y + 2.0f < (float)pipe_gap_y[p] || (flappy_y + 10.0f) > (float)(pipe_gap_y[p] + gap_h)) {
                        flappy_game_over = true;
                    }
                }
            }

            // Draw Bird into RAM
            st7789_fill_rect(50, (int)flappy_y, 16, 12, COLOR_YELLOW);
            st7789_fill_rect(60, (int)flappy_y + 3, 4, 4, COLOR_WHITE); // Eye
            st7789_fill_rect(64, (int)flappy_y + 6, 4, 3, COLOR_ORANGE); // Beak

            // Header & Clean HUD
            draw_string(10, 10, "FLAPPY", COLOR_YELLOW, COLOR_BLACK, 2);
            char buf[32];
            snprintf(buf, sizeof(buf), "SCR:%04d LVL:%d", flappy_score, flappy_level);
            draw_string(105, 12, buf, COLOR_WHITE, COLOR_BLACK, 1);
            draw_battery_indicator(214, 11);
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_YELLOW);

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }


        // --- STATE 7: PSEUDO-3D ARCADE RACER ---
        else if (current_game == STATE_RACER) {
            if (racer_game_over) {
                draw_game_over_modal("CRASHED!", racer_score, 0, COLOR_RED);
                sfx_game_over();
                wait_modal_confirm_or_timeout();
                reset_racer_game();
                last_activity_time = esp_timer_get_time();
                continue;
            }

            // --- CONTROLS & SPEED PHYSICS ---
            int tilt = 0;
            bool turbo = false;

            // Handle Spinout timer (if hit oil slick)
            if (spinout_timer > 0) {
                spinout_timer--;
                tilt = (spinout_timer % 2 == 0) ? -1 : 1;
                player_x += (tilt < 0) ? -0.1f : 0.1f;
                racer_speed_mph -= 3.0f;
                if (racer_speed_mph < 20.0f) racer_speed_mph = 20.0f;
            } else {
                // Steering
                if (ev.left_pressed) { player_x -= 0.08f; tilt = -1; }
                if (ev.right_pressed) { player_x += 0.08f; tilt = 1; }
            }

            // Clamp player road position (-1.2 to +1.2)
            if (player_x < -1.25f) player_x = -1.25f;
            if (player_x > 1.25f) player_x = 1.25f;

            // Off-Road (Grass) Check -> Slow down & rumble!
            bool off_road = (fabsf(player_x) > 0.85f);
            float target_speed = 75.0f;

            if (gpio_get_level(BTN_ROTATE) == 0) { // ROTATE = TURBO BOOST
                target_speed = 135.0f;
                turbo = true;
            } else if (gpio_get_level(BTN_DROP) == 0) { // DROP = BRAKE
                target_speed = 30.0f;
            }

            if (off_road) {
                if (target_speed > 35.0f) target_speed = 35.0f; // Off-road speed cap
                sfx_move(); // Rumble sound
            }

            // Smooth speed acceleration/deceleration
            if (racer_speed_mph < target_speed) racer_speed_mph += 2.5f;
            else if (racer_speed_mph > target_speed) racer_speed_mph -= 3.0f;

            track_pos += (racer_speed_mph / 60.0f);
            racer_score += (uint32_t)(racer_speed_mph * 0.1f);

            // --- FULL FRAME CLEAR & RENDER INTO RAM BUFFER ---
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Horizon & Sky Backdrop (Y = 0..75)
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 43, COLOR_PURPLE);
            st7789_fill_rect(0, 75, SCREEN_WIDTH, 5, COLOR_ORANGE);

            // Sun & Mountain Peaks
            st7789_fill_rect(105, 48, 30, 27, COLOR_YELLOW);
            for (int mx = 0; mx < 240; mx += 30) {
                st7789_fill_rect(mx, 70, 15, 10, COLOR_DARKGRAY);
            }

            // Dynamic Road Curvature (Left turns, Right turns, Straightaways)
            float curve = sinf(track_pos * 0.12f) * 55.0f + cosf(track_pos * 0.04f) * 25.0f;

            // Render Pseudo-3D Perspective Road Scanlines (Y = 80..315)
            for (int y = 80; y < 312; y += 8) {
                float perspective = (float)(y - 80) / 232.0f;
                float half_w = 12.0f + perspective * 98.0f;
                float center_x = 120.0f + (curve * perspective) - (player_x * perspective * 70.0f);

                int lx = (int)(center_x - half_w);
                int rx = (int)(center_x + half_w);

                // Alternating Grass & Curb Stripes
                bool stripe = (((int)(track_pos * 3.0f + y / 16)) % 2 == 0);
                uint16_t grass_col = stripe ? COLOR_GREEN : 0x03E0;
                uint16_t curb_col  = stripe ? COLOR_RED : COLOR_WHITE;

                // Draw Left & Right Grass
                if (lx > 0) st7789_fill_rect(0, y, (lx > 240 ? 240 : lx), 8, grass_col);
                if (rx < 240) st7789_fill_rect((rx < 0 ? 0 : rx), y, 240 - rx, 8, grass_col);

                // Draw Asphalt Road
                if (lx < 240 && rx > 0) {
                    int r_start = (lx < 0) ? 0 : lx;
                    int r_end   = (rx > 240) ? 240 : rx;
                    st7789_fill_rect(r_start, y, r_end - r_start, 8, COLOR_DARKGRAY);

                    // Red/White Curbings on Road Edges
                    st7789_fill_rect(r_start, y, 4, 8, curb_col);
                    st7789_fill_rect(r_end - 4, y, 4, 8, curb_col);

                    // Dashed White Lane Dividers
                    if (stripe && perspective > 0.2f) {
                        float lane_w = half_w * 0.65f;
                        st7789_fill_rect((int)(center_x - lane_w / 2) - 1, y, 2, 8, COLOR_WHITE);
                        st7789_fill_rect((int)(center_x + lane_w / 2) - 1, y, 2, 8, COLOR_WHITE);
                    }
                }
            }

            // --- TRAFFIC & OBSTACLE PHYSICS & RENDERING ---
            for (int i = 0; i < RACER_MAX_TRAFFIC; i++) {
                if (!traffic[i].active) continue;

                // Traffic Z movement relative to player speed
                float rel_speed = (racer_speed_mph / 75.0f) - traffic[i].speed_mod;
                traffic[i].z += rel_speed * 0.022f;

                // Respawn traffic when passed (Z > 1.1) or fallen behind (Z < 0.03)
                if (traffic[i].z > 1.15f) {
                    racer_cars_passed++;
                    racer_score += 150;
                    sfx_rotate();
                    traffic[i].z = 0.08f;
                    traffic[i].lane = (esp_random() % 3) - 1; // -1, 0, +1
                    traffic[i].type = (esp_random() % 4 == 0) ? 3 : ((esp_random() % 5 == 0) ? 2 : (esp_random() % 2));
                } else if (traffic[i].z < 0.02f) {
                    traffic[i].z = 0.95f;
                    traffic[i].lane = (esp_random() % 3) - 1;
                }

                // Render traffic on screen if in view (Z = 0.08 .. 1.0)
                if (traffic[i].z >= 0.08f && traffic[i].z <= 1.0f) {
                    float p = traffic[i].z;
                    int s_y = 80 + (int)(p * 220.0f);

                    float half_w = 12.0f + p * 98.0f;
                    float center_x = 120.0f + (curve * p) - (player_x * p * 70.0f);
                    int obj_x = (int)(center_x + traffic[i].lane * (half_w * 0.65f));

                    draw_scaled_traffic_obj(obj_x, s_y, p, traffic[i].color, traffic[i].type);

                    // --- COLLISION DETECTION (Player near Z = 0.82 .. 1.0) ---
                    if (p >= 0.78f && p <= 0.98f) {
                        int player_screen_x = 104 + (int)(player_x * 40.0f);
                        if (abs(obj_x - (player_screen_x + 16)) < 24) {

                            if (traffic[i].type == 3) { // Gold Bonus Star!
                                racer_score += 500;
                                sfx_line_clear();
                                traffic[i].z = 0.08f; // Collect!
                            } else if (traffic[i].type == 2) { // Oil Slick!
                                if (spinout_timer == 0) {
                                    spinout_timer = 18;
                                    sfx_hit();
                                }
                            } else { // Traffic Car / Truck Collision -> CRASH!
                                sfx_game_over();
                                racer_lives--;
                                racer_speed_mph = 15.0f;
                                traffic[i].z = 0.08f; // Clear traffic item
                                if (racer_lives <= 0) {
                                    racer_game_over = true;
                                }
                            }
                        }
                    }
                }
            }

            // Draw Player Supercar at bottom (screen X depends on steering position)
            int p_screen_x = 104 + (int)(player_x * 40.0f);
            if (p_screen_x < 10) p_screen_x = 10;
            if (p_screen_x > 198) p_screen_x = 198;
            draw_racer_player_car(p_screen_x, 265, tilt, turbo);

            // Clean Non-Overlapping HUD
            char racer_hud[40];
            snprintf(racer_hud, sizeof(racer_hud), "%03dMPH L:%d P:%d", (int)racer_speed_mph, racer_lives, racer_cars_passed);
            draw_string(8, 10, racer_hud, turbo ? COLOR_YELLOW : COLOR_WHITE, COLOR_BLACK, 1);

            char score_buf[20];
            snprintf(score_buf, sizeof(score_buf), "S:%05lu", (unsigned long)racer_score);
            draw_string(150, 10, score_buf, COLOR_CYAN, COLOR_BLACK, 1);
            draw_battery_indicator(214, 9);

            st7789_fill_rect(0, 24, SCREEN_WIDTH, 2, COLOR_CYAN);

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }










        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
