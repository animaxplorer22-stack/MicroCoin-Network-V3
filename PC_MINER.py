#!/usr/bin/env python3
"""
MICROCORE (MCX) PC MINER v5.0 - WITH GOSSIP DISCOVERY
Real ECDSA secp256k1 | Peer caching | Auto node discovery | No DNS required

Run: python3 pc_miner.py
"""

import asyncio
import json
import time
import hashlib
import os
import sys
import random
import signal
from datetime import datetime
from typing import Optional, List
import traceback

try:
    import websockets
except ImportError:
    os.system("pip install websockets")
    import websockets

try:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
except ImportError:
    os.system("pip install cryptography")
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

# ==================== GOSSIP DISCOVERY (NO DNS) ====================
# Only ONE bootnode needed - miners discover others via gossip
BOOTSTRAP_NODES = [
    "200.36.138.198:8080",  # Your Morocco node
]

PEER_CACHE_FILE = "microcore_peers.json"
NODE_PORT = 8080

def save_peers_to_cache(peers):
    try:
        unique = list(set(peers))
        with open(PEER_CACHE_FILE, 'w') as f:
            json.dump(unique, f, indent=2)
        print(f"[CACHE] Saved {len(unique)} peers")
    except Exception as e:
        print(f"[CACHE] Save failed: {e}")

def load_peers_from_cache():
    try:
        with open(PEER_CACHE_FILE, 'r') as f:
            peers = json.load(f)
        print(f"[CACHE] Loaded {len(peers)} peers from cache")
        return peers
    except:
        print(f"[CACHE] No cache file found")
        return []

def get_bootstrap_peers():
    peers = BOOTSTRAP_NODES.copy()
    peers.extend(load_peers_from_cache())
    seen = set()
    unique = []
    for p in peers:
        if p not in seen:
            seen.add(p)
            unique.append(p)
    return unique

# ==================== CONFIGURATION ====================
USERNAME = ""
WALLET_FILE = "microcore_pc_wallet.json"

INITIAL_STAKE = 100
LEVEL_STAKE_RANGE = 100
SIGNING_WINDOW_MS = 2500
SLASH_RATE = 0.10
UPTIME_PING_INTERVAL = 30
STATUS_INTERVAL = 60
MAX_RECONNECT_ATTEMPTS = 10
RECONNECT_DELAY = 5

# ==================== REAL CRYPTO FUNCTIONS ====================
def generate_private_key():
    priv = ec.generate_private_key(ec.SECP256K1())
    return priv.private_numbers().private_value.to_bytes(32, 'big').hex(), priv

def get_public_key_pem(priv_hex):
    priv = ec.derive_private_key(int(priv_hex, 16), ec.SECP256K1())
    pub = priv.public_key()
    return pub.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()

def get_wallet_address(pub_pem):
    return "MCR_" + hashlib.sha256(pub_pem.encode()).hexdigest()[:32].upper()

def get_validator_id(username, pub_pem):
    return hashlib.sha256(f"{username}{pub_pem}".encode()).hexdigest()[:32]

def sign_message(priv_hex, msg):
    priv = ec.derive_private_key(int(priv_hex, 16), ec.SECP256K1())
    r, s = decode_dss_signature(priv.sign(msg.encode(), ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, 'big').hex() + s.to_bytes(32, 'big').hex()

# ==================== WALLET ====================
class Wallet:
    def __init__(self, username, address, pub_pem, priv_hex):
        self.username = username
        self.address = address
        self.pub_pem = pub_pem
        self.priv_hex = priv_hex
    
    def get_validator_id(self):
        return get_validator_id(self.username, self.pub_pem)
    
    @classmethod
    def create_new(cls, username):
        priv_hex, _ = generate_private_key()
        pub_pem = get_public_key_pem(priv_hex)
        address = get_wallet_address(pub_pem)
        return cls(username, address, pub_pem, priv_hex)
    
    @classmethod
    def load(cls, filename):
        if not os.path.exists(filename):
            return None
        with open(filename, 'r') as f:
            data = json.load(f)
        return cls(data['username'], data['address'], data['pub_pem'], data['priv_hex'])
    
    def save(self, filename):
        with open(filename, 'w') as f:
            json.dump({'username': self.username, 'address': self.address, 'pub_pem': self.pub_pem, 'priv_hex': self.priv_hex}, f, indent=2)

# ==================== PC MINER ====================
class PCMiner:
    def __init__(self, wallet):
        self.wallet = wallet
        self.validator_id = wallet.get_validator_id()
        self.peers = get_bootstrap_peers()
        self.current_peer_index = 0
        self.discovered_peers = set(self.peers)
        
        self.websocket = None
        self.is_validator = False
        self.current_challenge = ""
        self.current_block_id = 0
        self.last_challenge_time = 0
        self.start_time = time.time()
        self.last_uptime_ping = 0
        self.last_status_report = 0
        self.reconnect_attempts = 0
        self.connected = False
        self.mining = True
        self.running = True
        
        self.total_rewards = 0
        self.blocks_signed = 0
        self.consecutive_misses = 0
        self.slash_count = 0
        self.current_stake = INITIAL_STAKE
        self.today_uptime = 0
        self.last_uptime_reset = time.time()
        self.current_level = self.calculate_level()
        
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)
        
        self.load_stats()
    
    def signal_handler(self, signum, frame):
        print("\n[SHUTDOWN] Stopping miner...")
        self.running = False
        self.mining = False
        self.save_stats()
        sys.exit(0)
    
    def calculate_level(self):
        level = ((self.current_stake - 1) // LEVEL_STAKE_RANGE) + 1
        return max(1, min(level, 100))
    
    def update_today_uptime(self):
        now = time.time()
        if now - self.last_uptime_reset > 86400:
            self.today_uptime = 0
            self.last_uptime_reset = now
        self.today_uptime += UPTIME_PING_INTERVAL
        if self.today_uptime > 86400:
            self.today_uptime = 86400
    
    def load_stats(self):
        stats_file = "pc_miner_stats.json"
        if os.path.exists(stats_file):
            try:
                with open(stats_file, 'r') as f:
                    data = json.load(f)
                    self.total_rewards = data.get('rewards', 0)
                    self.blocks_signed = data.get('blocks', 0)
                    self.slash_count = data.get('slashes', 0)
                    self.current_stake = data.get('stake', INITIAL_STAKE)
                    self.today_uptime = data.get('today_uptime', 0)
                    self.current_level = self.calculate_level()
            except:
                pass
    
    def save_stats(self):
        with open("pc_miner_stats.json", 'w') as f:
            json.dump({'rewards': self.total_rewards, 'blocks': self.blocks_signed, 'slashes': self.slash_count, 'stake': self.current_stake, 'today_uptime': self.today_uptime}, f, indent=2)
    
    def switch_to_next_peer(self):
        self.current_peer_index = (self.current_peer_index + 1) % len(self.peers) if self.peers else 0
        self.reconnect_attempts += 1
        if self.reconnect_attempts >= MAX_RECONNECT_ATTEMPTS:
            self.current_peer_index = 0
            self.reconnect_attempts = 0
        print(f"\n[FAILOVER] Switching to peer #{self.current_peer_index}\n")
    
    def get_current_peer_url(self):
        if not self.peers:
            return None
        peer = self.peers[self.current_peer_index]
        if "://" not in peer:
            peer = f"ws://{peer}"
        return peer
    
    def add_peer_from_gossip(self, peer):
        if peer not in self.discovered_peers:
            self.discovered_peers.add(peer)
            self.peers.append(peer)
            save_peers_to_cache(list(self.discovered_peers))
            print(f"[GOSSIP] Discovered new peer: {peer}")
    
    def add_reward(self, reward):
        self.total_rewards += reward
        self.current_stake += reward
        self.blocks_signed += 1
        self.consecutive_misses = 0
        self.current_level = self.calculate_level()
        self.save_stats()
        print(f"\n💰 REWARD: +{reward} MCX | Total: {self.total_rewards} | Stake: {self.current_stake} | Level: {self.current_level}")
    
    def handle_slash(self):
        slash = max(int(self.current_stake * SLASH_RATE), LEVEL_STAKE_RANGE)
        self.current_stake -= slash
        if self.current_stake < LEVEL_STAKE_RANGE:
            self.current_stake = LEVEL_STAKE_RANGE
        self.consecutive_misses += 1
        self.slash_count += 1
        self.current_level = self.calculate_level()
        self.save_stats()
        print(f"\n⚠️ SLASHED: -{slash} MCX | Stake: {self.current_stake} | Level: {self.current_level}")
        return self.slash_count < 5
    
    async def register(self):
        ts = time.time()
        sig = sign_message(self.wallet.priv_hex, f"{self.validator_id}{self.wallet.username}{self.current_stake}{ts}")
        msg = {
            "type": "register",
            "validator_id": self.validator_id,
            "username": self.wallet.username,
            "public_key": self.wallet.pub_pem,
            "wallet": self.wallet.address,
            "stake": self.current_stake,
            "level": self.current_level,
            "rewards": self.total_rewards,
            "blocks": self.blocks_signed,
            "uptime": int(time.time() - self.start_time),
            "today_uptime": self.today_uptime,
            "miner_type": "pc",
            "timestamp": ts,
            "signature": sig
        }
        if self.websocket:
            await self.websocket.send(json.dumps(msg))
            print(f"📡 Registered as '{self.wallet.username}'")
    
    async def send_uptime(self):
        uptime = int(time.time() - self.start_time)
        self.update_today_uptime()
        msg = {
            "type": "uptime_ping",
            "validator_id": self.validator_id,
            "username": self.wallet.username,
            "uptime_seconds": uptime,
            "today_uptime": self.today_uptime,
            "stake": self.current_stake,
            "level": self.current_level
        }
        if self.websocket:
            await self.websocket.send(json.dumps(msg))
    
    async def sign_block(self):
        sig = sign_message(self.wallet.priv_hex, f"{self.current_challenge}{self.validator_id}{self.current_block_id}")
        msg = {
            "type": "block_signature",
            "validator_id": self.validator_id,
            "username": self.wallet.username,
            "challenge": self.current_challenge,
            "signature": sig,
            "level": self.current_level,
            "stake": self.current_stake,
            "block_id": self.current_block_id,
            "timestamp": time.time()
        }
        if self.websocket:
            await self.websocket.send(json.dumps(msg))
            print(f"✍️ Signed block {self.current_block_id}")
    
    async def handle_message(self, data):
        try:
            msg = json.loads(data)
            msg_type = msg.get("type")
            
            if msg_type == "registered":
                print(f"✅ Registration confirmed | Level: {msg.get('level')} | Reward: {msg.get('reward')} MCX/block")
                self.reconnect_attempts = 0
            
            elif msg_type == "peers":
                # GOSSIP DISCOVERY: add all peers received from node
                for peer in msg.get("peers", []):
                    self.add_peer_from_gossip(peer)
                print(f"[GOSSIP] Received {len(msg.get('peers', []))} peers from node")
            
            elif msg_type == "challenge":
                self.current_challenge = msg.get("challenge", "")
                self.current_block_id = msg.get("block_id", 0)
                self.last_challenge_time = time.time()
                self.is_validator = True
                await self.sign_block()
                
                if hasattr(self, '_timeout_task'):
                    self._timeout_task.cancel()
                
                async def timeout_handler():
                    await asyncio.sleep(SIGNING_WINDOW_MS / 1000)
                    if self.is_validator:
                        print(f"⏰ TIMEOUT: Missed block {self.current_block_id}")
                        self.handle_slash()
                        self.is_validator = False
                
                self._timeout_task = asyncio.create_task(timeout_handler())
            
            elif msg_type == "block_accepted":
                if hasattr(self, '_timeout_task'):
                    self._timeout_task.cancel()
                reward = msg.get("reward", 0)
                self.add_reward(reward)
                self.is_validator = False
                print(f"✅ Block {msg.get('block_id')} ACCEPTED! +{reward} MCX")
            
            elif msg_type == "block_rejected":
                if hasattr(self, '_timeout_task'):
                    self._timeout_task.cancel()
                self.is_validator = False
                print(f"❌ Block {msg.get('block_id')} REJECTED")
            
            elif msg_type == "slash":
                print(f"⚠️ SLASH command received")
                self.handle_slash()
                self.is_validator = False
            
            elif msg_type == "level_update":
                new_stake = msg.get("stake", self.current_stake)
                if new_stake != self.current_stake:
                    self.current_stake = new_stake
                    self.current_level = self.calculate_level()
                    self.save_stats()
                    print(f"📊 Level update: Level {self.current_level}")
        
        except Exception as e:
            print(f"[ERROR] Message handling: {e}")
    
    async def connect_and_run(self):
        self.reconnect_attempts = 0
        
        while self.running:
            peer_url = self.get_current_peer_url()
            if not peer_url:
                print("[ERROR] No peers available. Check BOOTSTRAP_NODES")
                await asyncio.sleep(30)
                continue
            
            try:
                print(f"[CONN] Connecting to {peer_url}...")
                async with websockets.connect(peer_url, ping_interval=20, ping_timeout=10, close_timeout=5) as ws:
                    self.websocket = ws
                    self.connected = True
                    self.reconnect_attempts = 0
                    print(f"🔌 Connected to {peer_url}")
                    
                    # Request peers via gossip discovery
                    await ws.send(json.dumps({"type": "get_peers"}))
                    
                    await self.register()
                    
                    while self.running and self.mining:
                        if time.time() - self.last_uptime_ping > UPTIME_PING_INTERVAL:
                            await self.send_uptime()
                            self.last_uptime_ping = time.time()
                        
                        if time.time() - self.last_status_report > STATUS_INTERVAL:
                            self.print_status()
                            self.last_status_report = time.time()
                        
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                            await self.handle_message(raw)
                        except asyncio.TimeoutError:
                            pass
                        
                        if self.is_validator and (time.time() - self.last_challenge_time) > (SIGNING_WINDOW_MS / 1000 + 0.5):
                            print(f"⏰ Fallback timeout! Missed block {self.current_block_id}")
                            self.handle_slash()
                            self.is_validator = False
                        
                        await asyncio.sleep(0.05)
            
            except Exception as e:
                print(f"[ERROR] {e}")
                self.connected = False
                self.switch_to_next_peer()
                delay = RECONNECT_DELAY * min(self.reconnect_attempts + 1, 10)
                print(f"[CONN] Reconnecting in {delay}s...")
                await asyncio.sleep(delay)
            
            finally:
                self.websocket = None
    
    def print_status(self):
        uptime = int(time.time() - self.start_time)
        hours = uptime // 3600
        minutes = (uptime % 3600) // 60
        today_hours = self.today_uptime / 3600
        success_rate = 0
        total = self.blocks_signed + self.consecutive_misses
        if total > 0:
            success_rate = (self.blocks_signed / total) * 100
        
        print(f"\n{'='*50}")
        print(f"💻 PC MINER STATUS")
        print(f"{'='*50}")
        print(f"Username: {self.wallet.username}")
        print(f"Wallet: {self.wallet.address[:24]}...")
        print(f"Peers in cache: {len(self.discovered_peers)}")
        print(f"{'-'*40}")
        print(f"Level: {self.current_level} / 100")
        print(f"Stake: {self.current_stake:,} MCX")
        print(f"Rewards: {self.total_rewards:,} MCX")
        print(f"Blocks: {self.blocks_signed}")
        print(f"Missed: {self.consecutive_misses}")
        print(f"Success Rate: {success_rate:.1f}%")
        print(f"Slashes: {self.slash_count} / 5")
        print(f"{'-'*40}")
        print(f"Uptime: {hours}h {minutes}m")
        print(f"Today's Uptime: {today_hours:.1f}h / 24h")
        print(f"Connected: {'✅ Yes' if self.connected else '❌ No'}")
        print(f"{'='*50}\n")
    
    async def run(self):
        print(f"\n{'='*60}")
        print(f"MICROCORE PC MINER v5.0")
        print(f"GOSSIP DISCOVERY | PEER CACHING | NO DNS")
        print(f"{'='*60}")
        print(f"Username: {self.wallet.username}")
        print(f"Wallet: {self.wallet.address}")
        print(f"Bootstraps: {BOOTSTRAP_NODES}")
        print(f"Peers in cache: {len(self.discovered_peers)}")
        print(f"{'='*60}\n")
        
        await self.connect_and_run()

# ==================== MAIN ====================
async def main():
    print("\n" + "=" * 60)
    print("🔷 MICROCORE PC MINER - GOSSIP DISCOVERY 🔷")
    print("=" * 60)
    
    wallet = Wallet.load(WALLET_FILE)
    if not wallet:
        print("\n[FIRST RUN] No wallet found.")
        username = input("Enter your username: ").strip()
        if not username:
            username = f"pc_miner_{int(time.time())}"
        wallet = Wallet.create_new(username)
        wallet.save(WALLET_FILE)
        print(f"\n✅ Wallet created!")
        print(f"   Username: {wallet.username}")
        print(f"   Address: {wallet.address}")
        print(f"   Private Key: {wallet.priv_hex}")
        print(f"\n⚠️ SAVE THESE CREDENTIALS!")
    else:
        print(f"\n✅ Wallet loaded: {wallet.username}")
        print(f"   Address: {wallet.address[:32]}...")
    
    miner = PCMiner(wallet)
    await miner.run()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[EXIT] Goodbye!")