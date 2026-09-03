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
| D0 | GPIO0 | BTN_RIGHT / BATTERY_ADC | LP GPIO — deep sleep wakeup source & ADC1_CH0 battery voltage divider |
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

## Battery Voltage Monitor & Circuit (250mAh LiPo)

- **ADC Channel:** `ADC_UNIT_1`, `ADC_CHANNEL_0` on **D0 (GPIO0)** multiplexed with `BTN_RIGHT`.
- **Resistor Divider:**
  - $R_1 = 180\text{k}\Omega$ between **VBAT** (LiPo positive) and **D0 (GPIO0)**.
  - $R_2 = 180\text{k}\Omega$ between **D0 (GPIO0)** and **GND**.
  - Tactile switch connects **D0 (GPIO0)** directly to **GND** in parallel across $R_2$.
- **Voltage Divider Ratio:** $\frac{R_2}{R_1 + R_2} = \frac{180\text{k}\Omega}{360\text{k}\Omega} = \mathbf{0.5000}$.
  - 4.20V battery $\rightarrow$ 2.10V at D0 (safe for 3.3V ADC).
  - 3.70V battery $\rightarrow$ 1.85V at D0.
  - 3.25V cutoff $\rightarrow$ 1.625V at D0.
- **Button Press Discrimination:** When BTN_RIGHT is pressed, D0 is shorted to GND ($<0.4\text{V}$). The firmware detects the press and ignores ADC samples during presses to preserve the battery reading cache.
- **Persistent HUD Icon:** 18x9 px battery casing with positive terminal tip, rendered consistently across Arcade Menu and all 7 games (Tetris, Invaders, Breakout, Tennis, Snake, Flappy, Racer):
  - 3 Green Bars: $>70\%$ charge ($\ge 3.88\text{V}$)
  - 2 Yellow Bars: $35\% - 70\%$ charge ($3.56\text{V} - 3.88\text{V}$)
  - 1 Red Bar: $10\% - 35\%$ charge ($3.34\text{V} - 3.56\text{V}$)
  - Blinking Empty Red Bar: $<10\%$ critical warning ($< 3.34\text{V}$)

---

## Button Controls Per Game

| Button | Tetris | Invaders | Breakout | Tennis | Snake | Flappy | Racer | Menu |
|--------|--------|----------|----------|--------|-------|--------|-------|------|
| LEFT | Move left | Move left | Paddle left | — | Turn left | — | Steer left | — |
| RIGHT | Move right | Move right | Paddle right | — | Turn right | — | Steer right | — |
| ROTATE short | Rotate | Shoot | Speed +0.25x | Paddle up | Turn up | Flap | TURBO (135 MPH) + Flames | Scroll up |
| DROP short | Hard drop | — | Speed -0.25x | Paddle down | Turn down | Flap | BRAKE (30 MPH) | Scroll down |
| ROTATE long | — | — | — | — | — | — | — | Launch game |
| DROP long (1s) | Power off | Power off | Power off | Power off | Power off | Power off | Power off | Power off |
| LEFT / RIGHT | — | — | — | Speed -/+0.25x | — | — | — | Select game |

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

---

## Games & Progression

| # | Game | Features & Mechanics | Notes |
|---|------|----------------------|-------|
| 1 | Tetris | Speed scales with score | Full rotation, wall kicks |
| 2 | Space Invaders | **Waves 1..10+** | Aliens start lower, move faster, fire aggressively. Wave clear banner + bonus life |
| 3 | Breakout | **Levels 1..10+** | 10 unique layouts (Rainbow, Checkerboard, Pyramid, Silver 2-Hit, Fortress, Stripes, Ring Vault, Low Wall, Staggered, Omega Vault). Level clear banner + bonus life |
| 4 | Tennis (Pong) | Speed control (0.5x–4.0x) | Sub-step physics; LEFT/RIGHT = speed; 7-seg score |
| 5 | Snake | Grid-based | Speed increases as snake grows |
| 6 | Flappy Bird | **Balanced Controls & Level Progression** | 95px initial pipe gap (scales to 75px), 170px pipe spacing, gentle gravity (0.28), smooth flap (-4.2), 2px collision margin, LVL:X in HUD |
| 7 | Retro Racer | Full 3D OutRun-style Arcade Engine | Dynamic curves, 3D scaling traffic (Cars, Trucks), Oil Slicks (spinout), Gold Bonus Stars (+500pts), Off-road grass physics, Turbo Flames (135 MPH), 3-life system |

---

## Git History

```
ec6d3bf  Rework Racer into full 3D OutRun-style arcade racer
c209d48  Update CONTEXT.md with multi-level progression details
547f968  Add 10+ multi-level progression for Breakout & Space Invaders
8111087  Add CONTEXT.md project documentation & deep sleep wakeup fix
dc53037  Add TFT MOSFET power control (D7/GPIO17) + buzzer on D5/GPIO23
c296d0f  Tennis: double ball speed (vx 2.4->4.8, vy 0.7->1.4, accel 0.12->0.24)
f9689dc  Fix all games: full-frame double-buffering across all 7 games
51bd813  Initial commit: XIAO ESP32-C6 arcade console firmware
```

---

## File Structure

```
xiao_esp32c6_tetris/
├── CONTEXT.md          <- this file
├── main/
│   └── main.c          <- ALL code: games + display driver + power mgmt (~2000 lines)
├── platformio.ini
└── sdkconfig.defaults
```
