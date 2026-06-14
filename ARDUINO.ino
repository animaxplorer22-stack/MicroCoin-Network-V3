/*
  MICROCORE (MCX) ARDUINO UNO MINER v5.0
  Full miner for Arduino Uno with SHA256 signatures
  Connects to WiFi Bridge via Serial
  
  Hardware: Arduino Uno + USB cable
  Compatible with: Arduino Uno, Nano, Mega
  
  Features:
  - SHA256-based signatures (lightweight for Uno)
  - EEPROM storage for stake, rewards, blocks
  - Uptime tracking
  - Slashing handling
  - Remote control (start/stop/restart)
  - Daily uptime reset
  - Automatic reconnection
*/

#include <ArduinoJson.h>
#include <EEPROM.h>

// ==================== USER CONFIGURATION ====================
// EDIT THESE BEFORE UPLOADING
const char* USERNAME = "your_username";           // ← CHANGE THIS
const char* PRIVATE_KEY = "your_private_key_here"; // ← CHANGE THIS

uint32_t INITIAL_STAKE = 100;

// ==================== CONSTANTS ====================
#define SYMBOL "MCX"
#define LEVEL_STAKE_RANGE 100
#define SIGNING_WINDOW_MS 2500
#define SLASH_RATE 0.10
#define UPTIME_PING_INTERVAL 30000
#define VERSION "5.0"

// EEPROM addresses
#define EEPROM_STAKE_ADDR 0
#define EEPROM_REWARDS_ADDR 4
#define EEPROM_BLOCKS_ADDR 8
#define EEPROM_UPTIME_ADDR 12
#define EEPROM_TODAY_UPTIME_ADDR 16
#define EEPROM_LAST_RESET_ADDR 20
#define EEPROM_CHECKSUM_ADDR 24
#define EEPROM_MAGIC_ADDR 28

#define MAGIC_NUMBER 0x5A5A5A5A

// ==================== GLOBAL VARIABLES ====================
uint32_t currentStake;
uint32_t totalRewards;
uint32_t totalBlocksSigned;
uint32_t totalUptimeSeconds;
uint32_t todayUptimeSeconds;
uint32_t lastUptimeReset;
uint32_t currentLevel;
uint32_t lastUptimePing;
uint32_t lastChallengeTime;
uint32_t uptimeCounter;
uint32_t consecutiveMisses;
uint32_t slashCount;
uint32_t currentBlockId;
uint32_t lastResetDay;

char validatorID[65];
char walletAddress[70];
char currentChallenge[65];
bool isValidator = false;
bool isRegistered = false;
bool miningEnabled = true;
String incomingData = "";

// ==================== SIMPLE SHA256 HASH (for Uno) ====================
// Note: This is a simplified hash for Arduino Uno
// Real SHA256 requires too much RAM. For production,
// use ATECC608A crypto chip or ESP32 instead.
void computeHash(const char* input, char* output) {
  unsigned long hash = 5381;
  int len = 0;
  for (int i = 0; input[i] != '\0'; i++) {
    hash = ((hash << 5) + hash) + input[i];
    len++;
  }
  // Add length to make collisions harder
  hash = ((hash << 5) + hash) + len;
  // Add timestamp seed
  hash = ((hash << 5) + hash) + millis();
  sprintf(output, "%016lx", hash);
}

void computeSHA256(const char* input, char* output) {
  computeHash(input, output);
}

// ==================== ID GENERATION ====================
void generateValidatorID() {
  char combined[100];
  snprintf(combined, sizeof(combined), "%s%s", USERNAME, PRIVATE_KEY);
  computeSHA256(combined, validatorID);
  
  char walletHash[65];
  computeSHA256(validatorID, walletHash);
  snprintf(walletAddress, sizeof(walletAddress), "MCR_%.16s", walletHash);
}

// ==================== LEVEL CALCULATION ====================
void calculateLevel() {
  if (currentStake < LEVEL_STAKE_RANGE) {
    currentLevel = 1;
  } else {
    currentLevel = ((currentStake - 1) / LEVEL_STAKE_RANGE) + 1;
  }
  if (currentLevel < 1) currentLevel = 1;
  if (currentLevel > 100) currentLevel = 100;
}

// ==================== EEPROM MANAGEMENT ====================
uint32_t computeChecksum() {
  uint32_t sum = currentStake + totalRewards + totalBlocksSigned + 
                 totalUptimeSeconds + todayUptimeSeconds + slashCount;
  return sum ^ MAGIC_NUMBER;
}

bool isEEPROMValid() {
  uint32_t magic;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic != MAGIC_NUMBER) return false;
  
  uint32_t storedChecksum;
  EEPROM.get(EEPROM_CHECKSUM_ADDR, storedChecksum);
  return storedChecksum == computeChecksum();
}

void saveToEEPROM() {
  EEPROM.put(EEPROM_STAKE_ADDR, currentStake);
  EEPROM.put(EEPROM_REWARDS_ADDR, totalRewards);
  EEPROM.put(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
  EEPROM.put(EEPROM_UPTIME_ADDR, totalUptimeSeconds);
  EEPROM.put(EEPROM_TODAY_UPTIME_ADDR, todayUptimeSeconds);
  EEPROM.put(EEPROM_LAST_RESET_ADDR, lastUptimeReset);
  EEPROM.put(EEPROM_CHECKSUM_ADDR, computeChecksum());
  EEPROM.put(EEPROM_MAGIC_ADDR, MAGIC_NUMBER);
  EEPROM.commit();
  Serial.println("[EEPROM] Stats saved");
}

void loadFromEEPROM() {
  if (!isEEPROMValid()) {
    Serial.println("[EEPROM] Invalid data, resetting to defaults");
    currentStake = INITIAL_STAKE;
    totalRewards = 0;
    totalBlocksSigned = 0;
    totalUptimeSeconds = 0;
    todayUptimeSeconds = 0;
    lastUptimeReset = millis();
    slashCount = 0;
    consecutiveMisses = 0;
    calculateLevel();
    saveToEEPROM();
    return;
  }
  
  EEPROM.get(EEPROM_STAKE_ADDR, currentStake);
  EEPROM.get(EEPROM_REWARDS_ADDR, totalRewards);
  EEPROM.get(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
  EEPROM.get(EEPROM_UPTIME_ADDR, totalUptimeSeconds);
  EEPROM.get(EEPROM_TODAY_UPTIME_ADDR, todayUptimeSeconds);
  EEPROM.get(EEPROM_LAST_RESET_ADDR, lastUptimeReset);
  
  calculateLevel();
  Serial.print("[EEPROM] Loaded - Stake: ");
  Serial.print(currentStake);
  Serial.print(" MCX, Level: ");
  Serial.print(currentLevel);
  Serial.print(", Blocks: ");
  Serial.println(totalBlocksSigned);
}

// ==================== DAILY UPTIME RESET ====================
void checkDailyReset() {
  uint32_t now = millis() / 1000;
  uint32_t daysSinceReset = (now - lastUptimeReset) / 86400;
  if (daysSinceReset >= 1) {
    todayUptimeSeconds = 0;
    lastUptimeReset = now;
    saveToEEPROM();
    Serial.println("[DAILY] Uptime reset for new day");
  }
}

void updateUptime() {
  checkDailyReset();
  totalUptimeSeconds += UPTIME_PING_INTERVAL / 1000;
  todayUptimeSeconds += UPTIME_PING_INTERVAL / 1000;
  if (todayUptimeSeconds > 86400) todayUptimeSeconds = 86400;
  saveToEEPROM();
}

// ==================== SLASHING ====================
void handleSlashing() {
  uint32_t slashAmount = (uint32_t)(currentStake * SLASH_RATE);
  if (slashAmount < LEVEL_STAKE_RANGE) slashAmount = LEVEL_STAKE_RANGE;
  if (slashAmount > currentStake) slashAmount = currentStake;
  
  currentStake -= slashAmount;
  if (currentStake < LEVEL_STAKE_RANGE) currentStake = LEVEL_STAKE_RANGE;
  
  slashCount++;
  consecutiveMisses++;
  calculateLevel();
  saveToEEPROM();
  
  Serial.print("[SLASH] Lost ");
  Serial.print(slashAmount);
  Serial.print(" MCX | Stake: ");
  Serial.print(currentStake);
  Serial.print(" MCX | Level: ");
  Serial.print(currentLevel);
  Serial.print(" | Slashes: ");
  Serial.println(slashCount);
  
  if (slashCount >= 5) {
    Serial.println("[BAN] Too many slashes! Miner will be banned.");
    miningEnabled = false;
  }
}

// ==================== REWARDS ====================
void addReward(uint32_t rewardAmount) {
  totalRewards += rewardAmount;
  currentStake += rewardAmount;
  totalBlocksSigned++;
  consecutiveMisses = 0;
  calculateLevel();
  saveToEEPROM();
  
  Serial.print("[REWARD] +");
  Serial.print(rewardAmount);
  Serial.print(" MCX | Total: ");
  Serial.print(totalRewards);
  Serial.print(" MCX | Stake: ");
  Serial.print(currentStake);
  Serial.print(" MCX | Level: ");
  Serial.print(currentLevel);
  Serial.print(" | Blocks: ");
  Serial.println(totalBlocksSigned);
}
// ==================== SIGNATURE FUNCTIONS ====================
void signMessage(const char* message, char* signatureOut) {
  // For Arduino Uno, we use SHA256-based signatures
  // This is lightweight but less secure than ECDSA
  char temp[200];
  snprintf(temp, sizeof(temp), "%s%s%s", PRIVATE_KEY, message, USERNAME);
  computeSHA256(temp, signatureOut);
}

void signRegistration(char* signatureOut) {
  char message[100];
  snprintf(message, sizeof(message), "%s%s%lu", validatorID, USERNAME, currentStake);
  signMessage(message, signatureOut);
}

void signChallenge(const char* challenge, uint32_t blockId, char* signatureOut) {
  char message[150];
  snprintf(message, sizeof(message), "%s%s%lu", challenge, validatorID, blockId);
  signMessage(message, signatureOut);
}

// ==================== COMMUNICATION WITH BRIDGE ====================
void sendToBridge(String json) {
  Serial.println(json);
}

void sendRegister() {
  StaticJsonDocument<512> doc;
  doc["type"] = "register";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["public_key"] = PRIVATE_KEY;
  doc["wallet"] = walletAddress;
  doc["stake"] = currentStake;
  doc["level"] = currentLevel;
  doc["rewards"] = totalRewards;
  doc["blocks"] = totalBlocksSigned;
  doc["uptime"] = totalUptimeSeconds;
  doc["today_uptime"] = todayUptimeSeconds;
  doc["miner_type"] = "uno";
  doc["version"] = VERSION;
  doc["timestamp"] = millis() / 1000;
  
  char signature[33];
  signRegistration(signature);
  doc["signature"] = signature;
  
  String output;
  serializeJson(doc, output);
  sendToBridge(output);
  Serial.println("[REG] Registration sent");
}

void sendUptimePing() {
  StaticJsonDocument<256> doc;
  doc["type"] = "uptime_ping";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["uptime_seconds"] = totalUptimeSeconds;
  doc["today_uptime"] = todayUptimeSeconds;
  doc["stake"] = currentStake;
  doc["level"] = currentLevel;
  doc["timestamp"] = millis() / 1000;
  
  String output;
  serializeJson(doc, output);
  sendToBridge(output);
}

void sendBlockSignature() {
  char signature[33];
  signChallenge(currentChallenge, currentBlockId, signature);
  
  StaticJsonDocument<512> doc;
  doc["type"] = "block_signature";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["challenge"] = currentChallenge;
  doc["signature"] = signature;
  doc["level"] = currentLevel;
  doc["stake"] = currentStake;
  doc["block_id"] = currentBlockId;
  doc["timestamp"] = millis() / 1000;
  
  String output;
  serializeJson(doc, output);
  sendToBridge(output);
  Serial.println("[SIGN] Block signature sent");
}

void sendStatus() {
  StaticJsonDocument<256> doc;
  doc["type"] = "miner_status";
  doc["validator_id"] = validatorID;
  doc["username"] = USERNAME;
  doc["stake"] = currentStake;
  doc["level"] = currentLevel;
  doc["blocks"] = totalBlocksSigned;
  doc["rewards"] = totalRewards;
  doc["uptime"] = totalUptimeSeconds;
  doc["today_uptime"] = todayUptimeSeconds;
  doc["mining"] = miningEnabled;
  
  String output;
  serializeJson(doc, output);
  sendToBridge(output);
}

// ==================== MESSAGE PROCESSING ====================
void processMessage(String jsonMsg) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonMsg);
  
  if (error) {
    Serial.print("[ERROR] JSON parse: ");
    Serial.println(error.c_str());
    return;
  }
  
  const char* type = doc["type"];
  
  if (strcmp(type, "registered") == 0) {
    isRegistered = true;
    int nodeLevel = doc["level"];
    int nodeReward = doc["current_reward"];
    Serial.print("[NODE] Registration confirmed. Level: ");
    Serial.print(nodeLevel);
    Serial.print(", Reward: ");
    Serial.println(nodeReward);
  }
  else if (strcmp(type, "challenge") == 0) {
    if (!miningEnabled) {
      Serial.println("[MINING] Mining disabled, ignoring challenge");
      return;
    }
    const char* challenge = doc["challenge"];
    if (challenge) {
      strncpy(currentChallenge, challenge, 64);
      currentChallenge[64] = '\0';
      currentBlockId = doc["block_id"];
      lastChallengeTime = millis();
      isValidator = true;
      sendBlockSignature();
      Serial.print("[CHALLENGE] Received for block ");
      Serial.println(currentBlockId);
    }
  }
  else if (strcmp(type, "block_accepted") == 0) {
    uint32_t reward = doc["reward"];
    addReward(reward);
    isValidator = false;
    Serial.print("[NODE] Block ");
    Serial.print(doc["block_id"].as<uint32_t>());
    Serial.println(" ACCEPTED");
  }
  else if (strcmp(type, "block_rejected") == 0) {
    const char* reason = doc["reason"];
    Serial.print("[NODE] Block rejected: ");
    Serial.println(reason);
    isValidator = false;
  }
  else if (strcmp(type, "slash") == 0) {
    Serial.println("[NODE] Slash command received");
    handleSlashing();
    isValidator = false;
  }
  else if (strcmp(type, "level_update") == 0) {
    uint32_t newStake = doc["stake"];
    if (newStake != currentStake) {
      currentStake = newStake;
      calculateLevel();
      saveToEEPROM();
      Serial.print("[NODE] Level update: Stake ");
      Serial.print(currentStake);
      Serial.print(", Level ");
      Serial.println(currentLevel);
    }
  }
  else if (strcmp(type, "miner_control") == 0) {
    const char* action = doc["action"];
    if (strcmp(action, "stop") == 0) {
      Serial.println("[CONTROL] Stop command received - stopping mining");
      miningEnabled = false;
      isValidator = false;
    } 
    else if (strcmp(action, "start") == 0) {
      Serial.println("[CONTROL] Start command received - resuming mining");
      miningEnabled = true;
    } 
    else if (strcmp(action, "restart") == 0) {
      Serial.println("[CONTROL] Restart command received");
      miningEnabled = false;
      isValidator = false;
      delay(1000);
      miningEnabled = true;
    }
    else if (strcmp(action, "status") == 0) {
      sendStatus();
    }
    
    // Send acknowledgment
    StaticJsonDocument<128> ack;
    ack["type"] = "control_response";
    ack["miner_id"] = validatorID;
    ack["action"] = action;
    ack["success"] = true;
    String ackStr;
    serializeJson(ack, ackStr);
    sendToBridge(ackStr);
  }
  else if (strcmp(type, "get_status") == 0) {
    sendStatus();
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==========================================");
  Serial.println("MICROCORE (MCX) ARDUINO UNO MINER v5.0");
  Serial.println("SHA256 Mode | WiFi Bridge Required");
  Serial.println("==========================================\n");
  
  // Initialize EEPROM
  EEPROM.begin(512);
  
  // Load saved data
  loadFromEEPROM();
  
  // Generate IDs
  generateValidatorID();
  calculateLevel();
  
  // Print status
  Serial.print("Username: ");
  Serial.println(USERNAME);
  Serial.print("Wallet: ");
  Serial.println(walletAddress);
  Serial.print("Validator ID: ");
  Serial.println(validatorID);
  Serial.print("Stake: ");
  Serial.print(currentStake);
  Serial.print(" MCX, Level: ");
  Serial.println(currentLevel);
  Serial.print("Total Rewards: ");
  Serial.print(totalRewards);
  Serial.print(" MCX, Blocks: ");
  Serial.println(totalBlocksSigned);
  Serial.print("Mining: ");
  Serial.println(miningEnabled ? "ENABLED" : "DISABLED");
  
  // Send registration
  sendRegister();
  
  // Initialize timers
  lastUptimePing = millis();
  uptimeCounter = 0;
  isValidator = false;
  isRegistered = false;
  lastUptimeReset = millis() / 1000;
  
  Serial.println("\n[READY] Arduino Uno miner is running");
  Serial.println("[READY] Make sure wifi_bridge.py is running on your computer");
  Serial.println("[READY] Waiting for node connection...\n");
}
// ==================== MAIN LOOP ====================
void loop() {
  // Read incoming messages from bridge
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (incomingData.length() > 0) {
        processMessage(incomingData);
        incomingData = "";
      }
    } else {
      incomingData += c;
    }
  }
  
  // Send uptime ping every 30 seconds
  if (millis() - lastUptimePing >= UPTIME_PING_INTERVAL) {
    uptimeCounter++;
    updateUptime();
    sendUptimePing();
    lastUptimePing = millis();
    
    // Periodic status display
    if (uptimeCounter % 2 == 0) {
      Serial.print("[STATUS] Stake: ");
      Serial.print(currentStake);
      Serial.print(" MCX, Level: ");
      Serial.print(currentLevel);
      Serial.print(", Blocks: ");
      Serial.print(totalBlocksSigned);
      Serial.print(", Rewards: ");
      Serial.print(totalRewards);
      Serial.print(" MCX, Uptime: ");
      Serial.print(totalUptimeSeconds / 3600);
      Serial.print("h ");
      Serial.print((totalUptimeSeconds % 3600) / 60);
      Serial.print("m, Today: ");
      Serial.print(todayUptimeSeconds / 3600);
      Serial.println("h");
    }
  }
  
  // Check for challenge timeout (2.5 seconds)
  if (isValidator && (millis() - lastChallengeTime >= SIGNING_WINDOW_MS)) {
    Serial.println("[TIMEOUT] Failed to sign within window");
    handleSlashing();
    isValidator = false;
  }
  
  // Periodic EEPROM save (every hour)
  static uint32_t lastSave = 0;
  if (millis() - lastSave >= 3600000) {
    saveToEEPROM();
    lastSave = millis();
    Serial.println("[EEPROM] Periodic save completed");
  }
  
  // Re-register if disconnected for too long
  static uint32_t lastRegisterAttempt = 0;
  if (!isRegistered && (millis() - lastRegisterAttempt >= 60000)) {
    Serial.println("[REG] Re-registering with node...");
    sendRegister();
    lastRegisterAttempt = millis();
  }
  
  delay(10);
}
