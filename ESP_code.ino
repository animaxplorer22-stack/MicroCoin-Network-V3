/*
  MICROCORE (MCX) ESP32/ESP8266 MINER v5.0
  Full miner with ECDSA secp256k1 | Gossip Discovery | No DNS
  Hardware: ESP32 or ESP8266
  Features:
  - Real ECDSA secp256k1 signatures (mbedtls)
  - Gossip discovery with peer caching (SPIFFS)
  - EEPROM storage for stake/rewards
  - Uptime tracking with daily reset
  - Slashing handling
  - Remote control (start/stop/restart)
  - Auto reconnect with failover
  - LED status indication
*/

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <SPIFFS.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>

// ==================== USER CONFIGURATION ====================
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASSWORD = "your_wifi_password";

// HARDCODED BOOTNODES (NO DNS REQUIRED)
const char* BOOTSTRAP_NODES[] = {"YOUR_SERVER_IP:8080"};
const int BOOTSTRAP_COUNT = 1;

const int NODE_PORT = 8080;

// YOUR MINER IDENTITY (get from web wallet)
const char* USERNAME = "your_username";
const char* PRIVATE_KEY_HEX = "your_64_char_private_key_hex_here";

uint32_t INITIAL_STAKE = 100;

// ==================== CONSTANTS ====================
#define SYMBOL "MCX"
#define LEVEL_STAKE_RANGE 100
#define SIGNING_WINDOW_MS 2500
#define SLASH_RATE 0.10
#define UPTIME_PING_INTERVAL 30000
#define MAX_RECONNECT_ATTEMPTS 5
#define MAX_PEERS 20
#define VERSION "5.0"

// EEPROM addresses
#define EEPROM_STAKE_ADDR 0
#define EEPROM_REWARDS_ADDR 4
#define EEPROM_BLOCKS_ADDR 8
#define EEPROM_UPTIME_ADDR 12
#define EEPROM_TODAY_UPTIME_ADDR 16
#define EEPROM_LAST_RESET_ADDR 20
#define EEPROM_SLASH_COUNT_ADDR 24
#define EEPROM_CONSECUTIVE_MISSES_ADDR 28
#define EEPROM_CHECKSUM_ADDR 32
#define EEPROM_MAGIC_ADDR 36

#define MAGIC_NUMBER 0x5A5A5A5A

// ==================== CRYPTOGRAPHY CONTEXT ====================
mbedtls_ecdsa_context ecdsa;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
mbedtls_sha256_context sha256_ctx;

// ==================== GLOBAL VARIABLES ====================
WebSocketsClient webSocket;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

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
uint32_t reconnectAttempts;
uint32_t currentPeerIndex;
uint32_t nodeSwitchCount;

char validatorID[65];
char publicKeyHex[130];
char walletAddress[70];
char currentChallenge[65];
char currentNodeIP[16];
bool isValidator = false;
bool isRegistered = false;
bool wsConnected = false;
bool miningEnabled = true;

// Peer cache for gossip discovery
String peerList[MAX_PEERS];
int peerCount = 0;

// LED pin (built-in LED on most boards)
#ifdef ESP32
  #define LED_PIN 2
#else
  #define LED_PIN 2  // ESP8266 built-in LED
#endif

// ==================== LED FUNCTIONS ====================
void led_on() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void led_off() {
  digitalWrite(LED_PIN, HIGH);
}

void led_blink(int times, int duration) {
  for (int i = 0; i < times; i++) {
    led_on();
    delay(duration);
    led_off();
    delay(duration);
  }
}

// ==================== CRYPTO UTILITIES ====================
void hexToBytes(const char* hex, unsigned char* bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sscanf(hex + 2 * i, "%02hhx", &bytes[i]);
    }
}

void bytesToHex(const unsigned char* bytes, size_t len, char* hex) {
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + 2 * i, "%02x", bytes[i]);
    }
    hex[2 * len] = '\0';
}

void computeSHA256(const char* input, char* output) {
    unsigned char hash[32];
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)input, strlen(input));
    mbedtls_sha256_finish(&sha256_ctx, hash);
    bytesToHex(hash, 32, output);
}

void initCrypto() {
    mbedtls_ecdsa_init(&ecdsa);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    const char* personalization = "microcore_esp_miner_v5";
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char*)personalization, strlen(personalization));
    
    unsigned char privateKeyBytes[32];
    hexToBytes(PRIVATE_KEY_HEX, privateKeyBytes, 32);
    
    mbedtls_ecp_group_id grp_id = MBEDTLS_ECP_DP_SECP256K1;
    mbedtls_ecp_keypair keypair;
    mbedtls_ecp_keypair_init(&keypair);
    mbedtls_ecp_group_load(&keypair.grp, grp_id);
    mbedtls_mpi_read_binary(&keypair.d, privateKeyBytes, 32);
    mbedtls_ecp_mul(&keypair.grp, &keypair.Q, &keypair.d, &keypair.grp.G, NULL, NULL);
    mbedtls_ecdsa_from_keypair(&ecdsa, &keypair);
    
    unsigned char publicKeyBytes[65];
    size_t publicKeyLen = 65;
    mbedtls_ecp_point_write_binary(&keypair.grp, &keypair.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                   &publicKeyLen, publicKeyBytes, sizeof(publicKeyBytes));
    bytesToHex(publicKeyBytes, publicKeyLen, publicKeyHex);
    
    char pubHash[65];
    computeSHA256(publicKeyHex, pubHash);
    snprintf(walletAddress, sizeof(walletAddress), "MCR_%.32s", pubHash);
    
    char combined[200];
    snprintf(combined, sizeof(combined), "%s%s", USERNAME, publicKeyHex);
    computeSHA256(combined, validatorID);
    
    Serial.println("[CRYPTO] ECDSA secp256k1 initialized");
    Serial.printf("[CRYPTO] Username: %s\n", USERNAME);
    Serial.printf("[CRYPTO] Wallet: %s\n", walletAddress);
}

bool signMessage(const char* message, char* signatureOut) {
    unsigned char hash[32];
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)message, strlen(message));
    mbedtls_sha256_finish(&sha256_ctx, hash);
    
    unsigned char signature[64];
    size_t sigLen;
    
    int ret = mbedtls_ecdsa_sign(&ecdsa, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                                  signature, &sigLen, mbedtls_ctr_drbg_random, &ctr_drbg);
    
    if (ret != 0) {
        Serial.printf("[CRYPTO] Sign failed: %d\n", ret);
        return false;
    }
    
    bytesToHex(signature, sigLen, signatureOut);
    return true;
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
    EEPROM.put(EEPROM_SLASH_COUNT_ADDR, slashCount);
    EEPROM.put(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
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
        lastUptimeReset = millis() / 1000;
        slashCount = 0;
        consecutiveMisses = 0;
        currentPeerIndex = 0;
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
    EEPROM.get(EEPROM_SLASH_COUNT_ADDR, slashCount);
    EEPROM.get(EEPROM_CONSECUTIVE_MISSES_ADDR, consecutiveMisses);
    
    calculateLevel();
    Serial.printf("[EEPROM] Loaded - Stake: %lu %s, Level: %d, Blocks: %lu\n", 
                  currentStake, SYMBOL, currentLevel, totalBlocksSigned);
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
    
    Serial.printf("[SLASH] Lost %lu %s | Stake: %lu | Level: %d | Slashes: %lu\n",
                  slashAmount, SYMBOL, currentStake, currentLevel, slashCount);
    
    if (slashCount >= 5) {
        Serial.println("[BAN] Too many slashes! Miner will be banned.");
        miningEnabled = false;
    }
    led_blink(3, 100);
}

void addReward(uint32_t rewardAmount) {
    totalRewards += rewardAmount;
    currentStake += rewardAmount;
    totalBlocksSigned++;
    consecutiveMisses = 0;
    calculateLevel();
    saveToEEPROM();
    
    Serial.printf("[REWARD] +%lu %s | Total: %lu | Stake: %lu | Level: %d | Blocks: %lu\n",
                  rewardAmount, SYMBOL, totalRewards, currentStake, currentLevel, totalBlocksSigned);
    led_blink(1, 50);
}

// ==================== PEER CACHE (GOSSIP DISCOVERY) ====================
void savePeersToSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Mount failed");
        return;
    }
    File f = SPIFFS.open("/peers.json", "w");
    if (f) {
        f.print("{\"peers\":[");
        for (int i = 0; i < peerCount; i++) {
            if (i > 0) f.print(",");
            f.print("\""); f.print(peerList[i]); f.print("\"");
        }
        f.print("],\"version\":\"");
        f.print(VERSION);
        f.print("\"}");
        f.close();
        Serial.printf("[CACHE] Saved %d peers to SPIFFS\n", peerCount);
    }
    SPIFFS.end();
}

void loadPeersFromSPIFFS() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Mount failed, using bootstraps only");
        return;
    }
    if (SPIFFS.exists("/peers.json")) {
        File f = SPIFFS.open("/peers.json", "r");
        if (f) {
            String content = f.readString();
            f.close();
            
            StaticJsonDocument<2048> doc;
            DeserializationError error = deserializeJson(doc, content);
            if (!error) {
                JsonArray peers = doc["peers"];
                for (JsonVariant p : peers) {
                    if (peerCount < MAX_PEERS) {
                        peerList[peerCount] = p.as<String>();
                        peerCount++;
                    }
                }
                Serial.printf("[CACHE] Loaded %d peers from SPIFFS\n", peerCount);
            }
        }
    }
    SPIFFS.end();
    
    // Add bootstraps
    for (int i = 0; i < BOOTSTRAP_COUNT && peerCount < MAX_PEERS; i++) {
        bool exists = false;
        for (int j = 0; j < peerCount; j++) {
            if (peerList[j] == BOOTSTRAP_NODES[i]) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            peerList[peerCount] = BOOTSTRAP_NODES[i];
            peerCount++;
        }
    }
}

void addPeerFromGossip(String peer) {
    for (int i = 0; i < peerCount; i++) {
        if (peerList[i] == peer) return;
    }
    if (peerCount < MAX_PEERS) {
        peerList[peerCount] = peer;
        peerCount++;
        savePeersToSPIFFS();
        Serial.printf("[GOSSIP] Discovered new peer: %s\n", peer.c_str());
    }
}

void switchToNextPeer() {
    currentPeerIndex = (currentPeerIndex + 1) % peerCount;
    nodeSwitchCount++;
    
    String fullPeer = peerList[currentPeerIndex];
    int colonIndex = fullPeer.indexOf(':');
    if (colonIndex > 0) {
        fullPeer = fullPeer.substring(0, colonIndex);
    }
    fullPeer.toCharArray(currentNodeIP, 16);
    
    Serial.printf("[FAILOVER] Switching to peer: %s (switch #%lu)\n", currentNodeIP, nodeSwitchCount);
    
    if (webSocket.isConnected()) {
        webSocket.disconnect();
    }
    wsConnected = false;
    isRegistered = false;
    webSocket.begin(currentNodeIP, NODE_PORT, "/");
}