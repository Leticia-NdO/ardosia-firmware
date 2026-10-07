#include "InputManager.h"
#include <driver/adc.h>

// Recorded ADC values from real devices
// BACK CONF LEFT RGHT   UP DOWN
// 3597 2760 1530    6 2300    6
// 3470 2666 1480    6 2222    5
// 3470 2655 1470    3 2205    3

// Averages
// BACK CONF LEFT RGHT   UP DOWN
// 3512 2694 1493    5 2242    5

// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is being pressed
// These ranges are based on real world values above, and are much more tolerant of different
// devices than a fixed threshold check
// These values are calculated by taking the midpoint of the pairs of averaged values above
const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0) {}

void InputManager::begin() {
  // Configure ADC using ESP-IDF API directly to avoid GPIO reconfiguration
  // that Arduino's analogRead() triggers on every call in the dual framework
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_12);  // GPIO 0 (battery)
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_12);  // GPIO 1
  adc1_config_channel_atten(ADC1_CHANNEL_2, ADC_ATTEN_DB_12);  // GPIO 2

  // Power button is digital, keep using Arduino API
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

// ARDOSIA FORK CHANGE — median of three, instead of a single conversion.
//
// A single bad conversion becomes a BUTTON PRESS here, and on this ladder it
// becomes a direction specifically: both channels decode their bottom band
// open-ended toward zero (anything <= 750 on channel 1 is Right, <= 1120 on
// channel 2 is Down), and neither has a "reading is implausible" verdict. So a
// read that merely comes back low is never rejected — it is decoded.
//
// Measured on an X4 over five rounds of instrumentation: phantom Right presses
// fire with nothing touching the device, and a second conversion of the SAME
// pin taken microseconds later in the same loop iteration reads 4095 — full
// rest. So the voltage is fine and the conversion is not. The leading
// explanation is that the bad read is the first conversion after the CPU wakes
// from light sleep: this call site is the first ADC use after the delay() that
// ends loop(), and adc1_get_raw() takes no PM lock (only ADC2 has one) while
// powering the SAR up per call.
//
// Three conversions and the median kills it whichever way that lands, because
// only the FIRST of the three is a post-wake read — and it kills it even when
// two consecutive polls are both bad, which is what the measured 2-sample
// phantom duration requires. Same remedy, same ADC unit, and the same stated
// reason as BatteryMonitor::readMillivolts(), which already takes a median of
// seven here "to reject outliers from SPI/BLE/charging noise".
//
// Cost is two extra conversions per channel per poll, about 60us, against a
// 10ms poll. Upstream candidate: this is a crosspoint/FreeInk bug, not ours.
static uint32_t s_filteredSamples = 0;

static int medianOfThree(const adc1_channel_t channel) {
  const int a = adc1_get_raw(channel);
  const int b = adc1_get_raw(channel);
  const int c = adc1_get_raw(channel);

  // Branch-free median of three.
  const int lo = a < b ? a : b;
  const int hi = a < b ? b : a;
  int med = c < lo ? lo : (c > hi ? hi : c);

  // Count the polls where the three did not agree, so that "the phantom
  // stopped" can be told apart from "the filter is doing work". Without this,
  // a fix and a coincidence look identical. Diagnostic only — see BUTTON_DIAG
  // in src/main.cpp; it goes when that goes.
  if (a != b || b != c) s_filteredSamples++;

  return med;
}

uint32_t InputManager::getFilteredSampleCount() { return s_filteredSamples; }

uint8_t InputManager::getState() {
  uint8_t state = 0;

  // Read GPIO1 buttons using ESP-IDF API (no GPIO reconfiguration overhead)
  const int adcValue1 = medianOfThree(ADC1_CHANNEL_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  // Read GPIO2 buttons using ESP-IDF API
  const int adcValue2 = medianOfThree(ADC1_CHANNEL_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  // Read power button (digital, active LOW)
  if (digitalRead(POWER_BUTTON_PIN) == LOW) {
    state |= (1 << BTN_POWER);
  }

  return state;
}

void InputManager::update() {
  const unsigned long currentTime = millis();
  const uint8_t state = getState();

  // Always clear events first
  pressedEvents = 0;
  releasedEvents = 0;

  // Debounce
  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      // Calculate pressed and released events
      pressedEvents = state & ~currentState;
      releasedEvents = currentState & ~state;

      // If pressing buttons and wasn't before, start recording time
      if (pressedEvents > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      // If releasing a button and no other buttons being pressed, record finish time
      if (releasedEvents > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      currentState = state;
    }
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const {
  return currentState & (1 << buttonIndex);
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const {
  return pressedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyPressed() const {
  return pressedEvents > 0;
}

bool InputManager::wasReleased(const uint8_t buttonIndex) const {
  return releasedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyReleased() const {
  return releasedEvents > 0;
}

unsigned long InputManager::getHeldTime() const {
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const {
  return isPressed(BTN_POWER);
}
