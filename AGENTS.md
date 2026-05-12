# PlatformIO project for **ESP32** (`esp32dev` board, Arduino framework).

## Status
- **UART communication**: Implemented (UART0, 115200 baud, 8N1, 1024-byte RX buffer), pending STM32 hardware test
- **Touch screen**: NOT USED — navigation via BLE or STM32 control signals
- **Screen states**: MENU, DETAIL, QUICKTEST, RESULTS, TEST_PARAM_MENU, TESTING, TEST_RESULTS
- **3-way communication**: ESP32 ↔ STM32 via UART, ESP32 ↔ Phone via BLE
- **Menu auto-cycle**: Active (1 second interval)

## Commands
- `pio run` — build
- `pio run --target upload` — flash to device
- `pio device monitor` — serial monitor (115200 baud)
- `pio run --target clean` — clean build artifacts

Build artifacts live in `.pio/` (gitignored).

## Hardware
- **Display**: ST7789 240x320 via SPI — MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, BL=21, RST=-1
- **BLE**: advertises as `CYD_BLE_NODE` with read/write/notify characteristic
- **UART0**: TX=GPIO1, RX=GPIO3, 115200 baud, 1024-byte RX buffer — connects to STM32
- **Backlight**: PWM-controlled via LEDC (channel 0, 16-bit, 200Hz), logarithmic LUT

**Note**: UART0 shares pins with USB/Serial, so Serial Monitor cannot be used while connected to STM32.

## Architecture
Single source file: `src/main.cpp`. Initializes TFT display, BLE GATT server, PWM backlight, and UART0.

### Navigation State Machine

The navigation is controlled by three flags in `NavigationState` struct:

```cpp
struct NavigationState
{
    int home;       // 0 = at home, non-zero = in sub-screen
    int option1;    // 1-4: which top menu was selected
    int option2;    // 1-n: sub-option within option1
};
```

**option1 values:**
- 1 = QuickTest (immediate test)
- 2 = Test Parameter (4 animated test type options)
- 3 = (reserved for future)
- 4 = Settings (2 options: Device Manual, Parameters Info)

**option2 (when option1=2):**
- 1 = Test All
- 2 = Test TLF
- 3 = Test HLF
- 4 = Test Turbidity

**option2 (when option1=4):**
- 1 = Device Manual
- 2 = Parameters Info

### UI Components
- **Status bar**: 30px height, battery icon with percentage display
- **Navigation panel**: 20px height, breadcrumb path display
- **Option boxes**: 40px height, rounded rectangles with ">>" icon for selected
- **Back button**: 200x60px, red fill with white border
- **Progress bars**: Animated via sprites for flicker-free updates

### Font Files (in include/)
- `PTSerif28.h` - Results screens (28pt)
- `PTSerif14.h` - Navigation panel & battery % (14pt)

### UART Protocol (ESP32 ↔ STM32)
Uses UART0 (115200 baud, 8N1). Shares pins with USB, so Serial Monitor unavailable when connected to STM32.

**Receives from STM32 (input signals):**
| Message | Description |
|---------|-------------|
| `VAL:b<0-100>` | Battery percentage |
| `VAL:t<value>` | TLF sensor value |
| `VAL:tu<value>` | Turbidity value |
| `VAL:h<value>` | HLF value |
| `VAL:dt<datetime>` | Date/time |
| `CTL:up` | Navigate up |
| `CTL:dn` | Navigate down |
| `CTL:sel` | Select option |
| `CTL:bk` | Go back |
| `CTL:pwr` | Power off |

**Sends to STM32 (output signals):**
| Message | Description |
|---------|-------------|
| `CTL:tst` | Test all params |
| `CTL:er` | Erase memory |
| `CTL:t1` | Test TLF |
| `CTL:t2` | Test HLF |
| `CTL:t3` | Test Turbidity |

### BLE → UART → Phone
- **BLE → UART**: Phone commands forwarded to STM32
- **UART → BLE**: STM32 data relayed via BLE notifications
- **VAL: handling**: Parses sensor values (b, t, tu, h, dt) into separate variables

## Libraries
- `bodmer/TFT_eSPI@^2.5.43` — display only, config in `platformio.ini` `build_flags`
- `ESP32 BLE Arduino` — BLE GATT server with read/write/notify

## Conventions
- NEVER push to git — only commit when explicitly requested by the user
- `include/` holds generated header assets (fonts, logos)
- `lib/` is empty; private libs go here as subdirs per PlatformIO convention
- `test/` is empty; tests use PlatformIO Test Runner (`pio test`)
- No `delay()` in `loop()` except critical initialization

## Screen Details

### Menu Screen (home=0)
- 4 menu items with animated selection (1s auto-cycle)
- First item expanded (300px), others normal (270px)
- Icons: QuickTest (>>), Test Parameter (>>), Settings (gear), Info (i)

### Test Parameter Menu (home!=0, option1=2)
- Uses same `drawOptionBoxes()` function as home menu
- 4 options: Test All, Test TLF, Test HLF, Test Turbidity
- Auto-cycle active (1 second interval)

### Settings Menu (home!=0, option1=4)
- Uses same `drawOptionBoxes()` function
- 2 options: Device Manual, Parameters Info

### QuickTest Screen
- 13-second test with progress bar
- Auto-navigates to Results

### Test Results
- Single or multiple parameter results displayed