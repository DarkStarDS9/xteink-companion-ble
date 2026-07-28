#include "CompanionPeerStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace companionpeer {

namespace {

constexpr const char* kRoot = "/.crosspoint/companion";
constexpr const char* kPeersDir = "/.crosspoint/companion/peers";
constexpr const char* kIndexPath = "/.crosspoint/companion/peers.json";

std::string peerDir(const char* peerKey) { return std::string(kPeersDir) + "/" + peerKey; }

std::string assetPath(const char* peerKey, uint8_t assetId) {
  const char* name = assetId == kAssetIcon ? "icon.bin" : "buttons.bin";
  return peerDir(peerKey) + "/" + name;
}

std::string tokenPath(const char* peerKey) { return peerDir(peerKey) + "/token.bin"; }

bool ensureRootDirs() {
  if (!Storage.ready()) {
    LOG_ERR("CPEER", "SD not ready");
    return false;
  }
  return Storage.ensureDirectoryExists(kRoot) && Storage.ensureDirectoryExists(kPeersDir);
}

// The index is read, mutated and written on demand — never held. See the
// header's note on why nothing here is resident.
bool loadIndex(JsonDocument& doc) {
  if (!PersistableStoreBase::readDocFromFile(kIndexPath, doc)) {
    doc.to<JsonObject>();
    doc["seq"] = 0;
    doc["peers"].to<JsonArray>();
    return false;
  }
  if (!doc["peers"].is<JsonArray>()) doc["peers"].to<JsonArray>();
  return true;
}

bool saveIndex(const JsonDocument& doc) { return PersistableStoreBase::writeDocToFile(kIndexPath, doc); }

JsonObject findPeer(JsonDocument& doc, const char* peerKey) {
  for (JsonObject entry : doc["peers"].as<JsonArray>()) {
    const char* key = entry["key"] | "";
    if (strcmp(key, peerKey) == 0) return entry;
  }
  return JsonObject();
}

void hexEncode(const uint8_t* bytes, size_t len, char* out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool readWholeFile(const std::string& path, uint8_t* buf, size_t bufLen, size_t& bytesRead) {
  bytesRead = 0;
  HalFile file;
  if (!Storage.openFileForRead("CPEER", path, file)) return false;
  const size_t size = file.size();
  if (size > bufLen) {
    file.close();
    return false;
  }
  const int read = file.read(buf, size);
  file.close();
  if (read < 0) return false;
  bytesRead = static_cast<size_t>(read);
  return true;
}

bool writeWholeFile(const std::string& path, const uint8_t* data, size_t len) {
  HalFile file;
  if (!Storage.openFileForWrite("CPEER", path, file)) return false;
  const size_t written = file.write(data, len);
  file.flush();
  file.close();
  return written == len;
}

// The button map is validated here rather than at render time so a malformed
// push is rejected with ASSET_REJECTED_FORMAT while the app can still do
// something about it — an app whose map half-parsed would draw nonsense hints
// with no way to find out why.
bool buttonMapParses(const uint8_t* data, size_t len) {
  if (len < 5) return false;  // 4-byte tag + entry count
  const uint8_t count = data[4];
  size_t offset = 5;
  for (uint8_t i = 0; i < count; ++i) {
    if (offset + 3 > len) return false;
    const uint8_t labelLen = data[offset + 2];
    offset += 3 + labelLen;
    if (offset > len) return false;
  }
  return true;
}

// Deletes a peer's directory and everything under it. Used by LRU eviction.
void removePeerDir(const char* peerKey) {
  const std::string dir = peerDir(peerKey);
  Storage.remove((dir + "/token.bin").c_str());
  Storage.remove((dir + "/buttons.bin").c_str());
  Storage.remove((dir + "/icon.bin").c_str());
  Storage.removeDir((dir + "/data").c_str());
  Storage.rmdir(dir.c_str());
}

// Drops least-recently-seen peers until at most kMaxPeers remain. Called from
// touch(), so the cap is enforced at the moment a new peer pushes the count
// over it rather than lazily at render.
void evictOverflow(JsonDocument& doc) {
  JsonArray peers = doc["peers"].as<JsonArray>();
  while (peers.size() > kMaxPeers) {
    size_t oldestIndex = 0;
    uint32_t oldestSeq = UINT32_MAX;
    for (size_t i = 0; i < peers.size(); ++i) {
      const uint32_t seq = peers[i]["seq"] | 0u;
      if (seq < oldestSeq) {
        oldestSeq = seq;
        oldestIndex = i;
      }
    }
    const char* key = peers[oldestIndex]["key"] | "";
    LOG_INF("CPEER", "evicting least-recently-seen peer %s", key);
    if (key[0] != '\0') removePeerDir(key);
    peers.remove(oldestIndex);
  }
}

}  // namespace

void makePeerKey(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen], char keyOut[kPeerKeyLen]) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, appId, kIdLen);
  mbedtls_sha256_update(&ctx, installId, kIdLen);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  hexEncode(digest, 4, keyOut);
}

bool ensurePeer(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen], const char* displayName,
                char keyOut[kPeerKeyLen]) {
  makePeerKey(appId, installId, keyOut);
  if (!ensureRootDirs()) return false;
  if (!Storage.ensureDirectoryExists(peerDir(keyOut).c_str())) {
    LOG_ERR("CPEER", "could not create peer dir for %s", keyOut);
    return false;
  }

  JsonDocument doc;
  loadIndex(doc);
  const uint32_t seq = (doc["seq"] | 0u) + 1;
  doc["seq"] = seq;

  JsonObject entry = findPeer(doc, keyOut);
  if (entry.isNull()) {
    entry = doc["peers"].as<JsonArray>().add<JsonObject>();
    entry["key"] = keyOut;
    char hex[kIdLen * 2 + 1];
    hexEncode(appId, kIdLen, hex);
    entry["app"] = hex;
    hexEncode(installId, kIdLen, hex);
    entry["install"] = hex;
  }
  if (displayName && displayName[0] != '\0') entry["name"] = displayName;
  entry["seq"] = seq;

  evictOverflow(doc);
  return saveIndex(doc);
}

bool isEnrolled(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen]) {
  char key[kPeerKeyLen];
  makePeerKey(appId, installId, key);
  JsonDocument doc;
  if (!loadIndex(doc)) return false;
  return !findPeer(doc, key).isNull();
}

bool tokenMatches(const char* peerKey, const uint8_t* token, size_t tokenLen) {
  if (tokenLen != kTokenLen) return false;
  uint8_t stored[kTokenLen];
  size_t read = 0;
  if (!readWholeFile(tokenPath(peerKey), stored, sizeof(stored), read) || read != kTokenLen) return false;
  // Compare every byte regardless of the first mismatch. The link is
  // unencrypted so this is not load-bearing (see the protocol doc's pairing
  // section), but a timing-independent compare costs nothing here.
  uint8_t diff = 0;
  for (size_t i = 0; i < kTokenLen; ++i) diff |= static_cast<uint8_t>(stored[i] ^ token[i]);
  return diff == 0;
}

bool issueToken(const char* peerKey, uint8_t out[kTokenLen]) {
  esp_fill_random(out, kTokenLen);
  return writeWholeFile(tokenPath(peerKey), out, kTokenLen);
}

void assetTag(const char* peerKey, uint8_t assetId, uint8_t out[4]) {
  memset(out, 0, 4);
  HalFile file;
  if (!Storage.openFileForRead("CPEER", assetPath(peerKey, assetId), file)) return;
  uint8_t tag[4];
  const int read = file.read(tag, sizeof(tag));
  file.close();
  if (read == static_cast<int>(sizeof(tag))) memcpy(out, tag, sizeof(tag));
}

AssetStoreResult storeAsset(const char* peerKey, uint8_t assetId, const uint8_t* data, size_t len,
                            size_t expectedIconBytes) {
  if (assetId == kAssetIcon) {
    if (len != 4 + expectedIconBytes) return AssetStoreResult::RejectedSize;
  } else if (assetId == kAssetButtonMap) {
    if (len > kMaxButtonMapLen) return AssetStoreResult::RejectedSize;
    if (!buttonMapParses(data, len)) return AssetStoreResult::RejectedFormat;
  } else {
    return AssetStoreResult::RejectedFormat;
  }

  if (!Storage.ensureDirectoryExists(peerDir(peerKey).c_str())) return AssetStoreResult::RejectedStorage;
  if (!writeWholeFile(assetPath(peerKey, assetId), data, len)) return AssetStoreResult::RejectedStorage;
  return AssetStoreResult::Stored;
}

bool hasButtonMap(const char* peerKey) { return Storage.exists(assetPath(peerKey, kAssetButtonMap).c_str()); }

size_t readAssetBody(const char* peerKey, uint8_t assetId, uint8_t* buf, size_t bufLen) {
  HalFile file;
  if (!Storage.openFileForRead("CPEER", assetPath(peerKey, assetId), file)) return 0;
  const size_t size = file.size();
  if (size <= 4 || size - 4 > bufLen) {
    file.close();
    return 0;
  }
  file.seek(4);  // skip the opaque tag; only the body is drawn or routed
  const int read = file.read(buf, size - 4);
  file.close();
  return read > 0 ? static_cast<size_t>(read) : 0;
}

std::string dataFilePath(const char* peerKey, const char* fileName) {
  const std::string dir = peerDir(peerKey) + "/data";
  Storage.ensureDirectoryExists(dir.c_str());
  return dir + "/" + fileName;
}

std::string displayName(const char* peerKey) {
  JsonDocument doc;
  if (!loadIndex(doc)) return std::string();
  JsonObject entry = findPeer(doc, peerKey);
  if (entry.isNull()) return std::string();
  return std::string(entry["name"] | "");
}

void touch(const char* peerKey) {
  JsonDocument doc;
  if (!loadIndex(doc)) return;
  JsonObject entry = findPeer(doc, peerKey);
  if (entry.isNull()) return;
  const uint32_t seq = (doc["seq"] | 0u) + 1;
  doc["seq"] = seq;
  entry["seq"] = seq;
  evictOverflow(doc);
  saveIndex(doc);
}

size_t listIconTiles(char keysOut[][kPeerKeyLen], size_t maxTiles) {
  JsonDocument doc;
  if (!loadIndex(doc) || maxTiles == 0) return 0;
  JsonArray peers = doc["peers"].as<JsonArray>();

  // One tile per appId, most recently seen first. Selection sort over at most
  // kMaxPeers entries — trivially cheap next to the E-ink refresh that follows,
  // and it avoids allocating a sorted copy of the index.
  // Dedupe set sized to the tile cap, not to kMaxPeers: an appId can only be
  // chosen once, so at most maxTiles of them are ever recorded. 18 x 33 bytes
  // of stack, transient, versus an array of std::string that would heap-allocate
  // per entry on the render path.
  if (maxTiles > kMaxIconTiles) maxTiles = kMaxIconTiles;
  char chosenApps[kMaxIconTiles][kIdLen * 2 + 1] = {};
  size_t chosenCount = 0;
  size_t written = 0;

  while (written < maxTiles) {
    int bestIndex = -1;
    uint32_t bestSeq = 0;
    for (size_t i = 0; i < peers.size(); ++i) {
      const char* key = peers[i]["key"] | "";
      const char* app = peers[i]["app"] | "";
      if (key[0] == '\0') continue;
      if (!Storage.exists(assetPath(key, kAssetIcon).c_str())) continue;

      bool alreadyChosen = false;
      for (size_t c = 0; c < chosenCount; ++c) {
        if (strcmp(chosenApps[c], app) == 0) {
          alreadyChosen = true;
          break;
        }
      }
      if (alreadyChosen) continue;

      const uint32_t seq = peers[i]["seq"] | 0u;
      if (bestIndex < 0 || seq > bestSeq) {
        bestIndex = static_cast<int>(i);
        bestSeq = seq;
      }
    }
    if (bestIndex < 0) break;

    const char* key = peers[bestIndex]["key"] | "";
    snprintf(keysOut[written], kPeerKeyLen, "%s", key);
    if (chosenCount < kMaxIconTiles) {
      snprintf(chosenApps[chosenCount++], kIdLen * 2 + 1, "%s", peers[bestIndex]["app"] | "");
    }
    ++written;
  }
  return written;
}

size_t listPeers(char keysOut[][kPeerKeyLen], size_t maxPeers) {
  JsonDocument doc;
  if (!loadIndex(doc)) return 0;
  JsonArray peers = doc["peers"].as<JsonArray>();

  size_t written = 0;
  uint32_t previousSeq = UINT32_MAX;
  while (written < maxPeers) {
    int bestIndex = -1;
    uint32_t bestSeq = 0;
    for (size_t i = 0; i < peers.size(); ++i) {
      const uint32_t seq = peers[i]["seq"] | 0u;
      if (seq >= previousSeq) continue;
      if (bestIndex < 0 || seq > bestSeq) {
        bestIndex = static_cast<int>(i);
        bestSeq = seq;
      }
    }
    if (bestIndex < 0) break;
    snprintf(keysOut[written++], kPeerKeyLen, "%s", peers[bestIndex]["key"] | "");
    previousSeq = bestSeq;
  }
  return written;
}

bool hasIcon(const char* peerKey) { return Storage.exists(assetPath(peerKey, kAssetIcon).c_str()); }

void forgetAllPeers() {
  JsonDocument doc;
  if (loadIndex(doc)) {
    for (JsonObject entry : doc["peers"].as<JsonArray>()) {
      const char* key = entry["key"] | "";
      if (key[0] != '\0') removePeerDir(key);
    }
  }
  Storage.remove(kIndexPath);
  Storage.rmdir(kPeersDir);
  LOG_INF("CPEER", "all peers forgotten");
}

bool anyEnrolled() {
  JsonDocument doc;
  if (!loadIndex(doc)) return false;
  return doc["peers"].as<JsonArray>().size() > 0;
}

}  // namespace companionpeer
