# NeonArcade

**A custom handheld arcade gaming system powered by the ESP32-S3.**

NeonArcade is an embedded gaming project developed in Arduino C++ for the ESP32-S3 N16R8 platform. It combines a custom game interface, 14 built-in games, dual-display output, procedural graphics, synthesized audio, persistent settings, battery monitoring, and power-management features in a compact hardware-oriented codebase.

The project is designed to explore the capabilities of the ESP32-S3 through software-rendered games, real-time input processing, multitasking, and direct interaction with display and peripheral hardware.

## Hardware Platform

* **Target microcontroller:** ESP32-S3 N16R8
* **Flash:** 16 MB nominal capacity
* **PSRAM:** 8 MB nominal capacity
* **CPU:** Dual-core Xtensa LX7, up to 240 MHz
* **Primary display:** 128 × 128 color TFT using the ST7735 driver
* **Secondary display:** 128 × 64 monochrome OLED using SSD1306
* **Input:** Eight configurable physical buttons
* **Audio output:** Buzzer/audio output connected to the configured GPIO
* **Battery monitoring:** ADC voltage measurement with configurable calibration

Actual memory availability and peripheral compatibility depend on the specific ESP32-S3 board and Arduino build configuration.

## Built-in Games

NeonArcade includes 14 games:

1. **Snake** — Classic snake gameplay. <img width="400" height="400" alt="image" src="https://github.com/user-attachments/assets/69cad504-2ac0-4ed3-95c3-f9a2e1999ed5" />

2. **Breakout** — Paddle-and-brick arcade action.
3. **Flappy** — Timing-based obstacle avoidance.
4. **Neon Dash** — Fast-paced neon platform gameplay.
5. **Starfire** — A space shooter featuring stages, enemies, bosses, shields, weapon upgrades, and bombs.
6. **Tetris** — Falling-block puzzle gameplay.
7. **Pong** — Paddle-based ball gameplay.
8. **Racer 3D** — A pseudo-3D racing game featuring curved roads, traffic, nitro, and a car garage.
9. **Nightmare** — A horror-themed survival game.
10. **Asteroids** — Space combat with rotation, thrust, shooting, and asteroid splitting.
11. **Craft 3D** — A block-based world featuring terrain, caves, water, materials, trees, animals, and day/night transitions.
12. **Hide & Seek** — A 3D-style horror game involving exploration and avoiding an enemy.
13. **Anomaly** — A horror experience built around identifying environmental anomalies.
14. **Lost Road** — A story-driven horror game involving exploration, collecting five car parts, and avoiding a pursuing monster.

Each game has its own initialization, update, and rendering functions managed by a shared game-selection system.

## Graphics and Rendering

The graphics engine is built around Arduino C++ and Adafruit_GFX.

Features include:

* RGB565 color rendering.
* Custom frame-buffer drawing routines.
* Procedural environments and graphical effects.
* Animated menus and game icons.
* Particle effects, gradients, shadows, and color transitions.
* Pseudo-3D perspective rendering for selected games.
* Frame timing and FPS monitoring.
* Configurable TFT SPI clock speed.
* Display alignment and offset calibration.

The main rendering canvas is 128 × 128 pixels. Graphics are drawn in memory before being transferred to the TFT display.

## Dual-Display Interface

### TFT Display

The color TFT is the primary gaming display. It renders the main menu, game environments, animations, score panels, and gameplay interface.

### OLED Display

The secondary OLED provides supporting information such as scores, battery information, button guidance, and selected game indicators.

The two displays use separate SPIClass instances and independently configured GPIO connections.

## Audio Engine

The firmware contains a custom software-generated audio system featuring:

* Background melody playback.
* Short navigation sounds.
* Jump, collision, success, and failure effects.
* Horror stingers and atmospheric sound effects.
* Frequency sweeps and pitch transitions.
* Attack and release envelopes for smoother tones.
* Configurable sound volume and audio enable settings.

The implementation uses LEDC and includes an I2S output path. Actual sound quality and output compatibility depend on the connected hardware and the selected audio configuration.

## Hardware Resource Management

### CPU

The system supports selectable CPU frequencies of 80, 160, and 240 MHz through the ESP32 Arduino API.

### Memory

The code uses internal heap allocation for its primary graphics buffer, a second static frame buffer, and additional working memory for rendering, sound processing, and game state.

The ESP32-S3 platform also provides access to PSRAM when correctly enabled by the board configuration. The firmware reports available heap, minimum free heap, detected flash capacity, and detected PSRAM capacity in the system information screen.

### Multitasking

The firmware creates separate FreeRTOS tasks for background jobs, TFT rendering, audio processing, and system information.

This structure separates several recurring operations instead of placing every activity in a single monolithic update function.

### Input Processing

Eight physical buttons provide directional input and action controls. The firmware supports:

* Configurable button mapping.
* Press and hold detection.
* Repeated input while a button is held.
* Navigation and game actions.
* Wake-up handling for selected buttons.

## Battery and Temperature Monitoring

The firmware samples the configured battery ADC input and applies filtering to reduce measurement noise.

Battery-related features include:

* Calibratable voltage measurement.
* Approximate battery percentage estimation.
* Low-battery indication.
* Smoothed voltage readings.
* A heuristic charging-status indicator based on voltage changes.

The temperature display uses the ESP32's internal temperature-reading facility. It is intended for approximate chip-temperature monitoring and should not be treated as an accurate ambient-temperature sensor.

Battery percentage and charging status are estimates, not measurements from a dedicated fuel gauge or charger-status IC.

**Hardware requirement:** Accurate battery voltage measurement depends on a correctly designed voltage divider, safe ADC voltage levels, and calibration for the actual board.

## Persistent Storage

NeonArcade uses the Arduino `Preferences` API to store configuration and selected game progress in non-volatile storage.

Stored information includes settings, high scores, and save data for supported games.

Features include:

* Persistent display and audio settings.
* Configurable CPU and SPI speeds.
* Button mapping persistence.
* Battery and temperature calibration values.
* Selected game save states.
* High-score storage.

The current implementation does not require a microSD card for its configuration and save-data mechanisms.

## Settings and Power Management

The settings interface includes:

* Display brightness adjustment.
* OLED configuration.
* Sound and volume settings.
* Background music control.
* Display alignment calibration.
* Button remapping.
* Battery calibration.
* Temperature calibration.
* Live system information.
* Automatic idle dimming.
* Configurable inactivity sleep.
* Manual sleep and wake-up handling.
* CPU and TFT SPI frequency selection.

The firmware also includes ESP32 deep-sleep functionality with configured GPIO wake-up handling.

## Software Dependencies

The source code uses the following libraries and ESP32 framework components:

* Arduino framework for ESP32.
* Adafruit GFX Library.
* Adafruit SSD1306.
* Adafruit ST7735.
* Arduino Preferences.
* ESP32 GPIO, LEDC, ADC, sleep, heap, and I2S APIs.
* FreeRTOS facilities provided by the ESP32 framework.

Use a compatible ESP32-S3 board definition and install the required libraries before compiling.

## Building the Project

1. Install Arduino IDE.
2. Install the ESP32 board package.
3. Select the correct ESP32-S3 board configuration.
4. Install the required Adafruit libraries.
5. Open `NeonArcade_v7.ino`.
6. Verify the GPIO assignments against your physical wiring.
7. Compile and upload the firmware.

The project is hardware-specific. Display initialization, SPI pins, button wiring, audio output, and battery measurement must match the target device.

## Current Scope

This repository contains the firmware for a self-contained handheld gaming system.

The current source focuses on local gameplay, graphics, audio, hardware input, system settings, and persistent game data. Wi-Fi networking, Bluetooth controller support, camera functionality, and external storage management are not presented as implemented features in this release.

## Project Status

Experimental embedded gaming firmware under active development.

Features and hardware compatibility may change as the project evolves.

## License

No license has been selected for this repository yet.
