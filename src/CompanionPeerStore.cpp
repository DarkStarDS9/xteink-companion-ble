#include "CompanionPeerStore.h"

#include "CompanionBle.h"
#include "CompanionUiDeclaration.h"

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
  const char* name = assetId == kAssetIcon ? "icon.bin" : "ui.bin";
  return peerDir(peerKey) + "/" + name;
}

std::string tokenPath(const char* peerKey) { return peerDir(peerKey) + "/token.bin"; }

constexpr const char* kImagesIndexName = "images.json";

std::string imagesDir(const char* peerKey) { return peerDir(peerKey) + "/images"; }
std::string imagesIndexPath(const char* peerKey) { return peerDir(peerKey) + "/" + kImagesIndexName; }

std::string imageSlotPath(const char* peerKey, uint32_t slot) {
  char name[24];
  snprintf(name, sizeof(name), "img_%u.raw", static_cast<unsigned>(slot));
  return imagesDir(peerKey) + "/" + name;
}

constexpr const char* kListsFileName = "lists.json";

std::string listsPath(const char* peerKey) { return peerDir(peerKey) + "/" + kListsFileName; }
// Written first, then renamed onto listsPath() -- see storeListDocument().
// Never left behind on a clean push; a stale one only survives a crash
// mid-write, and the next push overwrites it before ever reading it, so it
// costs nothing to leave cleanup to that path plus removePeerDir().
std::string listsTmpPath(const char* peerKey) { return peerDir(peerKey) + "/" + kListsFileName + ".tmp"; }

// The images index is read/mutated/written on demand, same discipline as
// peers.json — see the header's "NOTHING HERE IS RESIDENT" note.
bool loadImagesIndex(const char* peerKey, JsonDocument& doc) {
  if (!PersistableStoreBase::readDocFromFile(imagesIndexPath(peerKey).c_str(), doc)) {
    doc.to<JsonObject>();
    doc["nextSeq"] = 0;
    doc["images"].to<JsonArray>();
    return false;
  }
  if (!doc["images"].is<JsonArray>()) doc["images"].to<JsonArray>();
  return true;
}

bool saveImagesIndex(const char* peerKey, const JsonDocument& doc) {
  return PersistableStoreBase::writeDocToFile(imagesIndexPath(peerKey).c_str(), doc);
}

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

// Converts a kFieldListDoc push straight to lists.json's JSON shape as
// companiontodo::parseDocument() walks it, writing each fragment to `file`
// as it goes rather than building a JsonDocument in RAM first.
//
// Why not a JsonDocument, matching images.json/peers.json's own idiom (the
// preferred approach per docs/companion-todo-list-design.md's storage
// section): measured against ArduinoJson v7's real allocator (v7's
// JsonDocument::memoryUsage() always reports 0 now -- it dropped the fixed
// pool for per-node heap allocation -- so this was measured with a custom
// Allocator tracking live bytes instead of trusting that call). A document
// shaped the way the design doc assumes -- a few hundred short items --
// costs ~49 KB live, already uncomfortable stacked on the 16 KB
// `g_activeBuf` still resident while this runs, against the ~166 KB
// headroom the design doc's own §4/§8 cites. But `kMaxListDocLen` (16 KB)
// does not actually bound item *count* the way that estimate assumes: the
// wire format's minimum per-item cost is 4 bytes (a 2-byte id, 1 checked
// byte, 1 zero-length textLen), so a legally-sized push can carry over 4000
// near-empty items. Built and measured that exact document: ~4092 items,
// ~450 KB live in the same JsonDocument approach -- multiples of this
// device's entire ~380 KB RAM, from a push that is not oversize by any rule
// this protocol enforces. A malformed-content DoS, not a hardware bug.
// Streaming straight to disk instead keeps this function's own RAM flat
// (one small write buffer, not proportional to item count) regardless of
// how a pushed document chooses to spend its 16 KB, and gets the
// temp-file-then-rename atomicity storeListDocument() needs "for free" in
// the same move, per docs/companion-declared-shape-design.md-style
// measure-before-deciding rather than assuming the round-number cap alone
// bounds this.
class JsonListWriter : public companiontodo::Visitor {
 public:
  explicit JsonListWriter(HalFile& file) : file_(file) {}

  // False if any underlying write failed. Checked by the caller instead of
  // trusting parseDocument()'s own Ok/Malformed verdict alone -- a full SD
  // card can fail a write in the middle of an otherwise well-formed
  // document, and that must not look like Stored either.
  bool ok() const { return ok_; }

  void onDocument(uint32_t revision) override {
    writeRaw("{\"revision\":");
    writeUint(revision);
    writeRaw(",\"lists\":[");
  }
  void onListStart(uint16_t listId, const char* title, uint8_t titleLen) override {
    if (listIndex_++ > 0) writeRaw(",");
    writeRaw("{\"listId\":");
    writeUint(listId);
    writeRaw(",\"title\":");
    writeJsonString(title, titleLen);
    writeRaw(",\"groups\":[");
    groupIndex_ = 0;
  }
  void onGroupStart(uint16_t groupId, const char* label, uint8_t labelLen) override {
    if (groupIndex_++ > 0) writeRaw(",");
    writeRaw("{\"groupId\":");
    writeUint(groupId);
    writeRaw(",\"label\":");
    writeJsonString(label, labelLen);
    writeRaw(",\"items\":[");
    itemIndex_ = 0;
  }
  void onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) override {
    if (itemIndex_++ > 0) writeRaw(",");
    writeRaw("{\"itemId\":");
    writeUint(itemId);
    writeRaw(",\"text\":");
    writeJsonString(text, textLen);
    writeRaw(checked ? ",\"checked\":true}" : ",\"checked\":false}");
  }
  void onGroupEnd(uint16_t /*groupId*/) override { writeRaw("]}"); }
  void onListEnd(uint16_t /*listId*/) override { writeRaw("]}"); }

  // Closes the document. Always safe to call, even on a document that never
  // got past onDocument() (or was never called at all) -- the caller
  // discards this file entirely unless parseDocument() returned Ok, so an
  // unbalanced close on a document that failed early is harmless.
  void finish() { writeRaw("]}"); }

 private:
  void writeRaw(const char* s) {
    const size_t len = strlen(s);
    if (file_.write(reinterpret_cast<const uint8_t*>(s), len) != len) ok_ = false;
  }
  void writeUint(uint32_t v) {
    char buf[11];
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
    writeRaw(buf);
  }
  // Wire text is arbitrary bytes the phone chose (parseDocument() validates
  // structure, never UTF-8 well-formedness), so only what JSON itself
  // requires is escaped: quote, backslash, and control characters. Anything
  // else -- including multi-byte UTF-8 sequences -- passes through verbatim.
  void writeJsonString(const char* s, uint8_t len) {
    writeRaw("\"");
    for (uint8_t i = 0; i < len; ++i) {
      const uint8_t c = static_cast<uint8_t>(s[i]);
      switch (c) {
        case '"':
          writeRaw("\\\"");
          break;
        case '\\':
          writeRaw("\\\\");
          break;
        case '\n':
          writeRaw("\\n");
          break;
        case '\r':
          writeRaw("\\r");
          break;
        case '\t':
          writeRaw("\\t");
          break;
        default:
          if (c < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            writeRaw(buf);
          } else {
            const uint8_t one[1] = {c};
            if (file_.write(one, 1) != 1) ok_ = false;
          }
      }
    }
    writeRaw("\"");
  }

  HalFile& file_;
  bool ok_ = true;
  size_t listIndex_ = 0;
  size_t groupIndex_ = 0;
  size_t itemIndex_ = 0;
};

// The UI declaration's byte layout, its validation rules and its host gtest
// suite all live in CompanionUiDeclaration.{h,cpp} — this file cannot be
// host-built (ArduinoJson, PersistableStore, HalStorage), and the codec is
// pure, so it was extracted exactly as CompanionBatchModel and
// CompanionConnPolicy were. Nothing here parses declaration bytes itself.

// Deletes a peer's directory and everything under it. Used by LRU eviction.
void removePeerDir(const char* peerKey) {
  const std::string dir = peerDir(peerKey);
  Storage.remove((dir + "/token.bin").c_str());
  Storage.remove((dir + "/ui.bin").c_str());
  Storage.remove((dir + "/icon.bin").c_str());
  Storage.remove((dir + "/" + kImagesIndexName).c_str());
  for (uint32_t slot = 0; slot < kMaxImagesPerPeer; ++slot) Storage.remove(imageSlotPath(peerKey, slot).c_str());
  Storage.removeDir((dir + "/images").c_str());
  Storage.remove((dir + "/" + kListsFileName).c_str());
  Storage.remove((dir + "/" + kListsFileName + ".tmp").c_str());
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
                const char* userName, char keyOut[kPeerKeyLen]) {
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
  if (userName && userName[0] != '\0') entry["user"] = userName;
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
  } else if (assetId == kAssetUiDeclaration) {
    if (len > kMaxUiDeclarationLen) return AssetStoreResult::RejectedSize;
    switch (companionui::parseAsset(data, len, nullptr)) {
      case companionui::ParseResult::Ok:
        break;
      // Reported separately from the generic format failure so a client
      // mid-migration is told which of the two it is; see the enum's comment.
      case companionui::ParseResult::NoShape:
        return AssetStoreResult::RejectedNoShape;
      case companionui::ParseResult::Malformed:
        return AssetStoreResult::RejectedFormat;
    }
  } else {
    return AssetStoreResult::RejectedFormat;
  }

  if (!Storage.ensureDirectoryExists(peerDir(peerKey).c_str())) return AssetStoreResult::RejectedStorage;
  if (!writeWholeFile(assetPath(peerKey, assetId), data, len)) return AssetStoreResult::RejectedStorage;
  return AssetStoreResult::Stored;
}

bool hasUiDeclaration(const char* peerKey) { return Storage.exists(assetPath(peerKey, kAssetUiDeclaration).c_str()); }

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

std::string commitImage(const char* peerKey, const std::string& stagedPath, const uint8_t* contentId,
                        size_t contentIdLen) {
  if (!Storage.ensureDirectoryExists(imagesDir(peerKey).c_str())) {
    LOG_ERR("CPEER", "could not create images dir for %s", peerKey);
    return std::string();
  }

  JsonDocument doc;
  loadImagesIndex(peerKey, doc);
  const uint32_t seq = doc["nextSeq"] | 0u;
  doc["nextSeq"] = seq + 1;
  const uint32_t slot = seq % kMaxImagesPerPeer;
  const std::string destPath = imageSlotPath(peerKey, slot);

  // Every slot is reused every kMaxImagesPerPeer pushes, so the index is keyed
  // on slot, not append-only — the stale entry for this slot (if any) must be
  // dropped before adding the new one, or the array would grow without bound.
  JsonArray images = doc["images"].as<JsonArray>();
  for (size_t i = 0; i < images.size(); ++i) {
    if ((images[i]["slot"] | 0xFFFFFFFFu) == slot) {
      images.remove(i);
      break;
    }
  }

  // Renaming over an existing path is not guaranteed on every filesystem;
  // clear the slot first so this behaves the same whether or not it's reused.
  Storage.remove(destPath.c_str());
  if (!Storage.rename(stagedPath.c_str(), destPath.c_str())) {
    LOG_ERR("CPEER", "could not move staged image %s to %s", stagedPath.c_str(), destPath.c_str());
    return std::string();
  }

  JsonObject entry = images.add<JsonObject>();
  entry["slot"] = slot;
  entry["seq"] = seq;
  if (contentIdLen > 0) {
    const size_t n = contentIdLen > kMaxImageContentIdLen ? kMaxImageContentIdLen : contentIdLen;
    char hex[kMaxImageContentIdLen * 2 + 1];
    hexEncode(contentId, n, hex);
    entry["cid"] = hex;
  }

  if (!saveImagesIndex(peerKey, doc)) {
    LOG_ERR("CPEER", "could not save images index for %s", peerKey);
    // The image itself is safely on disk at destPath even if the index write
    // failed; worst case is a gallery that briefly forgets this one entry
    // (it will be overwritten in kMaxImagesPerPeer more pushes regardless),
    // not a lost or corrupt image.
  }
  return destPath;
}

size_t listImages(const char* peerKey, ImageEntry* out, size_t maxImages) {
  JsonDocument doc;
  if (!loadImagesIndex(peerKey, doc) || maxImages == 0) return 0;
  if (maxImages > kMaxImagesPerPeer) maxImages = kMaxImagesPerPeer;
  JsonArray images = doc["images"].as<JsonArray>();

  // Selection sort by seq ascending (oldest first) over at most
  // kMaxImagesPerPeer entries — trivially cheap, and avoids allocating a
  // sorted copy of the index. `used` is sized to the same cap because
  // commitImage() guarantees at most one entry per slot value.
  bool used[kMaxImagesPerPeer] = {false};
  size_t written = 0;
  while (written < maxImages) {
    int bestIndex = -1;
    uint32_t bestSeq = 0;
    for (size_t i = 0; i < images.size() && i < kMaxImagesPerPeer; ++i) {
      if (used[i]) continue;
      const uint32_t seq = images[i]["seq"] | 0u;
      if (bestIndex < 0 || seq < bestSeq) {
        bestIndex = static_cast<int>(i);
        bestSeq = seq;
      }
    }
    if (bestIndex < 0) break;
    used[bestIndex] = true;

    const uint32_t slot = images[bestIndex]["slot"] | 0u;
    out[written].path = imageSlotPath(peerKey, slot);
    out[written].seq = bestSeq;
    const char* cid = images[bestIndex]["cid"] | "";
    snprintf(out[written].contentIdHex, sizeof(out[written].contentIdHex), "%s", cid);
    ++written;
  }
  return written;
}

ListStoreResult storeListDocument(const char* peerKey, const uint8_t* data, size_t len) {
  if (!Storage.ensureDirectoryExists(peerDir(peerKey).c_str())) return ListStoreResult::RejectedStorage;

  const std::string tmpPath = listsTmpPath(peerKey);
  // Clear any temp file left behind by a push that never reached a clean
  // END (crash, disconnect mid-transfer) -- openFileForWrite() below would
  // otherwise append to or otherwise collide with it depending on HalFile's
  // open-mode semantics.
  Storage.remove(tmpPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("CPEER", tmpPath, file)) return ListStoreResult::RejectedStorage;

  JsonListWriter writer(file);
  const companiontodo::ParseResult parseResult = companiontodo::parseDocument(data, len, writer);
  writer.finish();
  file.flush();
  const bool writeOk = writer.ok();
  file.close();

  if (parseResult != companiontodo::ParseResult::Ok || !writeOk) {
    // Never renamed onto lists.json -- the peer's previously-good document
    // (if any) is untouched. This temp file may itself be malformed JSON
    // (parseDocument() can fire some callbacks before discovering a later
    // byte is bad); that is fine, since nothing ever reads it back.
    Storage.remove(tmpPath.c_str());
    return parseResult != companiontodo::ParseResult::Ok ? ListStoreResult::RejectedFormat
                                                          : ListStoreResult::RejectedStorage;
  }

  // Same "clear the destination first" discipline as commitImage() --
  // renaming over an existing path is not guaranteed on every filesystem.
  const std::string destPath = listsPath(peerKey);
  Storage.remove(destPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("CPEER", "could not move %s to %s", tmpPath.c_str(), destPath.c_str());
    Storage.remove(tmpPath.c_str());
    return ListStoreResult::RejectedStorage;
  }
  return ListStoreResult::Stored;
}

bool loadListDocument(const char* peerKey, companiontodo::Visitor& visitor) {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(listsPath(peerKey).c_str(), doc)) return false;

  const uint32_t revision = doc["revision"] | 0u;
  visitor.onDocument(revision);
  for (JsonObject list : doc["lists"].as<JsonArray>()) {
    const uint16_t listId = static_cast<uint16_t>(list["listId"] | 0u);
    const char* title = list["title"] | "";
    visitor.onListStart(listId, title, static_cast<uint8_t>(strnlen(title, 255)));
    for (JsonObject group : list["groups"].as<JsonArray>()) {
      const uint16_t groupId = static_cast<uint16_t>(group["groupId"] | 0u);
      const char* label = group["label"] | "";
      visitor.onGroupStart(groupId, label, static_cast<uint8_t>(strnlen(label, 255)));
      for (JsonObject item : group["items"].as<JsonArray>()) {
        const uint16_t itemId = static_cast<uint16_t>(item["itemId"] | 0u);
        const bool checked = item["checked"] | false;
        const char* text = item["text"] | "";
        visitor.onItem(itemId, checked, text, static_cast<uint8_t>(strnlen(text, 255)));
      }
      visitor.onGroupEnd(groupId);
    }
    visitor.onListEnd(listId);
  }
  return true;
}

std::string displayName(const char* peerKey) {
  JsonDocument doc;
  if (!loadIndex(doc)) return std::string();
  JsonObject entry = findPeer(doc, peerKey);
  if (entry.isNull()) return std::string();
  return std::string(entry["name"] | "");
}

std::string userName(const char* peerKey) {
  JsonDocument doc;
  if (!loadIndex(doc)) return std::string();
  JsonObject entry = findPeer(doc, peerKey);
  if (entry.isNull()) return std::string();
  return std::string(entry["user"] | "");
}

bool isImageCapable(const char* peerKey) {
  // readAssetBody() strips the opaque tag, so this is the declaration *body*:
  // shape byte first, then the button count. It walked the layout by hand
  // until the shape byte was added — a second copy of the offset arithmetic
  // that would have silently read the shape as a button count. Both callers
  // now share the one parser.
  uint8_t raw[kMaxUiDeclarationLen];
  const size_t len = readAssetBody(peerKey, kAssetUiDeclaration, raw, sizeof(raw));
  companionui::DeclarationInfo info;
  if (companionui::parseBody(raw, len, &info) != companionui::ParseResult::Ok) return false;
  return (info.capabilities & companionble::kUiCapabilityImageGallery) != 0;
}

bool readDeclaredShape(const char* peerKey, companionble::ContentShape* out) {
  // Same read-then-shared-parse shape as isImageCapable() above, and for the
  // same reason: the offsets live in exactly one place (companionui::parseBody)
  // so a layout change cannot silently desync one caller from the others.
  uint8_t raw[kMaxUiDeclarationLen];
  const size_t len = readAssetBody(peerKey, kAssetUiDeclaration, raw, sizeof(raw));
  companionui::DeclarationInfo info;
  if (companionui::parseBody(raw, len, &info) != companionui::ParseResult::Ok) return false;
  if (out) *out = info.shape;
  return true;
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
