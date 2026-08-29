/*
  ============================================================
   SWTC — SERIAL LINK QUALITY TEST : TANK NODE  (NodeMCU ESP-12E)
  ============================================================

   Transmitter half of the wired-link trial. Sends framed, CRC-protected
   packets down a single wire so the ESP32 can measure how many survive.
   Pair with tests/serial-link-test-local-node.

   Goal: find out whether plain 3.3 V TTL UART is reliable enough over your
   ~15 m run BEFORE spending on RS-485 transceivers.

   ----------------------------------------------------------------
   WIRING  (only ONE signal wire needed — telemetry is one-way)
   ----------------------------------------------------------------
     NodeMCU TX (D10 / GPIO1)  ------------->  ESP32 GPIO32 (RX)
     GND                       <----------->   GND   (already common
                                                via your power pair)

   Both MCUs are 3.3 V logic, so this is a direct connection — no level
   shifter. A solid common ground is essential; without it UART has no
   voltage reference and you get garbage regardless of wire quality.

   This uses hardware UART0, which is the same port the USB bridge uses.
   That is deliberate: it makes the test representative of the real
   implementation. Consequences:
     - The USB serial monitor on THIS node will show binary garbage. Normal.
     - All the useful statistics are printed by the ESP32, not here.
     - Flashing still works (nothing is wired to GPIO3/RX).
     - At boot the ROM emits a burst at 74880 baud; the receiver rejects it
       on preamble/CRC, which also exercises resynchronisation.

   ----------------------------------------------------------------
   WHAT TO TEST
   ----------------------------------------------------------------
   Start at 9600. If the ESP32 reports ~0 CRC errors and ~0 missed frames
   for a few minutes, step up (19200 -> 38400 -> 57600 -> 115200) until
   errors appear. The highest clean rate tells you your margin: real
   telemetry is ~36 bytes at 4 Hz, so even 9600 is far more than enough —
   headroom above that is what proves the wire is healthy rather than
   marginal.

   Board: "NodeMCU 1.0 (ESP-12E Module)"  |  link baud set below
  ============================================================
*/

#include <ESP8266WiFi.h>

#define LINK_BAUD        9600     // must match the local node
#define FRAME_PERIOD_MS  100      // 10 frames/sec
#define PRE0             0xAA
#define PRE1             0x55

// 32 bytes total — roughly the size of a real v6 telemetry frame.
typedef struct __attribute__((packed)) {
  uint8_t  pre0;
  uint8_t  pre1;
  uint16_t seq;
  uint8_t  pad[26];
  uint16_t crc;
} TestFrame;

static uint16_t s_seq = 0;

static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

void setup() {
  // Radio off: removes WiFi interrupt jitter so this measures the WIRE.
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(10);

  Serial.begin(LINK_BAUD);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);   // off (active-LOW)
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < FRAME_PERIOD_MS) return;
  last = millis();

  TestFrame f;
  f.pre0 = PRE0;
  f.pre1 = PRE1;
  f.seq  = s_seq++;
  for (uint8_t i = 0; i < sizeof(f.pad); i++) f.pad[i] = (uint8_t)(s_seq + i);
  f.crc = crc16((const uint8_t*)&f + 2, sizeof(f) - 4);   // seq + pad

  Serial.write((const uint8_t*)&f, sizeof(f));

  digitalWrite(LED_BUILTIN, LOW);
  delay(2);
  digitalWrite(LED_BUILTIN, HIGH);
}
