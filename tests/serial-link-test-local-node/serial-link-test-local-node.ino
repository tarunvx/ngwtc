/*
  ============================================================
   SWTC — SERIAL LINK QUALITY TEST : LOCAL NODE  (ESP32 WROOM-32)
  ============================================================

   Receiver half of the wired-link trial. Pair with
   tests/serial-link-test-tank-node.

   Reads framed, CRC-protected packets off a single wire and reports the
   three numbers that decide whether plain TTL UART is good enough over
   your ~15 m run, or whether you need RS-485 transceivers.

   ----------------------------------------------------------------
   WIRING
   ----------------------------------------------------------------
     NodeMCU TX (D10 / GPIO1)  ------------->  ESP32 GPIO32  (LINK_RX_PIN)
     GND                       <----------->   GND

   GPIO32 is free on the v6 local node (it was a float input in v5, now
   sourced over the link). Both sides are 3.3 V logic — direct connection.

   Statistics print here once per second; the tank node stays silent
   because its UART0 is carrying the link.

   ----------------------------------------------------------------
   READING THE OUTPUT
   ----------------------------------------------------------------
     good    frames that passed CRC
     crcErr  frames that arrived corrupted  -> electrical noise on the wire
     missed  sequence numbers that never arrived -> dropped/garbled framing
     resync  times the parser had to hunt for the preamble again

   Verdict at a given baud rate, over several minutes:
     crcErr = 0, missed = 0        wire is HEALTHY at this rate
     a few per minute              MARGINAL — drop the baud or use RS-485
     continuous errors             wire/noise is too poor for plain TTL

   Real telemetry is only ~36 bytes at 4 Hz, so 9600 baud already carries
   ~25x the needed traffic. Testing higher rates is about proving margin,
   not throughput.

   Board: "ESP32 Dev Module"  |  monitor at 115200
  ============================================================
*/

#include <Arduino.h>

#define LINK_RX_PIN   32       // wire from the tank node's TX
#define LINK_BAUD     9600     // must match the tank node
#define REPORT_MS     1000
#define PRE0          0xAA
#define PRE1          0x55
#define FRAME_LEN     32

static uint8_t  s_buf[FRAME_LEN];
static uint8_t  s_idx   = 0;
static uint8_t  s_state = 0;    // 0=hunt PRE0, 1=hunt PRE1, 2=collecting

// Window counters
static uint32_t s_good = 0, s_crcErr = 0, s_missed = 0, s_resync = 0;
static uint32_t s_rawBytes = 0;    // distinguishes "no wire" from "bad baud"
// Cumulative
static uint32_t s_totGood = 0, s_totCrc = 0, s_totMissed = 0;

static uint16_t s_prevSeq = 0;
static bool     s_havePrev = false;

static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static void onFrame() {
  uint16_t want = crc16(s_buf + 2, FRAME_LEN - 4);
  uint16_t got  = (uint16_t)s_buf[30] | ((uint16_t)s_buf[31] << 8);
  if (want != got) { s_crcErr++; s_totCrc++; return; }

  uint16_t seq = (uint16_t)s_buf[2] | ((uint16_t)s_buf[3] << 8);
  if (s_havePrev) {
    uint16_t expected = (uint16_t)(s_prevSeq + 1);
    if (seq != expected) {
      uint16_t gap = (uint16_t)(seq - expected);
      s_missed += gap;
      s_totMissed += gap;
    }
  }
  s_prevSeq  = seq;
  s_havePrev = true;
  s_good++;
  s_totGood++;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("   SERIAL LINK QUALITY TEST — LOCAL NODE (RX)"));
  Serial.println(F("=================================================="));
  Serial.printf ("  RX pin=GPIO%d   baud=%d   frame=%d bytes\n",
                 LINK_RX_PIN, LINK_BAUD, FRAME_LEN);
  Serial.println(F("  expecting 10 frames/sec from the tank node"));
  Serial.println(F("--------------------------------------------------"));

  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, -1);   // RX only
}

void loop() {
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    s_rawBytes++;
    switch (s_state) {
      case 0:
        if (b == PRE0) { s_buf[0] = b; s_state = 1; }
        break;
      case 1:
        if (b == PRE1) { s_buf[1] = b; s_idx = 2; s_state = 2; }
        else if (b == PRE0) { /* stay, could be the real start */ }
        else { s_state = 0; s_resync++; }
        break;
      case 2:
        s_buf[s_idx++] = b;
        if (s_idx >= FRAME_LEN) { onFrame(); s_state = 0; }
        break;
    }
  }

  static uint32_t lastReport = 0;
  if (millis() - lastReport < REPORT_MS) return;
  lastReport = millis();

  uint32_t good = s_good, crc = s_crcErr, miss = s_missed, re = s_resync;
  uint32_t raw  = s_rawBytes;
  s_good = s_crcErr = s_missed = s_resync = 0;
  s_rawBytes = 0;

  if (good == 0 && crc == 0) {
    if (raw == 0) {
      Serial.printf("[--] ZERO bytes on GPIO%d  -> wrong pin? (must be GPIO%d, NOT the"
                    " pin marked RX), broken wire, no common GND, or TX node not running\n",
                    LINK_RX_PIN, LINK_RX_PIN);
    } else {
      Serial.printf("[--] %lu bytes/s arriving but NO valid frames  -> baud mismatch"
                    " (both must be %d) or heavy corruption\n",
                    (unsigned long)raw, LINK_BAUD);
    }
    s_havePrev = false;
    return;
  }

  uint32_t offered = good + crc + miss;
  float    ok      = offered ? (100.0f * (float)good / (float)offered) : 0.0f;

  Serial.printf("good=%2lu/s  crcErr=%lu  missed=%lu  resync=%lu  |  %.1f%% ok"
                "  |  raw=%lu B/s  |  totals good=%lu crc=%lu missed=%lu\n",
                (unsigned long)good, (unsigned long)crc, (unsigned long)miss,
                (unsigned long)re, ok, (unsigned long)raw,
                (unsigned long)s_totGood, (unsigned long)s_totCrc,
                (unsigned long)s_totMissed);
}
