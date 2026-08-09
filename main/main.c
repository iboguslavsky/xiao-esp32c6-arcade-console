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

#define BTN_RIGHT       GPIO_NUM_0  // D0
#define BTN_LEFT        GPIO_NUM_1  // D1
#define BTN_ROTATE      GPIO_NUM_2  // D2
#define BTN_DROP        GPIO_NUM_16 // D6

#define PIN_BUZZER      GPIO_NUM_17 // D7

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

static ButtonEvents get_button_events(void) {
    ButtonEvents ev = {0};

    ev.left_pressed  = (gpio_get_level(BTN_LEFT) == 0);
    ev.right_pressed = (gpio_get_level(BTN_RIGHT) == 0);

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
// AUDIO / BUZZER DRIVER (LEDC PWM)
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

    // No backlight pin on this board - display always on
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

static void reset_space_invaders(void) {
    inv_score = 0; inv_lives = 3; inv_game_over = false; inv_dir = 1; ship_x = 105;
    p_bullet_x = -1; p_bullet_y = -1; e_bullet_x = -1; e_bullet_y = -1;

    for (int r = 0; r < INV_ROWS; r++) {
        for (int c = 0; c < INV_COLS; c++) {
            int idx = r * INV_COLS + c;
            invaders[idx].x = 12 + c * 35;
            invaders[idx].y = 48 + r * 24;
            invaders[idx].alive = true;
            if (r == 0) { invaders[idx].color = COLOR_CYAN; invaders[idx].bmp = ALIEN_SQUID; }
            else if (r < 3) { invaders[idx].color = COLOR_GREEN; invaders[idx].bmp = ALIEN_CRAB; }
            else { invaders[idx].color = COLOR_YELLOW; invaders[idx].bmp = ALIEN_JELLY; }
        }
    }
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(10, 10, "SPACE INVADERS", COLOR_GREEN, COLOR_BLACK, 2);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_GREEN);
}

static void render_space_invaders(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "SCORE:%04lu  LIVES:%d", (unsigned long)inv_score, inv_lives);
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
} Brick;

static Brick bricks[BRK_COUNT];
static float ball_x = 120, ball_y = 200, ball_vx = 7.0f, ball_vy = -8.0f;
static int paddle_x = 96, paddle_w = 48;
static uint32_t brk_score = 0;
static int brk_lives = 3;
static bool brk_game_over = false;

static void reset_breakout(void) {
    brk_score = 0; brk_lives = 3; brk_game_over = false;
    ball_x = 120; ball_y = 200; ball_vx = 4.5f; ball_vy = -5.0f; paddle_x = 96;

    static const uint16_t row_colors[5] = { COLOR_RED, COLOR_ORANGE, COLOR_YELLOW, COLOR_GREEN, COLOR_CYAN };
    for (int r = 0; r < BRK_ROWS; r++) {
        for (int c = 0; c < BRK_COLS; c++) {
            int idx = r * BRK_COLS + c;
            bricks[idx].x = 5 + c * 33;
            bricks[idx].y = 50 + r * 14;
            bricks[idx].w = 30;
            bricks[idx].h = 10;
            bricks[idx].alive = true;
            bricks[idx].color = row_colors[r];
        }
    }
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(25, 10, "BREAKOUT", COLOR_ORANGE, COLOR_BLACK, 2);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_ORANGE);

    // Draw all 35 colorful bricks on game start
    for (int i = 0; i < BRK_COUNT; i++) {
        if (bricks[i].alive) {
            st7789_fill_rect(bricks[i].x, bricks[i].y, bricks[i].w, bricks[i].h, bricks[i].color);
        }
    }
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
    pong_vx = (esp_random() % 2 == 0) ? 2.4f : -2.4f;
    pong_vy = ((esp_random() % 100) / 70.0f) - 0.7f;
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
static int food_x = 10, food_y = 10;
static int snake_score = 0;
static bool snake_game_over = false;

static void spawn_food(void) {
    food_x = (esp_random() % 18) + 1;
    food_y = (esp_random() % 20) + 1;
}

static void reset_snake_game(void) {
    snake_len = 4; snake_dir = 1; snake_score = 0; snake_game_over = false;
    for (int i = 0; i < snake_len; i++) {
        snake[i].x = 8 - i; snake[i].y = 10;
    }
    spawn_food();
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_GREEN);
    draw_string(25, 10, "SNAKE", COLOR_GREEN, COLOR_BLACK, 2);
}

// ============================================================================
// GAME 6: FLAPPY BIRD
// ============================================================================
static float flappy_y = 150.0f, flappy_vy = 0.0f;
static float pipe_x[2] = {240, 380};
static int pipe_gap_y[2] = {120, 160};
static int flappy_score = 0;
static bool flappy_game_over = false;

static void reset_flappy_game(void) {
    flappy_y = 150.0f; flappy_vy = 0.0f; flappy_score = 0; flappy_game_over = false;
    pipe_x[0] = 240; pipe_gap_y[0] = (esp_random() % 120) + 70;
    pipe_x[1] = 380; pipe_gap_y[1] = (esp_random() % 120) + 70;
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_YELLOW);
    draw_string(20, 10, "FLAPPY BIRD", COLOR_YELLOW, COLOR_BLACK, 2);
}

// ============================================================================
// GAME 7: RETRO RACING / HIGHWAY DODGER
// ============================================================================
static int player_lane = 1; // 0=Left, 1=Center, 2=Right
static float enemy_y[2] = {-60, -180};
static int enemy_lane[2] = {0, 2};
static uint32_t racer_score = 0;
static bool racer_game_over = false;

static void reset_racer_game(void) {
    player_lane = 1; racer_score = 0; racer_game_over = false;
    enemy_y[0] = -50; enemy_lane[0] = esp_random() % 3;
    enemy_y[1] = -190; enemy_lane[1] = esp_random() % 3;
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_CYAN);
    draw_string(15, 10, "RETRO RACER", COLOR_CYAN, COLOR_BLACK, 2);
}

// Detailed Pixel Art FPV / Rear Supercar Sprite (32x24 px)
static void draw_racer_fpv_car(int x, int y, int tilt) {
    // Slicks / Wide Tires
    st7789_fill_rect(x - 2, y + 10, 6, 12, COLOR_DARKGRAY);
    st7789_fill_rect(x + 28, y + 10, 6, 12, COLOR_DARKGRAY);

    // Main Red Sports Body
    st7789_fill_rect(x + 4, y + 4, 24, 16, COLOR_RED);
    st7789_fill_rect(x + 2, y + 12, 28, 8, COLOR_RED);

    // Rear Windshield
    st7789_fill_rect(x + 8, y + 6, 16, 6, COLOR_BLUE);

    // Rear Spoiler
    st7789_fill_rect(x + 1, y, 30, 3, COLOR_RED);
    st7789_fill_rect(x + 3, y + 3, 3, 3, COLOR_BLACK);
    st7789_fill_rect(x + 26, y + 3, 3, 3, COLOR_BLACK);

    // Taillights
    st7789_fill_rect(x + 4, y + 14, 6, 3, COLOR_YELLOW);
    st7789_fill_rect(x + 22, y + 14, 6, 3, COLOR_YELLOW);

    // Exhaust & Plate
    st7789_fill_rect(x + 12, y + 16, 8, 3, COLOR_DARKGRAY);
    st7789_fill_rect(x + 6, y + 18, 4, 2, COLOR_WHITE);
    st7789_fill_rect(x + 22, y + 18, 4, 2, COLOR_WHITE);

    // Steering Bank Tilt indicator
    if (tilt < 0) st7789_fill_rect(x + 2, y + 18, 4, 4, COLOR_YELLOW);
    else if (tilt > 0) st7789_fill_rect(x + 26, y + 18, 4, 4, COLOR_YELLOW);
}


// ============================================================================
// POWER MANAGEMENT & HARDWARE LOCKED DEEP SLEEP (0% GLOW)
// ============================================================================
static void enter_power_down_deep_sleep(void) {
    sfx_powerdown();

    // 1. ST7789 Software Power-Down Sequence
    st7789_cmd(0x28); // DISPOFF - turn off display
    vTaskDelay(pdMS_TO_TICKS(50));
    st7789_cmd(0x10); // SLPIN - LCD controller sleep mode
    vTaskDelay(pdMS_TO_TICKS(120)); // 120ms for internal rails to discharge

    // 2. Assert RST LOW = ST7789 hardware reset (disables all internal LCD circuits)
    //    DO NOT hold DC - it is driven by the SPI peripheral on every transaction
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // 3. Lock RST at 0V for entire deep sleep - ST7789 init will re-assert on wakeup
    gpio_hold_en((gpio_num_t)PIN_NUM_RST);

    // Wait for any held button to be released
    while (gpio_get_level(BTN_DROP) == 0) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Wakeup on any button press (GPIO0=RIGHT, GPIO1=LEFT, GPIO2=ROTATE)
    uint64_t mask = (1ULL << BTN_RIGHT) | (1ULL << BTN_LEFT) | (1ULL << BTN_ROTATE);
    esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
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
    fb_present();
}

static void draw_arcade_menu(void) {
    st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
    draw_string(20, 10, "ARCADE CONSOLE", COLOR_CYAN, COLOR_BLACK, 2);
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
    ESP_LOGI(TAG, "Starting XIAO Arcade Console...");

    // Release RST hold from previous deep sleep session
    gpio_hold_dis((gpio_num_t)PIN_NUM_RST);

    gpio_config_t btn_config = {
        .pin_bit_mask = (1ULL << BTN_LEFT) | (1ULL << BTN_RIGHT) | (1ULL << BTN_ROTATE) | (1ULL << BTN_DROP),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_config);

    gpio_config_t out_config = {
        .pin_bit_mask = (1ULL << PIN_NUM_DC) | (1ULL << PIN_NUM_RST),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&out_config);

    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
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

    buzzer_init();
    st7789_init();

    current_game = STATE_MENU;
    draw_arcade_menu();

    int64_t last_drop_time = esp_timer_get_time();
    int64_t last_activity_time = esp_timer_get_time(); // 5-minute inactivity timer tracker

    while (1) {
        // Read non-blocking input events
        ButtonEvents ev = get_button_events();

        // Activity check: Reset 5-min inactivity timer on any button interaction
        if (ev.left_pressed || ev.right_pressed || ev.rotate_short_click || ev.drop_short_click ||
            gpio_get_level(BTN_ROTATE) == 0 || gpio_get_level(BTN_DROP) == 0) {
            last_activity_time = esp_timer_get_time();
        }

        // AUTO-SHUTDOWN: 5 minutes (300,000,000 us) of inactivity -> Deep Sleep
        if ((esp_timer_get_time() - last_activity_time) >= 300000000ULL) {
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
                draw_string(15, 130, "GAME OVER!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(15, 160, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_tetris_game();
                last_drop_time = esp_timer_get_time(); continue;
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
        }

        // --- STATE 2: SPACE INVADERS (60 FPS DOUBLE BUFFERED ZERO-FLICKER) ---
        else if (current_game == STATE_INVADERS) {
            if (inv_game_over) {
                draw_string(25, 140, "GAME OVER!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                fb_present();
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_space_invaders(); continue;
            }

            // Clear RAM Framebuffer for 100% flicker-free rendering
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Header & Clean Non-Overlapping HUD
            draw_string(10, 10, "INVADERS", COLOR_GREEN, COLOR_BLACK, 2);
            char inv_buf[32];
            snprintf(inv_buf, sizeof(inv_buf), "SCR:%04lu L:%d", (unsigned long)inv_score, inv_lives);
            draw_string(135, 12, inv_buf, COLOR_WHITE, COLOR_BLACK, 1);
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

            // Enemy Missile Firing
            if (e_bullet_y < 0 && (esp_random() % 15 == 0)) {
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

            // Move Invader Matrix
            static int move_timer = 0;
            if (++move_timer >= 6) {
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
                draw_string(25, 140, "GAME OVER!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                fb_present();
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_breakout(); continue;
            }

            // Clear RAM Framebuffer
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);

            // Header & Clean Non-Overlapping HUD
            draw_string(10, 10, "BREAKOUT", COLOR_CYAN, COLOR_BLACK, 2);
            char brk_buf[32];
            snprintf(brk_buf, sizeof(brk_buf), "SCR:%04lu L:%d", (unsigned long)brk_score, brk_lives);
            draw_string(135, 12, brk_buf, COLOR_WHITE, COLOR_BLACK, 1);
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_CYAN);

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

            // Victory check: All bricks destroyed -> Reset bricks!
            if (!any_alive) {
                sfx_line_clear();
                reset_breakout();
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

                // Brick Collisions
                for (int i = 0; i < BRK_COUNT; i++) {
                    if (bricks[i].alive) {
                        if (ball_x + 6.0f >= (float)bricks[i].x && ball_x <= (float)(bricks[i].x + bricks[i].w) &&
                            ball_y + 6.0f >= (float)bricks[i].y && ball_y <= (float)(bricks[i].y + bricks[i].h)) {
                            bricks[i].alive = false;
                            ball_vy = -ball_vy;
                            step_vy = -step_vy;
                            brk_score += 20;
                            sfx_rotate(); // Short single beep - no freeze!
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
                        ball_vx = (esp_random() % 2 == 0) ? 4.5f : -4.5f;
                        ball_vy = -5.0f;
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





        // --- STATE 4: PONG / TENNIS ---
        else if (current_game == STATE_PONG) {
            if (pong_game_over) {
                if (player_score >= 9) draw_string(25, 140, "YOU WIN!", COLOR_GREEN, COLOR_BLACK, 2);
                else draw_string(25, 140, "COMP WINS!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_pong_game(); continue;
            }

            // Player Paddle Controls (ROTATE = UP, DROP = DOWN)
            float old_py = p_paddle_y;
            if (gpio_get_level(BTN_ROTATE) == 0 && p_paddle_y > 35.0f) {
                p_paddle_y -= 7.0f;
                if (p_paddle_y < 35.0f) p_paddle_y = 35.0f;
            }
            if (gpio_get_level(BTN_DROP) == 0 && p_paddle_y < 312 - p_paddle_h - 2) {
                p_paddle_y += 7.0f;
                if (p_paddle_y > 312 - p_paddle_h - 2) p_paddle_y = 312 - p_paddle_h - 2;
            }

            if (old_py != p_paddle_y) {
                st7789_fill_rect(10, (int)old_py, 6, p_paddle_h, COLOR_BLACK);
                st7789_fill_rect(10, (int)p_paddle_y, 6, p_paddle_h, COLOR_GREEN);
            }

            // Computer AI Paddle Tracking
            float old_cy = c_paddle_y;
            float target_y = pong_by - (c_paddle_h / 2.0f);
            if (c_paddle_y < target_y - 4) c_paddle_y += 1.8f;
            else if (c_paddle_y > target_y + 4) c_paddle_y -= 1.8f;

            if (c_paddle_y < 35.0f) c_paddle_y = 35.0f;
            if (c_paddle_y > 312 - c_paddle_h - 2) c_paddle_y = 312 - c_paddle_h - 2;

            if (old_cy != c_paddle_y) {
                st7789_fill_rect(224, (int)old_cy, 6, c_paddle_h, COLOR_BLACK);
                st7789_fill_rect(224, (int)c_paddle_y, 6, c_paddle_h, COLOR_CYAN);
            }


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

                // Player Paddle Bounce (Left side X=10)
                if (pong_vx < 0 && pong_bx <= 16.0f && pong_bx >= 8.0f &&
                    pong_by + 6.0f >= p_paddle_y && pong_by <= p_paddle_y + p_paddle_h) {
                    pong_bx = 16.0f;
                    pong_vx = fabsf(pong_vx) + 0.12f; // Slightly accelerate
                    step_vx = pong_vx / (float)sub_steps;
                    float hit_offset = (pong_by + 3.0f) - (p_paddle_y + p_paddle_h / 2.0f);
                    pong_vy = hit_offset * 0.18f;
                    step_vy = pong_vy / (float)sub_steps;
                    sfx_rotate();
                }

                // Computer Paddle Bounce (Right side X=224)
                if (pong_vx > 0 && pong_bx + 6.0f >= 224.0f && pong_bx <= 232.0f &&
                    pong_by + 6.0f >= c_paddle_y && pong_by <= c_paddle_y + c_paddle_h) {
                    pong_bx = 218.0f;
                    pong_vx = -fabsf(pong_vx) - 0.12f;
                    step_vx = pong_vx / (float)sub_steps;
                    float hit_offset = (pong_by + 3.0f) - (c_paddle_y + c_paddle_h / 2.0f);
                    pong_vy = hit_offset * 0.18f;
                    step_vy = pong_vy / (float)sub_steps;
                    sfx_rotate();
                }

                // Point Scored: Computer Misses (Right) -> Player Point
                if (pong_bx >= 236.0f) {
                    player_score++;
                    sfx_line_clear();
                    draw_retro_squarish_num(65, 45, player_score, COLOR_GREEN);
                    if (player_score >= 9) pong_game_over = true;
                    else {
                        pong_bx = 120; pong_by = 160;
                        pong_vx = -2.4f; pong_vy = 1.2f;
                    }
                    break;
                }

                // Point Scored: Player Misses (Left) -> Computer Point
                if (pong_bx <= 4.0f) {
                    comp_score++;
                    sfx_hit();
                    draw_retro_squarish_num(153, 45, comp_score, COLOR_CYAN);
                    if (comp_score >= 9) pong_game_over = true;
                    else {
                        pong_bx = 120; pong_by = 160;
                        pong_vx = 2.4f; pong_vy = -1.2f;
                    }
                    break;
                }
            }


            // Draw Anti-Aliased Sub-pixel Ball
            draw_subpixel_ball(pong_bx, pong_by, old_bx, old_by);

            // Automatic Score Digit Repair (Restores missing segments when ball passes near digits)
            if (pong_by <= 85.0f) {
                draw_retro_squarish_num(65, 45, player_score, COLOR_GREEN);
                draw_retro_squarish_num(153, 45, comp_score, COLOR_CYAN);
            }

            // Re-render Paddles, Court Borders, and Dotted Net
            st7789_fill_rect(10, (int)p_paddle_y, 6, p_paddle_h, COLOR_GREEN);
            st7789_fill_rect(224, (int)c_paddle_y, 6, c_paddle_h, COLOR_CYAN);

            // Re-assert top and bottom white border lines so paddle erases never clip them
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_WHITE);
            st7789_fill_rect(0, 312, SCREEN_WIDTH, 2, COLOR_WHITE);

            for (int y = 40; y < 310; y += 16) {
                st7789_fill_rect(119, y, 2, 8, COLOR_DARKGRAY);
            }

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // --- STATE 5: SNAKE ---
        else if (current_game == STATE_SNAKE) {
            if (snake_game_over) {
                draw_string(25, 140, "GAME OVER!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_snake_game(); continue;
            }

            // Direction Input (D-Pad)
            if (ev.left_pressed && snake_dir != 1) snake_dir = 0;
            else if (ev.right_pressed && snake_dir != 0) snake_dir = 1;
            else if (gpio_get_level(BTN_ROTATE) == 0 && snake_dir != 3) snake_dir = 2;
            else if (gpio_get_level(BTN_DROP) == 0 && snake_dir != 2) snake_dir = 3;

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

            // Draw Score
            char buf[32];
            snprintf(buf, sizeof(buf), "SCORE:%04d", snake_score);
            draw_string(140, 10, buf, COLOR_WHITE, COLOR_BLACK, 1);

            vTaskDelay(pdMS_TO_TICKS(110));
            continue;
        }

        // --- STATE 6: FLAPPY BIRD (60 FPS DOUBLE BUFFERED ZERO-FLICKER) ---
        else if (current_game == STATE_FLAPPY) {
            if (flappy_game_over) {
                draw_string(25, 140, "GAME OVER!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                fb_present();
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_flappy_game(); continue;
            }

            // Clear RAM Framebuffer for 100% flicker-free rendering
            st7789_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
            draw_string(20, 10, "FLAPPY BIRD", COLOR_YELLOW, COLOR_BLACK, 2);
            st7789_fill_rect(0, 32, SCREEN_WIDTH, 2, COLOR_YELLOW);

            // Flap Impulse
            if (ev.rotate_short_click || ev.drop_short_click) {
                flappy_vy = -5.5f;
                sfx_shoot();
            }

            flappy_vy += 0.42f; // Gravity
            flappy_y += flappy_vy;

            if (flappy_y < 35 || flappy_y > 300) flappy_game_over = true;

            // Scroll Pipes
            for (int p = 0; p < 2; p++) {
                pipe_x[p] -= 3.0f;
                if (pipe_x[p] < -30) {
                    pipe_x[p] = 240;
                    pipe_gap_y[p] = (esp_random() % 120) + 70;
                    flappy_score++;
                    sfx_line_clear();
                }

                // Draw Pipe Top & Bottom cleanly into RAM
                if (pipe_x[p] < 240 && pipe_x[p] + 28 > 0) {
                    int px = (int)pipe_x[p];
                    int pw = 26;
                    if (px < 0) { pw += px; px = 0; }
                    st7789_fill_rect(px, 35, pw, pipe_gap_y[p] - 35, COLOR_GREEN);
                    st7789_fill_rect(px, pipe_gap_y[p] + 70, pw, 315 - (pipe_gap_y[p] + 70), COLOR_GREEN);
                }

                // Collision check with Bird (X=50..66)
                if (pipe_x[p] <= 66 && pipe_x[p] + 26 >= 50) {
                    if (flappy_y < pipe_gap_y[p] || flappy_y + 12 > pipe_gap_y[p] + 70) {
                        flappy_game_over = true;
                    }
                }
            }

            // Draw Bird into RAM
            st7789_fill_rect(50, (int)flappy_y, 16, 12, COLOR_YELLOW);
            st7789_fill_rect(60, (int)flappy_y + 3, 4, 4, COLOR_WHITE); // Eye

            char buf[32];
            snprintf(buf, sizeof(buf), "SCORE:%04d", flappy_score);
            draw_string(140, 10, buf, COLOR_WHITE, COLOR_BLACK, 1);

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }


        // --- STATE 7: PSEUDO-3D FPV ARCADE RACER ---
        else if (current_game == STATE_RACER) {

            if (racer_game_over) {
                draw_string(25, 140, "CRASHED!", COLOR_RED, COLOR_BLACK, 2);
                draw_string(25, 170, "PRESS ROTATE", COLOR_WHITE, COLOR_BLACK, 1);
                sfx_game_over();
                while (gpio_get_level(BTN_ROTATE) != 0) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(200)); reset_racer_game(); continue;
            }

            // Controls: Steering & Turbo Boost
            static float p_x_pos = 0.0f; // -1.0 (Left) to +1.0 (Right)
            int tilt = 0;
            if (ev.left_pressed) { p_x_pos -= 0.12f; tilt = -1; }
            if (ev.right_pressed) { p_x_pos += 0.12f; tilt = 1; }
            if (p_x_pos < -1.2f) p_x_pos = -1.2f;
            if (p_x_pos > 1.2f) p_x_pos = 1.2f;

            float speed = 1.0f;
            if (gpio_get_level(BTN_ROTATE) == 0) speed = 2.0f; // Turbo boost!
            else if (gpio_get_level(BTN_DROP) == 0) speed = 0.4f; // Brake

            racer_score += (uint32_t)(speed * 3.0f);

            static float road_z = 0.0f;
            road_z += speed * 0.18f;

            // Clear Sky & Render Horizon Backdrop (Y = 0..80)
            st7789_fill_rect(0, 35, SCREEN_WIDTH, 45, COLOR_PURPLE);
            st7789_fill_rect(0, 75, SCREEN_WIDTH, 5, COLOR_ORANGE);

            // Sun & Horizon Mountain Peaks
            st7789_fill_rect(105, 50, 30, 25, COLOR_YELLOW);
            for (int mx = 0; mx < 240; mx += 30) {
                st7789_fill_rect(mx, 70, 15, 10, COLOR_DARKGRAY);
            }

            // Render Pseudo-3D Perspective Road Scanlines (Y = 80..320)
            float curve = sinf(road_z * 0.3f) * 45.0f;

            for (int y = 80; y < 310; y += 8) {
                float perspective = (float)(y - 80) / 230.0f;
                float half_w = 14.0f + perspective * 96.0f;
                float center_x = 120.0f + (curve * perspective) - (p_x_pos * perspective * 75.0f);

                int lx = (int)(center_x - half_w);
                int rx = (int)(center_x + half_w);

                // Alternating Grass Color (Green / Dark Green)
                bool stripe = (((int)(road_z * 4.0f + y / 16)) % 2 == 0);
                uint16_t grass_col = stripe ? COLOR_GREEN : 0x03E0;
                uint16_t curb_col  = stripe ? COLOR_RED : COLOR_WHITE;

                // Draw Left & Right Grass
                if (lx > 0) st7789_fill_rect(0, y, lx, 8, grass_col);
                if (rx < 240) st7789_fill_rect(rx, y, 240 - rx, 8, grass_col);

                // Draw Asphalt Road
                if (lx < 240 && rx > 0) {
                    int r_start = (lx < 0) ? 0 : lx;
                    int r_end   = (rx > 240) ? 240 : rx;
                    st7789_fill_rect(r_start, y, r_end - r_start, 8, COLOR_DARKGRAY);

                    // Red/White Curbings on Road Edges
                    st7789_fill_rect(r_start, y, 4, 8, curb_col);
                    st7789_fill_rect(r_end - 4, y, 4, 8, curb_col);

                    // Dashed Center Lane Marker
                    if (stripe && perspective > 0.2f) {
                        st7789_fill_rect((int)center_x - 1, y, (int)(2.0f * perspective) + 1, 8, COLOR_WHITE);
                    }
                }
            }

            // Update & Render 3D Scaling Enemy Cars
            for (int e = 0; e < 2; e++) {
                enemy_y[e] += speed * 0.04f;
                if (enemy_y[e] > 1.0f) {
                    enemy_y[e] = 0.05f;
                    enemy_lane[e] = (esp_random() % 3) - 1; // -1, 0, +1
                }

                float e_perspective = enemy_y[e];
                int e_screen_y = 80 + (int)(e_perspective * 220.0f);

                if (e_screen_y >= 80 && e_screen_y <= 300) {
                    float e_half_w = 14.0f + e_perspective * 96.0f;
                    float e_center_x = 120.0f + (curve * e_perspective) - (p_x_pos * e_perspective * 75.0f);
                    int e_x = (int)(e_center_x + enemy_lane[e] * (e_half_w * 0.65f));

                    int w = (int)(6.0f + e_perspective * 24.0f);
                    int h = (int)(4.0f + e_perspective * 16.0f);

                    st7789_fill_rect(e_x - w / 2, e_screen_y, w, h, COLOR_CYAN);
                    st7789_fill_rect(e_x - w / 4, e_screen_y + h / 4, w / 2, h / 2, COLOR_WHITE);

                    // Collision check near player (screen Y=250..290)
                    if (e_screen_y >= 250 && e_screen_y <= 290) {
                        int player_screen_x = 104;
                        if (abs((e_x - w / 2) - player_screen_x) < 22) {
                            racer_game_over = true;
                            sfx_hit();
                        }
                    }
                }
            }

            // Draw Detailed FPV / Rear Supercar at bottom center
            draw_racer_fpv_car(104, 265, tilt);

            // Draw HUD
            char buf[32];
            snprintf(buf, sizeof(buf), "SPD:%03d MPH  DIST:%05lu", (int)(speed * 75), (unsigned long)racer_score);
            draw_string(10, 10, buf, COLOR_WHITE, COLOR_BLACK, 1);

            // Push complete frame atomically over 40MHz SPI DMA
            fb_present();

            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }










        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
