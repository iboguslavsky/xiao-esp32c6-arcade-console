# 🕹️ XIAO ESP32-C6 Retro Arcade Console

A pocket-sized, battery-powered 8-bit & 16-bit style arcade console built around the **Seeed Studio XIAO ESP32-C6** (RISC-V 160MHz) and a vibrant **2.0" ST7789 IPS LCD (240x320)**.

Designed for true portable retro gaming, featuring **7 built-in arcade games**, dynamic multi-level progression, piezophonic sound effects, sub-microamp deep sleep power management, and hardware battery level monitoring.

---

## 📷 Photos & Builds
<!-- Add project photos, enclosure, and action shots below -->
![Console Overview](photos/console_front.jpg)
*Figure 1: Assembled Console Front View*

![Internal Wiring](photos/internals.jpg)
*Figure 2: Component Layout & Wiring*

---

## ✨ Features

- **7 Complete Arcade Games:**
  1. **Tetris** – Smooth tetromino rotation, wall-kicks, line clears, and score-based drop acceleration.
  2. **Space Invaders (10+ Waves)** – Advancing alien armada, escalating difficulty, bullet collision detection, and bonus life rewards.
  3. **Breakout (10 Unique Layouts)** – Rainbow, Checkerboard, Pyramid, Silver 2-Hit bricks, Fortress, and Omega Vault with custom level clear banners.
  4. **Tennis (Pong)** – High-speed sub-step physics, dynamic ball acceleration, and adjustable paddle speed ($0.5\times$ to $4.0\times$).
  5. **Snake** – Classic greed grid mechanics with speed scaling as length increases.
  6. **Flappy Bird** – Hand-tuned physics (gentle gravity, smooth flap impulse, generous 95px initial pipe gaps scaling across levels).
  7. **Retro Racer** – Full pseudo-3D OutRun-style arcade engine with perspective road curving, oncoming traffic (Cars & Trucks), oil slicks, bonus stars, turbo boost (135 MPH) with exhaust flames, and off-road grass deceleration.
- **Flicker-Free Display Pipeline:** Full 153.6 KB RAM double-buffer (240x320x2 bytes RGB565) pushed via DMA-capable 40MHz hardware SPI.
- **Real-Time Battery HUD:** Persistent battery fuel gauge casing with 3-color charge stages (Green / Yellow / Red / Blinking Low) rendered across all game screens and the arcade selector menu.
- **Smart Power Cutoff & Sleep:**
  - Dedicated P-channel / N-channel MOSFET circuit cuts LCD VCC to eliminate backlight/display quiescent drain.
  - Sub-microamp ESP32-C6 deep sleep with instant wake-up on any gaming button press.
  - Automatic 5-minute inactivity shutdown timer.

---

## 🛠️ Hardware Components

| Component | Specifications | Source / Notes |
|---|---|---|
| **MCU Board** | Seeed Studio XIAO ESP32-C6 | RISC-V Single-core @ 160MHz, 320KB SRAM, 4MB Flash, USB-C |
| **Display** | 2.0" IPS TFT LCD (GMT020-02-7P) | 240x320 Resolution, ST7789V controller, 4-wire SPI interface |
| **Battery** | 3.7V 250mAh LiPo (Lithium Polymer) | Standard single-cell LiPo with on-board JST or direct solder pads |
| **Power Switch MOSFET** | P-MOSFET / N-MOSFET power switch | Gate controlled by GPIO17 (D7) to cut TFT power rail in sleep |
| **Sound** | Passive Piezo Buzzer (12mm) | Driven via hardware LEDC PWM timer on GPIO23 (D5) |
| **Controls** | 4x Tactile Momentary Pushbuttons | LEFT, RIGHT, ROTATE / ACTION, DROP / SELECT |
| **Battery Divider** | $2\times 180\text{k}\Omega$ 1% SMD Resistors + 100nF Cap | Voltage divider on VBAT multiplexed onto GPIO0 (D0) |

---

## 🔌 Pinout & Wiring Map

The Seeed Studio XIAO ESP32-C6 features 14 Castellated/Through-hole pins. Below is the exact hardware connection map:

| XIAO Pin | ESP32-C6 GPIO | Direction | Function | Connection Details |
|---|---|---|---|---|
| **D0** | **GPIO0** | Input / ADC | **BTN_RIGHT / BATTERY_ADC** | Right button tactile switch (active LOW) + midpoint of battery divider (`ADC1_CH0`) |
| **D1** | **GPIO1** | Input | **BTN_LEFT** | Left button tactile switch (to GND, internal pull-up) |
| **D2** | **GPIO2** | Input | **BTN_ROTATE** | Rotate / Action button tactile switch (to GND, internal pull-up) |
| **D3** | **GPIO21** | Output | **SPI MOSI (SDA)** | ST7789 Display SDA / MOSI line |
| **D4** | **GPIO22** | Output | **SPI CLK (SCL)** | ST7789 Display SCL / SCLK clock line |
| **D5** | **GPIO23** | Output | **BUZZER** | Positive pin of piezo buzzer (Negative to GND, driven by LEDC PWM) |
| **D6** | **GPIO16** | Input | **BTN_DROP** | Drop / Select button tactile switch (to GND, internal pull-up) |
| **D7** | **GPIO17** | Output | **TFT_PWR_GATE** | Gate of LCD power MOSFET (HIGH = Display ON, LOW = Display OFF) |
| **D8** | **GPIO19** | Output | **SPI DC** | ST7789 Display Data/Command control line |
| **D9** | **GPIO20** | Output | **SPI CS** | ST7789 Display Chip Select (active LOW) |
| **D10** | **GPIO18** | Output | **SPI RST** | ST7789 Display Reset line (active LOW) |
| **3V3** | — | Power | **3.3V Output** | Clean 3.3V supply rail from on-board LDO |
| **GND** | — | Power | **Ground** | Common system ground |
| **BAT+ / BAT-** | — | Power | **Battery Terminal Pads** | Underside pads of XIAO for 3.7V LiPo connection |

> ⚠️ **Important Deep-Sleep Wakeup Note:**
> On the ESP32-C6, **GPIO16 (D6 / BTN_DROP)** is *not* in the Low-Power (LP) RTC domain. Deep sleep wakeup is hardware-configured across **D0, D1, and D2** (RIGHT, LEFT, ROTATE). Pressing any of these three buttons instantly awakens the console from sleep.

---

## ⚡ Schematics & Circuit Subsystems

### 1. Battery Voltage Sensing Divider
To measure the 250mAh LiPo voltage ($3.2\text{V} - 4.2\text{V}$) without wasting valuable GPIOs, the battery monitor is multiplexed with **D0 (GPIO0 / `BTN_RIGHT`)**:

```
           VBAT (LiPo Positive: 3.2V - 4.2V)
               │
              ┌┴┐
              │ │ R1 (180kΩ, 1%)
              └┬┘
               ├─── Multiplexed into D0 (GPIO0 / ADC1_CH0)
              ┌┴┐  └─── Connected to Right Tactile Switch
              │ │ R2 (180kΩ, 1%)
              └┬┘  ┌─── Optional 100nF Ceramic Filter Cap
               ├───┤
               │   └─── (Switch shorts D0 directly to GND when pressed)
              GND
```

- **Divider Ratio:** $0.5000$ ($\frac{180\text{k}\Omega}{180\text{k}\Omega + 180\text{k}\Omega}$)
- **Safe Range:** Full charge ($4.20\text{V}$) scales down to $2.10\text{V}$ at the ADC pin (comfortably within the 3.3V ADC attenuation limit).
- **Press Discrimination:** When the right button is pressed, D0 is pulled down to $0.0\text{V}$. The firmware detects the physical button click and freezes ADC sampling during the press, preserving the cached battery reading on the screen with zero flicker.

### 2. LCD Power Cutoff (True Low-Power Deep Sleep)
The GMT020-02-7P display module has its backlight tied internally to display VCC. To completely extinguish the screen and prevent parasitic leakage in sleep:
- A MOSFET is placed inline with the display's power rail.
- Controlled via **GPIO17 (D7)** with hardware RTC pad hold (`gpio_hold_en`), keeping the display completely turned off during deep sleep.
- Holding the **DROP** button for > 1 second safely parks the display, powers down peripherals, and puts the ESP32-C6 into deep sleep mode.

---

## 🎮 Game Controls Matrix

| Action / Button | Arcade Menu | Tetris | Space Invaders | Breakout | Tennis (Pong) | Snake | Flappy Bird | Retro Racer |
|---|---|---|---|---|---|---|---|---|
| **LEFT (D1)** | Launch Game | Move Left | Move Left | Paddle Left | Speed -0.25x | Turn Left | — | Steer Left |
| **RIGHT (D0)** | Launch Game | Move Right | Move Right | Paddle Right | Speed +0.25x | Turn Right | — | Steer Right |
| **ROTATE (D2)** *(Click)* | Scroll Up | Rotate Piece | Shoot Missile | Ball Speed + | Paddle Up | Turn Up | Flap Wing | **TURBO Boost (135 MPH)** |
| **DROP (D6)** *(Click)* | Scroll Down | Hard Drop | — | Ball Speed - | Paddle Down | Turn Down | Flap Wing | **BRAKE (30 MPH)** |
| **ROTATE (D2)** *(Hold 1s)* | — | Exit to Menu | Exit to Menu | Exit to Menu | Exit to Menu | Exit to Menu | Exit to Menu | Exit to Menu |
| **DROP (D6)** *(Hold 1s)* | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** | **Deep Sleep** |

---

## 💻 Building and Flashing

This project is built using [PlatformIO](https://platformio.org/) with the native **ESP-IDF** framework for maximum performance and direct register/DMA control.

### 1. Prerequisites
- Python 3.10+
- PlatformIO Core CLI (`pip install platformio`) or PlatformIO IDE extension in VS Code.

### 2. Clone the Repository
```bash
git clone https://github.com/your-username/xiao-esp32c6-arcade-console.git
cd xiao-esp32c6-arcade-console
```

### 3. Build Firmware
```bash
pio run
```

### 4. Flash to XIAO ESP32-C6
Connect your Seeed XIAO ESP32-C6 via USB-C and flash:
```bash
# Linux
pio run --target upload --upload-port /dev/ttyACM0

# macOS
pio run --target upload --upload-port /dev/cu.usbmodem*

# Windows
pio run --target upload --upload-port COMx
```

### 5. Open Serial Monitor (Diagnostics & Logs)
```bash
pio device monitor -b 115200
```

---

## 📂 Project Architecture

```
xiao_esp32c6_tetris/
├── main/
│   ├── CMakeLists.txt        # IDF component registration
│   └── main.c                # Unified engine: display DMA, sound, battery, menu & 7 games
├── photos/                   # Build photos & hardware showcase
├── platformio.ini            # Board configuration (ESP-IDF framework, 160MHz)
├── sdkconfig.defaults        # Optimal FreeRTOS tick & flash parameters
├── CONTEXT.md                # Fast context reference for AI/developer pair programming
└── README.md                 # Project documentation & assembly guide
```

---

## 📜 License
This project is open-source under the MIT License. Feel free to build, remix, and share!
