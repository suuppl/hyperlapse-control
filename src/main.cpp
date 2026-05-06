#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>

// Physical pin → GP number (-1 = non-GPIO). Index = physical pin (1-40); [0] unused.
static constexpr int8_t picoWpins[41] = {
    -1,  //  0  (unused)
     0,  //  1  GP0
     1,  //  2  GP1
    -1,  //  3  GND
     2,  //  4  GP2
     3,  //  5  GP3
     4,  //  6  GP4
     5,  //  7  GP5
    -1,  //  8  GND
     6,  //  9  GP6
     7,  // 10  GP7
     8,  // 11  GP8
     9,  // 12  GP9
    -1,  // 13  GND
    10,  // 14  GP10
    11,  // 15  GP11
    12,  // 16  GP12
    13,  // 17  GP13
    -1,  // 18  GND
    14,  // 19  GP14
    15,  // 20  GP15
    16,  // 21  GP16
    17,  // 22  GP17
    -1,  // 23  GND
    18,  // 24  GP18
    19,  // 25  GP19
    20,  // 26  GP20
    21,  // 27  GP21
    -1,  // 28  GND
    22,  // 29  GP22
    -1,  // 30  RUN
    26,  // 31  GP26 / A0
    27,  // 32  GP27 / A1
    -1,  // 33  ADC GND
    28,  // 34  GP28 / A2
    -1,  // 35  ADC VREF
    -1,  // 36  3V3 Out
    -1,  // 37  3V3 En
    -1,  // 38  GND
    -1,  // 39  VSYS
    -1,  // 40  VBUS
};

static const uint8_t PIN_TFT_DC      = picoWpins[17];  // physical 17 → GP13
static const uint8_t PIN_ENC_A       = picoWpins[16];  // physical 16 → GP12
static const uint8_t PIN_ENC_B       = picoWpins[15];  // physical 15 → GP11
static const uint8_t PIN_ENC_BTN     = picoWpins[14];  // physical 14 → GP10
static const uint8_t PIN_LED_FOCUS   = picoWpins[ 4];  // physical  4 → GP2
static const uint8_t PIN_LED_SHUTTER = picoWpins[ 5];  // physical  5 → GP3
static const uint8_t PIN_INTERLOCK   = picoWpins[ 7];  // physical  7 → GP5

static bool    pinsSwapped      = false;
static bool    pinsActiveLow    = true;  // true = active LOW, false = active HIGH
static uint8_t focusPin         = picoWpins[12];  // physical 12 → GP9 (TRS tip, default)
static uint8_t shutterPin       = picoWpins[ 9];  // physical  9 → GP6 (TRS ring, default)

static inline uint8_t activeLevel()   { return pinsActiveLow ? LOW  : HIGH; }
static inline uint8_t inactiveLevel() { return pinsActiveLow ? HIGH : LOW; }

static void setFocusPin(uint8_t level) {
    digitalWrite(focusPin, level);
    digitalWrite(PIN_LED_FOCUS, level == activeLevel() ? HIGH : LOW);
}
static void setShutterPin(uint8_t level) {
    digitalWrite(shutterPin, level);
    digitalWrite(PIN_LED_SHUTTER, level == activeLevel() ? HIGH : LOW);
}

static const uint32_t DEBOUNCE_MS  = 50;
static const uint32_t LONGPRESS_MS = 1000;

static uint32_t focusMs   = 500;
static uint32_t shutterMs = 200;

Adafruit_ST7735 tft(&SPI1, -1, PIN_TFT_DC, -1);

static const char AP_SSID[]    = "timelapse";
static const char AP_PASS[]    = "timelapse";
static const char FW_VERSION[] = "1.1.0";
static WiFiServer wifiServer(80);
static char       wifiIP[16]  = "192.168.4.1";

static bool wifiEnabled = true;

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

static uint8_t menuScroll = 0;
static const uint8_t MENU_VISIBLE = 6;

static volatile int8_t  encRaw      = 0;
static volatile uint8_t isrEncState = 0;
static          int8_t  encCarry    = 0;

static uint8_t  rawBtn       = HIGH;
static uint8_t  debouncedBtn = HIGH;
static uint32_t btnChangeMs  = 0;
static uint32_t btnPressMs   = 0;
static bool     btnLongFired = false;

enum MenuSection : uint8_t { MS_BACK = 0, MS_CAMERA, MS_WIFI, MS_INFO, MS_COUNT };

enum CameraItem : uint8_t {
    CI_UP = 0, CI_FOCUS_EN, CI_FOCUS_MS, CI_PRE_FOCUS, CI_SHUTTER_MS,
    CI_SWAP_PINS, CI_INVERT_PINS, CI_COUNT
};
enum WifiItem : uint8_t { WI_UP = 0, WI_STATE, WI_IP, WI_COUNT };
enum InfoItem : uint8_t { II_UP = 0, II_VERSION, II_COUNT };

static uint8_t menuLevel   = 0;  // 0 = section list, 1 = items within section
static uint8_t menuSection = 0;
static uint8_t menuIdx     = 0;
static bool    menuEditing = false;

// Tracks last displayed remaining-second value; UINT32_MAX forces immediate redraw on arm.
static uint32_t lastDisplayedRemSec = UINT32_MAX;

static bool     lastInterlockState = true;
static uint32_t interlockMsgMs     = 0;  // non-zero = show "LOCKED" message until expired

struct WebSettings {
    uint32_t intervalSec;
    bool     focusEnabled;
    uint32_t focusMs;
    bool     preFocus;
    uint32_t shutterMs;
    bool     pinsSwapped;
    bool     pinsActiveLow;
    bool     wifiEnabled;
};
static bool        pendingWebValid = false;
static WebSettings pendingWeb;

// ==========================================================================
// WiFi helpers

static void startWiFi() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);

    IPAddress ip = WiFi.softAPIP();
    snprintf(wifiIP, sizeof(wifiIP), "%u.%u.%u.%u",
             ip[0], ip[1], ip[2], ip[3]);

    wifiServer.begin();
}

static void stopWiFi() {
    wifiServer.stop();
    WiFi.softAPdisconnect(true);
}

// ===========================================================================

static uint32_t stepForInterval(uint32_t s) {
    if (s < 60)  return 1;
    if (s < 300) return 5;
    if (s < 600) return 10;
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
// Menu helpers

static uint8_t sectionItemCount() {
    switch (menuSection) {
        case MS_CAMERA: return (uint8_t)CI_COUNT;
        case MS_WIFI:   return (uint8_t)WI_COUNT;
        case MS_INFO:   return (uint8_t)II_COUNT;
        default:        return 0;
    }
}

// ===========================================================================
// Draw helpers

static void drawPinState() {
    tft.fillRect(92, 0, 36, 14, ST7735_BLACK);
    tft.setTextSize(1);
    tft.setCursor(92, 4);
    tft.setTextColor(digitalRead(focusPin) == activeLevel() ? ST7735_GREEN : 0x7BEF);
    tft.print('F');
    tft.setCursor(104, 4);
    tft.setTextColor(digitalRead(shutterPin) == activeLevel() ? ST7735_GREEN : 0x7BEF);
    tft.print('S');
    tft.setCursor(116, 4);
    tft.setTextColor(digitalRead(PIN_INTERLOCK) == HIGH ? ST7735_GREEN : ST7735_RED);
    tft.print('L');
}

static void drawInterval() {
    tft.fillRect(0, 24, 128, 24, ST7735_BLACK);
    char buf[12];
    formatTime(intervalSec, buf, sizeof(buf));
    tft.setTextSize(3);
    tft.setTextColor(ST7735_WHITE);
    tft.setCursor(4, 24);
    tft.print(buf);
}

static void drawStatus() {
    tft.fillRect(0, 64, 128, 22, (state != DISARMED) ? ST7735_YELLOW : ST7735_GREEN);
    tft.setTextSize(2);
    tft.setTextColor(ST7735_BLACK);
    if (state != DISARMED) {
        tft.setCursor(32, 68);
        tft.print("ARMED");
    } else {
        tft.setCursor(18, 68);
        tft.print("DISARMED");
    }
}

static void drawCountdown() {
    tft.fillRect(0, 100, 128, 28, ST7735_BLACK);
    if (state == DISARMED) {
        tft.setTextSize(1);
        if (interlockMsgMs != 0) {
            tft.setTextColor(ST7735_RED);
            tft.setCursor(4, 104);
            tft.print("LOCKED");
        } else {
            tft.setTextColor(0x7BEF);
            tft.setCursor(4, 104);
            tft.print("press: arm");
            tft.setCursor(4, 116);
            tft.print("hold: menu");
        }
    } else if (state == FOCUSING) {
        tft.fillRect(0, 100, 128, 28, ST7735_CYAN);
        tft.setTextSize(2);
        tft.setTextColor(ST7735_BLACK);
        tft.setCursor(18, 106);
        tft.print("FOCUSING");
    } else if (state == SHOOTING) {
        tft.fillRect(0, 100, 128, 28, ST7735_RED);
        tft.setTextSize(2);
        tft.setTextColor(ST7735_WHITE);
        tft.setCursor(39, 106);
        tft.print("FIRE!");
    } else if (state == ARMED) {
        uint32_t intervalMs = intervalSec * 1000UL;
        uint32_t elapsed    = millis() - lastShotMs + shutterMs;
        uint32_t remaining  = (elapsed >= intervalMs) ? 0 : (intervalMs - elapsed + 999) / 1000;
        char nbuf[12];
        formatTime(remaining, nbuf, sizeof(nbuf));
        tft.setTextSize(1);
        tft.setTextColor(ST7735_CYAN);
        tft.setCursor(4, 104);
        tft.print("NEXT: ");
        tft.print(nbuf);
        tft.setTextColor(0x7BEF);
        tft.setCursor(4, 116);
        tft.print("press: disarm");
    }
}

static void drawMenuItem(uint8_t i) {
    uint8_t visibleIndex = i - menuScroll;
    if (visibleIndex >= MENU_VISIBLE) return;

    uint8_t y = 26 + visibleIndex * 14;
    bool sel     = (i == menuIdx);
    bool editing = sel && menuEditing;

    uint16_t dimCol = 0x7BEF;
    uint16_t selCol = ST7735_WHITE;
    uint16_t selDimCol = 0x9CF3;
    uint16_t valCol = editing ? ST7735_YELLOW : (sel ? selCol : dimCol);

    tft.fillRect(0, y, 128, 12, ST7735_BLACK);
    tft.setTextSize(1);
    tft.setCursor(0, y + 2);
    bool isUp   = (i == 0);

    if (menuLevel == 0) {
        static const char* const names[MS_COUNT] = {"^ Exit", "CAMERA", "WIFI", "INFO"};
        tft.setTextColor(isUp ? (sel ? ST7735_CYAN : dimCol) : (sel ? selCol : dimCol));
        tft.print(sel ? "> " : "  ");
        if (i < MS_COUNT) tft.print(names[i]);
        return;
    }

    // Level 1 — items within a section

    bool isInfo = !isUp &&
                  ((menuSection == MS_WIFI && i == WI_IP) ||
                   (menuSection == MS_INFO && (i == II_VERSION)));

    if (isInfo) {
        tft.setTextColor(sel ? selDimCol : dimCol);
        tft.print(sel ? '>' : ' ');
        tft.print(' ');
    } else {
        tft.setTextColor(isUp ? (sel ? ST7735_CYAN : dimCol) : (sel ? selCol : dimCol));
        tft.print(editing ? '#' : (sel ? '>' : ' '));
        tft.print(' ');
    }

    switch (menuSection) {
        case MS_CAMERA:
            switch ((CameraItem)i) {
                case CI_FOCUS_EN:
                    tft.print("Focus:      ");
                    tft.setTextColor(valCol);
                    tft.print(focusEnabled ? "ON" : "OFF");
                    break;
                case CI_FOCUS_MS:
                    tft.print("Focus ms:   ");
                    tft.setTextColor(valCol);
                    tft.print((unsigned)focusMs);
                    break;
                case CI_PRE_FOCUS:
                    tft.print("Pre-focus:  ");
                    tft.setTextColor(valCol);
                    tft.print(preFocus ? "ON" : "OFF");
                    break;
                case CI_SHUTTER_MS:
                    tft.print("Shutter ms: ");
                    tft.setTextColor(valCol);
                    tft.print((unsigned)shutterMs);
                    break;
                case CI_SWAP_PINS:
                    tft.print("Tip/Ring:   ");
                    tft.setTextColor(valCol);
                    tft.print(pinsSwapped ? "S/F" : "F/S");
                    break;
                case CI_INVERT_PINS:
                    tft.print("Active Lvl: ");
                    tft.setTextColor(valCol);
                    tft.print(pinsActiveLow ? "Low" : "High");
                    break;
                case CI_UP:
                    tft.print("^ Back");
                    break;
                default: break;
            }
            break;

        case MS_WIFI:
            switch ((WifiItem)i) {
                case WI_STATE:
                    tft.print("WiFi:      ");
                    tft.setTextColor(valCol);
                    tft.print(wifiEnabled ? "ON" : "OFF");
                    break;
                case WI_IP:
                    tft.print("IP:");
                    tft.print(wifiIP);
                    break;
                case WI_UP:
                    tft.print("^ Back");
                    break;
                default: break;
            }
            break;

        case MS_INFO: {
            switch ((InfoItem)i) {
                case II_VERSION:
                    tft.print("FW:");
                    tft.print(FW_VERSION);
                    break;
                case II_UP:
                    tft.print("^ Back");
                    break;
                default: break;
            }
            break;
        }
    }
}

static void updateDisplay() {
    if (!needFullRedraw && !needIntervalRedraw && !needStatusRedraw &&
        !needCountdownRedraw && !needPinStateRedraw) return;

    if (needFullRedraw) {
        needFullRedraw = needIntervalRedraw = needStatusRedraw =
            needCountdownRedraw = needPinStateRedraw = false;
        tft.fillScreen(ST7735_BLACK);

        if (state == MENU) {
            static const char* const sectionNames[MS_COUNT] = {"", "CAMERA", "WIFI", "INFO"};

            tft.setTextSize(2);
            tft.setTextColor(ST7735_WHITE);
            tft.setCursor(4, 4);
            tft.print(menuLevel == 0 ? "SETTINGS" : sectionNames[menuSection]);

            uint8_t total = (menuLevel == 0) ? (uint8_t)MS_COUNT : sectionItemCount();

            if (menuIdx < menuScroll) menuScroll = menuIdx;
            if (menuIdx >= menuScroll + MENU_VISIBLE)
                menuScroll = menuIdx - MENU_VISIBLE + 1;
            if (total > MENU_VISIBLE && menuScroll > total - MENU_VISIBLE)
                menuScroll = total - MENU_VISIBLE;

            uint8_t end = min((uint8_t)(menuScroll + MENU_VISIBLE), total);
            for (uint8_t i = menuScroll; i < end; i++) {
                drawMenuItem(i);
            }

            tft.setTextSize(1);
            tft.setTextColor(ST7735_WHITE);
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

static void applyWebSettings(const WebSettings& s) {
    if (s.pinsSwapped != pinsSwapped) {
        setFocusPin(inactiveLevel());
        setShutterPin(inactiveLevel());
        pinsSwapped = s.pinsSwapped;
        focusPin   = pinsSwapped ? picoWpins[ 9] : picoWpins[12];
        shutterPin = pinsSwapped ? picoWpins[12] : picoWpins[ 9];
        pinMode(focusPin,   OUTPUT); setFocusPin(inactiveLevel());
        pinMode(shutterPin, OUTPUT); setShutterPin(inactiveLevel());
    }
    if (s.pinsActiveLow != pinsActiveLow) {
        setFocusPin(inactiveLevel());
        setShutterPin(inactiveLevel());
        pinsActiveLow = s.pinsActiveLow;
        setFocusPin(inactiveLevel());
        setShutterPin(inactiveLevel());
    }
    if (s.intervalSec != intervalSec) { intervalSec = s.intervalSec; needIntervalRedraw = true; }
    focusEnabled = s.focusEnabled;
    focusMs      = s.focusMs;
    preFocus     = s.preFocus;
    shutterMs    = s.shutterMs;
    if (s.wifiEnabled != wifiEnabled) {
        if (!s.wifiEnabled) { stopWiFi();  wifiEnabled = false; }
        else                { startWiFi(); wifiEnabled = true;  }
    }
    needFullRedraw = true;
}

static void adjustMenuItemValue(int8_t dir) {
    if (menuSection != MS_CAMERA) return;
    switch ((CameraItem)menuIdx) {
        case CI_FOCUS_MS: {
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
        case CI_SHUTTER_MS: {
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
            if (pendingWebValid && (prev == ARMED || prev == FOCUSING || prev == SHOOTING)) {
                pendingWebValid = false;
                applyWebSettings(pendingWeb);
            }
            setFocusPin(inactiveLevel());
            setShutterPin(inactiveLevel());
            if (prev == MENU) needFullRedraw = true;
            else { needStatusRedraw = true; needCountdownRedraw = true; needPinStateRedraw = true; }
            break;
        case ARMED:
            if (pendingWebValid && prev == SHOOTING) {
                pendingWebValid = false;
                applyWebSettings(pendingWeb);
            }
            setFocusPin(inactiveLevel());
            setShutterPin(inactiveLevel());
            if (prev == DISARMED || prev == MENU) lastShotMs = millis();
            lastDisplayedRemSec = UINT32_MAX;  // force immediate redraw with correct value
            if (prev == MENU) needFullRedraw = true;
            else { needStatusRedraw = true; needCountdownRedraw = true; needPinStateRedraw = true; }
            break;
        case FOCUSING:
            setFocusPin(activeLevel());
            needCountdownRedraw = true;
            needPinStateRedraw  = true;
            break;
        case SHOOTING:
            setShutterPin(activeLevel());
            needCountdownRedraw = true;
            needPinStateRedraw  = true;
            break;
        case MENU:
            menuLevel   = 0;
            menuIdx     = 1;
            menuScroll  = 0;
            menuEditing = false;
            needFullRedraw = true;
            break;
    }
}

static void handleShortPress() {
    switch (state) {
        case DISARMED:
            if (digitalRead(PIN_INTERLOCK) == HIGH) {
                enterState(ARMED);
            } else {
                interlockMsgMs = millis();
                needCountdownRedraw = true;
            }
            break;
        case ARMED:
        case FOCUSING:
        case SHOOTING:
            enterState(DISARMED);
            break;
        case MENU:
            if (menuLevel == 0) {
                if (menuIdx == MS_BACK) {
                    enterState(DISARMED);
                    return;
                }
                menuLevel   = 1;
                menuSection = menuIdx;
                menuIdx     = 1;
                menuScroll  = 0;
                menuEditing = false;
                needFullRedraw = true;
            } else {
                bool isUp = (menuIdx == 0);
                if (isUp) {
                    menuLevel  = 0;
                    menuIdx    = menuSection;
                    menuScroll = 0;
                    menuEditing = false;
                    needFullRedraw = true;
                    break;
                }
                switch (menuSection) {
                    case MS_CAMERA:
                        switch ((CameraItem)menuIdx) {
                            case CI_FOCUS_EN:
                                focusEnabled = !focusEnabled;
                                needFullRedraw = true;
                                break;
                            case CI_PRE_FOCUS:
                                preFocus = !preFocus;
                                needFullRedraw = true;
                                break;
                            case CI_SWAP_PINS:
                                setFocusPin(inactiveLevel());
                                setShutterPin(inactiveLevel());
                                pinsSwapped = !pinsSwapped;
                                focusPin   = pinsSwapped ? picoWpins[ 9] : picoWpins[12];
                                shutterPin = pinsSwapped ? picoWpins[12] : picoWpins[ 9];
                                pinMode(focusPin,   OUTPUT); setFocusPin(inactiveLevel());
                                pinMode(shutterPin, OUTPUT); setShutterPin(inactiveLevel());
                                needFullRedraw = true;
                                break;
                            case CI_INVERT_PINS:
                                setFocusPin(inactiveLevel());
                                setShutterPin(inactiveLevel());
                                pinsActiveLow = !pinsActiveLow;
                                setFocusPin(inactiveLevel());
                                setShutterPin(inactiveLevel());
                                needFullRedraw = true;
                                break;
                            case CI_FOCUS_MS:
                            case CI_SHUTTER_MS:
                                menuEditing = !menuEditing;
                                needFullRedraw = true;
                                break;
                            default: break;
                        }
                        break;
                    case MS_WIFI:
                        if ((WifiItem)menuIdx == WI_STATE) {
                            if (wifiEnabled) { stopWiFi();  wifiEnabled = false; }
                            else             { startWiFi(); wifiEnabled = true;  }
                            needFullRedraw = true;
                        }
                        break;
                    case MS_INFO:
                        break;
                }
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
// Web interface

static const char PAGE_HTML[] = R"EOF(<!DOCTYPE html>
<html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Timelapse Control</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#ddd;font-family:monospace;padding:1em;max-width:480px;margin:0 auto}
h1{color:#0ff;letter-spacing:3px;padding:.5em 0;border-bottom:1px solid #222;margin-bottom:.8em}
.row{display:flex;justify-content:space-between;align-items:center;padding:.35em 0;border-bottom:1px solid #111}
.lbl{color:#7bef;font-size:.85em}
.val{font-weight:bold}
.arm{color:#ff0}.dis{color:#0f0}.focusing{color:#0ff}.shooting{color:#f00}
.pon{color:#0f0}.poff{color:#333}.lflash{color:#f00;font-weight:bold}
.btn{background:#1a1a1a;color:#ddd;border:1px solid #444;padding:.5em 1.2em;cursor:pointer;font:1em monospace;border-radius:3px;margin:.2em}
.go{width:100%;display:block;background:#333300;border-color:#ff0;color:#ff0}
.stop{width:100%;display:block;background:#003300;border-color:#0f0;color:#0f0}
.exitmenu{background:#001a33;border-color:#07f;color:#7af}
.save{background:#001933;border-color:#06f;color:#6af;width:100%;margin-top:.6em}
.savepend{background:#332200;border-color:#ff0;color:#ff0}
input[type=number]{background:#111;color:#ddd;border:1px solid #444;padding:.3em .5em;font:1em monospace;width:90px;border-radius:3px}
select{background:#111;color:#ddd;border:1px solid #444;padding:.3em .5em;font:1em monospace;border-radius:3px}
.tabs{display:flex;gap:3px;margin:.8em 0 0}
.tab{flex:1;background:#111;color:#7bef;border:1px solid #333;padding:.4em;cursor:pointer;font:1em monospace;border-radius:3px 3px 0 0;border-bottom:none}
.tab.on{background:#1a1a1a;color:#fff;border-color:#555}
.panel{border:1px solid #333;padding:.6em;display:none}
.panel.on{display:block}
.info{color:#7bef;font-size:.85em;padding:.3em 0;border-bottom:1px solid #111}
#disp{margin-bottom:.8em}
</style></head>
<body>
<h1 style="text-align:center;">Timelapse Control</h1>
<div id='disp'></div>
<button id='ab' class='btn go' onclick='arm()'>ARM</button>
<div class='tabs'>
<button class='tab on' onclick='tab(0)' id='t0'>CAMERA</button>
<button class='tab' onclick='tab(1)' id='t1'>WIFI</button>
<button class='tab' onclick='tab(2)' id='t2'>INFO</button>
</div>
<div id='p0' class='panel on'>
<form id='sf' onsubmit='save(event)'>
<div class='row'><span class='lbl'>Interval (s)</span><input type='number' id='iv' name='interval' min='1' max='3600'></div>
<div class='row'><span class='lbl'>Focus</span><select id='fe' name='focusEnabled'><option value='1'>ON</option><option value='0'>OFF</option></select></div>
<div class='row'><span class='lbl'>Focus ms</span><input type='number' id='fm' name='focusMs' min='50' max='5000' step='50'></div>
<div class='row'><span class='lbl'>Pre-focus</span><select id='pf' name='preFocus'><option value='1'>ON</option><option value='0'>OFF</option></select></div>
<div class='row'><span class='lbl'>Shutter ms</span><input type='number' id='sm' name='shutterMs' min='50' max='2000' step='50'></div>
<div class='row'><span class='lbl'>Tip / Ring</span><select id='ps' name='pinsSwapped'><option value='0'>F / S</option><option value='1'>S / F</option></select></div>
<div class='row'><span class='lbl'>Active level</span><select id='pi' name='pinsActiveLow'><option value='0'>LOW</option><option value='1'>HIGH</option></select></div>
<button id='sb' class='btn save' type='submit'>SAVE SETTINGS</button>
</form>
</div>
<div id='p1' class='panel'>
<div class='row'><span class='lbl'>WiFi</span><select id='we' onchange='saveWifi(this.value)'><option value='1'>ON</option><option value='0'>OFF</option></select></div>
<div class='info' id='wi-ip'>IP: —</div>
</div>
<div id='p2' class='panel'>
<div class='info' id='inf-fw'>Firmware: —</div>
</div>
<script>
var ok=false,lockFlash=false;
function tab(n){
  for(var i=0;i<3;i++){
    var on=i===n;
    document.getElementById('t'+i).className='tab'+(on?' on':'');
    document.getElementById('p'+i).className='panel'+(on?' on':'');
  }
}
function fmt(s){
  if(s>=3600)return Math.floor(s/3600)+'h'+pad(Math.floor((s%3600)/60))+'m'+pad(s%60)+'s';
  if(s>=60)return Math.floor(s/60)+'m'+pad(s%60)+'s';
  return s+'s';
}
function pad(n){return String(n).padStart(2,'0');}
function upd(d){
  var sc=d.state==='ARMED'?'arm':d.state==='DISARMED'?'dis':d.state==='FOCUSING'?'focusing':d.state==='SHOOTING'?'shooting':'dis';
  var firing=(d.state==='FOCUSING'||d.state==='SHOOTING');
  var h='';
  h+='<div class="row"><span class="lbl">STATUS</span><span class="val '+sc+'">'+d.state+'</span></div>';
  h+='<div class="row"><span class="lbl">INTERVAL</span><span class="val">'+d.intervalSec+'s</span></div>';
  h+='<div class="row"><span class="lbl">NEXT SHOT</span><span class="val" style="color:#0ff">'+(d.state==='ARMED'?d.countdown+'s':'---')+'</span></div>';
  h+='<div class="row">'+(firing?'<span class="val" style="color:#0f0">&#9632; FIRING</span>':'<span class="val" style="color:#333">--- idle ---</span>')+'</div>';
  h+='<div class="row"><span class="lbl">FOCUS PIN</span><span class="'+(d.focusActive?'pon':'poff')+'">'+(d.focusActive?'ACTIVE':'---')+'</span><span class="lbl">SHUTTER PIN</span><span class="'+(d.shutterActive?'pon':'poff')+'">'+(d.shutterActive?'ACTIVE':'---')+'</span></div>';
  h+='<div class="row"><span class="lbl">INTERLOCK</span><span class="val '+(!d.interlockEnabled&&lockFlash?'lflash':d.interlockEnabled?'pon':'poff')+'">'+(d.interlockEnabled?'ENABLED':'DISABLED')+'</span></div>';
  document.getElementById('disp').innerHTML=h;
  var ab=document.getElementById('ab');
  if(d.state==='DISARMED'){ab.textContent='ARM';ab.className='btn go';}
  else if(d.state==='MENU'){ab.textContent='Exit Menu';ab.className='btn exitmenu';}
  else{ab.textContent='DISARM';ab.className='btn stop';}
  var sb=document.getElementById('sb');
  if(sb){sb.className='btn save'+(d.pendingSettings?' savepend':'');sb.textContent=d.pendingSettings?'SAVE SETTINGS (pending)':'SAVE SETTINGS';}
  document.getElementById('wi-ip').textContent='IP: '+d.ip;
  document.getElementById('inf-fw').textContent='Firmware: '+d.fwVersion;
  if(!ok){
    function s(id,v){document.getElementById(id).value=v;}
    s('iv',d.intervalSec);s('fe',d.focusEnabled?'1':'0');s('fm',d.focusMs);
    s('pf',d.preFocus?'1':'0');s('sm',d.shutterMs);
    s('ps',d.pinsSwapped?'1':'0');s('pi',d.pinsActiveLow?'1':'0');
    s('we',d.wifiEnabled?'1':'0');
    ok=true;
  }
}
function poll(){fetch('/api/state').then(function(r){return r.json();}).then(upd).catch(function(){});}
function arm(){
  fetch('/api/arm',{method:'POST'}).then(function(r){return r.json();}).then(function(d){
    if(d.locked){lockFlash=true;poll();setTimeout(function(){lockFlash=false;poll();},1500);}
    else{poll();}
  });
}
function save(e){
  e.preventDefault();
  var p=new URLSearchParams(new FormData(document.getElementById('sf')));
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()}).then(poll);
}
function saveWifi(v){
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'wifiEnabled='+v}).then(poll);
}
poll();setInterval(poll,100);
</script>
</body></html>
)EOF";

static String urlParam(const String& body, const char* key) {
    String k = String(key) + "=";
    int i = body.indexOf(k);
    if (i < 0) return String();
    i += k.length();
    int j = body.indexOf('&', i);
    return j < 0 ? body.substring(i) : body.substring(i, j);
}

static void serveMainPage(WiFiClient& client) {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
    client.print(PAGE_HTML);
}

static void serveState(WiFiClient& client) {
    uint32_t countdown = 0;
    if (state == ARMED) {
        uint32_t elapsed    = millis() - lastShotMs + shutterMs;
        uint32_t intervalMs = intervalSec * 1000UL;
        countdown = elapsed >= intervalMs ? 0 : (intervalMs - elapsed + 999) / 1000;
    }

    const char* stateStr =
        (state == DISARMED) ? "DISARMED" :
        (state == ARMED)    ? "ARMED" :
        (state == FOCUSING) ? "FOCUSING" :
        (state == SHOOTING) ? "SHOOTING" :
                              "MENU";

    char json[620];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"intervalSec\":%lu,\"countdown\":%lu,"
        "\"focusActive\":%s,\"shutterActive\":%s,"
        "\"focusEnabled\":%s,\"focusMs\":%lu,\"preFocus\":%s,"
        "\"shutterMs\":%lu,\"pinsSwapped\":%s,\"pinsActiveLow\":%s,"
        "\"wifiEnabled\":%s,\"ip\":\"%s\","
        "\"interlockEnabled\":%s,\"pendingSettings\":%s,"
        "\"uptimeSec\":%lu,\"fwVersion\":\"%s\"}",
        stateStr,
        (unsigned long)intervalSec,
        (unsigned long)countdown,
        digitalRead(focusPin) == activeLevel() ? "true" : "false",
        digitalRead(shutterPin) == activeLevel() ? "true" : "false",
        focusEnabled ? "true" : "false",
        (unsigned long)focusMs,
        preFocus ? "true" : "false",
        (unsigned long)shutterMs,
        pinsSwapped ? "true" : "false",
        pinsActiveLow ? "true" : "false",
        wifiEnabled ? "true" : "false",
        wifiIP,
        digitalRead(PIN_INTERLOCK) == HIGH ? "true" : "false",
        pendingWebValid ? "true" : "false",
        (unsigned long)(millis() / 1000),
        FW_VERSION
    );

    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
    client.print(json);
}

static void handleArm(WiFiClient& client) {
    if (state == DISARMED && digitalRead(PIN_INTERLOCK) != HIGH) {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false,\"locked\":true}");
        return;
    }
    if (state == DISARMED) enterState(ARMED);
    else                   enterState(DISARMED);
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}");
}

static WebSettings parseWebSettings(const String& body, const WebSettings& base) {
    WebSettings s = base;
    String v;

    v = urlParam(body, "interval");
    if (v.length()) s.intervalSec = (uint32_t)constrain(v.toInt(), 1L, 3600L);

    v = urlParam(body, "focusEnabled");
    if (v.length()) s.focusEnabled = (bool)v.toInt();

    v = urlParam(body, "focusMs");
    if (v.length()) {
        s.focusMs = (uint32_t)constrain(v.toInt(), 50L, 5000L);
        while (s.intervalSec * 1000UL <= s.focusMs)
            s.intervalSec = min(s.intervalSec + stepForInterval(s.intervalSec), (uint32_t)3600);
    }

    v = urlParam(body, "preFocus");
    if (v.length()) s.preFocus = (bool)v.toInt();

    v = urlParam(body, "shutterMs");
    if (v.length()) s.shutterMs = (uint32_t)constrain(v.toInt(), 50L, 2000L);

    v = urlParam(body, "pinsSwapped");
    if (v.length()) s.pinsSwapped = (bool)v.toInt();

    v = urlParam(body, "pinsActiveLow");
    if (v.length()) s.pinsActiveLow = (bool)v.toInt();

    v = urlParam(body, "wifiEnabled");
    if (v.length()) s.wifiEnabled = (bool)v.toInt();

    return s;
}

static void handleSettings(WiFiClient& client, const String& body) {
    bool active = (state == ARMED || state == FOCUSING || state == SHOOTING);
    WebSettings base = pendingWebValid
        ? pendingWeb
        : WebSettings{intervalSec, focusEnabled, focusMs, preFocus,
                      shutterMs, pinsSwapped, pinsActiveLow, wifiEnabled};
    WebSettings s = parseWebSettings(body, base);
    if (active) {
        pendingWeb      = s;
        pendingWebValid = true;
    } else {
        applyWebSettings(s);
    }
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}");
}

static void handleWebClient() {
    if (!wifiEnabled) return;
    WiFiClient client = wifiServer.accept();
    if (!client) return;

    client.setTimeout(500);
    String reqLine = client.readStringUntil('\n');

    int contentLen = 0;
    String hdr;
    while (true) {
        hdr = client.readStringUntil('\n');
        hdr.trim();
        if (hdr.isEmpty()) break;
        if (hdr.startsWith("Content-Length:") || hdr.startsWith("content-length:"))
            contentLen = hdr.substring(hdr.indexOf(':') + 1).toInt();
    }

    String body;
    if (contentLen > 0) {
        char buf[256];
        int n = client.readBytes(buf, min(contentLen, 255));
        buf[n] = '\0';
        body = buf;
    }

    reqLine.trim();
    if      (reqLine.startsWith("GET / ") || reqLine == "GET /") serveMainPage(client);
    else if (reqLine.startsWith("GET /api/state"))               serveState(client);
    else if (reqLine.startsWith("POST /api/arm"))                handleArm(client);
    else if (reqLine.startsWith("POST /api/settings"))           handleSettings(client, body);
    else client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found");

    client.stop();
}

// ===========================================================================

static void encoderISR() {
    uint8_t curr = (digitalRead(PIN_ENC_B) << 1) | digitalRead(PIN_ENC_A);
    switch ((isrEncState << 2) | curr) {
        case 0b0001: case 0b0111: case 0b1110: case 0b1000: encRaw--; break;
        case 0b0010: case 0b1011: case 0b1101: case 0b0100: encRaw++; break;
    }
    isrEncState = curr;
}

static void readEncoder() {
    noInterrupts();
    int8_t raw = encRaw;
    encRaw = 0;
    interrupts();

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
            uint8_t total = (menuLevel == 0) ? (uint8_t)MS_COUNT : sectionItemCount();
            int16_t newIdx = (int16_t)menuIdx + dir;
            if (newIdx < 0) newIdx = total - 1;
            if (newIdx >= total) newIdx = 0;
            menuIdx = (uint8_t)newIdx;
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
            uint32_t elapsed    = now - lastShotMs + shutterMs;
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
    pinMode(PIN_LED_FOCUS,   OUTPUT);  digitalWrite(PIN_LED_FOCUS,   LOW);
    pinMode(PIN_LED_SHUTTER, OUTPUT);  digitalWrite(PIN_LED_SHUTTER, LOW);
    pinMode(PIN_INTERLOCK,   INPUT);
    lastInterlockState = digitalRead(PIN_INTERLOCK) == HIGH;

    pinMode(focusPin,    OUTPUT);
    pinMode(shutterPin,  OUTPUT);
    setFocusPin(inactiveLevel());
    setShutterPin(inactiveLevel());

    pinMode(PIN_ENC_A,   INPUT);
    pinMode(PIN_ENC_B,   INPUT);
    pinMode(PIN_ENC_BTN, INPUT);

    SPI1.setSCK(picoWpins[19]);  // physical 19 → GP14
    SPI1.setTX(picoWpins[20]);   // physical 20 → GP15
    SPI1.begin();
    tft.initR(INITR_GREENTAB);
    tft.setRotation(2);

    isrEncState = (digitalRead(PIN_ENC_B) << 1) | digitalRead(PIN_ENC_A);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);

    startWiFi();

    updateDisplay();
}

void loop() {
    bool interlockNow = digitalRead(PIN_INTERLOCK) == HIGH;
    if (interlockNow != lastInterlockState) {
        lastInterlockState = interlockNow;
        needPinStateRedraw = true;
        if (!interlockNow && state != DISARMED && state != MENU)
            enterState(DISARMED);
    }

    if (interlockMsgMs != 0 && millis() - interlockMsgMs >= 1500) {
        interlockMsgMs = 0;
        needCountdownRedraw = true;
    }

    readEncoder();
    readButton();
    updateStateMachine();

    handleWebClient();

    // Redraw countdown whenever remaining-second changes (relative to lastShotMs,
    // not absolute time — avoids stale lastCountdownSec causing wrong initial value).
    if (state == ARMED) {
        uint32_t elapsed    = millis() - lastShotMs + shutterMs;
        uint32_t intervalMs = intervalSec * 1000UL;
        uint32_t remSec     = (elapsed >= intervalMs) ? 0 : (intervalMs - elapsed + 999) / 1000;
        if (remSec != lastDisplayedRemSec) {
            lastDisplayedRemSec = remSec;
            needCountdownRedraw = true;
        }
    }

    updateDisplay();
    
}
