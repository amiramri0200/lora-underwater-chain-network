#include <SPI.h>
#include <SD.h>
#include <LoRa.h>

// Same debug switch as others
#define QUICK_DEBUG
#ifdef QUICK_DEBUG
  const unsigned long WATCHDOG_TIMEOUT = 50000UL;
  const unsigned long STATUS_PRINT     = 20000UL;
#else
  const unsigned long WATCHDOG_TIMEOUT = 180000UL;
  const unsigned long STATUS_PRINT     = 300000UL;
#endif

#define SDCARD_SS_PIN 4
const int ACCEPT_FROM = 3;  // Only accept from Node 3

bool sdOK = false;
int currentTest = 0;
unsigned long lastRx = 0;
unsigned long lastStatus = 0;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8,  5,  8};

void setConfig(int t) {
  LoRa.setSpreadingFactor(SF[t]);
  LoRa.setCodingRate4(CR[t]);
  Serial.print(F("Node 4 → Test ")); Serial.println(t+1);
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

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200); while (!Serial); delay(500);
  Serial.println(F("\n=== Node 4 - FINAL RECEIVER ==="));

  for (int i = 0; i < 5; i++) {
    if (SD.begin(SDCARD_SS_PIN)) { sdOK = true; break; }
    delay(300);
  }

  if (!LoRa.begin(868E6)) while (1);
  LoRa.setTxPower(14); LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
  setConfig(0);
  LoRa.receive();

  lastRx = lastStatus = millis();
}

void loop() {
  int sz = LoRa.parsePacket();
  if (sz > 15) {
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

    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    int newTest = detectTest(testName);
    if (newTest >= 0 && newTest != currentTest) {
      currentTest = newTest;
      setConfig(currentTest);
    }

    char logEntry[250];
    snprintf(logEntry, sizeof(logEntry), "%s RSSI:%d SNR:%.2f%s", buf, rssi, snr, isLast ? " [END OF TEST]" : "");

    Serial.print(F("RCVD: ")); Serial.print(buf);
    Serial.print(F(" RSSI:")); Serial.print(rssi);
    Serial.print(F(" SNR:")); Serial.println(snr);
    if (isLast) Serial.println(F("*** END OF TEST DETECTED ***"));

    logWithRetry(logEntry);

    lastRx = millis();
    digitalWrite(LED_BUILTIN, HIGH); delay(100); digitalWrite(LED_BUILTIN, LOW);
  }

  // Watchdog (just reinit + scan)
  if (millis() - lastRx > WATCHDOG_TIMEOUT) {
    LoRa.end(); delay(200); LoRa.begin(868E6);
    LoRa.setTxPower(14); LoRa.enableCrc();
    LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
    setConfig(currentTest);
    LoRa.receive();
    lastRx = millis();
  }

  if (millis() - lastStatus > STATUS_PRINT) {
    Serial.print(F("Node 4 alive - Test ")); Serial.print(currentTest+1);
    Serial.print(F(" | Last packet ")); Serial.println((millis()-lastRx)/1000);
    lastStatus = millis();
  }
}