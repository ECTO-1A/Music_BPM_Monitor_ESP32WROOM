#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =================== Display Setup ===================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1          // Reset pin (or -1 if sharing Arduino reset)
#define OLED_ADDR     0x3C        // Most common I2C address for 0.96" OLED

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =================== Audio / BPM Setup ===================

// Use an ADC1 pin (recommended): GPIO34
const int AUDIO_PIN = 34;

// Sampling & timing
const int SAMPLE_INTERVAL_MS          = 2;    // ~500 Hz sampling
const int DISPLAY_UPDATE_INTERVAL_MS  = 300;  // Update OLED every 300 ms

// Beat interval constraints
// These limit what we consider a "real" beat interval.
const int MIN_BEAT_INTERVAL_MS  = 300;   // ~200 BPM max (60000 / 300)
const int MAX_BEAT_INTERVAL_MS  = 1500;  // ~40 BPM min

// Envelope + dynamic threshold
const float DC_ALPHA        = 0.001f;   // DC offset tracking (slow)
const float ENV_ALPHA       = 0.15f;    // envelope smoothing (faster)
const float MEAN_ALPHA      = 0.001f;   // long-term average of envelope
const float THRESH_FACTOR   = 1.8f;     // how much above mean to count as beat
const float THRESH_MIN      = 40.0f;    // floor for very quiet passages

// For a moving estimate of BPM
const int MAX_BEAT_HISTORY  = 8;

// State variables
unsigned long lastSampleTime       = 0;
unsigned long lastBeatTime         = 0;
unsigned long lastDisplayUpdate    = 0;
bool          aboveThreshold       = false;

float         dcOffset             = 0.0f;
float         env                  = 0.0f;
float         envMean              = 0.0f;

unsigned long beatIntervals[MAX_BEAT_HISTORY];
int beatIndex  = 0;
int beatCount  = 0;

float currentBpm      = 0.0f;
bool  signalPresent   = false;

// =================== Helper Functions ===================

void drawBpmOnDisplay(float bpm, bool signalPresent) {
  display.clearDisplay();

  // Title
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("VINYL BPM METER");

  // Line
  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);

  // BPM Value
  display.setTextSize(3);
  display.setCursor(0, 20);

  if (!signalPresent || bpm <= 0.0f) {
    display.print("--");
  } else {
    display.print((int)(bpm + 0.5)); // rounded BPM
  }

  display.setTextSize(2);
  display.setCursor(80, 28);
  display.print("BPM");

  // Status text at bottom
  display.setTextSize(1);
  display.setCursor(0, 54);

  if (!signalPresent || bpm <= 0.0f) {
    display.print("Waiting for stable beat...");
  } else {
    display.print("Detecting...");
  }

  display.display();
}

float computeAverageBpm() {
  if (beatCount < 2) {
    return 0.0f;  // Not enough data
  }

  // Average beat intervals within allowed range
  unsigned long sum = 0;
  int valid = 0;
  for (int i = 0; i < beatCount; i++) {
    unsigned long interval = beatIntervals[i];
    if (interval >= (unsigned long)MIN_BEAT_INTERVAL_MS && 
        interval <= (unsigned long)MAX_BEAT_INTERVAL_MS) {
      sum += interval;
      valid++;
    }
  }

  if (valid == 0) {
    return 0.0f;
  }

  float avgIntervalMs = (float)sum / (float)valid;
  float bpm = 60000.0f / avgIntervalMs; // 60,000 ms per minute

  // Simple "octave folding" for sanity:
  // Try half and double and keep the one in a reasonable DJ range (roughly 70-180).
  if (bpm > 0.0f) {
    float candidates[3];
    candidates[0] = bpm * 0.5f;
    candidates[1] = bpm;
    candidates[2] = bpm * 2.0f;

    float best = bpm;
    float bestScore = 0.0f;

    for (int i = 0; i < 3; i++) {
      float t = candidates[i];
      if (t < 40.0f || t > 220.0f) continue;  // discard crazy values

      // Score tempos closer to the mid-range (around 110) higher.
      float score = 0.0f;
      float center = 110.0f;
      float dist = fabs(t - center);

      // Within +/-60 BPM of center gets a non-zero score that drops with distance.
      if (dist <= 60.0f) {
        score = 1.0f - (dist / 60.0f); // 1.0 at center, down to 0 at edges
      }

      if (score > bestScore) {
        bestScore = score;
        best = t;
      }
    }

    bpm = best;
  }

  return bpm;
}

// =================== Setup / Loop ===================

void setup() {
  // Optional serial debug
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32 BPM meter...");

  // ADC setup
  analogReadResolution(12); // 0-4095
  analogSetPinAttenuation(AUDIO_PIN, ADC_11db); // extend range closer to 3.3V

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;); // Don't proceed, loop forever
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32 BPM Meter");
  display.println("Init display & ADC...");
  display.display();
  delay(1000);

  // Initialize beat history
  for (int i = 0; i < MAX_BEAT_HISTORY; i++) {
    beatIntervals[i] = 0;
  }
}

void loop() {
  unsigned long now = millis();

  // ================== Sampling & Beat Detection ==================
  if (now - lastSampleTime >= (unsigned long)SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;

    int raw = analogRead(AUDIO_PIN);

    // Track DC offset (average raw level)
    dcOffset = (1.0f - DC_ALPHA) * dcOffset + DC_ALPHA * raw;

    // AC component as "signal"
    float signal = raw - dcOffset;
    float amplitude = fabs(signal);

    // Build a smoothed envelope of the amplitude
    env = env + ENV_ALPHA * (amplitude - env);

    // Long-term average of envelope for dynamic threshold
    envMean = envMean + MEAN_ALPHA * (env - envMean);

    float dynThresh = envMean * THRESH_FACTOR;
    if (dynThresh < THRESH_MIN) {
      dynThresh = THRESH_MIN;
    }

    bool strongNow = (env > dynThresh);

    // Rising edge detection for a beat
    if (strongNow) {
      if (!aboveThreshold) {
        unsigned long timeSinceLastBeat = now - lastBeatTime;

        if (timeSinceLastBeat > (unsigned long)MIN_BEAT_INTERVAL_MS) {
          // Register a beat
          if (lastBeatTime != 0) {
            // Store interval (skip very first beat because no previous)
            beatIntervals[beatIndex] = timeSinceLastBeat;
            beatIndex = (beatIndex + 1) % MAX_BEAT_HISTORY;
            if (beatCount < MAX_BEAT_HISTORY) {
              beatCount++;
            }

            currentBpm = computeAverageBpm();

            Serial.print("Beat interval: ");
            Serial.print(timeSinceLastBeat);
            Serial.print(" ms -> BPM ~ ");
            Serial.println(currentBpm);
          }

          lastBeatTime = now;
        }

        aboveThreshold = true;
      }
    } else {
      aboveThreshold = false;
    }

    // Determine if there is a signal at all in recent time
    if (lastBeatTime != 0 && (now - lastBeatTime <= 3000)) {
      signalPresent = true;
    } else {
      // If it's been a long time since last beat, consider no signal
      signalPresent = false;
      currentBpm = 0.0f;
      beatCount = 0;
    }
  }

  // ================== Display Update (slower) ==================
  if (now - lastDisplayUpdate >= (unsigned long)DISPLAY_UPDATE_INTERVAL_MS) {
    lastDisplayUpdate = now;
    drawBpmOnDisplay(currentBpm, signalPresent);
  }
}
