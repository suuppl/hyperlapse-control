# hyperlapse-control

Intervalometer firmware for **Raspberry Pi Pico W**. Triggers a camera shutter via a 3.5 mm TRS jack at a configurable interval. Input is a rotary encoder with pushbutton; status is shown on a 1.44" ST7735S TFT display. A browser-accessible web UI is served over the Pico W's built-in Wi-Fi.

## Hardware

| Physical pin | GP pin | Function |
|---|---|---|
| 17 | GP13 | TFT DC (register/data select) |
| 16 | GP12 | Encoder A (external pull-up) |
| 15 | GP11 | Encoder B (external pull-up) |
| 14 | GP10 | Encoder button (external pull-up) |
| 10 | GP7 | Focus output → TRS tip (default) |
| 9  | GP6 | Shutter output → TRS ring (default) |
| 20 | SPI1 TX | TFT MOSI (hardware SPI) |
| 19 | SPI1 SCK | TFT SCK (hardware SPI) |

TFT CS is hardwired to GND (always selected); RST is hardwired to 3.3 V (no reset line needed). Both are passed as `-1` to the Adafruit driver.

Camera connector: 3.5 mm TRS. By default, focus is on the tip (GP7) and shutter is on the ring (GP6). Sleeve is GND. Default polarity is active LOW (open-collector style — pull to GND to activate). Both the tip/ring assignment and the polarity can be changed in the settings menu.

## Building

```bash
~/.platformio/penv/bin/pio run                      # compile
~/.platformio/penv/bin/pio run -e rpipicow -t upload # compile + flash
~/.platformio/penv/bin/pio device monitor           # serial monitor (unused by default)
```

Dependencies (resolved automatically by PlatformIO):
- `adafruit/Adafruit ST7735 and ST7789 Library`
- `adafruit/Adafruit GFX Library`

> **Note:** The official PlatformIO `raspberrypi` platform does not support Pico W. The project uses the community `maxgerhardt/platform-raspberrypi` platform with the `earlephilhower` core — this is already configured in `platformio.ini`.

## Web UI

The Pico W boots as a Wi-Fi access point:

| | |
|---|---|
| SSID | `hyperlapse` |
| Password | `hyperlapse` |
| URL | `http://192.168.4.1` |

The single-page UI shows current state, countdown to next shot, and live pin status. All settings can be changed without touching the device. Wi-Fi can be disabled from the settings menu.

---

## src/main.cpp — structure

Everything lives in one file. Sections in order:

1. Pin constants and output state globals
2. Display object
3. State machine enum and timing globals
4. Partial-redraw dirty flags
5. Encoder ISR globals
6. Button debounce globals
7. Menu enum and state
8. Helper functions: `stepForInterval`, `adjustInterval`, `formatTime`
9. Draw helpers: `drawPinState`, `drawInterval`, `drawStatus`, `drawCountdown`, `drawMenuItem`, `updateDisplay`
10. Logic: `adjustMenuItemValue`, `enterState`, `handleShortPress`, `handleLongPress`
11. Web interface: `PAGE_HTML`, `serveMainPage`, `serveState`, `handleArm`, `handleSettings`, `handleWebClient`
12. ISR: `encoderISR`
13. Input readers: `readEncoder`, `readButton`
14. `updateStateMachine`
15. `setup` / `loop`

---

## State machine

```
            ┌──────────────────┐                                                        ┌──────────────────┐         
            │                  │                                                        │                  │  short  
            │                  │                                                        │                  │  press  
            │       MENU       │                   ┌───────────────────────────────────►│     FOCUSING     ├────────┐
            │                  │                   │                                    │                  │        │
            │                  │                   │                                    │                  │        │
            └─────────┬────────┘                   │ focusOn && preFocus:               └─────────┬────────┘        │
                   ▲  │                            │   after (intervalMs - focusMs)               │                 │
                   │  │ long                       │ focusOn && !preFocus:                after   │                 │
              long │  │ press                      │   after intervalMs                   focusMs │                 │
              press│  │                            │                                              │                 │
                   │  ▼                            │                                              ▼                 │
            ┌──────┴───────────┐  short  ┌─────────┴────────┐    !focusOn:              ┌──────────────────┐        │
            │                  │  press  │                  │      after intervalMs     │                  │        │
entry       │                  ├────────►│                  ├──────────────────────────►│                  │        │
───────────►│     DISARMED     │         │       ARMED      │                           │     SHOOTING     │        │
            │                  │◄────────┤                  │◄──────────────────────────┤                  │        │
            │                  │  short  │                  │           after shutterMs │                  │        │
            └──────────────────┘  press  └──────────────────┘                           └─────────┬────────┘        │
                   ▲  ▲                                                                           │                 │
                   │  │                                                                           │ short           │
                   │  │                                                                           │ press           │
                   │  │                                                                           │                 │
                   │  │                                                                           │                 │
                   │  └───────────────────────────────────────────────────────────────────────────┘                 │
                   │                                                                                                │
                   └────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### States

| State | Entry action | Exit condition |
|-------|-------------|----------------|
| `DISARMED` | Both pins inactive | Short press → `ARMED` |
| `ARMED` | Both pins inactive, record `lastShotMs` | Elapsed ≥ `fireMs` → `FOCUSING` or `SHOOTING` |
| `FOCUSING` | Focus pin active | Elapsed ≥ `focusMs` → `SHOOTING` |
| `SHOOTING` | Shutter pin active | Elapsed ≥ `shutterMs` → `ARMED` |
| `MENU` | Show settings screen | Long press → `DISARMED` |

Short press while `ARMED`, `FOCUSING`, or `SHOOTING` immediately disarms (safety).  
Long press while `DISARMED` enters `MENU`. Long press while in `MENU` exits.

### Pre-focus timing

When **Focus** and **Pre-focus** are both enabled, `FOCUSING` starts `focusMs` before the interval deadline so the shutter fires at exactly `intervalSec` seconds after the previous shot:

```
fireMs = intervalMs - focusMs
```

`focusMs < intervalMs` is enforced as an invariant (see Menu below), so this path is always valid. If focus is disabled, `fireMs = intervalMs` and the shot fires at the end of the full interval.

### Interval steps

To cover 1 s – 3600 s without an unwieldy number of encoder clicks:

| Range | Step |
|-------|------|
| 1–60 s | 1 s |
| 61–300 s | 5 s |
| 301–600 s | 10 s |
| 601–3600 s | 30 s |

Decreasing the interval automatically pulls Focus ms down.

---

## Menu

Long press from `DISARMED` opens the settings screen. The encoder scrolls items; short press acts on the selected item; long press exits back to `DISARMED`.

| Item | Type | Action |
|------|------|--------|
| Focus | toggle | Enable/disable the focus pulse before each shot |
| Focus ms | numeric (50–5000, step 50) | Duration of focus pulse — increasing past the current interval automatically bumps the interval up; decreasing the interval automatically pulls Focus ms down |
| Pre-focus | toggle | Start focus pulse before interval end (see above) |
| Shutter ms | numeric (50–2000, step 50) | Duration of shutter pulse |
| Tip/Ring | toggle | Which signal is on the TRS tip/ring: Focus/Shutter (default) or Shutter/Focus |
| Active | toggle | Switch output polarity: LOW ↔ HIGH |
| WiFi | toggle | Enable/disable the Wi-Fi access point |

**Numeric items** require entering edit mode first: short press toggles edit mode (cursor shows `$`); the encoder then adjusts the value. Short press again exits edit mode.

**Toggle items** (Focus, Pre-focus, Tip, Invert, WiFi) flip on short press directly — no edit mode.
