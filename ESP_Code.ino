/*
  MICROCORE (MCX) ESP32/ESP8266 MINER v5.0 - WITH GOSSIP DISCOVERY
  Hardware: ESP32 or ESP8266
  Real ECDSA secp256k1 | Peer caching | No DNS required

  Instructions:
  1. Install ESP32/ESP8266 board support in Arduino IDE
  2. Install libraries: WebSockets, ArduinoJson, mbedtls
  3. Edit WIFI_SSID, WIFI_PASSWORD, USERNAME, PRIVATE_KEY_HEX below
  4. Upload to ESP32/ESP8266
*/

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <SPIFFS.h>

// ==================== USER CONFIGURATION ====================
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASSWORD = "your_wifi_password";

// GOSSIP DISCOVERY - only ONE bootnode needed (miners discover others via gossip)
const char* BOOTSTRAP_NODES[] = {"200.36.138.198:8080"};
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

// EEPROM addresses
#define EEPROM_STAKE_ADDR 0
#define EEPROM_REWARDS_ADDR 4
#define EEPROM_BLOCKS_ADDR 8
#define EEPROM_UPTIME_ADDR 12
#define EEPROM_CURRENT_NODE_ADDR 16
#define EEPROM_CHECKSUM_ADDR 20
#define EEPROM_PEER_COUNT_ADDR 24

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
uint32_t uptimeSeconds;
uint32_t currentLevel;
uint32_t lastUptimePing;
uint32_t lastChallengeTime;
uint32_t uptimeCounter;
uint32_t consecutiveMisses;
uint32_t slashCount;
uint32_t currentBlockId;
uint32_t reconnectAttempts;
uint32_t currentNodeIndex;

char validatorID[65];
char publicKeyHex[130];
char walletAddress[70];
char currentChallenge[65];
char currentNodeIP[16];
bool isValidator = false;
bool isRegistered = false;
bool wsConnected = false;

// Peer cache for gossip discovery
String peerList[MAX_PEERS];
int peerCount = 0;
int currentPeerIndex = 0;

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
        f.print("]}");
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
            
            StaticJsonDocument<1024> doc;
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
    String newIP = peerList[currentPeerIndex];
    // Extract IP from "IP:PORT" format
    int colonIndex = newIP.indexOf(':');
    if (colonIndex > 0) {
        newIP = newIP.substring(0, colonIndex);
    }
    newIP.toCharArray(currentNodeIP, 16);
    Serial.printf("[FAILOVER] Switching to peer: %s\n", currentNodeIP);
    if (webSocket.isConnected()) {
        webSocket.disconnect();
    }
    wsConnected = false;
    isRegistered = false;
    webSocket.begin(currentNodeIP, NODE_PORT, "/");
}

// ==================== STAKING & LEVELS ====================
void calculateLevel() {
    currentLevel = ((currentStake - 1) / LEVEL_STAKE_RANGE) + 1;
    if (currentLevel < 1) currentLevel = 1;
    if (currentLevel > 100) currentLevel = 100;
}

uint32_t computeChecksum() {
    uint32_t sum = currentStake + totalRewards + totalBlocksSigned + uptimeSeconds + slashCount;
    return sum ^ 0x5A5A5A5A;
}

void saveToEEPROM() {
    EEPROM.begin(512);
    EEPROM.put(EEPROM_STAKE_ADDR, currentStake);
    EEPROM.put(EEPROM_REWARDS_ADDR, totalRewards);
    EEPROM.put(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
    EEPROM.put(EEPROM_UPTIME_ADDR, uptimeSeconds);
    EEPROM.put(EEPROM_CURRENT_NODE_ADDR, currentPeerIndex);
    uint32_t checksum = computeChecksum();
    EEPROM.put(EEPROM_CHECKSUM_ADDR, checksum);
    EEPROM.put(EEPROM_PEER_COUNT_ADDR, peerCount);
    EEPROM.commit();
    EEPROM.end();
    Serial.println("[EEPROM] Stats saved");
}

void loadFromEEPROM() {
    EEPROM.begin(512);
    EEPROM.get(EEPROM_STAKE_ADDR, currentStake);
    EEPROM.get(EEPROM_REWARDS_ADDR, totalRewards);
    EEPROM.get(EEPROM_BLOCKS_ADDR, totalBlocksSigned);
    EEPROM.get(EEPROM_UPTIME_ADDR, uptimeSeconds);
    EEPROM.get(EEPROM_CURRENT_NODE_ADDR, currentPeerIndex);
    
    uint32_t storedChecksum;
    EEPROM.get(EEPROM_CHECKSUM_ADDR, storedChecksum);
    EEPROM.end();
    
    uint32_t calculatedChecksum = computeChecksum();
    
    if (currentStake < 100 || currentStake > 10000000 || storedChecksum != calculatedChecksum) {
        Serial.println("[EEPROM] Invalid data, resetting to defaults");
        currentStake = INITIAL_STAKE;
        totalRewards = 0;
        totalBlocksSigned = 0;
        uptimeSeconds = 0;
        slashCount = 0;
        currentPeerIndex = 0;
        calculateLevel();
        saveToEEPROM();
    }
    
    calculateLevel();
    Serial.printf("[EEPROM] Loaded - Stake: %lu %s, Level: %d\n", currentStake, SYMBOL, currentLevel);
}

// ==================== SLASHING & REWARDS ====================
void handleSlashing() {
    uint32_t slashAmount = (uint32_t)(currentStake * SLASH_RATE);
    if (slashAmount < LEVEL_STAKE_RANGE) slashAmount = LEVEL_STAKE_RANGE;
    if (slashAmount > currentStake) slashAmount = currentStake;
    
    currentStake -= slashAmount;
    if (currentStake < LEVEL_STAKE_RANGE) currentStake = LEVEL_STAKE_RANGE;
    
    slashCount++;
    calculateLevel();
    saveToEEPROM();
    consecutiveMisses++;
    
    Serial.printf("[SLASH] Lost %lu %s | Stake: %lu | Level: %d | Slashes: %lu\n",
                  slashAmount, SYMBOL, currentStake, currentLevel, slashCount);
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
}

// ==================== WEBSOCKET COMMUNICATION ====================
void sendRegister() {
    StaticJsonDocument<512> doc;
    doc["type"] = "register";
    doc["validator_id"] = validatorID;
    doc["username"] = USERNAME;
    doc["public_key"] = publicKeyHex;
    doc["wallet"] = walletAddress;
    doc["stake"] = currentStake;
    doc["level"] = currentLevel;
    doc["rewards"] = totalRewards;
    doc["blocks"] = totalBlocksSigned;
    doc["uptime"] = uptimeSeconds;
    doc["miner_type"] = "esp32";
    
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%lu", timeClient.getEpochTime());
    doc["timestamp"] = timestamp;
    
    char messageToSign[256];
    snprintf(messageToSign, sizeof(messageToSign), "%s%s%lu%s", validatorID, USERNAME, currentStake, timestamp);
    char signature[130];
    if (signMessage(messageToSign, signature)) {
        doc["signature"] = signature;
    }
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
    Serial.println("[NET] Registration sent");
}

void sendUptimePing() {
    StaticJsonDocument<256> doc;
    doc["type"] = "uptime_ping";
    doc["validator_id"] = validatorID;
    doc["username"] = USERNAME;
    doc["uptime_seconds"] = uptimeCounter;
    doc["stake"] = currentStake;
    doc["level"] = currentLevel;
    doc["timestamp"] = timeClient.getEpochTime();
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
}

void sendBlockSignature() {
    char messageToSign[256];
    snprintf(messageToSign, sizeof(messageToSign), "%s%s%lu", currentChallenge, validatorID, currentBlockId);
    
    char signature[130];
    if (!signMessage(messageToSign, signature)) {
        Serial.println("[ERROR] Failed to sign challenge");
        return;
    }
    
    StaticJsonDocument<512> doc;
    doc["type"] = "block_signature";
    doc["validator_id"] = validatorID;
    doc["username"] = USERNAME;
    doc["challenge"] = currentChallenge;
    doc["signature"] = signature;
    doc["level"] = currentLevel;
    doc["stake"] = currentStake;
    doc["timestamp"] = timeClient.getEpochTime();
    doc["block_id"] = currentBlockId;
    
    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
    Serial.printf("[SIGN] Block %lu signed\n", currentBlockId);
}

// ==================== WEBSOCKET EVENT HANDLER ====================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected");
            isValidator = false;
            isRegistered = false;
            wsConnected = false;
            reconnectAttempts++;
            if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
                switchToNextPeer();
                reconnectAttempts = 0;
            }
            break;
            
        case WStype_CONNECTED:
            Serial.println("[WS] Connected to node");
            wsConnected = true;
            reconnectAttempts = 0;
            // Request peers via gossip discovery
            webSocket.sendTXT("{\"type\":\"get_peers\"}");
            sendRegister();
            break;
            
        case WStype_TEXT: {
            StaticJsonDocument<2048> doc;
            DeserializationError error = deserializeJson(doc, payload);
            if (error) {
                Serial.printf("[ERROR] JSON parse: %s\n", error.c_str());
                break;
            }
            
            const char* type = doc["type"];
            
            if (strcmp(type, "registered") == 0) {
                isRegistered = true;
                int nodeLevel = doc["level"];
                Serial.printf("[REGISTERED] Level: %d\n", nodeLevel);
            }
            else if (strcmp(type, "peers") == 0) {
                // GOSSIP DISCOVERY: add all peers received from node
                JsonArray peers = doc["peers"];
                for (JsonVariant p : peers) {
                    addPeerFromGossip(p.as<String>());
                }
                Serial.printf("[GOSSIP] Received %d peers from node\n", peers.size());
            }
            else if (strcmp(type, "challenge") == 0) {
                strncpy(currentChallenge, doc["challenge"], 64);
                currentChallenge[64] = '\0';
                currentBlockId = doc["block_id"];
                lastChallengeTime = millis();
                isValidator = true;
                sendBlockSignature();
            }
            else if (strcmp(type, "block_accepted") == 0) {
                uint32_t reward = doc["reward"];
                addReward(reward);
                isValidator = false;
                Serial.printf("[ACCEPT] Block %lu, reward %lu %s\n", (uint32_t)doc["block_id"], reward, SYMBOL);
            }
            else if (strcmp(type, "block_rejected") == 0) {
                const char* reason = doc["reason"];
                Serial.printf("[REJECT] %s\n", reason);
                isValidator = false;
            }
            else if (strcmp(type, "slash") == 0) {
                handleSlashing();
                isValidator = false;
            }
            else if (strcmp(type, "level_update") == 0) {
                uint32_t newStake = doc["stake"];
                if (newStake != currentStake) {
                    currentStake = newStake;
                    calculateLevel();
                    saveToEEPROM();
                    Serial.printf("[LEVEL] New stake: %lu, Level: %d\n", currentStake, currentLevel);
                }
            }
            break;
        }
        
        default:
            break;
    }
}

// ==================== WIFI CONNECTION ====================
void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\nWiFi connection failed! Restarting...");
        ESP.restart();
    }
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n==========================================");
    Serial.println("MICROCORE ESP32/ESP8266 MINER v5.0");
    Serial.println("GOSSIP DISCOVERY | ECDSA | PEER CACHING");
    Serial.println("==========================================\n");
    
    initCrypto();
    loadFromEEPROM();
    calculateLevel();
    loadPeersFromSPIFFS();
    
    if (currentPeerIndex < peerCount) {
        String ip = peerList[currentPeerIndex];
        int colonIndex = ip.indexOf(':');
        if (colonIndex > 0) ip = ip.substring(0, colonIndex);
        ip.toCharArray(currentNodeIP, 16);
    } else {
        strcpy(currentNodeIP, "200.36.138.198");
    }
    
    Serial.printf("Username: %s\n", USERNAME);
    Serial.printf("Wallet: %s\n", walletAddress);
    Serial.printf("Peers in cache: %d\n", peerCount);
    for (int i = 0; i < min(peerCount, 5); i++) {
        Serial.printf("  Peer %d: %s\n", i+1, peerList[i].c_str());
    }
    
    connectWiFi();
    timeClient.begin();
    timeClient.update();
    
    webSocket.begin(currentNodeIP, NODE_PORT, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    
    lastUptimePing = millis();
    uptimeCounter = 0;
    isValidator = false;
    isRegistered = false;
    reconnectAttempts = 0;
    
    Serial.println("\n[READY] ESP32/ESP8266 miner running with GOSSIP DISCOVERY\n");
}

// ==================== MAIN LOOP ====================
void loop() {
    webSocket.loop();
    timeClient.update();
    
    if (!isRegistered && (millis() - lastUptimePing > 30000)) {
        if (wsConnected) {
            sendRegister();
        } else {
            switchToNextPeer();
            webSocket.begin(currentNodeIP, NODE_PORT, "/");
        }
        lastUptimePing = millis();
    }
    
    if (millis() - lastUptimePing >= UPTIME_PING_INTERVAL) {
        uptimeCounter++;
        sendUptimePing();
        lastUptimePing = millis();
        
        if (uptimeCounter % 2 == 0) {
            Serial.printf("[STATUS] Stake: %lu %s, Level: %d, Blocks: %lu, Rewards: %lu %s\n",
                          currentStake, SYMBOL, currentLevel, totalBlocksSigned, totalRewards, SYMBOL);
        }
    }
    
    if (isValidator && (millis() - lastChallengeTime >= SIGNING_WINDOW_MS)) {
        Serial.println("[TIMEOUT] Failed to sign within window");
        handleSlashing();
        isValidator = false;
    }
    
    static uint32_t lastSave = 0;
    if (millis() - lastSave >= 3600000) {
        saveToEEPROM();
        savePeersToSPIFFS();
        lastSave = millis();
    }
    
    delay(10);
}