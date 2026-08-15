# XIAO ESP32-C6 Arcade Console — Project Context

> **Purpose:** Drop this file into any AI session to restore full project context instantly.
> Last updated: 2026-08-15

---

## Hardware

| Item | Detail |
|------|--------|
| MCU Board | Seeed Studio XIAO ESP32-C6 |
| Display | GMT020-02-7P — 2.0" ST7789 TFT LCD, 240x320, SPI |
| Backlight | NOT via GPIO — permanently wired to VCC on this board |
| TFT Power | MOSFET on D7 (GPIO17): HIGH = TFT on, LOW = TFT off |
| Buzzer | Piezo on D5 (GPIO23) via LEDC PWM |
| Buttons | 4x tactile switches, active LOW, internal pull-up |

---

## Pin Map

| Pin | GPIO | Function | Notes |
|-----|------|----------|-------|
| D0 | GPIO0 | BTN_RIGHT | LP GPIO — deep sleep wakeup source |
| D1 | GPIO1 | BTN_LEFT | LP GPIO — deep sleep wakeup source |
| D2 | GPIO2 | BTN_ROTATE | LP GPIO — deep sleep wakeup source |
| D3 | GPIO21 | SPI MOSI (SDA) | |
| D4 | GPIO22 | SPI CLK (SCL) | |
| D5 | GPIO23 | Piezo Buzzer (LEDC PWM) | Was backlight pin (no backlight on this board) |
| D6 | GPIO16 | BTN_DROP | NOT an LP GPIO — cannot be wakeup source |
| D7 | GPIO17 | TFT Power MOSFET gate | HIGH=TFT on, LOW=TFT off |
| D8 | GPIO19 | SPI DC | |
| D9 | GPIO20 | SPI CS | |
| D10 | GPIO18 | SPI RST | |

> CRITICAL: D6/GPIO16 (BTN_DROP) is NOT an LP GPIO on ESP32-C6.
> It CANNOT be a deep sleep wakeup source.
> Wakeup is via D0/D1/D2 (RIGHT/LEFT/ROTATE) only.

---

## Button Controls Per Game

| Button | Tetris | Invaders | Breakout | Tennis | Snake | Flappy | Menu |
|--------|--------|----------|----------|--------|-------|--------|------|
| LEFT | Move left | Move left | Paddle left | — | Turn left | — | — |
| RIGHT | Move right | Move right | Paddle right | — | Turn right | — | — |
| ROTATE short | Rotate | Shoot | Speed +0.25x | Paddle up | Turn up | Flap | Scroll up |
| DROP short | Hard drop | — | Speed -0.25x | Paddle down | Turn down | — | Scroll down |
| ROTATE long | — | — | — | — | — | — | Launch game |
| DROP long (1s) | Power off | Power off | Power off | Power off | Power off | Power off | Power off |
| LEFT / RIGHT | — | — | — | Speed -/+0.25x | — | — | Select game |

---

## Display / Rendering Architecture

- **Framebuffer:** 153.6 KB RAM double-buffer (240x320x2 bytes), DMA-capable
- **Pattern every tick:** `st7789_fill_rect(0,0,240,320,BLACK)` -> draw all -> `fb_present()`
- **No XOR, no sprite erasure** — full clear+redraw = zero trails/ghosting
- **SPI:** 40 MHz, SPI2_HOST, polling mode, 4096-byte DMA chunks in fb_present()

---

## Power Management

### Deep Sleep Entry (hold DROP ~1 second)
1. DISPOFF + SLPIN — ST7789 software shutdown
2. D7 LOW — MOSFET cuts TFT VCC (no backfeed possible with VCC gone)
3. gpio_hold_en(PIN_TFT_PWR) — holds MOSFET off during sleep
4. RST LOW + gpio_hold_en(PIN_NUM_RST) — belt-and-suspenders
5. gpio_pullup_en() on BTN_RIGHT/LEFT/ROTATE — LP domain pull-ups must be explicit
6. Wait: DROP release -> all wakeup buttons HIGH -> 500ms debounce
7. esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW) -> sleep

### Wakeup (press RIGHT, LEFT, or ROTATE)
- app_main() called fresh (deep sleep = full reboot on ESP32-C6)
- gpio_hold_dis(PIN_TFT_PWR) + gpio_hold_dis(PIN_NUM_RST)
- D7 HIGH -> MOSFET powers TFT -> 150ms rail stabilization -> SPI init -> ST7789 init -> menu

### Hard-Won Lessons (DO NOT repeat these mistakes)
- DO NOT call spi_bus_free() or gpio_reset_pin() on SPI pins before sleep — corrupts LP wakeup subsystem
- DO NOT gpio_hold_en(DC pin) — SPI peripheral needs DC; holding it causes SPI to hang on wakeup
- ESP_GPIO_WAKEUP_GPIO_LOW is LEVEL-triggered — all wakeup pins must be clearly HIGH before
  esp_deep_sleep_start() or it wakes immediately (spurious wakeup on button release)
- ESP32-C6 LP GPIOs are GPIO0-7 only — BTN_DROP (GPIO16) cannot be a wakeup source
- gpio_pullup_en() must be called explicitly before sleep — LP domain pull-ups don't auto-persist

---

## Games & Multi-Level Systems

| # | Game | Levels / Progression | Notes |
|---|------|----------------------|-------|
| 1 | Tetris | Speed scales with score | Full rotation, wall kicks |
| 2 | Space Invaders | **Waves 1..10+** | Aliens start lower, move faster, fire aggressively. Wave clear banner + bonus life |
| 3 | Breakout | **Levels 1..10+** | 10 unique brick layouts (Rainbow, Checkerboard, Pyramid, Silver 2-Hit, Fortress, Stripes, Ring Vault, Low Wall, Staggered, Omega Vault). Level clear banner + bonus life |
| 4 | Tennis (Pong) | Speed control (0.5x–4.0x) | Sub-step physics; LEFT/RIGHT = speed; 7-seg score |
| 5 | Snake | Grid-based | Speed increases as snake grows |
| 6 | Flappy Bird | Distance score | Pipe obstacles |
| 7 | Racer | WIP / placeholder | Moving background, FPV |

---

## Git History

```
547f968  Add 10+ multi-level progression for Breakout & Space Invaders
8111087  Add CONTEXT.md project documentation & deep sleep wakeup fix
dc53037  Add TFT MOSFET power control (D7/GPIO17) + buzzer on D5/GPIO23
c296d0f  Tennis: double ball speed (vx 2.4->4.8, vy 0.7->1.4, accel 0.12->0.24)
f9689dc  Fix all games: full-frame double-buffering across all 7 games
51bd813  Initial commit: XIAO ESP32-C6 arcade console firmware
```

---

## Toolchain / Build

- PlatformIO: `pio run` / `pio run -t upload`
- Framework: ESP-IDF 5.5.4
- Compiler: riscv32-esp-elf-gcc 14.2.0+20260121

---

## Outstanding Work

1. Racer rework — moving background, race car sprites, FPV perspective (requested, not started)
2. ZX Spectrum emulator — feasibility discussion only, not started

---

## File Structure

```
xiao_esp32c6_tetris/
├── CONTEXT.md          <- this file
├── main/
│   └── main.c          <- ALL code: games + display driver + power mgmt (~1950 lines)
├── platformio.ini
└── sdkconfig.defaults
```
