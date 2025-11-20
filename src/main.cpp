#include <Arduino.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <GxEPD2_BW.h>
#include <SPI.h>
#include "image.h"

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8  // SPI Clock
#define EPD_MOSI 10 // SPI MOSI (Master Out Slave In)
#define EPD_CS 21   // Chip Select
#define EPD_DC 4    // Data/Command
#define EPD_RST 5   // Reset
#define EPD_BUSY 6  // Busy

// Button pins (ADC resistor ladders)
#define BTN_GPIO1 1 // 4 buttons: Back, Confirm, Left, Right
#define BTN_GPIO2 2 // 2 buttons: Volume Up, Volume Down

// Button enum
enum Button
{
  NONE = 0,
  RIGHT,
  LEFT,
  CONFIRM,
  BACK,
  VOLUME_UP,
  VOLUME_DOWN
};

// Display command enum
enum DisplayCommand
{
  DISPLAY_NONE = 0,
  DISPLAY_HEADER,
  DISPLAY_TEXT,
  TOGGLE_IMAGE
};

// Button ADC thresholds
const int BTN_THRESHOLD = 100; // Threshold tolerance
const int BTN_RIGHT_VAL = 3;
const int BTN_LEFT_VAL = 1470;
const int BTN_CONFIRM_VAL = 2655;
const int BTN_BACK_VAL = 3470;
const int BTN_VOLUME_DOWN_VAL = 3;
const int BTN_VOLUME_UP_VAL = 2205;

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
  default:
    return "";
  }
}

// Get currently pressed button by reading ADC values
Button GetPressedButton()
{
  int btn1 = analogRead(BTN_GPIO1);
  int btn2 = analogRead(BTN_GPIO2);

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

// GxEPD2 display - Using GxEPD2_426_GDEQ0426T82
// Note: XteinkX4 has 4.26" 800x480 display
GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT> display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// FreeRTOS task for non-blocking display updates
TaskHandle_t displayTaskHandle = NULL;
volatile DisplayCommand displayCommand = DISPLAY_NONE;
volatile Button currentPressedButton = NONE;
volatile bool imageVisible = false;

// Display update task running on separate core
void displayUpdateTask(void *parameter)
{
  while (1)
  {
    if (displayCommand != DISPLAY_NONE)
    {
      DisplayCommand cmd = displayCommand;
      displayCommand = DISPLAY_NONE;

      if (cmd == DISPLAY_HEADER)
      {
        // Use full window for initial welcome screen with header
        display.setFullWindow();
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);

          // Header font
          display.setFont(&FreeMonoBold18pt7b);
          display.setCursor(20, 50);
          display.print("XteinkX4 Sample");

          // Button text with smaller font
          display.setFont(&FreeMonoBold12pt7b);
          display.setCursor(20, 100);
          display.print(getButtonName(currentPressedButton));
        } while (display.nextPage());
      }
      else if (cmd == DISPLAY_TEXT)
      {
        // Use partial refresh for text updates
        display.setPartialWindow(0, 75, display.width(), 150);
        display.firstPage();
        do
        {
          display.fillScreen(GxEPD_WHITE);
          display.setFont(&FreeMonoBold12pt7b);
          display.setCursor(20, 100);
          display.print(getButtonName(currentPressedButton));
        } while (display.nextPage());
      }
      else if (cmd == TOGGLE_IMAGE)
      {
        // Toggle image visibility and use partial refresh
        imageVisible = !imageVisible;
        display.setPartialWindow(20, 150, 263, 280);
        display.firstPage();
        do
        {
          if (imageVisible)
          {
            display.drawBitmap(20, 150, dr_mario, 263, 280, GxEPD_BLACK);
          }
          else
          {
            display.fillRect(20, 150, 263, 280, GxEPD_WHITE);
          }
        } while (display.nextPage());
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup()
{
  Serial.begin(115200);

  // Wait for serial monitor
  unsigned long start = millis();
  while (!Serial && (millis() - start) < 3000)
  {
    delay(10);
  }

  Serial.println("\n=================================");
  Serial.println("  xteink x4 sample");
  Serial.println("=================================");
  Serial.println();

  // Initialize button pins
  pinMode(BTN_GPIO1, INPUT);
  pinMode(BTN_GPIO2, INPUT);

  // Initialize SPI with custom pins
  SPI.begin(EPD_SCLK, -1, EPD_MOSI, EPD_CS);

  // Initialize display
  display.init(115200);

  // Setup display properties
  display.setRotation(3); // 270 degrees
  display.setTextColor(GxEPD_BLACK);

  Serial.println("Display initialized");

  // Draw initial welcome screen
  currentPressedButton = NONE;
  displayCommand = DISPLAY_HEADER;

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

void loop()
{
  static Button lastButton = NONE;
  Button currentButton = GetPressedButton();

  // Detect button press (transition from NONE to a button)
  if (currentButton != NONE && lastButton == NONE)
  {
    Serial.print("Button: ");
    Serial.println(getButtonName(currentButton));
    currentPressedButton = currentButton;

    if (currentButton == CONFIRM)
    {
      displayCommand = TOGGLE_IMAGE;
    }
    else
    {
      displayCommand = DISPLAY_TEXT;
    }
  }

  lastButton = currentButton;
  delay(50);
}
