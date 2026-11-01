/*
 * StickS3-CanonRemote
 * Autonomous Bluetooth Low Energy (BLE) Remote for Canon EOS Cameras
 * Hardware: M5Stack M5StickS3 (ESP32-S3, 135x240 ST7789v2 LCD)
 *
 * Description:
 *   Standalone BLE remote compatible with Canon EOS cameras (BR-E1 protocol).
 *   Includes persistent NimBLE bond storage fix for ESP32 Arduino Core 3.x,
 *   30 FPS flicker-free sprite rendering, zero-latency search abort,
 *   and two-stage power saving.
 *
 * Author: wbk & Echo
 * License: MIT
 */

#include <M5Unified.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>

#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)
#include "host/ble_hs.h"
#include "host/ble_store.h"
#endif

// Hardware Button GPIO definitions for M5StickS3
#define PIN_BTN_FRONT      11  // Blue front button: Execute / Toggle abort
#define PIN_BTN_SIDE       12  // Side button: Next menu item + Instant abort
#define PIN_BTN_EXT_GROVE  10  // External button on Grove port (G10)
#define PIN_BTN_EXT_HAT    5   // External button on Top Hat header (G5)

// Power management & 30 FPS display timing constants
const unsigned long FRAME_INTERVAL_US     = 33333;  // Exact 30.00 FPS (33.33 ms)
const unsigned long IDLE_SLEEP_TIMEOUT_MS = 60000;  // 1 minute: screen off + BLE standby
const unsigned long IDLE_OFF_TIMEOUT_MS   = 300000; // 5 minutes: full hardware power off
const unsigned long DEBOUNCE_DELAY_MS     = 25;     // Minimal physical contact filter

// Global abort flag shared between UI loop and BLE worker
volatile bool g_abortSearch = false;

// ==================== NIMBLE BOND PERSISTENCE ENGINE ====================
// DO NOT MODIFY: ESP32 Core 3.x NimBLE RAM-only key persistence workaround
#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)
static ble_store_write_fn* orig_store_write_cb = nullptr;

static int custom_store_write_cb(int obj_type, const union ble_store_value *val) {
    int rc = 0;
    if (orig_store_write_cb != nullptr) {
        rc = orig_store_write_cb(obj_type, val);
    }

    if (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        Preferences p;
        p.begin("canon_ble", false);
        p.putBytes("our_sec", &val->sec, sizeof(struct ble_store_value_sec));
        p.end();
    } else if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        Preferences p;
        p.begin("canon_ble", false);
        p.putBytes("peer_sec", &val->sec, sizeof(struct ble_store_value_sec));
        p.end();
    }
    return rc;
}

static void restoreNimbleBondKeys() {
    if (orig_store_write_cb == nullptr && ble_hs_cfg.store_write_cb != custom_store_write_cb) {
        orig_store_write_cb = ble_hs_cfg.store_write_cb;
        ble_hs_cfg.store_write_cb = custom_store_write_cb;
    }

    if (orig_store_write_cb != nullptr) {
        Preferences p;
        p.begin("canon_ble", true);
        union ble_store_value val;

        if (p.getBytesLength("our_sec") == sizeof(struct ble_store_value_sec)) {
            memset(&val, 0, sizeof(val));
            p.getBytes("our_sec", &val.sec, sizeof(struct ble_store_value_sec));
            orig_store_write_cb(BLE_STORE_OBJ_TYPE_OUR_SEC, &val);
        }

        if (p.getBytesLength("peer_sec") == sizeof(struct ble_store_value_sec)) {
            memset(&val, 0, sizeof(val));
            p.getBytes("peer_sec", &val.sec, sizeof(struct ble_store_value_sec));
            orig_store_write_cb(BLE_STORE_OBJ_TYPE_PEER_SEC, &val);
        }
        p.end();
    }
}
#endif

// ==================== CANON BLE REMOTE PROTOCOL ====================
static const BLEUUID CANON_SVC_UUID("00050000-0000-1000-0000-d8492fffa821");
static const BLEUUID CANON_PAIR_UUID("00050002-0000-1000-0000-d8492fffa821");
static const BLEUUID CANON_TRIG_UUID("00050003-0000-1000-0000-d8492fffa821");

#define CMD_SHUTTER_RELEASE 0b10000000
#define CMD_AUTOFOCUS       0b01000000
#define CMD_MODE_IMMEDIATE  0b00001100

using BleAddressType = decltype(((BLEAdvertisedDevice*)nullptr)->getAddressType());

class CanonAdvCallback : public BLEAdvertisedDeviceCallbacks {
public:
    BLEAddress* pTargetAddr;
    BleAddressType* pTargetType;
    bool* pFoundFlag;

    CanonAdvCallback(BLEAddress* addr, BleAddressType* type, bool* found)
        : pTargetAddr(addr), pTargetType(type), pFoundFlag(found) {}

    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (g_abortSearch) {
            advertisedDevice.getScan()->stop();
            return;
        }
        if (advertisedDevice.haveServiceUUID() &&
            advertisedDevice.isAdvertisingService(CANON_SVC_UUID)) {
            *pTargetAddr = advertisedDevice.getAddress();
            *pTargetType = advertisedDevice.getAddressType();
            *pFoundFlag = true;
            advertisedDevice.getScan()->stop();
        }
    }
};

class CanonBLERemote {
private:
    String remoteName;
    BLEAddress targetAddress = BLEAddress("00:00:00:00:00:00");
    BleAddressType addressType = (BleAddressType)0;
    BLEClient* pClient = nullptr;
    BLERemoteCharacteristic* pTriggerChar = nullptr;
    bool cameraSaved = false;
    Preferences storage;

    bool sendHandshake(BLERemoteService* pService) {
        BLERemoteCharacteristic* pPairChar = pService->getCharacteristic(CANON_PAIR_UUID);
        if (pPairChar == nullptr) return false;

        uint8_t payload[32];
        size_t len = remoteName.length() + 1;
        if (len > sizeof(payload)) len = sizeof(payload);
        payload[0] = 0x03;
        memcpy(&payload[1], remoteName.c_str(), len - 1);

        pPairChar->writeValue(payload, len, false);
        delay(200);
        return true;
    }

public:
    CanonBLERemote(String name) : remoteName(name) {}

    void init() {
        BLEDevice::init(remoteName.c_str());

        BLESecurity* pSecurity = new BLESecurity();
        pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
        pSecurity->setCapability(ESP_IO_CAP_NONE);
        pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)
        restoreNimbleBondKeys();
#endif

        pClient = BLEDevice::createClient();

        storage.begin("canon_ble", true);
        if (storage.getBytesLength("addr_obj") == sizeof(BLEAddress)) {
            storage.getBytes("addr_obj", &targetAddress, sizeof(BLEAddress));
            addressType = (BleAddressType)storage.getUChar("type", 0);
            cameraSaved = true;
        }
        storage.end();
    }

    bool hasPairedCamera() const {
        return cameraSaved;
    }

    bool isConnected() const {
        return (pClient != nullptr && pClient->isConnected());
    }

    void requestAbort() {
        g_abortSearch = true;
        BLEScan* pScan = BLEDevice::getScan();
        if (pScan != nullptr) {
            pScan->stop();
        }
#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)
        ble_gap_conn_cancel();
#endif
    }

    void disconnect() {
        if (pClient != nullptr && pClient->isConnected()) {
            pClient->disconnect();
            delay(100);
        }
        pTriggerChar = nullptr;
    }

    bool connect() {
        if (!cameraSaved || g_abortSearch) return false;
        if (isConnected() && pTriggerChar != nullptr) return true;

        pTriggerChar = nullptr;

        bool ok = pClient->connect(targetAddress, addressType);
        if (g_abortSearch) {
            disconnect();
            return false;
        }

        if (!ok && !g_abortSearch) {
            bool found = false;
            BLEScan* pScan = BLEDevice::getScan();
            pScan->setAdvertisedDeviceCallbacks(new CanonAdvCallback(&targetAddress, &addressType, &found));
            pScan->setInterval(1349);
            pScan->setWindow(449);
            pScan->setActiveScan(true);
            pScan->start(2, false);

            if (found && !g_abortSearch) {
                delay(100);
                ok = pClient->connect(targetAddress, addressType);
            }
        }

        if (!ok || g_abortSearch) {
            if (isConnected()) disconnect();
            return false;
        }

        delay(150);
        BLERemoteService* pService = pClient->getService(CANON_SVC_UUID);
        if (pService == nullptr) {
            pClient->disconnect();
            return false;
        }

        if (!sendHandshake(pService)) {
            pClient->disconnect();
            return false;
        }

        pTriggerChar = pService->getCharacteristic(CANON_TRIG_UUID);
        if (pTriggerChar == nullptr) {
            pClient->disconnect();
            return false;
        }

        delay(100);
        return true;
    }

    bool pair(unsigned int timeoutSeconds) {
        disconnect();
        if (g_abortSearch) return false;

        bool found = false;
        BLEScan* pScan = BLEDevice::getScan();
        pScan->setAdvertisedDeviceCallbacks(new CanonAdvCallback(&targetAddress, &addressType, &found));
        pScan->setInterval(1349);
        pScan->setWindow(449);
        pScan->setActiveScan(true);
        pScan->start(timeoutSeconds, false);

        if (!found || g_abortSearch) return false;

        delay(200);
        if (g_abortSearch || !pClient->connect(targetAddress, addressType)) {
            return false;
        }

        BLERemoteService* pService = pClient->getService(CANON_SVC_UUID);
        if (pService == nullptr) {
            pClient->disconnect();
            return false;
        }

        if (!sendHandshake(pService)) {
            pClient->disconnect();
            return false;
        }

        delay(500);

        storage.begin("canon_ble", false);
        storage.putBytes("addr_obj", &targetAddress, sizeof(BLEAddress));
        storage.putUChar("type", (uint8_t)addressType);
        storage.end();
        cameraSaved = true;

        pTriggerChar = pService->getCharacteristic(CANON_TRIG_UUID);
        return (pTriggerChar != nullptr);
    }

    bool trigger() {
        if (!isConnected()) pTriggerChar = nullptr;
        if (!connect() || pTriggerChar == nullptr) return false;

        uint8_t pressCmd = CMD_MODE_IMMEDIATE | CMD_SHUTTER_RELEASE;
        pTriggerChar->writeValue(&pressCmd, 1, false);
        delay(250);
        uint8_t relCmd = CMD_MODE_IMMEDIATE;
        pTriggerChar->writeValue(&relCmd, 1, false);
        return true;
    }

    bool focus() {
        if (!isConnected()) pTriggerChar = nullptr;
        if (!connect() || pTriggerChar == nullptr) return false;

        uint8_t pressCmd = CMD_MODE_IMMEDIATE | CMD_AUTOFOCUS;
        pTriggerChar->writeValue(&pressCmd, 1, false);
        delay(250);
        uint8_t relCmd = CMD_MODE_IMMEDIATE;
        pTriggerChar->writeValue(&relCmd, 1, false);
        return true;
    }
};

// ==================== APPLICATION LOGIC & 30 FPS UI ====================
const int MENU_ITEMS_COUNT = 4;
const char* menuLabels[] = {"Shutter", "Focus", "Video", "Pair"};

#define CLR_BG       TFT_BLACK
#define CLR_TEXT     TFT_WHITE
#define CLR_SELECT   0x07E0
#define CLR_TITLE    0x07FF
#define CLR_OK       0x07E0
#define CLR_ERR      0xF800
#define CLR_DIVIDER  0x630C
#define CLR_WARN     0xFD20

M5Canvas canvas(&M5.Display);
CanonBLERemote canonRemote("StickS3-Remote");

int activeMenuIndex = 0;
bool connectionState = false;
String currentStatusText = "Init...";
bool videoRecordingState = false;
int cachedBatteryLevel = -1;
volatile bool uiDirty = true;

// Non-blocking worker state
volatile bool bleWorkerBusy = false;
volatile int pendingActionIdx = -1;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

// Power saving state
unsigned long lastActivityMs = 0;
bool isScreenSleeping = false;

// Button states
bool lastFrontState = HIGH;
bool lastSideState  = HIGH;
bool lastGroveState = HIGH;
bool lastHatState   = HIGH;

unsigned long lastFrontDebounce = 0;
unsigned long lastSideDebounce  = 0;
unsigned long lastGroveDebounce = 0;
unsigned long lastHatDebounce   = 0;

void setRemoteStatusSafe(const String& text, bool connected) {
    portENTER_CRITICAL(&statusMux);
    currentStatusText = text;
    connectionState = connected;
    uiDirty = true;
    portEXIT_CRITICAL(&statusMux);
}

// Draw interface into off-screen sprite buffer (zero black-frame flicker)
void drawToCanvas() {
    String statusCopy;
    bool connCopy;
    portENTER_CRITICAL(&statusMux);
    statusCopy = currentStatusText;
    connCopy = connectionState;
    portEXIT_CRITICAL(&statusMux);

    canvas.fillScreen(CLR_BG);

    // Title bar
    canvas.setTextColor(CLR_TITLE, CLR_BG);
    canvas.setTextSize(1);
    canvas.setCursor(6, 6);
    canvas.print("Canon BLE");

    // Battery percentage display
    int bat = M5.Power.getBatteryLevel();
    cachedBatteryLevel = bat;
    if (bat >= 0) {
        canvas.setTextColor(bat > 20 ? CLR_OK : CLR_ERR, CLR_BG);
        canvas.setCursor(96, 6);
        canvas.printf("%3d%%", bat);
    }

    // Connection status label
    canvas.setTextSize(1);
    if (connCopy) {
        canvas.setTextColor(CLR_OK, CLR_BG);
        canvas.setCursor(6, 22);
        canvas.print("* ");
        canvas.print(statusCopy);
    } else {
        canvas.setTextColor(CLR_WARN, CLR_BG);
        canvas.setCursor(6, 22);
        canvas.print("o ");
        canvas.print(statusCopy);
    }

    canvas.drawFastHLine(0, 36, 135, CLR_DIVIDER);

    // Menu list
    for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
        int yPos = 46 + i * 36;

        if (i == activeMenuIndex) {
            canvas.fillRoundRect(6, yPos, 123, 28, 5, CLR_SELECT);
            canvas.setTextColor(TFT_BLACK, CLR_SELECT);
        } else {
            canvas.setTextColor(CLR_TEXT, CLR_BG);
        }

        canvas.setTextSize(2);
        canvas.setCursor(14, yPos + 6);
        canvas.print(menuLabels[i]);

        if (i == 2 && videoRecordingState) {
            canvas.fillCircle(116, yPos + 14, 6, CLR_ERR);
        }
    }

    // Navigation hints
    canvas.drawFastHLine(0, 198, 135, CLR_DIVIDER);
    canvas.setTextColor(CLR_DIVIDER, CLR_BG);
    canvas.setTextSize(1);
    canvas.setCursor(6, 206);
    canvas.print("Blue/Ext: Execute");
    canvas.setCursor(6, 220);
    canvas.print("Side: Next Item");
}

void renderInterfaceNow() {
    if (isScreenSleeping) return;
    uiDirty = false;
    drawToCanvas();
    M5.Display.startWrite();
    canvas.pushSprite(0, 0);
    M5.Display.endWrite();
}

void wakeUpScreen() {
    lastActivityMs = millis();
    if (isScreenSleeping) {
        isScreenSleeping = false;
        M5.Display.wakeup();
        M5.Display.setBrightness(180);
        renderInterfaceNow();
    }
}

// Background FreeRTOS task executing BLE operations without freezing the UI loop
void bleWorkerTask(void* pvParameters) {
    while (true) {
        int cmd = pendingActionIdx;
        if (cmd >= 0) {
            pendingActionIdx = -1;
            bleWorkerBusy = true;

            if (cmd == 3) {
                bool paired = canonRemote.pair(15);
                if (!g_abortSearch) {
                    if (paired) {
                        activeMenuIndex = 0;
                        setRemoteStatusSafe("Paired!", true);
                    } else {
                        setRemoteStatusSafe("Pair Timeout", false);
                    }
                }
            } else {
                unsigned long startTime = millis();
                bool success = false;

                switch (cmd) {
                    case 0:
                        success = canonRemote.trigger();
                        break;
                    case 1:
                        success = canonRemote.focus();
                        break;
                    case 2:
                        success = canonRemote.trigger();
                        if (success && !g_abortSearch) {
                            videoRecordingState = !videoRecordingState;
                        }
                        break;
                }

                if (!g_abortSearch) {
                    unsigned long elapsed = millis() - startTime;
                    if (success) {
                        if (elapsed > 2000) {
                            setRemoteStatusSafe("OK (" + String(elapsed / 1000) + "s)", true);
                        } else {
                            setRemoteStatusSafe("OK (" + String(elapsed) + "ms)", true);
                        }
                    } else {
                        videoRecordingState = false;
                        setRemoteStatusSafe("No Camera", false);
                    }
                }
            }

            lastActivityMs = millis();
            bleWorkerBusy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void abortActiveSearch() {
    lastActivityMs = millis();
    pendingActionIdx = -1;
    g_abortSearch = true;
    canonRemote.requestAbort();
    videoRecordingState = false;
    setRemoteStatusSafe("Canceled", false);
}

void startMenuAction(int index) {
    lastActivityMs = millis();

    if (index != 3 && !canonRemote.hasPairedCamera()) {
        activeMenuIndex = 3;
        setRemoteStatusSafe("Pair first!", false);
        return;
    }

    g_abortSearch = false;
    bleWorkerBusy = true;

    if (index == 3) {
        setRemoteStatusSafe("Pairing 15s...", false);
    } else if (!canonRemote.isConnected()) {
        setRemoteStatusSafe("Connecting...", false);
    } else {
        setRemoteStatusSafe(String(menuLabels[index]) + "...", true);
    }

    pendingActionIdx = index;
}

void handleActionButton() {
    if (isScreenSleeping) {
        wakeUpScreen();
        return;
    }
    // Instant toggle: if currently searching, abort; if idle, start immediately
    if (bleWorkerBusy) {
        abortActiveSearch();
    } else {
        startMenuAction(activeMenuIndex);
    }
}

void processButtons() {
    unsigned long now = millis();

    bool curFront = digitalRead(PIN_BTN_FRONT);
    bool curSide  = digitalRead(PIN_BTN_SIDE);
    bool curGrove = digitalRead(PIN_BTN_EXT_GROVE);
    bool curHat   = digitalRead(PIN_BTN_EXT_HAT);

    // 1. Front Blue Button (GPIO 11) -> Execute or Abort immediately
    if (curFront != lastFrontState && (now - lastFrontDebounce) > DEBOUNCE_DELAY_MS) {
        lastFrontDebounce = now;
        lastFrontState = curFront;
        if (curFront == LOW) {
            handleActionButton();
        }
    }

    // 2. Side Button (GPIO 12) -> Next menu item (and abort search if active)
    if (curSide != lastSideState && (now - lastSideDebounce) > DEBOUNCE_DELAY_MS) {
        lastSideDebounce = now;
        lastSideState = curSide;
        if (curSide == LOW) {
            if (isScreenSleeping) {
                wakeUpScreen();
            } else {
                lastActivityMs = now;
                if (bleWorkerBusy) {
                    abortActiveSearch();
                }
                activeMenuIndex = (activeMenuIndex + 1) % MENU_ITEMS_COUNT;
                uiDirty = true;
            }
        }
    }

    // 3. External Grove Button (GPIO 10) -> Execute or Abort immediately
    if (curGrove != lastGroveState && (now - lastGroveDebounce) > DEBOUNCE_DELAY_MS) {
        lastGroveDebounce = now;
        lastGroveState = curGrove;
        if (curGrove == LOW) {
            handleActionButton();
        }
    }

    // 4. External Hat Header Button (GPIO 5) -> Execute or Abort immediately
    if (curHat != lastHatState && (now - lastHatDebounce) > DEBOUNCE_DELAY_MS) {
        lastHatDebounce = now;
        lastHatState = curHat;
        if (curHat == LOW) {
            handleActionButton();
        }
    }
}

void managePowerSaving() {
    if (bleWorkerBusy && !g_abortSearch) {
        lastActivityMs = millis();
        return;
    }

    unsigned long now = millis();
    unsigned long idleTime = now - lastActivityMs;

    // Critical battery protection (<= 8%)
    int bat = M5.Power.getBatteryLevel();
    if (bat >= 0 && bat <= 8) {
        canonRemote.disconnect();
        M5.Display.sleep();
        M5.Display.setBrightness(0);
        M5.Power.powerOff();
    }

    // Stage 2: 5 minutes of inactivity -> Full hardware shutdown
    if (idleTime >= IDLE_OFF_TIMEOUT_MS) {
        canonRemote.disconnect();
        M5.Display.sleep();
        M5.Display.setBrightness(0);
        M5.Power.powerOff();
    }

    // Stage 1: 1 minute of inactivity -> Screen sleep & disconnect idle BLE link
    if (!isScreenSleeping && idleTime >= IDLE_SLEEP_TIMEOUT_MS) {
        isScreenSleeping = true;
        if (canonRemote.isConnected()) {
            canonRemote.disconnect();
            videoRecordingState = false;
            setRemoteStatusSafe("Standby (Tap)", false);
        }
        M5.Display.setBrightness(0);
        M5.Display.sleep();
    }
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    // Disable unused 5V boost output and speaker amp to save battery
    M5.Power.setExtOutput(false);
    M5.Speaker.end();

    // Display & flicker-free SRAM sprite initialization
    M5.Display.setRotation(0);
    M5.Display.setBrightness(180);
    canvas.setPsram(false);
    canvas.createSprite(135, 240);

    Serial.begin(115200);
    Serial.println("StickS3-CanonRemote v1.2.1");

    pinMode(PIN_BTN_FRONT, INPUT_PULLUP);
    pinMode(PIN_BTN_SIDE, INPUT_PULLUP);
    pinMode(PIN_BTN_EXT_GROVE, INPUT_PULLUP);
    pinMode(PIN_BTN_EXT_HAT, INPUT_PULLUP);

    esp_err_t nvsStatus = nvs_flash_init();
    if (nvsStatus == ESP_ERR_NVS_NO_FREE_PAGES || nvsStatus == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    canonRemote.init();
    lastActivityMs = millis();

    if (canonRemote.hasPairedCamera()) {
        activeMenuIndex = 0;
        setRemoteStatusSafe("Standby (Tap)", false);
    } else {
        activeMenuIndex = 3;
        setRemoteStatusSafe("Need Pair", false);
    }
    renderInterfaceNow();

    // Start non-blocking BLE worker task
    xTaskCreatePinnedToCore(bleWorkerTask, "bleWorker", 8192, nullptr, 1, nullptr, 1);
}

void loop() {
    M5.update();

    processButtons();
    managePowerSaving();

    // Periodic link state and battery check (every 500 ms)
    static unsigned long lastMonitorTick = 0;
    if (!isScreenSleeping && !bleWorkerBusy && (millis() - lastMonitorTick >= 500)) {
        lastMonitorTick = millis();
        bool activeConn = canonRemote.isConnected();
        int currentBat = M5.Power.getBatteryLevel();

        if (connectionState && !activeConn) {
            videoRecordingState = false;
            setRemoteStatusSafe("Standby (Tap)", false);
        } else if (currentBat != cachedBatteryLevel) {
            uiDirty = true;
        }
    }

    // Strict 30.00 FPS frame pacing (every 33,333 microseconds)
    static unsigned long lastFrameUs = 0;
    unsigned long nowUs = micros();
    if (!isScreenSleeping && (nowUs - lastFrameUs >= FRAME_INTERVAL_US)) {
        lastFrameUs = nowUs;
        if (uiDirty) {
            renderInterfaceNow();
        }
    }

    // Light CPU yield (longer delay when screen is asleep to save power)
    delay(isScreenSleeping ? 30 : 4);
}