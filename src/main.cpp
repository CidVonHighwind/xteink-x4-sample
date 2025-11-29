#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <string.h>

#include "image.h"
#include "BatteryMonitor.h"
#include "builtinFonts/bookerly.h"
#include "builtinFonts/bookerly_large_bold.h"
#include "EpdFontRenderer.h"


#define SPI_FQ 40000000
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8  // SPI Clock
#define EPD_MOSI 10 // SPI MOSI (Master Out Slave In)
#define EPD_CS 21   // Chip Select
#define EPD_DC 4    // Data/Command
#define EPD_RST 5   // Reset
#define EPD_BUSY 6  // Busy

// Button pins
#define BTN_GPIO1 1 // 4 buttons on ADC resistor ladder: Back, Confirm, Left, Right
#define BTN_GPIO2 2 // 2 buttons on ADC resistor ladder: Volume Up, Volume Down
#define BTN_GPIO3 3 // Power button (digital)

#define UART0_RXD 20 // Used for USB connection detection
#define BAT_GPIO0 0 // Battery voltage

#define SD_SPI_CS   12
#define SD_SPI_MISO 7

static bool g_sdReady = false;

static BatteryMonitor g_battery(BAT_GPIO0);

static int rawBat = 0;

// Display command enum
enum DisplayCommand
{
  DISPLAY_NONE = 0,
  DISPLAY_INITIAL,
  DISPLAY_TEXT,
  DISPLAY_BATTERY,
  DISPLAY_SLEEP
};

volatile DisplayCommand displayCommand = DISPLAY_NONE;

// GxEPD2 display - Using GxEPD2_426_GDEQ0426T82
// Note: XteinkX4 has 4.26" 800x480 display
GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT> display(
  GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

auto smallFontRenderer = new EpdFontRenderer<GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT>>(new EpdFont(&bookerly), &display);
auto largeFontRenderer = new EpdFontRenderer<GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT>>(new EpdFont(&bookerly_large_bold), &display);

// FreeRTOS task for non-blocking display updates
TaskHandle_t displayTaskHandle = NULL;

// Button ADC thresholds
const int BTN_THRESHOLD = 100; // Threshold tolerance
const int BTN_RIGHT_VAL = 3;
const int BTN_LEFT_VAL = 1470;
const int BTN_CONFIRM_VAL = 2655;
const int BTN_BACK_VAL = 3470;
const int BTN_VOLUME_DOWN_VAL = 3;
const int BTN_VOLUME_UP_VAL = 2205;

// Button enum
enum Button
{
  NONE = 0,
  RIGHT,
  LEFT,
  CONFIRM,
  BACK,
  VOLUME_UP,
  VOLUME_DOWN,
  POWER
};

static Button lastButton = NONE;
volatile Button currentPressedButton = NONE;

// Power button timing
const unsigned long POWER_BUTTON_WAKEUP_MS = 1000; // Time required to confirm boot from sleep
const unsigned long POWER_BUTTON_SLEEP_MS = 1000;  // Time required to enter sleep mode


// Get button name as string
const char *getButtonName(Button btn)
{
  switch (btn)
  {
  case NONE:
    return "Press any button";
  case RIGHT:
    return "RIGHT pressed!";
  case LEFT:
    return "LEFT pressed!";
  case CONFIRM:
    return "CONFIRM pressed!";
  case BACK:
    return "BACK pressed!";
  case VOLUME_UP:
    return "VOLUME UP pressed!";
  case VOLUME_DOWN:
    return "VOLUME DOWN pressed!";
  case POWER:
    return "POWER pressed!";
  default:
    return "";
  }
}

// Get currently pressed button by reading ADC values (and digital for power button)
Button GetPressedButton()
{
  int btn1 = analogRead(BTN_GPIO1);
  int btn2 = analogRead(BTN_GPIO2);

  // Check BTN_GPIO3 (Power button) - digital read
  if (digitalRead(BTN_GPIO3) == LOW)
  {
    return POWER;
  }
  // Check BTN_GPIO1 (4 buttons on resistor ladder)
  if (btn1 < BTN_RIGHT_VAL + BTN_THRESHOLD)
  {
    return RIGHT;
  }
  else if (btn1 < BTN_LEFT_VAL + BTN_THRESHOLD)
  {
    return LEFT;
  }
  else if (btn1 < BTN_CONFIRM_VAL + BTN_THRESHOLD)
  {
    return CONFIRM;
  }
  else if (btn1 < BTN_BACK_VAL + BTN_THRESHOLD)
  {
    return BACK;
  }

  // Check BTN_GPIO2 (2 buttons on resistor ladder)
  if (btn2 < BTN_VOLUME_DOWN_VAL + BTN_THRESHOLD)
  {
    return VOLUME_DOWN;
  }
  else if (btn2 < BTN_VOLUME_UP_VAL + BTN_THRESHOLD)
  {
    return VOLUME_UP;
  }

  return NONE;
}

// Check if charging
bool isCharging()
{
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

// Draw battery information on display
void drawBatteryInfo()
{
  int cursorX = 20;
  int cursorY = 160;

  char tempString[100] = {};
  sprintf(tempString, "Power: %s", isCharging() ? "Charging" : "Battery");
  smallFontRenderer->renderString(tempString, &cursorX, &cursorY, GxEPD_BLACK);

  cursorX = 40;
  sprintf(tempString, "Raw: %i", g_battery.readRawMillivolts());
  smallFontRenderer->renderString(tempString, &cursorX, &cursorY, GxEPD_BLACK);

  cursorX = 40;
  sprintf(tempString, "Volts: %.2f V", g_battery.readVolts());
  smallFontRenderer->renderString(tempString, &cursorX, &cursorY, GxEPD_BLACK);

  cursorX = 40;
  sprintf(tempString, "Charge: %i%%", g_battery.readPercentage());
  smallFontRenderer->renderString(tempString, &cursorX, &cursorY, GxEPD_BLACK);
}

// Draw up to top file names from SD on the display, below battery info
static void drawSdTopFiles()
{
  // Layout constants aligned with drawBatteryInfo() block
  const int startX = 40;
  const int maxLines = 5;
  const int maxChars = 30;

  int cursorX = 20;
  int cursorY = 320;
  smallFontRenderer->renderString("Top 5 files on SD:", &cursorX, &cursorY, GxEPD_BLACK);

  auto drawTruncated = [&](int lineIdx, const char *text)
  {
    // Render a single line, truncating with ellipsis if needed
    String s(text ? text : "");
    if ((int) s.length() > maxChars)
    {
      s.remove(maxChars - 1);
      s += "…";
    }
    cursorX = startX;
    smallFontRenderer->renderString(s.c_str(), &cursorX, &cursorY, GxEPD_BLACK);
  };

  // Ensure SD is initialized using global flag; try to init if needed
  if (!g_sdReady)
  {
    if (SD.begin(SD_SPI_CS, SPI, SPI_FQ))
    {
      g_sdReady = true;
    }
  }

  if (!g_sdReady)
  {
    drawTruncated(0, "No card");
    return;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory())
  {
    drawTruncated(0, "No card");
    if (root) root.close();
    return;
  }

  int count = 0;
  for (File f = root.openNextFile(); f && count < maxLines; f = root.openNextFile())
  {
    if (!f.isDirectory())
    {
      const char *name = f.name();
      // Ensure only name + extension, no leading path
      const char *basename = name;
      if (basename)
      {
        const char *slash = strrchr(basename, '/');
        if (slash && *(slash + 1))
          basename = slash + 1;
      }
      drawTruncated(count, basename ? basename : "");
      count++;
    }
    f.close();
  }

  if (count == 0)
  {
    drawTruncated(0, "Empty");
  }

  root.close();
}

// Display update task running on separate core
void displayUpdateTask(void *parameter)
{
  while (1)
  {
    if (displayCommand != DISPLAY_NONE)
    {
      DisplayCommand cmd = displayCommand;
      displayCommand = DISPLAY_NONE;

      if (cmd == DISPLAY_INITIAL)
      {
        // Use full window for initial welcome screen
        display.setFullWindow();
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);

          // Header font
          int cursorX = 20;
          int cursorY = 50;
          largeFontRenderer->renderString("Xteink X4 Sample", &cursorX, &cursorY, GxEPD_BLACK);

          // Button text with smaller font
          cursorX = 20;
          cursorY = 100;
          smallFontRenderer->renderString(getButtonName(currentPressedButton), &cursorX, &cursorY, GxEPD_BLACK);

          // Draw battery information
          drawBatteryInfo();
          // Draw top 3 SD files below the battery block
          drawSdTopFiles();

          // Draw image at bottom right
          int16_t imgWidth = 263;
          int16_t imgHeight = 280;
          int16_t imgMargin = 20;
          int16_t imgX = 480 - imgMargin - imgWidth;
          int16_t imgY = 800 - imgMargin - imgHeight;
          display.drawBitmap(imgX, imgY, dr_mario, imgWidth, imgHeight, GxEPD_BLACK);
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_TEXT)
      {
        // Use partial refresh for text updates
        display.setPartialWindow(0, 75, display.width(), 200);
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          int cursorX = 20;
          int cursorY = 100;
          smallFontRenderer->renderString(getButtonName(currentPressedButton), &cursorX, &cursorY, GxEPD_BLACK);
          drawBatteryInfo();
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_BATTERY)
      {
        // Use partial refresh for battery updates
        display.setPartialWindow(0, 135, display.width(), 150);
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          drawBatteryInfo();
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_SLEEP)
      {
        // Use full window for sleep screen
        display.setFullWindow();
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          // Header font

          int width, height;
          largeFontRenderer->font->getTextDimensions("Sleeping...", &width, &height);
          int cursorX = (display.width() - width) / 2;
          int cursorY = 380;
          largeFontRenderer->renderString("Sleeping...", &cursorX, &cursorY, GxEPD_BLACK);
        } while (display.nextPage());
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// Verify long press on wake-up from deep sleep
void verifyWakeupLongPress()
{
  // Temporarily configure pin as digital input to check state
  pinMode(BTN_GPIO3, INPUT_PULLUP);

  unsigned long pressStart = millis();
  bool abortBoot = false;

  // Monitor button state for the duration
  while (millis() - pressStart < POWER_BUTTON_WAKEUP_MS)
  {
    // If button reads HIGH (released) before time is up
    if (digitalRead(BTN_GPIO3) == HIGH)
    {
      abortBoot = true;
      break;
    }
    delay(10);
  }

  if (abortBoot)
  {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_GPIO3, ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
  }
  else
  {
    lastButton = POWER;
  }
}

// Enter deep sleep mode
void enterDeepSleep()
{
  displayCommand = DISPLAY_SLEEP;
  Serial.println("Power button released after a long press. Entering deep sleep.");
  delay(2000); // Allow Serial buffer to empty and display to update

  // Enable Wakeup on LOW (button press)
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_GPIO3, ESP_GPIO_WAKEUP_GPIO_LOW);

  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void setup()
{
  // Check if boot was triggered by the Power Button (Deep Sleep Wakeup)
  // If triggered by RST pin or Battery insertion, this will be false, allowing normal boot.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO)
  {
    verifyWakeupLongPress();
  }

  Serial.begin(115200);

  // Wait for serial monitor
  unsigned long start = millis();
  while (!Serial && (millis() - start) < 3000)
  {
    delay(10);
  }

  if (Serial)
  {
    // delay for monitor to start reading
    delay(1000);
  }


  Serial.println("\n=================================");
  Serial.println("  xteink x4 sample");
  Serial.println("=================================");
  Serial.println();

  // Initialize button pins
  pinMode(BAT_GPIO0, INPUT);
  pinMode(BTN_GPIO1, INPUT);
  pinMode(BTN_GPIO2, INPUT);
  pinMode(BTN_GPIO3, INPUT_PULLUP); // Power button

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK,SD_SPI_MISO, EPD_MOSI, EPD_CS);
  // Initialize display
  SPISettings spi_settings(SPI_FQ, MSBFIRST, SPI_MODE0);
  display.init(115200, true, 2, false, SPI, spi_settings);

  // SD Card Initialization
  if (!SD.begin(SD_SPI_CS, SPI, SPI_FQ))
  {
    Serial.print("\n SD card not detected\n");
  }
  else
  {
    Serial.print("\n SD card detected\n");
    g_sdReady = true;
  }

  // Setup display properties
  display.setRotation(3); // 270 degrees
  display.setTextColor(GxEPD_BLACK);

  Serial.println("Display initialized");


  // Draw initial welcome screen
  currentPressedButton = NONE;
  displayCommand = DISPLAY_INITIAL;

  // Create display update task on core 0 (main loop runs on core 1)
  xTaskCreatePinnedToCore(displayUpdateTask,  // Task function
                          "DisplayUpdate",    // Task name
                          4096,               // Stack size
                          NULL,               // Parameters
                          1,                  // Priority
                          &displayTaskHandle, // Task handle
                          0                   // Core 0
  );

  Serial.println("Display task created");
  Serial.println("Setup complete!\n");
}

#ifdef DEBUG_IO
void debugIO()
{
  // Log raw analog levels of BTN1 and BTN2 not more often than once per second
  rawBat = analogRead(BAT_GPIO0);
  int rawBtn1 = analogRead(BTN_GPIO1);
  int rawBtn2 = analogRead(BTN_GPIO2);
  int rawBtn3 = digitalRead(BTN_GPIO3);
  Serial.print("ADC BTN1=");
  Serial.print(rawBtn1);
  Serial.print("    BTN2=");
  Serial.print(rawBtn2);
  Serial.print("    BTN3=");
  Serial.print(rawBtn3);
  Serial.println("");

  // log battery info
  Serial.printf("== Battery (charging: %s) ==\n", isCharging() ? "yes" : "no");
  Serial.print("Value from pin (raw/calibrated): ");
  Serial.print(rawBat);
  Serial.print(" / ");
  Serial.println(BatteryMonitor::millivoltsFromRawAdc(rawBat));
  Serial.print("Volts: ");
  Serial.println(g_battery.readVolts());
  Serial.print("Charge level: ");
  Serial.println(g_battery.readPercentage());
  Serial.println("");

  // SD card
}
#endif


void loop()
{
  Button currentButton = GetPressedButton();

  // Detect button press (transition from NONE to a button)
  if (currentButton != NONE && lastButton == NONE)
  {
    Serial.print("Button: ");
    Serial.println(getButtonName(currentButton));

    currentPressedButton = currentButton;
    displayCommand = DISPLAY_TEXT;

#ifdef DEBUG_IO
    debugIO();
#endif

    if (currentButton == POWER)
    {
      unsigned long startTime = millis();
      // Wait for button release
      while (digitalRead(BTN_GPIO3) == LOW)
        delay(50);

      unsigned long currentTime = millis();
      // Power button long pressed => go to sleep
      if (currentTime - startTime > POWER_BUTTON_SLEEP_MS)
        enterDeepSleep();
    }
  }

  lastButton = currentButton;

  delay(50);
}
