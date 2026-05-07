#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "PRI_logo.h"

TFT_eSPI tft = TFT_eSPI();

// UART2 receive buffer and state (Serial2 is already defined in ESP32 framework)
char uartRxBuffer[1024];
uint16_t uartRxIndex = 0;
String currentVal = "";
bool valChanged = false;

// BLE characteristic pointer for notifications
BLECharacteristic *pCharacteristic = nullptr;

// Screen state machine
enum ScreenState { SCREEN_MENU, SCREEN_DETAIL };
ScreenState currentScreen = SCREEN_MENU;
int detailItemIndex = 0;

#define MENU_ITEMS 4

// Menu item bounding boxes for touch detection
struct MenuItemRect {
    int x, y, w, h;
};
MenuItemRect menuRects[MENU_ITEMS];

// Back button bounding box
struct ButtonRect {
    int x, y, w, h;
};
ButtonRect backBtn = {60, 140, 200, 60};

// Function prototypes
void drawBatteryIcon();
void drawBtLines(int x, int y, uint16_t color);
void drawBtIcon(int x, int y);
void drawStatusBar();
void drawMenuItem(int index, bool selected);
void drawMenu();
void drawVal();
void drawDetailScreen();
void handleUart();

const char* menuLabels[MENU_ITEMS] = {"Test", "Settings", "Help", "Info"};
const char* menuIcons[MENU_ITEMS] = {">", "o", "?", "i"};
uint16_t menuColors[MENU_ITEMS] = {TFT_GREEN, TFT_GREEN, TFT_GREEN, TFT_GREEN};

#define BAR_COLOR 0x4A2A
#define BAR_DARKER 0x2A1A
#define BAR_LINE 0x6A4A
#define BT_CONNECTED_COLOR TFT_BLUE
#define BT_DISCONNECTED_COLOR 0x2A3A

int selectedIndex = 0;
unsigned long lastRotateTime = 0;
const unsigned long rotateInterval = 2000;

bool deviceConnected = false;
bool oldDeviceConnected = false;
int batteryPercent = 85;

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
    }
};

class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            Serial2.println(value.c_str());
        }
    }
};

void drawBatteryIcon() {
    int x = 10;
    int y = 12;
    int w = 24;
    int h = 14;
    int fillW = map(batteryPercent, 0, 100, 0, w - 4);

    tft.drawRoundRect(x, y, w, h, 2, TFT_WHITE);
    tft.fillRect(x + w, y + 4, 3, h - 8, TFT_WHITE);
    tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, 1, batteryPercent > 20 ? TFT_GREEN : TFT_RED);

    tft.setTextColor(TFT_WHITE, BAR_COLOR);
    tft.setTextFont(2);
    tft.setCursor(x + w + 8, y + 1);
    tft.print(batteryPercent);
    tft.print("%");
}

void drawBtLines(int x, int y, uint16_t color) {
    int X_SIZE = 12;
    int LINE_THICKNESS_X = 2;
    int LINE_THICKNESS_VERTICAL = 2;

    int midX = x + X_SIZE / 2;
    int midY = y + X_SIZE / 2;

    for (int i = 0; i < LINE_THICKNESS_X; ++i) {
        tft.drawLine(x + i, y, x + X_SIZE + i, y + X_SIZE, color);
    }

    for (int i = 0; i < LINE_THICKNESS_X; ++i) {
        tft.drawLine(x + X_SIZE + i, y, x + i, y + X_SIZE, color);
    }

    int vLineLengthSegment = (int)(0.9f * X_SIZE);
    int vLineY1 = midY - vLineLengthSegment;
    int vLineY2 = midY + vLineLengthSegment;

    for (int i = 0; i < LINE_THICKNESS_VERTICAL; ++i) {
        tft.drawLine(midX + i, vLineY1, midX + i, vLineY2, color);
    }

    for (int i = 0; i < LINE_THICKNESS_X; ++i) {
        tft.drawLine(x + X_SIZE + i, y, midX + i, vLineY1, color);
    }

    for (int i = 0; i < LINE_THICKNESS_X; ++i) {
        tft.drawLine(x + X_SIZE + i, y + X_SIZE, midX + i, vLineY2, color);
    }
}

void drawBtIcon(int x, int y) {
    uint16_t color = deviceConnected ? TFT_BLUE : 0x39C7;
    if (deviceConnected) {
        drawBtLines(x + 1, y + 1, 0x001A);
    }
    drawBtLines(x, y, color);
}

void drawStatusBar() {
    tft.fillRect(0, 0, 320, 40, BAR_COLOR);
    tft.drawLine(0, 40, 320, 40, BAR_LINE);
    drawBatteryIcon();
    drawBtIcon(290, 10);
}

void drawMenuItem(int index, bool selected) {
    int y = 50 + (index * 48);
    uint16_t bgColor = selected ? 0x5A3A : 0x2A1A;
    uint16_t borderColor = selected ? menuColors[index] : 0x4A3A;
    uint16_t textColor = selected ? menuColors[index] : TFT_LIGHTGREY;

    tft.fillRoundRect(20, y, 280, 42, 6, bgColor);
    tft.drawRoundRect(20, y, 280, 42, 6, borderColor);

    tft.setTextColor(textColor, bgColor);
    tft.setTextSize(1);
    tft.setCursor(40, y + 11);
    tft.print(menuIcons[index]);
    tft.setCursor(60, y + 11);
    tft.print(menuLabels[index]);

    if (selected) {
        tft.setTextColor(menuColors[index], bgColor);
        tft.drawString(">>", 260, y + 11);
    }

    menuRects[index].x = 20;
    menuRects[index].y = y;
    menuRects[index].w = 280;
    menuRects[index].h = 42;
}

void drawMenu() {
    tft.fillScreen(0x1A0A);
    drawStatusBar();
    for (int i = 0; i < MENU_ITEMS; i++) {
        drawMenuItem(i, i == selectedIndex);
    }
}

#define VAL_X 20
#define VAL_Y 220
#define VAL_W 280
#define VAL_H 20
#define VAL_BG 0x1A0A

void drawVal() {
    tft.fillRect(VAL_X, VAL_Y, VAL_W, VAL_H, VAL_BG);
    tft.setTextColor(TFT_GREEN, VAL_BG);
    tft.setTextFont(2);
    tft.setCursor(VAL_X + 5, VAL_Y + 2);
    tft.print("Sensor: ");
    tft.print(currentVal);
}

void drawDetailScreen() {
    tft.fillScreen(0x1A0A);
    drawStatusBar();

    tft.setTextColor(TFT_GREEN, 0x1A0A);
    tft.setTextFont(4);
    tft.setCursor(40, 60);
    tft.print("Detail: ");
    tft.print(menuLabels[detailItemIndex]);

    tft.setTextFont(2);
    tft.setCursor(40, 100);
    tft.print("This is the ");
    tft.print(menuLabels[detailItemIndex]);
    tft.print(" screen");

    tft.fillRoundRect(backBtn.x, backBtn.y, backBtn.w, backBtn.h, 10, TFT_RED);
    tft.drawRoundRect(backBtn.x, backBtn.y, backBtn.w, backBtn.h, 10, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextFont(4);
    tft.setCursor(backBtn.x + 60, backBtn.y + 18);
    tft.print("BACK");
}

void handleUart() {
    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n' || uartRxIndex >= sizeof(uartRxBuffer) - 1) {
            uartRxBuffer[uartRxIndex] = '\0';
            String rxData = String(uartRxBuffer);
            if (rxData.startsWith("VAL:")) {
                currentVal = rxData.substring(4);
                valChanged = true;
            } else if (pCharacteristic != nullptr && deviceConnected) {
                pCharacteristic->setValue(rxData.c_str());
                pCharacteristic->notify();
            }
            uartRxIndex = 0;
        } else {
            uartRxBuffer[uartRxIndex++] = c;
        }
    }
}

void setup() {
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);
    tft.init();
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(4);

    tft.pushImage(0, 0, 320, 240, (const uint16_t*)pri_logo);
    delay(1500);

    BLEDevice::init("CYD_BLE_NODE");
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
                         CHARACTERISTIC_UUID,
                         BLECharacteristic::PROPERTY_READ |
                         BLECharacteristic::PROPERTY_WRITE |
                         BLECharacteristic::PROPERTY_NOTIFY
                       );
    pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
    pCharacteristic->setValue("CYD Data");
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    Serial2.begin(115200, SERIAL_8N1, 16, 17);
    Serial2.setRxBufferSize(1024);
    Serial2.println("ESP32 UART Ready");

    drawMenu();
    drawVal();
    lastRotateTime = millis();
}

void loop() {
    unsigned long now = millis();
    if (currentScreen == SCREEN_MENU) {
        if (now - lastRotateTime >= rotateInterval) {
            int oldIndex = selectedIndex;
            selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
            drawMenuItem(oldIndex, false);
            drawMenuItem(selectedIndex, true);
            lastRotateTime = now;
        }
    }

    if (deviceConnected != oldDeviceConnected) {
        if (!deviceConnected) {
            delay(1500);
            BLEDevice::getAdvertising()->start();
        }
        drawStatusBar();
        oldDeviceConnected = deviceConnected;
    }

    handleUart();

    if (valChanged) {
        drawVal();
        valChanged = false;
    }
}
