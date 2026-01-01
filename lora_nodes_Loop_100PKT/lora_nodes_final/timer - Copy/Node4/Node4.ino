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
const unsigned long STATUS_PRINT = 60000UL;
// ════════════════════════════════════════════════════════════════

#define SDCARD_SS_PIN 4
const int ACCEPT_FROM = 3;

bool sdOK = false;
int currentTest = 0;
unsigned long lastRx = 0;
unsigned long testStartTime = 0;
unsigned long lastStatus = 0;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8, 5, 8};

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
    File f = SD.open("recv4.txt", FILE_WRITE);
    if (f) { f.println(data); f.close(); return; }
    delay(50);
  }
  SD.end(); delay(100); sdOK = SD.begin(SDCARD_SS_PIN);
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
  Serial.begin(115200); while (!Serial); delay(500);
  
  Serial.println(F("\n════════════════════════════════════════"));
  Serial.println(F("   Node 4 - FINAL RECEIVER (TIMER-BASED)"));
  Serial.println(F("════════════════════════════════════════"));
  Serial.print(F("TEST_DURATION: ")); 
  Serial.print(TEST_DURATION / 1000); 
  Serial.println(F(" seconds"));

  for (int i = 0; i < 5; i++) {
    if (SD.begin(SDCARD_SS_PIN)) { sdOK = true; break; }
    delay(300);
  }

  if (!LoRa.begin(868E6)) while (1);
  LoRa.setTxPower(14); LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
  setConfig(0);
  
  // Synchronized startup
  Serial.print(F("Waiting ")); Serial.print(INITIAL_START_DELAY/1000); Serial.println(F(" sec for sync..."));
  delay(INITIAL_START_DELAY);
  
  LoRa.receive();
  lastRx = testStartTime = lastStatus = millis();
  
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
  if (sz > 0) {
    char buf[220] = {};
    int i = 0;
    while (LoRa.available() && i < sizeof(buf)-1) buf[i++] = LoRa.read();
    buf[i] = '\0';

    char testName[30];
    int fromID, pktNum;
    bool isLast = strstr(buf, "|LAST|") != nullptr;

    if (sscanf(buf, "%29[^|]|ID:%d|num:%d", testName, &fromID, &pktNum) != 3) {
      LoRa.receive(); return;
    }
    if (fromID != ACCEPT_FROM) { LoRa.receive(); return; }
    // Temporary test - accept from any ID (1, 2, or 3)

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    // Optional: Check for mismatch
    int detectedTest = detectTest(testName);
    if (detectedTest >= 0 && detectedTest != currentTest) {
      Serial.print(F("⚠ MISMATCH: Timer=Test")); Serial.print(currentTest + 1);
      Serial.print(F(" Received=Test")); Serial.println(detectedTest + 1);
    }

    char logEntry[250];
    snprintf(logEntry, sizeof(logEntry), "%s RSSI:%d SNR:%.2f%s", 
             buf, rssi, snr, isLast ? " [END]" : "");

    Serial.print(F("RCVD: ")); Serial.print(buf);
    Serial.print(F(" RSSI:")); Serial.print(rssi);
    Serial.print(F(" SNR:")); Serial.println(snr);
    if (isLast) Serial.println(F("*** END OF TEST ***"));

    logWithRetry(logEntry);

    lastRx = millis();
    digitalWrite(LED_BUILTIN, HIGH); delay(100); digitalWrite(LED_BUILTIN, LOW);
  }

  // Simple Watchdog
  if (millis() - lastRx > WATCHDOG_TIMEOUT) {
    Serial.println(F("⚠ Watchdog - reinit LoRa"));
    reinitLoRa();
    lastRx = millis();
  }

  // Status heartbeat
  if (millis() - lastStatus > STATUS_PRINT) {
    unsigned long timeInTest = (millis() - testStartTime) / 1000;
    unsigned long timeRemaining = (TEST_DURATION / 1000) - timeInTest;
    
    Serial.print(F("Status: Test ")); Serial.print(currentTest+1);
    Serial.print(F(" | Time in test: ")); Serial.print(timeInTest);
    Serial.print(F("s | Switch in: ")); Serial.print(timeRemaining);
    Serial.println(F("s"));
    lastStatus = millis();
  }
}