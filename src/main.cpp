#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

static const uint8_t PIN_TFT_DC  = 3;
static const uint8_t PIN_ENC_A   = 5;
static const uint8_t PIN_ENC_B   = 6;
static const uint8_t PIN_ENC_BTN = 7;

static bool    pinsSwapped  = false;
static bool    pinsInverted = false;  // true = active HIGH, false = active LOW
static uint8_t focusPin     = 8;
static uint8_t shutterPin   = 9;

static inline uint8_t activeLevel()   { return pinsInverted ? HIGH : LOW; }
static inline uint8_t inactiveLevel() { return pinsInverted ? LOW  : HIGH; }

static const uint32_t DEBOUNCE_MS  = 50;
static const uint32_t LONGPRESS_MS = 1000;

static uint32_t focusMs   = 500;
static uint32_t shutterMs = 200;

Adafruit_ST7735 tft(-1, PIN_TFT_DC, -1);

enum State : uint8_t { DISARMED, ARMED, FOCUSING, SHOOTING, MENU };
static State    state        = DISARMED;
static uint32_t stateEnterMs = 0;
static uint32_t lastShotMs   = 0;

static uint32_t intervalSec  = 10;
static bool     focusEnabled = true;
static bool     preFocus     = true;

static bool needFullRedraw      = true;
static bool needIntervalRedraw  = false;
static bool needStatusRedraw    = false;
static bool needCountdownRedraw = false;
static bool needPinStateRedraw  = false;

static volatile int8_t  encRaw      = 0;
static volatile uint8_t isrEncState = 0;
static          int8_t  encCarry    = 0;

static uint8_t  rawBtn       = HIGH;
static uint8_t  debouncedBtn = HIGH;
static uint32_t btnChangeMs  = 0;
static uint32_t btnPressMs   = 0;
static bool     btnLongFired = false;

enum MenuItem : uint8_t {
    MI_FOCUS_EN = 0,
    MI_FOCUS_MS,
    MI_PRE_FOCUS,
    MI_SHUTTER_MS,
    MI_SWAP_PINS,
    MI_INVERT_PINS,
    MI_COUNT
};
static uint8_t menuIdx     = 0;
static bool    menuEditing = false;

// Tracks last displayed remaining-second value; UINT32_MAX forces immediate redraw on arm.
static uint32_t lastDisplayedRemSec = UINT32_MAX;

// ===========================================================================

static uint32_t stepForInterval(uint32_t s) {
    if (s <= 60)  return 1;
    if (s <= 300) return 5;
    if (s <= 600) return 10;
    return 30;
}

static void adjustInterval(int8_t dir) {
    uint32_t step = stepForInterval(intervalSec);
    if (dir > 0) {
        intervalSec = min(intervalSec + step, (uint32_t)3600);
    } else {
        intervalSec = (intervalSec > step) ? intervalSec - step : 1;
        // focusMs must stay below intervalMs; pull focusMs down to nearest 50ms step
        uint32_t intervalMs = intervalSec * 1000UL;
        if (focusMs >= intervalMs)
            focusMs = intervalMs - 50;
    }
    needIntervalRedraw = true;
}

static void formatTime(uint32_t s, char* buf, size_t len) {
    if (s >= 3600) {
        snprintf(buf, len, "%uh%02um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    } else if (s >= 60) {
        snprintf(buf, len, "%um%02us", (unsigned)(s / 60), (unsigned)(s % 60));
    } else {
        snprintf(buf, len, "%us", (unsigned)s);
    }
}

// ===========================================================================
// Draw helpers

static void drawPinState() {
    tft.fillRect(98, 0, 30, 14, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(100, 4);
    tft.setTextColor(digitalRead(focusPin) == activeLevel() ? ST77XX_GREEN : 0x7BEF);
    tft.print('F');
    tft.setCursor(116, 4);
    tft.setTextColor(digitalRead(shutterPin) == activeLevel() ? ST77XX_GREEN : 0x7BEF);
    tft.print('S');
}

static void drawInterval() {
    tft.fillRect(0, 18, 128, 24, ST77XX_BLACK);
    char buf[12];
    formatTime(intervalSec, buf, sizeof(buf));
    tft.setTextSize(3);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 18);
    tft.print(buf);
}

static void drawStatus() {
    tft.fillRect(0, 74, 128, 16, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor((state != DISARMED) ? ST77XX_GREEN : ST77XX_RED);
    tft.setCursor(4, 74);
    tft.print((state != DISARMED) ? "ARMED" : "DISARMED");
}

static void drawCountdown() {
    tft.fillRect(0, 100, 128, 28, ST77XX_BLACK);
    if (state == DISARMED) {
        tft.setTextSize(1);
        tft.setTextColor(0x7BEF);
        tft.setCursor(4, 104);
        tft.print("press: arm");
        tft.setCursor(4, 116);
        tft.print("hold: menu");
    } else if (state == FOCUSING || state == SHOOTING) {
        tft.fillRect(0, 100, 128, 28, ST77XX_GREEN);
        tft.setTextSize(2);
        tft.setTextColor(ST77XX_BLACK);
        tft.setCursor(34, 106);
        tft.print("FIRE!");
    } else if (state == ARMED) {
        uint32_t intervalMs = intervalSec * 1000UL;
        uint32_t elapsed    = millis() - lastShotMs;
        uint32_t remaining  = (elapsed >= intervalMs) ? 0 : (intervalMs - elapsed + 999) / 1000;
        char nbuf[12];
        formatTime(remaining, nbuf, sizeof(nbuf));
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_CYAN);
        tft.setCursor(4, 104);
        tft.print("NEXT: ");
        tft.print(nbuf);
        tft.setTextColor(0x7BEF);
        tft.setCursor(4, 116);
        tft.print("press: disarm");
    }
}

static void drawMenuItem(uint8_t i) {
    uint8_t  y       = 26 + i * 14;
    bool     sel     = (i == menuIdx);
    bool     editing = sel && menuEditing;
    uint16_t dimCol  = 0x7BEF;
    uint16_t selCol  = ST77XX_WHITE;
    uint16_t valCol  = editing ? ST77XX_YELLOW : (sel ? selCol : dimCol);

    tft.fillRect(0, y, 128, 12, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(0, y + 2);
    tft.setTextColor(sel ? selCol : dimCol);
    tft.print(editing ? '*' : (sel ? '>' : ' '));
    tft.print(' ');

    switch ((MenuItem)i) {
        case MI_FOCUS_EN:
            tft.print("Focus:     ");
            tft.setTextColor(valCol);
            tft.print(focusEnabled ? "ON" : "OFF");
            break;
        case MI_FOCUS_MS:
            tft.print("Focus ms:  ");
            tft.setTextColor(valCol);
            tft.print((unsigned)focusMs);
            break;
        case MI_PRE_FOCUS:
            tft.print("Pre-focus: ");
            tft.setTextColor(valCol);
            tft.print(preFocus ? "ON" : "OFF");
            break;
        case MI_SHUTTER_MS:
            tft.print("Shttr ms:  ");
            tft.setTextColor(valCol);
            tft.print((unsigned)shutterMs);
            break;
        case MI_SWAP_PINS:
            tft.print("Tip/Ring:  ");
            tft.setTextColor(valCol);
            tft.print(pinsSwapped ? "S/F" : "F/S");
            break;
        case MI_INVERT_PINS:
            tft.print("Active:    ");
            tft.setTextColor(valCol);
            tft.print(pinsInverted ? "Low" : "High");
            break;
        default: break;
    }
}

static void updateDisplay() {
    if (!needFullRedraw && !needIntervalRedraw && !needStatusRedraw &&
        !needCountdownRedraw && !needPinStateRedraw) return;

    if (needFullRedraw) {
        needFullRedraw = needIntervalRedraw = needStatusRedraw =
            needCountdownRedraw = needPinStateRedraw = false;
        tft.fillScreen(ST77XX_BLACK);

        if (state == MENU) {
            tft.setTextSize(2);
            tft.setTextColor(ST77XX_WHITE);
            tft.setCursor(4, 4);
            tft.print("SETTINGS");
            for (uint8_t i = 0; i < MI_COUNT; i++) drawMenuItem(i);
            tft.setTextSize(1);
            tft.setTextColor(0x7BEF);
            tft.setCursor(4, 118);
            tft.print("hold btn: exit");
            return;
        }

        tft.setTextSize(1);
        tft.setTextColor(0x7BEF);
        tft.setCursor(4, 4);
        tft.print("INTERVAL");

        drawInterval();
        drawStatus();
        drawCountdown();
        drawPinState();
        return;
    }

    if (needIntervalRedraw)  { needIntervalRedraw  = false; drawInterval();  }
    if (needStatusRedraw)    { needStatusRedraw    = false; drawStatus();    }
    if (needCountdownRedraw) { needCountdownRedraw = false; drawCountdown(); }
    if (needPinStateRedraw)  { needPinStateRedraw  = false; drawPinState();  }
}

// ===========================================================================

static void adjustMenuItemValue(int8_t dir) {
    switch ((MenuItem)menuIdx) {
        case MI_FOCUS_MS: {
            int32_t v = (int32_t)focusMs + dir * 50;
            focusMs = (uint32_t)(v < 50 ? 50 : v > 5000 ? 5000 : v);
            // focusMs must stay below intervalMs; bump interval up if needed
            while (intervalSec * 1000UL <= focusMs) {
                uint32_t step = stepForInterval(intervalSec);
                intervalSec = min(intervalSec + step, (uint32_t)3600);
                needIntervalRedraw = true;
            }
            break;
        }
        case MI_SHUTTER_MS: {
            int32_t v = (int32_t)shutterMs + dir * 50;
            shutterMs = (uint32_t)(v < 50 ? 50 : v > 2000 ? 2000 : v);
            break;
        }
        default: break;
    }
}

static void enterState(State next) {
    State prev = state;
    state        = next;
    stateEnterMs = millis();

    switch (next) {
        case DISARMED:
            digitalWrite(focusPin,   inactiveLevel());
            digitalWrite(shutterPin, inactiveLevel());
            if (prev == MENU) needFullRedraw = true;
            else { needStatusRedraw = true; needCountdownRedraw = true; needPinStateRedraw = true; }
            break;
        case ARMED:
            digitalWrite(focusPin,   inactiveLevel());
            digitalWrite(shutterPin, inactiveLevel());
            if (prev == DISARMED || prev == MENU) lastShotMs = millis();
            lastDisplayedRemSec = UINT32_MAX;  // force immediate redraw with correct value
            if (prev == MENU) needFullRedraw = true;
            else { needStatusRedraw = true; needCountdownRedraw = true; needPinStateRedraw = true; }
            break;
        case FOCUSING:
            digitalWrite(focusPin, activeLevel());
            needCountdownRedraw = true;
            needPinStateRedraw  = true;
            break;
        case SHOOTING:
            digitalWrite(shutterPin, activeLevel());
            needCountdownRedraw = true;
            needPinStateRedraw  = true;
            break;
        case MENU:
            menuIdx     = 0;
            menuEditing = false;
            needFullRedraw = true;
            break;
    }
}

static void handleShortPress() {
    switch (state) {
        case DISARMED:
            enterState(ARMED);
            break;
        case ARMED:
        case FOCUSING:
        case SHOOTING:
            enterState(DISARMED);
            break;
        case MENU:
            if (menuIdx == MI_FOCUS_EN) {
                focusEnabled = !focusEnabled;
                needFullRedraw = true;
            } else if (menuIdx == MI_PRE_FOCUS) {
                preFocus = !preFocus;
                needFullRedraw = true;
            } else if (menuIdx == MI_SWAP_PINS) {
                digitalWrite(focusPin,   inactiveLevel());
                digitalWrite(shutterPin, inactiveLevel());
                pinsSwapped = !pinsSwapped;
                focusPin   = pinsSwapped ? 9 : 8;
                shutterPin = pinsSwapped ? 8 : 9;
                pinMode(focusPin,   OUTPUT); digitalWrite(focusPin,   inactiveLevel());
                pinMode(shutterPin, OUTPUT); digitalWrite(shutterPin, inactiveLevel());
                needFullRedraw = true;
            } else if (menuIdx == MI_INVERT_PINS) {
                digitalWrite(focusPin,   inactiveLevel());
                digitalWrite(shutterPin, inactiveLevel());
                pinsInverted = !pinsInverted;
                digitalWrite(focusPin,   inactiveLevel());
                digitalWrite(shutterPin, inactiveLevel());
                needFullRedraw = true;
            } else {
                menuEditing = !menuEditing;
                needFullRedraw = true;
            }
            break;
    }
}

static void handleLongPress() {
    if (state == DISARMED) {
        enterState(MENU);
    } else if (state == MENU) {
        menuEditing = false;
        enterState(DISARMED);
    }
}

// ===========================================================================

ISR(PCINT2_vect) {
    uint8_t curr = (PIND >> 5) & 0x03;  // bit0=pin5(A), bit1=pin6(B)
    switch ((isrEncState << 2) | curr) {
        case 0b0001: case 0b0111: case 0b1110: case 0b1000: encRaw--; break;
        case 0b0010: case 0b1011: case 0b1101: case 0b0100: encRaw++; break;
    }
    isrEncState = curr;
}

static void readEncoder() {
    cli();
    int8_t raw = encRaw;
    encRaw = 0;
    sei();

    encCarry += raw;
    int8_t detents = encCarry / 4;
    encCarry -= detents * 4;

    if (detents == 0) return;

    int8_t dir   = (detents > 0) ? 1 : -1;
    int8_t count = (detents > 0) ? detents : -detents;

    if (state == DISARMED || state == ARMED) {
        for (int8_t i = 0; i < count; i++) adjustInterval(dir);
    } else if (state == MENU) {
        if (menuEditing) {
            for (int8_t i = 0; i < count; i++) adjustMenuItemValue(dir);
        } else {
            menuIdx = (uint8_t)(((int)menuIdx + MI_COUNT + dir) % MI_COUNT);
        }
        needFullRedraw = true;
    }
}

static void readButton() {
    uint32_t now = millis();
    uint8_t  raw = digitalRead(PIN_ENC_BTN);

    if (raw != rawBtn) {
        rawBtn      = raw;
        btnChangeMs = now;
    }
    if (now - btnChangeMs < DEBOUNCE_MS) return;

    if (raw != debouncedBtn) {
        debouncedBtn = raw;
        if (raw == LOW) {
            btnPressMs   = now;
            btnLongFired = false;
        } else {
            if (!btnLongFired) handleShortPress();
        }
    }

    if (debouncedBtn == LOW && !btnLongFired && (now - btnPressMs >= LONGPRESS_MS)) {
        btnLongFired = true;
        handleLongPress();
    }
}

static void updateStateMachine() {
    uint32_t now = millis();

    switch (state) {
        case DISARMED:
        case MENU:
            break;

        case ARMED: {
            uint32_t elapsed    = now - lastShotMs;
            uint32_t intervalMs = intervalSec * 1000UL;
            uint32_t fireMs = intervalMs;
            if (focusEnabled && preFocus && focusMs < intervalMs)
                fireMs = intervalMs - focusMs;

            if (elapsed >= fireMs)
                enterState(focusEnabled ? FOCUSING : SHOOTING);
            break;
        }

        case FOCUSING:
            if (now - stateEnterMs >= focusMs)
                enterState(SHOOTING);
            break;

        case SHOOTING:
            if (now - stateEnterMs >= shutterMs) {
                lastShotMs = now;
                enterState(ARMED);
            }
            break;
    }
}

// ===========================================================================

void setup() {
    pinMode(focusPin,    OUTPUT);
    pinMode(shutterPin,  OUTPUT);
    digitalWrite(focusPin,   inactiveLevel());
    digitalWrite(shutterPin, inactiveLevel());

    pinMode(PIN_ENC_A,   INPUT);
    pinMode(PIN_ENC_B,   INPUT);
    pinMode(PIN_ENC_BTN, INPUT);

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(2);

    isrEncState = (digitalRead(PIN_ENC_B) << 1) | digitalRead(PIN_ENC_A);
    PCICR  |= (1 << PCIE2);
    PCMSK2 |= (1 << PCINT21) | (1 << PCINT22);  // pin5=PCINT21, pin6=PCINT22

    updateDisplay();
}

void loop() {
    readEncoder();
    readButton();
    updateStateMachine();

    // Redraw countdown whenever remaining-second changes (relative to lastShotMs,
    // not absolute time — avoids stale lastCountdownSec causing wrong initial value).
    if (state == ARMED) {
        uint32_t elapsed    = millis() - lastShotMs;
        uint32_t intervalMs = intervalSec * 1000UL;
        uint32_t remSec     = (elapsed >= intervalMs) ? 0 : (intervalMs - elapsed + 999) / 1000;
        if (remSec != lastDisplayedRemSec) {
            lastDisplayedRemSec = remSec;
            needCountdownRedraw = true;
        }
    }

    updateDisplay();
}
