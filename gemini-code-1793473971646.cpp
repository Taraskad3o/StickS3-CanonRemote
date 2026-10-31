/*
 * StickS3-CanonRemote
 * Autonomous Bluetooth Low Energy (BLE) Remote for Canon EOS Cameras
 * Hardware: M5Stack M5StickS3 (ESP32-S3, 135x240 ST7789v2 LCD)
 *
 * Description:
 *   Standalone BLE remote compatible with Canon EOS cameras (BR-E1 protocol).
 *   Includes persistent NimBLE bond storage fix for ESP32 Arduino Core 3.x.
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
#define PIN_BTN_FRONT   11  // Blue main button
#define PIN_BTN_SIDE    12  // Side button
#define PIN_BTN_EXT     1   // Optional external button (Grove G1 or G4/G5)

// ==================== NIMBLE BOND PERSISTENCE ENGINE ====================
// ESP32 Core 3.x NimBLE RAM-only key persistence workaround
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

    void disconnect() {
        if (pClient != nullptr && pClient->isConnected()) {
            pClient->disconnect();
            delay(100);
        }
        pTriggerChar = nullptr;
    }

    bool connect() {
        if (!cameraSaved) return false;
        if (isConnected() && pTriggerChar != nullptr) return true;

        pTriggerChar = nullptr;

        bool ok = pClient->connect(targetAddress, addressType);
        if (!ok) {
            bool found = false;
            BLEScan* pScan = BLEDevice::getScan();
            pScan->setAdvertisedDeviceCallbacks(new CanonAdvCallback(&targetAddress, &addressType, &found));
            pScan->setInterval(1349);
            pScan->setWindow(449);
            pScan->setActiveScan(true);
            pScan->start(2, false);

            if (found) {
                delay(100);
                ok = pClient->connect(targetAddress, addressType);
            }
        }

        if (!ok) return false;

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

        bool found = false;
        BLEScan* pScan = BLEDevice::getScan();
        pScan->setAdvertisedDeviceCallbacks(new CanonAdvCallback(&targetAddress, &addressType, &found));
        pScan->setInterval(1349);
        pScan->setWindow(449);
        pScan->setActiveScan(true);
        pScan->start(timeoutSeconds, false);

        if (!found) return false;

        delay(200);
        if (!pClient->connect(targetAddress, addressType)) {
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

// ==================== APPLICATION LOGIC & UI ====================
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

CanonBLERemote canonRemote("StickS3-Remote");
int activeMenuIndex = 0;
bool connectionState = false;
String currentStatusText = "Init...";
bool videoRecordingState = false;
int cachedBatteryLevel = -1;

bool frontButtonPressed = false;
unsigned long frontPressStartTime = 0;
bool lastFrontReading = HIGH;
unsigned long lastFrontDebounce = 0;

bool lastSideReading = HIGH;

bool extButtonPressed = false;
unsigned long extPressStartTime = 0;
bool extLastReading = HIGH;
unsigned long lastExtDebounce = 0;
const unsigned long DEBOUNCE_DELAY_MS = 50;

void renderInterface() {
    M5.Display.startWrite();
    M5.Display.fillScreen(CLR_BG);

    // Title bar
    M5.Display.setTextColor(CLR_TITLE, CLR_BG);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(6, 6);
    M5.Display.print("Canon BLE");

    // Battery percentage display
    int bat = M5.Power.getBatteryLevel();
    cachedBatteryLevel = bat;
    if (bat >= 0) {
        M5.Display.setTextColor(bat > 20 ? CLR_OK : CLR_ERR, CLR_BG);
        M5.Display.setCursor(96, 6);
        M5.Display.printf("%3d%%", bat);
    }

    // Connection status label
    M5.Display.setTextSize(1);
    if (connectionState) {
        M5.Display.setTextColor(CLR_OK, CLR_BG);
        M5.Display.setCursor(6, 22);
        M5.Display.print("* ");
        M5.Display.print(currentStatusText);
    } else {
        M5.Display.setTextColor(CLR_WARN, CLR_BG);
        M5.Display.setCursor(6, 22);
        M5.Display.print("o ");
        M5.Display.print(currentStatusText);
    }

    M5.Display.drawFastHLine(0, 36, 135, CLR_DIVIDER);

    // Menu list
    for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
        int yPos = 46 + i * 36;

        if (i == activeMenuIndex) {
            M5.Display.fillRoundRect(6, yPos, 123, 28, 5, CLR_SELECT);
            M5.Display.setTextColor(TFT_BLACK, CLR_SELECT);
        } else {
            M5.Display.setTextColor(CLR_TEXT, CLR_BG);
        }

        M5.Display.setTextSize(2);
        M5.Display.setCursor(14, yPos + 6);
        M5.Display.print(menuLabels[i]);

        if (i == 2 && videoRecordingState) {
            M5.Display.fillCircle(116, yPos + 14, 6, CLR_ERR);
        }
    }

    // Navigation hints
    M5.Display.drawFastHLine(0, 198, 135, CLR_DIVIDER);
    M5.Display.setTextColor(CLR_DIVIDER, CLR_BG);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(6, 206);
    M5.Display.print("Blue: Tap=OK Hold=Nxt");
    M5.Display.setCursor(6, 220);
    M5.Display.print("Side: Next Item");
    M5.Display.endWrite();
}

void updateRemoteStatus(String text, bool connected) {
    currentStatusText = text;
    connectionState = connected;
    renderInterface();
}

void executeMenuAction(int index) {
    if (index == 3) {
        updateRemoteStatus("Pairing 15s...", false);
        if (canonRemote.pair(15)) {
            activeMenuIndex = 0;
            updateRemoteStatus("Paired!", true);
        } else {
            updateRemoteStatus("Pair Timeout", false);
        }
        return;
    }

    if (!canonRemote.hasPairedCamera()) {
        activeMenuIndex = 3;
        updateRemoteStatus("Pair first!", false);
        return;
    }

    if (!canonRemote.isConnected()) {
        updateRemoteStatus("Connecting...", false);
    } else {
        updateRemoteStatus(String(menuLabels[index]) + "...", true);
    }

    unsigned long startTime = millis();
    bool success = false;

    switch (index) {
        case 0:
            success = canonRemote.trigger();
            break;
        case 1:
            success = canonRemote.focus();
            break;
        case 2:
            success = canonRemote.trigger();
            if (success) videoRecordingState = !videoRecordingState;
            break;
    }

    unsigned long elapsed = millis() - startTime;
    if (success) {
        if (elapsed > 2000) {
            updateRemoteStatus("OK (" + String(elapsed / 1000) + "s)", true);
        } else {
            updateRemoteStatus("OK (" + String(elapsed) + "ms)", true);
        }
    } else {
        videoRecordingState = false;
        updateRemoteStatus("No Camera", false);
    }
}

void processBuiltinButtons() {
    unsigned long currentMs = millis();

    // Front blue button (GPIO 11): tap to execute, hold to cycle
    bool frontReading = digitalRead(PIN_BTN_FRONT);
    if (frontReading != lastFrontReading) {
        lastFrontDebounce = currentMs;
    }
    if ((currentMs - lastFrontDebounce) > DEBOUNCE_DELAY_MS) {
        if (frontReading == LOW && !frontButtonPressed) {
            frontButtonPressed = true;
            frontPressStartTime = currentMs;
        }
        if (frontReading == HIGH && frontButtonPressed) {
            frontButtonPressed = false;
            unsigned long duration = currentMs - frontPressStartTime;
            if (duration >= 500) {
                activeMenuIndex = (activeMenuIndex + 1) % MENU_ITEMS_COUNT;
                renderInterface();
            } else {
                executeMenuAction(activeMenuIndex);
            }
        }
    }
    lastFrontReading = frontReading;

    // Side button (GPIO 12): immediate navigation step
    bool sideReading = digitalRead(PIN_BTN_SIDE);
    if (sideReading == LOW && lastSideReading == HIGH) {
        delay(20);
        if (digitalRead(PIN_BTN_SIDE) == LOW) {
            activeMenuIndex = (activeMenuIndex + 1) % MENU_ITEMS_COUNT;
            renderInterface();
        }
    }
    lastSideReading = sideReading;
}

void processExternalButton() {
    int extReading = digitalRead(PIN_BTN_EXT);
    if (extReading != extLastReading) {
        lastExtDebounce = millis();
    }

    if ((millis() - lastExtDebounce) > DEBOUNCE_DELAY_MS) {
        if (extReading == LOW && !extButtonPressed) {
            extButtonPressed = true;
            extPressStartTime = millis();
        }
        if (extReading == HIGH && extButtonPressed) {
            extButtonPressed = false;
            unsigned long duration = millis() - extPressStartTime;
            if (duration >= 500) {
                activeMenuIndex = (activeMenuIndex + 1) % MENU_ITEMS_COUNT;
                renderInterface();
            } else {
                executeMenuAction(activeMenuIndex);
            }
        }
    }
    extLastReading = extReading;
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    // Disable audio amplifier to eliminate power drain and speaker pop
    M5.Speaker.end();

    // Display configuration for 135x240 LCD
    M5.Display.setRotation(0);
    M5.Display.setBrightness(180);

    Serial.begin(115200);
    Serial.println("StickS3-CanonRemote v1.0.0");

    pinMode(PIN_BTN_FRONT, INPUT_PULLUP);
    pinMode(PIN_BTN_SIDE, INPUT_PULLUP);
    pinMode(PIN_BTN_EXT, INPUT_PULLUP);

    esp_err_t nvsStatus = nvs_flash_init();
    if (nvsStatus == ESP_ERR_NVS_NO_FREE_PAGES || nvsStatus == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    canonRemote.init();

    if (canonRemote.hasPairedCamera()) {
        activeMenuIndex = 0;
        updateRemoteStatus("Standby (Tap)", false);
    } else {
        activeMenuIndex = 3;
        updateRemoteStatus("Need Pair", false);
    }
}

void loop() {
    M5.update();

    processBuiltinButtons();
    processExternalButton();

    // Check link state and battery
    static unsigned long lastMonitorTick = 0;
    if (millis() - lastMonitorTick > 500) {
        lastMonitorTick = millis();
        bool activeConn = canonRemote.isConnected();
        int currentBat = M5.Power.getBatteryLevel();

        if (connectionState && !activeConn) {
            videoRecordingState = false;
            updateRemoteStatus("Standby (Tap)", false);
        } else if (currentBat != cachedBatteryLevel) {
            renderInterface();
        }
    }

    delay(10);
}