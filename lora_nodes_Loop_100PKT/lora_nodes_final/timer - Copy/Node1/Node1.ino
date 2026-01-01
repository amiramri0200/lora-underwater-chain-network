#include <SPI.h>
#include <SD.h>
#include <LoRa.h>

// ════════════════════════════════════════════════════════════════
//                     PACKET DROP SIMULATION
// ════════════════════════════════════════════════════════════════
// Set probability from 0 to 100 (e.g., 20 = 20% chance to drop)
const int DROP_PROBABILITY = 20;  
// Set to true to enable drop simulation, false for normal operation
const bool SIMULATE_DROPS = false;

// ════════════════════════════════════════════════════════════════
//                     DEBUG / REAL TEST SWITCH
// ════════════════════════════════════════════════════════════════
//#define QUICK_DEBUG
#ifdef QUICK_DEBUG
  const int    PACKETS_PER_TEST       = 12;
  const unsigned long TX_INTERVAL     = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 60000UL;
  const unsigned long INITIAL_START_DELAY = 10000UL;
#else
  const int    PACKETS_PER_TEST       = 100;
  const unsigned long TX_INTERVAL     = 15000UL;
  const unsigned long PAUSE_BETWEEN_TESTS = 600000UL;
  const unsigned long INITIAL_START_DELAY = 13000UL;
#endif

// Calculate and display TEST_DURATION for reference
const unsigned long TEST_DURATION = (unsigned long)PACKETS_PER_TEST * TX_INTERVAL + PAUSE_BETWEEN_TESTS;
// ════════════════════════════════════════════════════════════════

#define SDCARD_SS_PIN 4
const int MY_ID = 1;

const char* testNames[4] = {"test1_SF7_CR5", "test2_SF7_CR8", "test3_SF12_CR5", "test4_SF12_CR8"};
const uint8_t SF[4] = {7, 7, 12, 12};
const uint8_t CR[4] = {5, 8, 5, 8};

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

void resetSDCard() {
  if (!sdOK) {
    Serial.println(F("SD not available - skip reset"));
    return;
  }
  
  const char* filename = "send1.txt";
  
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
  }
  
  File f = SD.open(filename, FILE_WRITE);
  if (f) {
    f.println("=== NEW TEST SESSION ===");
    f.print("Test Duration per config: ");
    f.print(TEST_DURATION / 1000);
    f.println(" seconds");
    if (SIMULATE_DROPS) {
        f.print("SIMULATION MODE: "); 
        f.print(DROP_PROBABILITY); 
        f.println("% Packet Loss");
    }
    f.println("========================================");
    f.close();
    Serial.println(F("✓ Created fresh log file"));
  } else {
    sdOK = false;
  }
}

void setConfig(int idx) {
  LoRa.setSpreadingFactor(SF[idx]);
  LoRa.setCodingRate4(CR[idx]);
  Serial.print(F("\n=== STARTING ")); Serial.println(testNames[idx]);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  unsigned long serialTimeout = millis();
  while (!Serial && millis() - serialTimeout < 3000) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    delay(100);
  }

  // Initialize Random Seed for Drops
  randomSeed(analogRead(A0));
  
  delay(500);  Serial.println(F("\n=== Node 1 - Sender (Timer-Based) ==="));
  
  if (SIMULATE_DROPS) {
    Serial.print(F("⚠ DROP SIMULATION ENABLED: "));
    Serial.print(DROP_PROBABILITY);
    Serial.println(F("% Chance to skip TX"));
  }

  Serial.print(F("TEST_DURATION: ")); 
  Serial.print(TEST_DURATION / 1000); 
  Serial.println(F(" seconds per config"));

  for (int i = 0; i < 5 && !sdOK; i++) {
    sdOK = SD.begin(SDCARD_SS_PIN);
    delay(200);
  }
  resetSDCard();

  if (!LoRa.begin(868E6)) { Serial.println("LoRa fail"); while (1); }
  LoRa.setTxPower(14);
  LoRa.enableCrc();
  LoRa.setSignalBandwidth(125E3);
  LoRa.setPreambleLength(12);
  LoRa.setSyncWord(0x34);
  setConfig(0);

  Serial.print(F("Waiting ")); Serial.print(INITIAL_START_DELAY/1000); Serial.println(F(" sec for sync..."));
  delay(INITIAL_START_DELAY);

  Serial.println(F("STARTING TRANSMISSIONS"));
  lastTx = millis();
}

void loop() {
  if (inPause) {
    if (millis() - pauseStart >= PAUSE_BETWEEN_TESTS) {
      inPause = false;
      currentTest = (currentTest + 1) % 4;  // Cycle 0→1→2→3→0
      packetCount = 0;
      setConfig(currentTest);
      
      if (currentTest == 0) {
        Serial.println(F("\n>>> ALL 4 TESTS DONE! REPEATING FROM TEST 1 <<<"));
      }
    }
    return;
  }

  if (millis() - lastTx >= TX_INTERVAL) {
    lastTx = millis();
    packetCount++; // Increment count even if we drop, to simulate sequence gap

    char msg[160];
    bool isLastPacket = (packetCount >= PACKETS_PER_TEST);
    const char* nextTestName = testNames[(currentTest + 1) % 4];

    // Build the message string
    if (isLastPacket) {
      snprintf(msg, sizeof(msg), "%s|ID:%d|num:%03d|LAST|NEXT:%s", 
               testNames[currentTest], MY_ID, packetCount, nextTestName);
    } else {
      snprintf(msg, sizeof(msg), "%s|ID:%d|num:%03d", 
               testNames[currentTest], MY_ID, packetCount);
    }

    // -----------------------------------------------------------
    // DROP SIMULATION LOGIC
    // -----------------------------------------------------------
    bool dropThisPacket = false;
    if (SIMULATE_DROPS) {
        if (random(100) < DROP_PROBABILITY) {
            dropThisPacket = true;
        }
    }

    if (dropThisPacket) {
        // Log that we skipped sending
        Serial.print(F("❌ SIMULATED DROP (Not Sending): ")); Serial.println(msg);
        
        char logBuf[200];
        snprintf(logBuf, sizeof(logBuf), "%s [DROPPED]", msg);
        log(logBuf);
        
        // Do NOT call LoRa.beginPacket...
        digitalWrite(LED_BUILTIN, LOW); // LED Off to indicate no TX
    } 
    else {
        // Normal Transmission
        LoRa.beginPacket();
        LoRa.print(msg);
        LoRa.endPacket();
        LoRa.receive(); // Put back in receive mode (though not used here, good practice)

        Serial.print(F("Sent: ")); Serial.println(msg);
        log(msg);
        digitalWrite(LED_BUILTIN, HIGH); delay(50); digitalWrite(LED_BUILTIN, LOW);
    }

    // -----------------------------------------------------------
    // CHECK END OF TEST
    // -----------------------------------------------------------
    if (isLastPacket) {
      Serial.print(F("\nTest ")); Serial.print(currentTest+1);
      Serial.println(F(" completed → pause then next test"));
      inPause = true;
      pauseStart = millis();
    }
  }
}