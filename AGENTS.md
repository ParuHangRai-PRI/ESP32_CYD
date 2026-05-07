# AGENTS.md

## Project
PlatformIO project for **ESP32** (`esp32dev` board, Arduino framework).

## Status
- **UART communication**: Implemented (115200 baud, 8N1, 1024-byte RX buffer), pending STM32 hardware test
- **Touch screen**: Failed hardware - not needed
- **Bluetooth icon**: Fixed (12x12 X, 1.8x vertical line, right-side connections only)
- **3-way communication**: Code complete (BLE→UART→TFT), pending hardware validation
- **Menu rotation**: Disabled per user request (no auto-rotate)
- **Detail screen**: Centered 200x60 BACK button with "Option: [Name]" text

## Commands
- `pio run` — build
- `pio run --target upload` — flash to device
- `pio device monitor` — serial monitor (115200 baud)
- `pio run --target clean` — clean build artifacts

Build artifacts live in `.pio/` (gitignored).

## Hardware
- **Display**: ST7789 240x320 via SPI — MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, BL=21, RST=-1 (no reset pin)
- **BLE**: advertises as `CYD_BLE_NODE` with a custom read/write/notify characteristic
- **UART2**: RX=16, TX=17, 115200 baud, 1024-byte RX buffer — connects to STM32

## Architecture
Single source file: `src/main.cpp`. Initializes TFT display, BLE GATT server, and UART2. Implements 3-way bi-directional communication:
- **BLE → UART**: Phone commands relayed to STM32 via Serial2
- **UART → BLE**: STM32 data relayed to phone via BLE notifications
- **UART → TFT**: `VAL:` prefixed strings update screen without flicker
- **Touch**: Menu selection and detail screen navigation with back button

## Bluetooth Icon (`drawBtLines`)
- X-shape: 12x12 square bounding box, perpendicular lines (90°), equal length
- Vertical line: 1.8x X height (0.9 × X_SIZE above and below intersection), 2px thick
- X lines: 2px thick
- Connections: Right-side only (tips of X to vertical line, 2px thick)
- All lines drawn with parallel strokes to simulate thickness (TFT_eSPI has no native thickness)

## UART Protocol
- **STM32 → ESP32**: Strings terminated with `\n`
  - `VAL:<data>` — sensor data displayed on TFT (flicker-free, updates only `VAL_X,VAL_Y 280x20` region)
  - `CMD:<command>` — relayed to BLE phone via `pCharacteristic->notify()`
- **Phone → ESP32 → STM32**: BLE write callbacks send data via `Serial2.println()`
- **Buffer**: 1024 bytes, non-blocking `Serial2.available()` with char-by-char read

## Screen Navigation
- **State machine**: `SCREEN_MENU` and `SCREEN_DETAIL`
- **Menu**: 4 items (Test, Settings, Help, Info), all TFT_GREEN, auto-rotate every 2s
- **Touch**: Tap menu item → detail screen; Tap "BACK" (red button, 200x60px) → menu
- **Detail screen**: Shows selected item title, description, and large back button

## Libraries
- `bodmer/TFT_eSPI@^2.5.43` — display/touch config in `platformio.ini` `build_flags`
- `ESP32 BLE Arduino` — BLE GATT server with read/write/notify

## Conventions
- `include/` holds generated header assets (logo bitmaps)
- `lib/` is empty; private libs go here as subdirs per PlatformIO convention
- `test/` is empty; tests use PlatformIO Test Runner (`pio test`)
- No `delay()` in `loop()` — all operations non-blocking
