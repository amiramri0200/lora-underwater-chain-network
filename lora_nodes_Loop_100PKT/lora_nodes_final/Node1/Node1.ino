#include <SPI.h>
#include <SD.h>
#include <LoRa.h>
// ←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←
//#define QUICK_DEBUG
#ifdef QUICK_DEBUG
  const int    PACKETS_PER_TEST       = 12;
  const unsigned long TX_INTERVAL     = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 60000UL;   // ← 60 seconds!

  const unsigned long INITIAL_START_DELAY = 10000UL;
#else
  const int    PACKETS_PER_TEST       = 100;
  const unsigned long TX_INTERVAL     = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 60000UL;   // ← 60 seconds even in real test!
  const unsigned long INITIAL_START_DELAY = 13000UL;   // or keep 10 min if you want
#endif
// ←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←

#define SDCARD_SS_PIN 4
const int MY_ID = 1;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8,  5,  8};   // coding rate denominator

int currentTest = 0;
int packetCount = 0;
bool inPause = false;
unsigned long pauseStart = 0;
unsigned long lastTx = 0;
bool sdOK = false;

void log(const char* s) {
  if (!sdOK) return;
  File f = SD.open("send1.txt", FILE_WRITE);
  if (f) { f.println(s); f.close(); }
}
//-------------
void resetSDCard() {
  if (!sdOK) {
    Serial.println(F("SD not available - skip reset"));
    return;
  }
  
  const char* filename = "send1.txt";
  
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
//--------------
void setConfig(int idx) {
  LoRa.setSpreadingFactor(SF[idx]);
  LoRa.setCodingRate4(CR[idx]);        // correct function name!
  Serial.print(F("\n=== STARTING ")); Serial.println(testNames[idx]);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);              // faster baud = faster debug!
  while (!Serial);
  Serial.println(F("\nNode 1 - Sender"));

  for (int i = 0; i < 5 && !sdOK; i++) {
    sdOK = SD.begin(SDCARD_SS_PIN);
    delay(200);
  }
  resetSDCard();   // format SD card

  if (!LoRa.begin(868E6)) { Serial.println("LoRa fail"); while (1); }
  LoRa.setTxPower(14);
  LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3);
  LoRa.setPreambleLength(12);
  LoRa.setSyncWord(0x34);
  setConfig(0);

  Serial.print(F("Waiting ")); Serial.print(INITIAL_START_DELAY/1000); Serial.println(F(" sec..."));
  delay(INITIAL_START_DELAY);

  Serial.println(F("STARTING TRANSMISSIONS"));
  lastTx = millis();
}

void loop() {
  if (inPause) {
    if (millis() - pauseStart >= PAUSE_BETWEEN_TESTS) {
      inPause = false;

      // === TEST TRANSITION LOGIC ===
      if (currentTest == 3) {
        // We just finished Test 4 → restart from Test 1
        currentTest = 0;
        Serial.println(F("\n>>> ALL 4 TESTS DONE! Waiting 60 seconds then REPEATING FROM TEST 1 <<<"));
      } else {
        // Normal progression: 0→1, 1→2, 2→3
        currentTest++;
        Serial.print(F("\n>>> TEST ")); Serial.print(currentTest);
        Serial.println(F(" FINISHED → MOVING TO NEXT TEST <<<"));
      }

      packetCount = 0;
      setConfig(currentTest);
      Serial.print(F("=== STARTING ")); Serial.println(testNames[currentTest]);
    }
    return;
  }

  if (millis() - lastTx >= TX_INTERVAL) {
    lastTx = millis();
    packetCount++;

    char msg[160];
    bool isLastPacket = (packetCount >= PACKETS_PER_TEST);

    if (isLastPacket) {
      const char* nextTestName = (currentTest == 3) ? testNames[0] : testNames[currentTest + 1];
      snprintf(msg, sizeof(msg), "%s|ID:%d|num:%03d|LAST|NEXT:%s", 
               testNames[currentTest], MY_ID, packetCount, nextTestName);
    } else {
      snprintf(msg, sizeof(msg), "%s|ID:%d|num:%03d", 
               testNames[currentTest], MY_ID, packetCount);
    }

    LoRa.beginPacket();
    LoRa.print(msg);
    LoRa.endPacket();
    LoRa.receive();

    Serial.print(F("Sent: ")); Serial.println(msg);
    log(msg);
    digitalWrite(LED_BUILTIN, HIGH); delay(50); digitalWrite(LED_BUILTIN, LOW);

    if (isLastPacket) {
      Serial.print(F("\nTest ")); Serial.print(currentTest+1);
      Serial.println(F(" completed → 60-second pause then next test"));
      inPause = true;
      pauseStart = millis();
    }
  }
}