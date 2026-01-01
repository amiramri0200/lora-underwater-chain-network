#include <SPI.h>
#include <SD.h>
#include <LoRa.h>

// ================================================================
//                     DEBUG / REAL TEST SWITCH
// ================================================================
//#define QUICK_DEBUG
#ifdef QUICK_DEBUG
  const unsigned long WATCHDOG_TIMEOUT     = 40000UL;
  const unsigned long STATUS_PRINT         = 20000UL;
  const unsigned long PAUSE_BETWEEN_TESTS  = 53000UL;   // Must match Node 1!
#else
  const unsigned long WATCHDOG_TIMEOUT     = 91000UL;
  const unsigned long STATUS_PRINT         = 300000UL;
  const unsigned long PAUSE_BETWEEN_TESTS  = 53000UL;
#endif
// ================================================================

#define SDCARD_SS_PIN 4
const int MY_ID       = 3;
const int ACCEPT_FROM = 2;        // Only accept from Node 2

bool sdOK = false;
int currentTest = 0;
unsigned long lastRx = 0;
unsigned long lastStatus = 0;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8,  5,  8};
//-----------------------------------------
void resetSDCard() {
  if (!sdOK) {
    Serial.println(F("SD not available - skip reset"));
    return;
  }
  
  const char* filename = "send3.txt";
  
  // ════════════════════════════════════════════════════════════════
  // Step 1: Delete old file (with retry)
  // ════════════════════════════════════════════════════════════════
  if (SD.exists(filename)) {
    Serial.print(F("Deleting old file: "));
    Serial.println(filename);
    
    bool deleted = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      if (SD.remove(filename)) {
        deleted = true;
        Serial.println(F("  → Deletion successful"));
        break;
      }
      delay(100);
    }
    
    if (!deleted) {
      Serial.println(F("  → FAILED to delete old file!"));
      Serial.println(F("  → Will try to overwrite instead"));
    }
  } else {
    Serial.print(F("No old file found: "));
    Serial.println(filename);
  }
  
  // ════════════════════════════════════════════════════════════════
  // Step 2: Create fresh file with header
  // ════════════════════════════════════════════════════════════════
  File f = SD.open(filename, FILE_WRITE);
  
  if (f) {
    // Write header (WITHOUT F() macro in File methods!)
    f.println("=== NEW TEST SESSION ===");
    f.print("Started at: ");
    f.print(millis() / 1000);
    f.println(" seconds since boot");
    f.println("Format: testName|ID:x|num:xxx");
    f.println("========================================");
    f.close();
    
    Serial.println(F("✓ Created fresh log file with header"));
  } else {
    Serial.println(F("✗ FAILED to create log file!"));
    Serial.println(F("  → SD card may be full or corrupted"));
    sdOK = false;  // Mark SD as unavailable
  }
}
//------------------------------------------------------------------
void setConfig(int t) {
  LoRa.setSpreadingFactor(SF[t]);
  LoRa.setCodingRate4(CR[t]);          // MKR WAN 1310 needs this!
  Serial.print(F("Switched to Test ")); Serial.println(t+1);
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
  Serial.println(F("Watchdog → reinit LoRa"));
  LoRa.end(); delay(200);
  LoRa.begin(868E6);
  LoRa.setTxPower(14); LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3); LoRa.setPreambleLength(12); LoRa.setSyncWord(0x34);
  setConfig(currentTest);
  LoRa.receive();
}

bool scanMode() {
  Serial.println(F("\nSCAN MODE: Searching for signal..."));
  
  for (int cycle = 0; cycle < 8; cycle++) {
    Serial.print(F("Scan cycle ")); Serial.print(cycle+1); Serial.println(F("/8"));
    
    for (int t = 0; t < 4; t++) {
      setConfig(t);
      LoRa.receive();
      
      Serial.print(F("  Try Test ")); Serial.print(t+1); Serial.print(F("... "));
      
      unsigned long scanStart = millis();
      const unsigned long SCAN_TIMEOUT = 16000;  // 16 seconds per config
      
      // ════════════════════════════════════════════════════════════
      // ACTIVE POLLING with multiple packet chances
      // ════════════════════════════════════════════════════════════
      while (millis() - scanStart < SCAN_TIMEOUT) {
        int sz = LoRa.parsePacket();
        
        if (sz > 10) {
          char buf[220] = {};  // Large buffer for full packets
          int i = 0;
          while (LoRa.available() && i < sizeof(buf)-1) {
            buf[i++] = LoRa.read();
          }
          buf[i] = '\0';
          
          // ════════════════════════════════════════════════════════
          // Try strict parsing first
          // ════════════════════════════════════════════════════════
          char testName[30];
          int fromID, pktNum;
          
          if (sscanf(buf, "%29[^|]|ID:%d|num:%d", testName, &fromID, &pktNum) == 3) {
            // ✅ PERFECT! Fully valid packet
            currentTest = t;
            lastRx = millis();
            
            Serial.println(F("FOUND!"));
            Serial.print(F("    → Packet: ")); Serial.println(buf);
            Serial.print(F("    → Locked to Test ")); Serial.println(t+1);
            
            // Drain any remaining bytes
            while (LoRa.available()) LoRa.read();
            
            LoRa.receive();
            return true;  // ✅ SUCCESS
            
          } else if (strstr(buf, "test") && strstr(buf, "ID:")) {
            // ⚠️ Packet is malformed but recognizable
            // This is likely the right config, but packet was corrupted
            Serial.println(F("partial"));
            Serial.print(F("    → Data: ")); Serial.println(buf);
            Serial.println(F("    → Staying on this config for more packets..."));
            
            // Don't break! Keep listening on this config
            // Maybe next packet will be clean
            continue;
            
          } else {
            // ❌ Total garbage
            Serial.println(F("garbage"));
            Serial.print(F("    → Data: ")); Serial.println(buf);
            // Continue listening on this config
            continue;
          }
        }
        
        yield();  // Keep watchdog happy
        delay(5); // Avoid busy loop
      }
      
      // Timeout on this config
      Serial.println(F("timeout"));
    }
  }
  
  // Nothing found after all attempts
  Serial.println(F("\nScan FAILED → Defaulting to Test 1"));
  currentTest = 0;
  setConfig(0);
  LoRa.receive();
  return false;  // ❌ FAILURE
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200); while (!Serial); delay(500);
  Serial.println(F("\n=== Node 3 - Forwarder (2 → 4) ==="));

  for (int i = 0; i < 5; i++) {
    if (SD.begin(SDCARD_SS_PIN)) { sdOK = true; break; }
    delay(300);
  }
  resetSDCard();   // format SD card

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

    // Forward with Node 3 ID
    char out[220];
    if (isLast && strlen(nextTestName))
      snprintf(out, sizeof(out), "%s|ID:%d|num:%03d|LAST|NEXT:%s", testName, MY_ID, pktNum, nextTestName);
    else
      snprintf(out, sizeof(out), "%s|ID:%d|num:%03d", testName, MY_ID, pktNum);

    LoRa.beginPacket();
    LoRa.print(out);
    LoRa.endPacket();
    delay(8);                    // SPI killer delay
    LoRa.receive();

    // === SYNC LOGIC ===
    if (isLast && strlen(nextTestName) > 0) {
      int next = detectTest(nextTestName);
      if (next >= 0) {
        Serial.print(F("LAST packet → waiting "));
        Serial.print(PAUSE_BETWEEN_TESTS/1000); Serial.println(F("s"));
        delay(PAUSE_BETWEEN_TESTS);
        currentTest = next;
        setConfig(currentTest);
        //Serial.println(F("Switched to next test!"));
      }
    } else {
      int newTest = detectTest(testName);
      if (newTest >= 0 && newTest != currentTest) {
        currentTest = newTest;
        setConfig(currentTest);
      }
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

  // Watchdog + Scan
  if (millis() - lastRx > WATCHDOG_TIMEOUT) {
    //if (millis() - lastRx < WATCHDOG_TIMEOUT * 1.5) {
    //  reinitLoRa();
    //} else {
    //  scanMode();
    //}
    scanMode();
    lastRx = millis();
  }

  if (millis() - lastStatus > STATUS_PRINT) {
    Serial.print(F("Node 3 alive - Test ")); Serial.print(currentTest+1);
    Serial.print(F(" | LastRx ")); Serial.println((millis()-lastRx)/1000);
    lastStatus = millis();
  }
}