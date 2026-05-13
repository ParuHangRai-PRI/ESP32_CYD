#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>
#include "PRI_logo.h"
#include "PTSerif28.h"
#include "PTSerif14.h"

TFT_eSPI tft = TFT_eSPI();

/** UART receive buffer
 * Stores incoming characters from STM32 */
char uartRxBuffer[1024];
uint16_t uartRxIndex = 0;

/** Sensor values from STM32 */
String batteryVal = "";
String tlfVal = "";
String turbidityVal = "";
String hlfVal = "";
String dateTimeVal = "";
bool sensorValChanged = false;

/** Control signals from STM32 */
bool ctlUp = false;
bool ctlDown = false;
bool ctlSelect = false;
bool ctlBack = false;
bool ctlPowerOff = false;
bool ctlTestAll = false;
bool ctlErase = false;
bool ctlTestTlf = false;
bool ctlTestHlf = false;
bool ctlTestTurb = false;
bool ctlReceived = false;

BLECharacteristic *pCharacteristic = nullptr;

/** Screen state enumeration
 * MENU: Main menu screen
 * DETAIL: Item detail screen */
enum ScreenState
{
    SCREEN_MENU,
    SCREEN_DETAIL,
    SCREEN_QUICKTEST,
    SCREEN_RESULTS,
    SCREEN_TEST_PARAM_MENU,
    SCREEN_TESTING,
    SCREEN_TEST_RESULTS
};

/** ============================================================================
 * NAVIGATION STATE MACHINE
 * ============================================================================
 * The navigation is controlled by three flags in NavigationState struct:
 *   - home:  0 = at home screen, non-zero = in a sub-screen
 *   - option1: 1-4 represents which top-level menu was selected:
 *              1 = QuickTest  (immediate test, no sub-options)
 *              2 = Test Parameter  (shows 4 test type options)
 *              3 = Device Manual  (reserved for future, not implemented)
 *              4 = Settings  (shows Device Manual & Parameters Info)
 *   - option2: 1-n for sub-options within selected option1
 *              For option1=2: 1=Test All, 2=Test TLF, 3=Test HLF, 4=Test Turbidity
 *              For option1=4: 1=Device Manual, 2=Parameters Info
 *
 * Navigation Logic:
 *   - If home == 0: display Home menu with 4 options
 *   - If home != 0:
 *       - If option1 == 1: start QuickTest immediately
 *       - If option1 == 2: display Test Parameter menu with 4 animated options
 *       - If option1 == 3: (reserved for future use)
 *       - If option1 == 4: display Settings menu with 2 options
 * ============================================================================ */

struct NavigationState
{
    int home;    /**< 0 = at home, non-zero = in sub-screen */
    int option1; /**< 1-4: which top menu was selected */
    int option2; /**< 1-n: sub-option within option1 */
};

NavigationState navState = {0, 0, 0};

enum TestType
{
    TEST_ALL,
    TEST_TLF,
    TEST_HLF,
    TEST_TURB
};

TestType currentTest;
int testParamIndex = 0;
int selectedTestParamItem = 0;
bool testParamMenuAnimating = false;
int testParamAnimationProgress = 0;
int prevTestParamItem = 0;
uint32_t lastTestParamAnimationFrameTime = 0;

ScreenState currentScreen = SCREEN_MENU;
int detailItemIndex = 0;

uint32_t quickTestStartTime = 0;
bool quickTestActive = false;
int menuAutoCycleCount = 0;

TFT_eSprite quickTestBarSprite(&tft);
bool quickTestBarSpriteInitialized = false;

uint32_t testStartTime = 0;
bool testActive = false;
TFT_eSprite testBarSprite(&tft);
bool testBarSpriteInitialized = false;

bool autoNavigate = false;
uint32_t autoNavigateStartTime = 0;
int autoNavigateStep = 0;

#define MENU_ITEMS 4

/** ============================================================================
 * UI COMPONENTS
 * ============================================================================ */

/** Menu item rectangle bounds for hit detection
 * Stores x, y, width, height for button presses */
struct MenuItemRect
{
    int x, y, w, h;
};
MenuItemRect menuRects[MENU_ITEMS];
MenuItemRect testParamRects[4];

/** PWM Backlight Control
 * Look-up table for logarithmic dimming curve
 * arr = 65535 * brightness^3 / 100^3 */
const uint16_t brightness_lut[15] = {0, 5, 43, 147, 349, 683, 1180, 1874, 2798, 3984, 5466, 7275, 9445, 12009, 14999};

const int BACKLIGHT_PIN = 21;
const int PWM_CHANNEL = 0;
const int PWM_RESOLUTION = 16;
const int PWM_FREQ = 200;

/** Menu Labels
 * option1=1: QuickTest - immediate test */
const char *menuLabels[MENU_ITEMS] = {"QuickTest", "Test Parameter", "Settings", "Info"};

/** Test Parameter Labels (option1=2)
 * 4 test types available */
const char *testParamLabels[4] = {"Test All", "Test TLF", "Test HLF", "Test Turbidity"};

/** Settings Labels (option1=4)
 * Device info options */
const char *settingsLabels[2] = {"Device Manual", "Parameters Info"};

/** ============================================================================
 * DEVICE STATE
 * ============================================================================ */
bool deviceConnected = false;
bool oldDeviceConnected = false;
int batteryPercent = 85;

/** ============================================================================
 * FUNCTION PROTOTYPES
 * ============================================================================ */
void drawMenu();                                                               /**< Draws home menu with 4 options */
void drawTestParamMenu();                                                      /**< Draws test parameter menu with 4 options */
void drawTestingScreen();                                                      /**< Draws testing progress screen */
void drawTestResultsScreen();                                                  /**< Draws test results screen */
void drawOptionBoxes(int optionCount, const char **labels, int selectedIndex); /**< Unified menu drawer */
void animateOptionSelection(int optionCount, const char **labels, int &selectedIndex, int &prevIndex, bool &animating, int &animationProgress, uint32_t &lastFrameTime);
void startMenuAnimation(int newSelected);          /**< Start home menu animation */
void animateMenuSelection();                       /**< Animate home menu selection */
void startTestParamMenuAnimation(int newSelected); /**< Start test param menu animation */
void animateTestParamMenuSelection();              /**< Animate test param menu selection */
void drawSettingsMenu();                           /**< Draws settings menu with 2 options */
void startTest();                                  /**< Start test sequence */
void updateTest();                                 /**< Update test progress bar */
int getTestDuration(TestType t);                   /**< Get test duration in ms */

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define NOTIF_BAR_H 30
#define NAV_PANEL_H 20

/** BLE server connection callbacks
 * Tracks device connection/disconnection */
class MyServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        deviceConnected = true;
        Serial.println("BT connected");
        Serial2.println("CTL:t1");
    }
    void onDisconnect(BLEServer *pServer)
    {
        deviceConnected = false;
        Serial.println("ECHO:BT disconnected");
        Serial2.println("ECHO:BT disconnected");
    }
};

/** BLE characteristic write callbacks
 * Forwards BLE commands to STM32 via UART */
class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0)
        {
            Serial.println(value.c_str());
        }
    }
};

/** Controls the LCD backlight brightness
 * @param brightness_level: Index into brightness_lut (0-14) */
void LCD_Backlight_CTL(uint8_t brightness_level)
{
    if (brightness_level > 14)
        brightness_level = 14;
    uint16_t duty = brightness_lut[brightness_level];
    ledcWrite(PWM_CHANNEL, duty);
}

void setBacklightBrightness(uint8_t level)
{
    LCD_Backlight_CTL(level);
}

/** Draws the battery icon in status bar
 * @param bg_color: The background color of notification bar */
void drawBatteryIcon()
{
    int batW = 30;
    int batH = 16;
    int x = 8;
    int y = (NOTIF_BAR_H - batH) / 2;

    // Battery outline
    tft.drawRoundRect(x, y, batW, batH, 2, TFT_WHITE);
    // Battery tip
    tft.fillRect(x + batW, y + 5, 3, batH - 10, TFT_WHITE);

    // Battery fill level
    int fillW = map(batteryPercent, 0, 100, 0, batW - 4);
    tft.fillRoundRect(x + 2, y + 2, fillW, batH - 4, 1, TFT_GREEN);

    // Battery percentage text with PT Serif 14
    tft.loadFont(PTSerif14);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(x + batW + 6, y + 2);
    tft.print(batteryPercent);
    tft.print("%");
    tft.unloadFont();
}

/** Draws the notification bar at top of screen
 * Contains battery icon */
void drawStatusBar()
{
    tft.fillRect(0, 0, 320, NOTIF_BAR_H, TFT_BLACK);
    drawBatteryIcon();
}

/** Draws a chevron separator in navigation panel
 * @param x: Starting X position */
void drawChevron(int x)
{
    int navY = NOTIF_BAR_H;
    tft.drawLine(x, navY, x + 14, navY + NAV_PANEL_H / 2, TFT_BLACK);
    tft.drawLine(x, navY + NAV_PANEL_H, x + 14, navY + NAV_PANEL_H / 2, TFT_BLACK);
}

/** Draws the navigation bar below status bar
 * Unified breadcrumb display based on current screen */
void drawNavigationPanel()
{
    int navY = NOTIF_BAR_H;
    tft.fillRect(0, navY, 320, NAV_PANEL_H, TFT_WHITE);
    tft.drawLine(0, navY + NAV_PANEL_H, 320, navY + NAV_PANEL_H, TFT_BLACK);

    tft.loadFont(PTSerif14);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);

    const char *pathLabels[4];
    int pathCount = 1;
    pathLabels[0] = "Home";

    if (navState.home != 0)
    {
        if (navState.option1 >= 1 && navState.option1 <= MENU_ITEMS)
        {
            pathLabels[pathCount++] = menuLabels[navState.option1 - 1];
        }
        else if (navState.option1 == 4)
        {
            pathLabels[pathCount++] = "Settings";
        }

        if (navState.option1 == 2 && navState.option2 >= 1 && navState.option2 <= 4)
        {
            pathLabels[pathCount++] = testParamLabels[navState.option2 - 1];
        }
        else if (navState.option1 == 4 && navState.option2 >= 1 && navState.option2 <= 2)
        {
            pathLabels[pathCount++] = settingsLabels[navState.option2 - 1];
        }
    }

    int cursorX = 10;
    for (int i = 0; i < pathCount; i++)
    {
        tft.setCursor(cursorX, navY + 3);
        tft.print(pathLabels[i]);
        cursorX += tft.textWidth(pathLabels[i]) + 6;

        if (i < pathCount - 1)
        {
            drawChevron(cursorX);
            cursorX += 20;
        }
    }
    tft.unloadFont();
}

/** Draws the menu item with icon
 * @param index: Menu item index (0-3)
 * @param x: X position of the menu item
 * @param btnW: Width of the menu item (270 or 300) */
void drawMenuItem(int index, int x, int btnW)
{
    int startY = NOTIF_BAR_H + NAV_PANEL_H + 8;
    int btnH = 40;
    int gap = 6;
    int y = startY + index * (btnH + gap);
    int radius = 6;

    // Fill the rectangle with white background
    tft.fillRoundRect(x, y, btnW, btnH, radius, TFT_WHITE);
    // Outer border
    tft.drawRoundRect(x, y, btnW, btnH, radius, TFT_BLACK);
    // Inner border for thicker appearance
    tft.drawRoundRect(x + 1, y + 1, btnW - 2, btnH - 2, radius - 1, TFT_BLACK);

    // Menu label text
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextFont(2);
    tft.setCursor(60, y + 12);
    tft.print(menuLabels[index]);

    // Draw icon for expanded items (300px width)
    if (btnW > 280)
    {
        tft.setTextColor(TFT_BLACK, TFT_WHITE);
        tft.setTextFont(4);
        int iconY = y + 10;
        int iconX = 20;

        if (index == 0)
        {
            tft.setCursor(iconX, iconY);
            tft.print(">>");
        }
        else if (index == 1)
        {
            tft.setCursor(iconX, iconY);
            tft.print(">");
        }
        else if (index == 2)
        {
            int gx = iconX + 8;
            int gy = iconY + 10;
            tft.fillCircle(gx, gy, 6, TFT_BLACK);
            tft.fillCircle(gx, gy, 3, TFT_WHITE);
            for (int a = 0; a < 360; a += 45)
            {
                float rad = a * PI / 180;
                float offsetX = -sin(rad);
                float offsetY = cos(rad);
                tft.drawLine(gx + cos(rad) * 6, gy + sin(rad) * 6,
                             gx + cos(rad) * 8, gy + sin(rad) * 8, TFT_BLACK);
                tft.drawLine(gx + cos(rad) * 6 + offsetX, gy + sin(rad) * 6 + offsetY,
                             gx + cos(rad) * 8 + offsetX, gy + sin(rad) * 8 + offsetY, TFT_BLACK);
            }
        }
        else if (index == 3)
        {
            tft.setTextFont(2);
            tft.fillCircle(iconX + 8, iconY + 10, 10, TFT_BLACK);
            tft.setCursor(iconX + 8, iconY + 2);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.print("i");
        }
    }

    // Store rectangle for touch detection
    menuRects[index].x = x;
    menuRects[index].y = y;
    menuRects[index].w = btnW;
    menuRects[index].h = btnH;
}

/** Animation state variables
 * Current and previous selected item for transition */
int selectedMenuItem = 0;
bool menuAnimating = false;
int animationProgress = 0;
int prevSelectedItem = 0;
uint32_t lastAnimationFrameTime = 0;

/** Animation sprites for smooth rendering */
TFT_eSprite spriteRow1(&tft);
TFT_eSprite spriteRow2(&tft);
bool spritesInitialized = false;

void startQuickTest();

/** Ease-in-out quadratic function
 * @param t: Current time (0-1024)
 * @return: Eased value (0-1024) */
int easeInOutQuad(int t)
{
    return t < 512 ? (t * t) / 1024 : 1023 - ((1023 - t) * (1023 - t)) / 1024;
}

/** Animates menu selection transition
 * Animates the expanding/contracting of menu items
 * Only animates the two items changing */
void animateMenuSelection()
{
    if (!menuAnimating)
        return;

    animateOptionSelection(MENU_ITEMS, menuLabels, selectedMenuItem, prevSelectedItem, menuAnimating, animationProgress, lastAnimationFrameTime);

    if (!menuAnimating)
    {
        if (currentScreen == SCREEN_MENU && menuAutoCycleCount >= MENU_ITEMS && selectedMenuItem == 0 && prevSelectedItem == MENU_ITEMS - 1)
        {
            menuAutoCycleCount = 0;
            startQuickTest();
        }
        else if (autoNavigateStep == 3)
        {
            drawTestParamMenu();
            autoNavigateStep = 4;
        }
    }
}

/** ============================================================================
 * UNIFIED MENU DRAWING FUNCTIONS
 * ============================================================================ */

/** Initiates menu animation when selection changes
 * @param newSelected: New menu item index to select */
void startMenuAnimation(int newSelected)
{
    if (newSelected != selectedMenuItem && !menuAnimating)
    {
        prevSelectedItem = selectedMenuItem;
        selectedMenuItem = newSelected;
        animationProgress = 0;
        menuAnimating = true;
    }
}

/** Draws unified option boxes for both Home menu and Test Parameter menu
 * - First option is expanded (300px), others are normal (270px)
 * - Shows ">>" icon for expanded item
 * - Stores button rectangles for hit detection
 * @param optionCount: Number of options (2-4)
 * @param labels: Array of label strings
 * @param selectedIndex: Currently selected option index */
void animateOptionSelection(int optionCount, const char **labels, int &selectedIndex, int &prevIndex, bool &animating, int &animationProgress, uint32_t &lastFrameTime)
{
    if (!animating)
        return;

    int startY = NOTIF_BAR_H + NAV_PANEL_H + 8;
    int btnH = 40;
    int gap = 6;
    int radius = 10;

    uint32_t now = millis();
    if (now - lastFrameTime < 40)
        return;
    lastFrameTime = now;

    if (!spritesInitialized)
    {
        spriteRow1.createSprite(320, btnH);
        spriteRow2.createSprite(320, btnH);
        spritesInitialized = true;
    }

    animationProgress += 64;
    if (animationProgress >= 1024)
    {
        animationProgress = 1024;
        animating = false;
    }

    int easeVal = easeInOutQuad(animationProgress);
    int rows[2] = {prevIndex, selectedIndex};
    TFT_eSprite *sprites[2] = {&spriteRow1, &spriteRow2};

    for (int idx = 0; idx < 2; idx++)
    {
        int i = rows[idx];
        int y = startY + i * (btnH + gap);
        TFT_eSprite *sprite = sprites[idx];

        sprite->fillSprite(TFT_WHITE);

        int x, w;
        if (i == prevIndex)
        {
            x = 10 + 30 * easeVal / 1024;
            w = 300 - 30 * easeVal / 1024;
        }
        else
        {
            x = 40 - 30 * easeVal / 1024;
            w = 270 + 30 * easeVal / 1024;
        }

        sprite->fillRoundRect(x, 0, w, btnH, radius, TFT_WHITE);
        sprite->drawRoundRect(x, 0, w, btnH, radius, TFT_BLACK);
        sprite->drawRoundRect(x + 1, 1, w - 2, btnH - 2, radius - 1, TFT_BLACK);

        sprite->setTextColor(TFT_BLACK, TFT_WHITE);
        sprite->setTextFont(2);
        sprite->setCursor(50, 12);
        sprite->print(labels[i]);

        if (w > 280)
        {
            sprite->setTextColor(TFT_BLACK, TFT_WHITE);
            sprite->setTextFont(4);
            sprite->setCursor(20, 10);
            sprite->print(">>");
        }

        menuRects[i].x = x;
        menuRects[i].y = y;
        menuRects[i].w = w;
        menuRects[i].h = btnH;

        sprite->pushSprite(0, y);
    }
}

void drawOptionBoxes(int optionCount, const char **labels, int selectedIndex)
{
    int startY = NOTIF_BAR_H + NAV_PANEL_H + 8;
    int btnH = 40;
    int gap = 6;
    int radius = 6;

    for (int i = 0; i < optionCount; i++)
    {
        int w = (i == selectedIndex) ? 300 : 270;
        int x = (i == selectedIndex) ? 10 : 40;
        int y = startY + i * (btnH + gap);

        tft.fillRoundRect(x, y, w, btnH, radius, TFT_WHITE);
        tft.drawRoundRect(x, y, w, btnH, radius, TFT_BLACK);
        tft.drawRoundRect(x + 1, y + 1, w - 2, btnH - 2, radius - 1, TFT_BLACK);

        tft.setTextColor(TFT_BLACK, TFT_WHITE);
        tft.setTextFont(2);
        tft.setCursor(60, y + 12);
        tft.print(labels[i]);

        if (w > 280)
        {
            tft.setTextColor(TFT_BLACK, TFT_WHITE);
            tft.setTextFont(4);
            tft.setCursor(20, y + 10);
            tft.print(">>");
        }

        menuRects[i].x = x;
        menuRects[i].y = y;
        menuRects[i].w = w;
        menuRects[i].h = btnH;
    }
}

/** ============================================================================
 * SCREEN DRAWING FUNCTIONS
 * ============================================================================ */

/** Draws the Home menu screen
 * Uses drawOptionBoxes() to display 4 menu items
 * First item expanded (300px), others normal (270px)
 * Resets animation state */
void drawMenu()
{
    navState.home = 0;
    navState.option1 = 0;
    navState.option2 = 0;
    currentScreen = SCREEN_MENU;
    quickTestActive = false;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();
    drawOptionBoxes(MENU_ITEMS, menuLabels, selectedMenuItem);
    selectedMenuItem = 0;
    prevSelectedItem = 0;
    animationProgress = 1024;
    menuAnimating = false;
}

void drawQuickTestScreen()
{
    navState.home = 1;
    navState.option1 = 1;
    navState.option2 = 0;
    currentScreen = SCREEN_QUICKTEST;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();

    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextFont(4);
    tft.setCursor(40, 60);
    tft.print("Testing");

    // Progress bar frame
    int barX = 20;
    int barY = 120;
    int barW = 280;
    int barH = 20;
    tft.drawRect(barX, barY, barW, barH, TFT_BLACK);

    int innerW = barW - 2;
    int innerH = barH - 2;
    if (!quickTestBarSpriteInitialized)
    {
        quickTestBarSprite.createSprite(innerW, innerH);
        quickTestBarSpriteInitialized = true;
    }

    uint32_t elapsed = millis() - quickTestStartTime;
    if (elapsed > 13000)
        elapsed = 13000;
    int fillW = (int)((uint32_t)innerW * elapsed / 13000);
    if (fillW > innerW)
        fillW = innerW;

    quickTestBarSprite.fillSprite(TFT_WHITE);
    if (fillW > 0)
        quickTestBarSprite.fillRect(0, 0, fillW, innerH, TFT_GREEN);
    quickTestBarSprite.pushSprite(barX + 1, barY + 1);
}

void drawResultsScreen()
{
    navState.home = 1;
    navState.option1 = 1;
    navState.option2 = 0;
    currentScreen = SCREEN_RESULTS;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();

    tft.loadFont(PTSerif28);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setCursor(20, 60);
    tft.print("Results:");

    tft.setCursor(20, 120);
    tft.print("Parameter = 123.999 adc");
    tft.unloadFont();
}

void drawTestParamMenu()
{
    navState.home = 1;
    navState.option1 = 2;
    navState.option2 = 0;
    currentScreen = SCREEN_TEST_PARAM_MENU;
    testParamMenuAnimating = false;
    selectedTestParamItem = 0;
    prevTestParamItem = 0;
    testParamAnimationProgress = 1024;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();
    drawOptionBoxes(4, testParamLabels, selectedTestParamItem);
    delay(100);
}

void drawSettingsMenu()
{
    navState.home = 1;
    navState.option1 = 4;
    navState.option2 = 0;
    currentScreen = SCREEN_DETAIL;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();
    drawOptionBoxes(2, settingsLabels, 0);
}

void startTestParamMenuAnimation(int newSelected)
{
    if (newSelected != selectedTestParamItem && !testParamMenuAnimating)
    {
        prevTestParamItem = selectedTestParamItem;
        selectedTestParamItem = newSelected;
        testParamAnimationProgress = 0;
        testParamMenuAnimating = true;
    }
}

void animateTestParamMenuSelection()
{
    animateOptionSelection(4, testParamLabels, selectedTestParamItem, prevTestParamItem, testParamMenuAnimating, testParamAnimationProgress, lastTestParamAnimationFrameTime);
}

int getTestDuration(TestType t)
{
    if (t == TEST_ALL)
        return 39000;
    return 13000;
}

void drawTestingScreen()
{
    navState.home = 1;
    navState.option1 = 2;
    navState.option2 = testParamIndex + 1;
    currentScreen = SCREEN_TESTING;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();

    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextFont(4);
    tft.setCursor(40, 60);
    tft.print("Testing");

    if (currentTest == TEST_ALL)
    {
        tft.print(" ABC");
    }
    else
    {
        tft.print(" ");
        tft.print(testParamLabels[testParamIndex]);
    }

    int barX = 20;
    int barY = 120;
    int barW = 280;
    int barH = 20;
    tft.drawRect(barX, barY, barW, barH, TFT_BLACK);

    int innerW = barW - 2;
    int innerH = barH - 2;
    if (!testBarSpriteInitialized)
    {
        testBarSprite.createSprite(innerW, innerH);
        testBarSpriteInitialized = true;
    }

    uint32_t elapsed = millis() - testStartTime;
    int duration = getTestDuration(currentTest);
    if (elapsed > duration)
        elapsed = duration;
    int fillW = (int)((uint64_t)innerW * elapsed / duration);
    if (fillW > innerW)
        fillW = innerW;

    testBarSprite.fillSprite(TFT_WHITE);
    if (fillW > 0)
        testBarSprite.fillRect(0, 0, fillW, innerH, TFT_GREEN);
    testBarSprite.pushSprite(barX + 1, barY + 1);
}

void drawTestResultsScreen()
{
    navState.home = 1;
    navState.option1 = 2;
    navState.option2 = testParamIndex + 1;
    currentScreen = SCREEN_TEST_RESULTS;
    tft.fillScreen(TFT_WHITE);
    drawStatusBar();
    drawNavigationPanel();

    tft.loadFont(PTSerif28);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setCursor(20, 60);
    tft.print("Results:");

    if (currentTest == TEST_ALL)
    {
        tft.setCursor(20, 100);
        tft.print("Parameter A = 123.999 adc");
        tft.setCursor(20, 140);
        tft.print("Parameter B = 456.789 adc");
        tft.setCursor(20, 180);
        tft.print("Parameter C = 789.123 adc");
    }
    else
    {
        tft.setCursor(20, 120);
        tft.print("Parameter ");
        tft.print((char)('A' + testParamIndex));
        tft.print(" = 123.999 adc");
    }
    tft.unloadFont();
}

void startTest()
{
    testActive = true;
    testStartTime = millis();
    currentScreen = SCREEN_TESTING;
    drawTestingScreen();
}

void updateTest()
{
    if (!testActive)
        return;

    uint32_t elapsed = millis() - testStartTime;
    int duration = getTestDuration(currentTest);
    if (elapsed >= duration)
    {
        testActive = false;
        drawTestResultsScreen();
        return;
    }

    int barX = 20;
    int barY = 120;
    int barW = 280;
    int barH = 20;
    int innerW = barW - 2;
    int innerH = barH - 2;
    int fillW = (int)((uint64_t)innerW * elapsed / duration);
    if (fillW > innerW)
        fillW = innerW;

    if (!testBarSpriteInitialized)
    {
        testBarSprite.createSprite(innerW, innerH);
        testBarSpriteInitialized = true;
    }

    testBarSprite.fillSprite(TFT_WHITE);
    if (fillW > 0)
        testBarSprite.fillRect(0, 0, fillW, innerH, TFT_GREEN);
    testBarSprite.pushSprite(barX + 1, barY + 1);
}

void startQuickTest()
{
    detailItemIndex = 0;
    quickTestActive = true;
    quickTestStartTime = millis();
    currentScreen = SCREEN_QUICKTEST;
    drawQuickTestScreen();
}

void updateQuickTest()
{
    if (!quickTestActive)
        return;

    uint32_t elapsed = millis() - quickTestStartTime;
    if (elapsed >= 13000)
    {
        quickTestActive = false;
        drawResultsScreen();
        autoNavigate = true;
        autoNavigateStartTime = millis();
        autoNavigateStep = 1;
        return;
    }

    int barX = 20;
    int barY = 120;
    int barW = 280;
    int barH = 20;
    int innerW = barW - 2;
    int innerH = barH - 2;
    int fillW = (int)((uint32_t)innerW * elapsed / 13000);
    if (fillW > innerW)
        fillW = innerW;

    if (!quickTestBarSpriteInitialized)
    {
        quickTestBarSprite.createSprite(innerW, innerH);
        quickTestBarSpriteInitialized = true;
    }

    quickTestBarSprite.fillSprite(TFT_WHITE);
    if (fillW > 0)
        quickTestBarSprite.fillRect(0, 0, fillW, innerH, TFT_GREEN);
    quickTestBarSprite.pushSprite(barX + 1, barY + 1);
}

/** Handles UART communication with STM32
 * Receives VAL: and CTL: signals, forwards BLE commands to STM32 */
void handleUart()
{
    while (Serial.available())
    {
        char c = Serial.read();
        Serial.write(c);
        if (c == '\n' || uartRxIndex >= sizeof(uartRxBuffer) - 1)
        {
            uartRxBuffer[uartRxIndex] = '\0';
            String rxData = String(uartRxBuffer);

            if (rxData.startsWith("VAL:"))
            {
                String val = rxData.substring(4);
                if (val.startsWith("b"))
                {
                    batteryVal = val.substring(1);
                    sensorValChanged = true;
                }
                else if (val.startsWith("t"))
                {
                    tlfVal = val.substring(1);
                    sensorValChanged = true;
                }
                else if (val.startsWith("tu"))
                {
                    turbidityVal = val.substring(2);
                    sensorValChanged = true;
                }
                else if (val.startsWith("h"))
                {
                    hlfVal = val.substring(1);
                    sensorValChanged = true;
                }
                else if (val.startsWith("dt"))
                {
                    dateTimeVal = val.substring(2);
                    sensorValChanged = true;
                }
            }
            else if (rxData.startsWith("CTL:"))
            {
                String ctl = rxData.substring(4);
                if (ctl == "up")
                    ctlUp = true;
                else if (ctl == "dn")
                    ctlDown = true;
                else if (ctl == "sel")
                    ctlSelect = true;
                else if (ctl == "bk")
                    ctlBack = true;
                else if (ctl == "pwr")
                    ctlPowerOff = true;
                else if (ctl == "tst")
                    ctlTestAll = true;
                else if (ctl == "er")
                    ctlErase = true;
                else if (ctl == "t1")
                    ctlTestTlf = true;
                else if (ctl == "t2")
                    ctlTestHlf = true;
                else if (ctl == "t3")
                    ctlTestTurb = true;
                ctlReceived = true;
            }
            else if (pCharacteristic != nullptr && deviceConnected)
            {
                pCharacteristic->setValue(rxData.c_str());
                pCharacteristic->notify();
            }
            uartRxIndex = 0;
        }
        else
        {
            uartRxBuffer[uartRxIndex++] = c;
        }
    }
}

void setup()
{
    // Initialize PWM for backlight control
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(BACKLIGHT_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, 0);
    delay(100);
    ledcWrite(PWM_CHANNEL, brightness_lut[8]);
    delay(100);

    // Initialize TFT display
    tft.init();
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(4);

    // Show PRI logo boot screen
    tft.pushImage(0, 0, 320, 240, (const uint16_t *)pri_logo);
    delay(1500);

    // Initialize BLE server
    BLEDevice::init("CYD_BLE_NODE");
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
    pCharacteristic->setValue("CYD Data");
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    // Initialize UART communication with STM32
    Serial2.setRxBufferSize(1024);
    Serial2.begin(115200, SERIAL_8N1, 27, 22);
    Serial2.println("ESP32 UART Ready (STM32)");

    delay(500);
    LCD_Backlight_CTL(12);

    // Draw main menu
    drawMenu();
}

void loop()
{
    // Update FPS counter
    // updateFpsCounter();

    // Run menu animation if active
    if (menuAnimating)
    {
        animateMenuSelection();
        // updateFpsDisplay();
    }

    // Run test param menu animation if active
    if (testParamMenuAnimating)
    {
        animateTestParamMenuSelection();
    }

    // Handle BLE connection state changes
    if (deviceConnected != oldDeviceConnected)
    {
        if (!deviceConnected)
        {
            delay(1500);
            BLEDevice::getAdvertising()->start();
        }
        drawStatusBar();
        oldDeviceConnected = deviceConnected;
    }

    if (currentScreen == SCREEN_QUICKTEST)
    {
        updateQuickTest();
    }

    if (currentScreen == SCREEN_TESTING)
    {
        updateTest();
    }

    // Auto navigate after QuickTest
    if (autoNavigate)
    {
        if (autoNavigateStep == 1 && currentScreen == SCREEN_RESULTS && millis() - autoNavigateStartTime > 3000)
        {
            drawMenu();
            autoNavigateStep = 2;
        }
        else if (autoNavigateStep == 2 && currentScreen == SCREEN_MENU && !menuAnimating)
        {
            startMenuAnimation(1);
            autoNavigateStep = 3;
        }
        // Step 3 handled in animateMenuSelection
        else if (autoNavigateStep == 4 && currentScreen == SCREEN_TEST_PARAM_MENU)
        {
            static uint32_t waitStart = 0;
            if (waitStart == 0)
                waitStart = millis();
            if (millis() - waitStart > 1000)
            {
                selectedTestParamItem = 3;
                testParamIndex = 3;
                currentTest = TEST_ALL;
                startTest();
                autoNavigate = false;
                autoNavigateStep = 0;
                waitStart = 0;
            }
        }
    }

    // Handle UART communication with STM32
    handleUart();

    // Process control signals from STM32 (input signals)
    if (ctlReceived)
    {
        if (ctlUp)
        {
            ctlUp = false;
            if (currentScreen == SCREEN_MENU)
            {
                int prev = (selectedMenuItem - 1 + MENU_ITEMS) % MENU_ITEMS;
                startMenuAnimation(prev);
            }
            else if (currentScreen == SCREEN_TEST_PARAM_MENU)
            {
                int prev = (selectedTestParamItem - 1 + 4) % 4;
                startTestParamMenuAnimation(prev);
            }
        }
        if (ctlDown)
        {
            ctlDown = false;
            if (currentScreen == SCREEN_MENU)
            {
                int next = (selectedMenuItem + 1) % MENU_ITEMS;
                startMenuAnimation(next);
            }
            else if (currentScreen == SCREEN_TEST_PARAM_MENU)
            {
                int next = (selectedTestParamItem + 1) % 4;
                startTestParamMenuAnimation(next);
            }
        }
        if (ctlSelect)
        {
            ctlSelect = false;
            if (currentScreen == SCREEN_MENU && selectedMenuItem == 0)
            {
                startQuickTest();
            }
            else if (currentScreen == SCREEN_MENU && selectedMenuItem == 1)
            {
                drawTestParamMenu();
            }
            else if (currentScreen == SCREEN_MENU && selectedMenuItem == 2)
            {
                drawSettingsMenu();
            }
            else if (currentScreen == SCREEN_TEST_PARAM_MENU)
            {
                testParamIndex = selectedTestParamItem;
                currentTest = (TestType)selectedTestParamItem;
                startTest();
            }
        }
        if (ctlBack)
        {
            ctlBack = false;
            if (currentScreen != SCREEN_MENU)
            {
                drawMenu();
            }
        }
        if (ctlPowerOff)
        {
            ctlPowerOff = false;
            tft.fillScreen(TFT_BLACK);
            LCD_Backlight_CTL(0);
            esp_deep_sleep_start();
        }
        if (ctlTestAll)
        {
            ctlTestAll = false;
            Serial.println("CTL:tst");
        }
        if (ctlErase)
        {
            ctlErase = false;
            Serial.println("CTL:er");
        }
        if (ctlTestTlf)
        {
            ctlTestTlf = false;
            Serial.println("CTL:t1");
        }
        if (ctlTestHlf)
        {
            ctlTestHlf = false;
            Serial.println("CTL:t2");
        }
        if (ctlTestTurb)
        {
            ctlTestTurb = false;
            Serial.println("CTL:t3");
        }
        ctlReceived = false;
    }

    // Update battery from sensor value
    if (sensorValChanged && batteryVal.length() > 0)
    {
        batteryPercent = batteryVal.toInt();
        drawStatusBar();
        sensorValChanged = false;
    }
}
