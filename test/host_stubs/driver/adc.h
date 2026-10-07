#pragma once

// Host-test stand-in for the ESP-IDF ADC driver, so that
// lib/InputManager/src/InputManager.cpp — the REAL file, not a copy — can be
// compiled and exercised on the host. The button ladder's whole failure mode is
// "one bad conversion becomes a direction press", and that is decidable without
// a device as long as the test gets to choose what each conversion returns.
//
// adcFeed() queues the values adc1_get_raw() will hand out, in order, per
// channel. Running past the end of a queue repeats its last value, which is
// what a pin sitting still does.

#include <cstdint>
#include <vector>

typedef enum { ADC_WIDTH_BIT_12 = 3 } adc_bits_width_t;
typedef enum { ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC1_CHANNEL_0 = 0, ADC1_CHANNEL_1 = 1, ADC1_CHANNEL_2 = 2 } adc1_channel_t;

inline std::vector<int>& adcQueue(const adc1_channel_t ch) {
  static std::vector<int> q[3];
  return q[static_cast<int>(ch)];
}
inline std::size_t& adcCursor(const adc1_channel_t ch) {
  static std::size_t c[3] = {0, 0, 0};
  return c[static_cast<int>(ch)];
}

inline void adcFeed(const adc1_channel_t ch, const std::vector<int>& values) {
  adcQueue(ch) = values;
  adcCursor(ch) = 0;
}

inline int adc1_get_raw(const adc1_channel_t ch) {
  std::vector<int>& q = adcQueue(ch);
  if (q.empty()) return 4095;                   // nothing fed: pin at rest
  std::size_t& i = adcCursor(ch);
  const int v = q[i];
  if (i + 1 < q.size()) i++;                    // last value repeats
  return v;
}

inline void adc1_config_width(adc_bits_width_t) {}
inline void adc1_config_channel_atten(adc1_channel_t, adc_atten_t) {}
