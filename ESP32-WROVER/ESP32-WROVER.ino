#include <driver/dac.h>
#include <math.h>

#define TRIG 18
#define ECHO 19

const int sampleRate = 8000;
const float pi = 3.14159265;

const float beepFreq = 800.0;   // фиксированная частота
const float volume   = 0.005;     // громкость

// измерение расстояния
float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30 мс таймаут
  float distance = duration * 0.034 / 2.0;       // см
  if (distance == 0 || distance > 400) distance = 400;
  return distance;
}

void setup() {
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  dac_output_enable(DAC_CHANNEL_1);  // GPIO25 → PAM8403 → динамик
}

void playBeep(int durationMs) {
  int samples = (sampleRate * durationMs) / 1000;
  for (int i = 0; i < samples; i++) {
    float s = sin(2 * pi * beepFreq * i / sampleRate);
    uint8_t sample = (uint8_t)(127 + 127 * s * volume);

    dac_output_voltage(DAC_CHANNEL_1, sample);

    delayMicroseconds(1000000 / sampleRate);
  }
}

void loop() {
  float dist = getDistance(TRIG, ECHO);

  // зависимость пульсации от расстояния
  int interval;
  if (dist <= 30) {
    interval = 0; // непрерывный
  } else {
    interval = map((int)dist, 30, 400, 80, 1200); // от 80 до 1200 мс
  }

  if (interval == 0) {
    playBeep(50); // бесконечные короткие бипы → воспринимается как непрерывный звук
  } else {
    playBeep(50);
    delay(interval);
  }
}
