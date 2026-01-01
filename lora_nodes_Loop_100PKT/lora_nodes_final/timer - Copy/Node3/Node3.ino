#include <SPI.h>
#include <SD.h>
#include <LoRa.h>

// ════════════════════════════════════════════════════════════════
//                     DEBUG / REAL TEST SWITCH
// ════════════════════════════════════════════════════════════════
//#define QUICK_DEBUG
#ifdef QUICK_DEBUG
  const int PACKETS_PER_TEST = 12;
  const unsigned long TX_INTERVAL = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 60000UL;
  const unsigned long INITIAL_START_DELAY = 10000UL;
#else
  const int PACKETS_PER_TEST = 100;
  const unsigned long TX_INTERVAL = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 600000UL;
  const unsigned long INITIAL_START_DELAY = 13000UL;
#endif

// Timer: (100 × 15s) + 60s + 5s buffer = 1565 seconds
const unsigned long SYNC_BUFFER = 5000UL;
const unsigned long TEST_DURATION = (unsigned long)PACKETS_PER_TEST * TX_INTERVAL + PAUSE_BETWEEN_TESTS + SYNC_BUFFER;
const unsigned long WATCHDOG_TIMEOUT = 120000UL;
// ════════════════════════════════════════════════════════════════

#define SDCARD_SS_PIN 4
const int MY_ID = 3;
const int ACCEPT_FROM = 2;

bool sdOK = false;
int currentTest = 0;
unsigned long lastRx = 0;
unsigned long testStartTime = 0;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8, 5, 8};

void resetSDCard() {
  if (!sdOK) return;
  const char* filename = "send3.txt";
  
  if (SD.exists(filename)) {
    for (int attempt = 0; attempt < 3; attempt++) {
      if (SD.remove(filename)) break;
      delay(100);
    }
  }
  
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    f.println("=== NEW TEST SESSION (TIMER-BASED) ===");
    f.print("Test Duration: ");
    f.print(TEST_DURATION / 1000);
    f.println(" seconds per config");
    f.println("========================================");
    f.close();
  } else {
    sdOK = false;
  }
}

void setConfig(int t) {
  LoRa.setSpreadingFactor(SF[t]);
  LoRa.setCodingRate4(CR[t]);
  Serial.print(F("Config → Test ")); Serial.print(t+1);
  Serial.print(F(" (SF")); Serial.print(SF[t]);
  Serial.print(F(" CR4/")); Serial.print(CR[t]);
  Serial.println(F(")"));
}

void logWithRetry(const char* data) {
  if (!sdOK) return;
  for (int i = 0; i < 3; i++) {
    File f = SD.open("send3.txt", FILE_WRITE);
    if (f) { f.println(data); f.close(); return; }
    delay(50);
  }
  SD.end(); delay(100);
  sdOK = SD.begin(SDCARD_SS_PIN);
}

int detectTest(const char* name) {
  for (int i = 0; i < 4; i++)
    if (strncmp(name, testNames[i], strlen(testNames[i])) == 0) return i;
  return -1;
}

void reinitLoRa() {
  Serial.println(F("Reinit LoRa..."));
  LoRa.end(); delay(200);
  LoRa.begin(868E6);
  LoRa.setTxPower(14); LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
  setConfig(currentTest);
  LoRa.receive();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200); 
  // Wait for Serial OR timeout after 3 seconds
  unsigned long serialTimeout = millis();
  while (!Serial && millis() - serialTimeout < 3000) {
    // Wait with timeout
  }
  delay(500);
  
  Serial.println(F("\n════════════════════════════════════════"));
  Serial.println(F("   Node 3 - Forwarder (TIMER-BASED)"));
  Serial.println(F("════════════════════════════════════════"));
  Serial.print(F("TEST_DURATION: ")); 
  Serial.print(TEST_DURATION / 1000); 
  Serial.println(F(" seconds"));

  for (int i = 0; i < 5; i++) {
    if (SD.begin(SDCARD_SS_PIN)) { sdOK = true; break; }
    delay(300);
  }
  resetSDCard();

  if (!LoRa.begin(868E6)) while (1);
  LoRa.setTxPower(14); LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
  setConfig(0);
  
  // Synchronized startup
  Serial.print(F("Waiting ")); Serial.print(INITIAL_START_DELAY/1000); Serial.println(F(" sec for sync..."));
  delay(INITIAL_START_DELAY);
  
  LoRa.receive();
  lastRx = testStartTime = millis();
  
  Serial.println(F("TIMER STARTED - Listening...\n"));
}

void loop() {
  // ════════════════════════════════════════════════════════════════
  // TIMER-BASED TEST SWITCHING
  // ════════════════════════════════════════════════════════════════
  if (millis() - testStartTime >= TEST_DURATION) {
    int nextTest = (currentTest + 1) % 4;
    
    Serial.println(F("\n════════════════════════════════════════"));
    Serial.print(F("⏱ TIMER: Test ")); Serial.print(currentTest + 1);
    Serial.print(F(" → Test ")); Serial.println(nextTest + 1);
    Serial.println(F("════════════════════════════════════════\n"));
    
    currentTest = nextTest;
    setConfig(currentTest);
    testStartTime = millis();
    lastRx = millis();
  }
  
  // ════════════════════════════════════════════════════════════════
  // PACKET RECEPTION
  // ════════════════════════════════════════════════════════════════
  int sz = LoRa.parsePacket();
  if (sz > 15) {
    char buf[220] = {};
    int i = 0;
    while (LoRa.available() && i < sizeof(buf)-1) buf[i++] = LoRa.read();
    buf[i] = '\0';

    char testName[30], nextTestName[30] = "";
    int fromID, pktNum;
    bool isLast = strstr(buf, "|LAST|") != nullptr;

    if (strstr(buf, "NEXT:")) {
      char* p = strstr(buf, "NEXT:");
      sscanf(p, "NEXT:%29[^|]", nextTestName);
    }

    if (sscanf(buf, "%29[^|]|ID:%d|num:%d", testName, &fromID, &pktNum) != 3) {
      LoRa.receive(); return;
    }
    if (fromID != ACCEPT_FROM) { LoRa.receive(); return; }

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    // Forward with MY_ID
    char out[220];
    if (isLast && strlen(nextTestName))
      snprintf(out, sizeof(out), "%s|ID:%d|num:%03d|LAST|NEXT:%s", testName, MY_ID, pktNum, nextTestName);
    else
      snprintf(out, sizeof(out), "%s|ID:%d|num:%03d", testName, MY_ID, pktNum);

    LoRa.beginPacket();
    LoRa.print(out);
    LoRa.endPacket();
    delay(8);
    LoRa.receive();

    // Optional: Check for mismatch
    int detectedTest = detectTest(testName);
    if (detectedTest >= 0 && detectedTest != currentTest) {
      Serial.print(F("⚠ MISMATCH: Timer=Test")); Serial.print(currentTest + 1);
      Serial.print(F(" Received=Test")); Serial.println(detectedTest + 1);
    }

    // Log
    char logEntry[250];
    snprintf(logEntry, sizeof(logEntry), "%s RSSI:%d SNR:%.2f", out, rssi, snr);
    Serial.print(F("FWD → ")); Serial.print(out);
    if (isLast) Serial.print(F(" [LAST]"));
    Serial.print(F(" RSSI:")); Serial.print(rssi);
    Serial.print(F(" SNR:")); Serial.println(snr);
    logWithRetry(logEntry);

    lastRx = millis();
    digitalWrite(LED_BUILTIN, HIGH); delay(40); digitalWrite(LED_BUILTIN, LOW);
  }

  // Simple Watchdog
  if (millis() - lastRx > WATCHDOG_TIMEOUT) {
    Serial.println(F("⚠ Watchdog - reinit LoRa"));
    reinitLoRa();
    lastRx = millis();
  }
}