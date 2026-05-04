# hyperlapse-control

Arduino Uno firmware for a hyperlapse intervalometer. Triggers a camera shutter via a 3.5 mm TRS jack at a configurable interval. Input is a rotary encoder with pushbutton; status is shown on a 1.44" ST7735S TFT display.

## Hardware

| Pin | Function |
|-----|----------|
| 3   | TFT DC (register/data select) |
| 5   | Encoder A (external pull-up) |
| 6   | Encoder B (external pull-up) |
| 7   | Encoder button (external pull-up) |
| 8   | Focus output → TRS tip (default) |
| 9   | Shutter output → TRS ring (default) |
| 11  | TFT MOSI (hardware SPI) |
| 13  | TFT SCK (hardware SPI) |

TFT CS is hardwired to GND (always selected); RST is hardwired to 5V (no reset line needed). Both are passed as `-1` to the Adafruit driver.

Camera connector: 3.5 mm TRS. By default, focus is on the tip (pin 8) and shutter is on the ring (pin 9). Sleeve is GND. Default polarity is active LOW (open-collector style — pull to GND to activate). Both the tip/ring assignment and the polarity can be changed in the settings menu.

## Building

```bash
~/.platformio/penv/bin/pio run                 # compile
~/.platformio/penv/bin/pio run --target upload # compile + flash
~/.platformio/penv/bin/pio device monitor      # serial monitor (unused by default)
```

Dependencies (resolved automatically by PlatformIO):
- `adafruit/Adafruit ST7735 and ST7789 Library`
- `adafruit/Adafruit GFX Library`

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
11. ISR: `PCINT2_vect`
12. Input readers: `readEncoder`, `readButton`
13. `updateStateMachine`
14. `setup` / `loop`

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

---

## Menu

Long press from `DISARMED` opens the settings screen. The encoder scrolls items; short press acts on the selected item; long press exits back to `DISARMED`.

| Item | Type | Action |
|------|------|--------|
| Focus | toggle | Enable/disable the focus pulse before each shot |
| Focus ms | numeric (50–5000, step 50) | Duration of focus pulse — increasing past the current interval automatically bumps the interval up; decreasing the interval automatically pulls Focus ms down |
| Pre-focus | toggle | Start focus pulse before interval end (see above) |
| Shutter ms | numeric (50–2000, step 50) | Duration of shutter pulse |
| Tip | toggle | Which signal is on the TRS tip: Focus (default) or Shutter |
| Invert | toggle | Switch output polarity: active LOW ↔ active HIGH |

**Numeric items** require entering edit mode first: short press toggles edit mode (cursor shows `*`); the encoder then adjusts the value. Short press again exits edit mode.

**Toggle items** (Focus, Pre-focus, Tip, Invert) flip on short press directly — no edit mode.

---

## Microcontroller practices

### No `delay()`

All timing uses `millis()` comparisons. The `loop()` function returns on every iteration, keeping the encoder ISR, button debounce, and display updates responsive regardless of which state the machine is in.

### PCINT encoder ISR

Pins 5 and 6 are in the ATmega328P Port D PCINT2 group. The ISR fires on any edge of either pin and uses a 4-entry Gray-code table to determine direction, accumulating a raw signed count in `encRaw`. The main loop drains `encRaw` atomically (cli/sei), accumulates a remainder (`encCarry`), and converts 4 raw counts to 1 logical detent — matching the physical click spacing of a standard 4-pulse-per-detent encoder.

Polling would miss transitions during the ~50 ms SPI display updates; the ISR catches every edge regardless of what the CPU is doing.

### Partial display redraw

Five boolean dirty flags (`needFullRedraw`, `needIntervalRedraw`, `needStatusRedraw`, `needCountdownRedraw`, `needPinStateRedraw`) gate each display region independently. Only changed regions are redrawn, eliminating the flicker that a full `fillScreen()` on every loop iteration would cause. A full redraw is only triggered on state transitions that change the overall screen layout (e.g., entering or leaving the menu).

### Countdown display uses ceiling division

The remaining-second display uses `(intervalMs - elapsed + 999) / 1000` (integer ceiling) rather than floor. Floor division means even 1 ms of elapsed time rounds the display down by a full second; ceiling keeps the display at the interval value for the entire first second after arming.

The dirty flag for countdown updates compares the computed remaining second against the last displayed value, so redraws fire exactly when the number changes — not on every loop or on arbitrary absolute-time ticks.

### Button long-press without blocking

`readButton()` records the timestamp of the falling edge (press) and checks on every call whether `LONGPRESS_MS` has elapsed. The `btnLongFired` flag prevents the subsequent rising edge (release) from also firing a short-press action.

### Safe ISR/main data sharing

`encRaw` is declared `volatile`. The main loop disables interrupts (`cli()`), copies and clears `encRaw`, then re-enables (`sei()`) before operating on the copy. This keeps the critical section to three instructions, avoiding missed counts.

### Pin polarity abstraction

`activeLevel()` and `inactiveLevel()` return the correct `HIGH`/`LOW` value based on `pinsInverted`. All `digitalWrite` calls for the focus and shutter outputs go through these helpers, so inverting polarity from the menu works everywhere without scattered conditionals.
