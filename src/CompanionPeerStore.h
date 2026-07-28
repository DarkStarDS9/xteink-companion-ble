#pragma once

// Per-peer storage for the Companion Display Protocol v6.
//
// A "peer" is one phone's copy of one app: the pair (appId, installId). Each
// gets a directory on the SD card holding its pairing token, its button map,
// its sleep-screen icon and a scratch area for staged content. See
// docs/companion-multi-app-design.md §4 and the storage section of
// docs/companion-display-protocol.md.
//
// NOTHING HERE IS RESIDENT. Every function reads what it needs from SD, uses
// it, and drops it. That is deliberate and is what makes the whole v6 feature
// set fit the memory budget in docs/companion-multi-app-design.md §11: the peer
// index at its 32-entry cap is several kilobytes of JSON, which this part has
// no room to hold across a session on top of NimBLE's ~63 KB and the 48 KB
// framebuffer. The only per-peer state the firmware keeps in RAM is the
// foreground peer's key, name and button map, held by CompanionModeActivity.
//
// Layout:
//   /.crosspoint/companion/peers.json           index: key -> appId, installId, name, seq
//   /.crosspoint/companion/peers/<key>/token.bin    16 raw bytes
//   /.crosspoint/companion/peers/<key>/buttons.bin  the wire asset: 4-byte tag + body
//   /.crosspoint/companion/peers/<key>/icon.bin     the wire asset: 4-byte tag + bitmap
//   /.crosspoint/companion/peers/<key>/data/        scratch (staged image)
//
// Assets are stored as the exact bytes the phone pushed, tag included, rather
// than re-encoded as JSON. The tag has to survive verbatim anyway (the device
// never interprets it — see the protocol doc's "Asset digests"), and a binary
// blob costs no parser and no JsonDocument.

#include <cstddef>
#include <cstdint>
#include <string>

namespace companionpeer {

// Bytes in an appId / installId / pairing token. All opaque to the firmware,
// which only ever compares them for equality.
inline constexpr size_t kIdLen = 16;
inline constexpr size_t kTokenLen = 16;

// A peerKey is 8 hex chars plus a NUL.
inline constexpr size_t kPeerKeyLen = 9;

// Hard cap on enrolled peers. Not a disk-space concern — peers.json is parsed
// into a transient JsonDocument, so an unbounded index is an unbounded
// allocation, which SCOPE.md §3 forbids. The least recently seen peer is
// evicted (directory and all) when a 33rd enrolls.
inline constexpr size_t kMaxPeers = 32;

// Most icon tiles the sleep screen draws (6 x 3 at 64x64 on an 800x480 panel).
// A display cap, separate from kMaxPeers: more than this many paired apps is
// implausible, and a grid that scrolls would need input the sleep screen does
// not take.
inline constexpr size_t kMaxIconTiles = 18;

// Longest display name stored, matching the protocol's own 24-byte cap.
inline constexpr size_t kMaxNameLen = 24;

// Asset ids, matching the content field ids they arrive as.
inline constexpr uint8_t kAssetButtonMap = 0x05;
inline constexpr uint8_t kAssetIcon = 0x06;

// Longest button-map asset accepted (tag included). Far more than the seven
// physical buttons need; a longer push is rejected rather than truncated,
// because a half-stored control scheme is worse than none.
inline constexpr size_t kMaxButtonMapLen = 512;

// Derives the directory name for a peer: the first 8 hex chars of SHA-256 over
// appId || installId. Deterministic, so the same phone:app pair always lands in
// the same directory without the index having to be searched by id.
void makePeerKey(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen], char keyOut[kPeerKeyLen]);

// Looks the peer up in the index; creates its directory and index entry if it
// is new. Bumps its sequence number either way (see touch()). Returns false
// only if the SD card is unusable.
bool ensurePeer(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen], const char* displayName,
                char keyOut[kPeerKeyLen]);

// True if this peer already has an index entry — i.e. the user has confirmed a
// pairing for it at some point.
bool isEnrolled(const uint8_t appId[kIdLen], const uint8_t installId[kIdLen]);

// Constant-time-ish token comparison. Returns false when the peer has no token
// stored, which is the same outcome as a wrong one: show the pairing prompt.
bool tokenMatches(const char* peerKey, const uint8_t* token, size_t tokenLen);

// Fills `out` with a fresh random token and persists it, replacing any previous
// one. Called only after the user confirms a pairing on-device.
bool issueToken(const char* peerKey, uint8_t out[kTokenLen]);

// Reads the stored 4-byte tag of an asset, or writes four zero bytes when the
// device holds no copy of it — which is exactly what HELLO_OK's digest block
// reports for "push me this".
void assetTag(const char* peerKey, uint8_t assetId, uint8_t out[4]);

// Stores an asset exactly as pushed (tag included). Rejects a button map that
// does not parse or an icon of the wrong length — see the ASSET_ACK result
// codes in the protocol doc.
enum class AssetStoreResult : uint8_t {
  Stored = 0x00,
  RejectedSize = 0x01,
  RejectedFormat = 0x02,
  RejectedStorage = 0x03,
};
AssetStoreResult storeAsset(const char* peerKey, uint8_t assetId, const uint8_t* data, size_t len,
                            size_t expectedIconBytes);

// True if this peer has a stored button map. ACQUIRE is refused without one:
// an app that has not said what its buttons do cannot reach the screen.
bool hasButtonMap(const char* peerKey);

// Reads an asset body (tag stripped) into `buf`. Returns the number of bytes
// read, 0 if absent or larger than `bufLen`.
size_t readAssetBody(const char* peerKey, uint8_t assetId, uint8_t* buf, size_t bufLen);

// Path of a file inside this peer's scratch directory, creating the directory
// if needed. Used for the staged image — per-peer rather than one global
// scratch file, so two apps staging at once cannot collide.
std::string dataFilePath(const char* peerKey, const char* fileName);

// Display name from the index, or an empty string.
std::string displayName(const char* peerKey);

// Marks the peer as most recently seen and evicts beyond kMaxPeers.
void touch(const char* peerKey);

// Fills `keysOut` with one peerKey per distinct appId, most recently seen
// first, for the sleep-screen icon grid — the same app paired from two phones
// is one tile, not two. Only peers that actually have an icon are listed.
// Returns how many were written.
size_t listIconTiles(char keysOut[][kPeerKeyLen], size_t maxTiles);

// Fills `keysOut` with every enrolled peerKey, most recently seen first.
// Returns how many were written. Unlike listIconTiles() this does not group by
// appId or require an icon — it is the raw index, for diagnostics.
size_t listPeers(char keysOut[][kPeerKeyLen], size_t maxPeers);

// True if this peer has a stored sleep-screen icon.
bool hasIcon(const char* peerKey);

// Deletes every peer directory and the index. Returns the device to its
// never-paired state so a first-contact enrollment can be re-tested without
// physically clearing the SD card between runs.
void forgetAllPeers();

// True if any peer is enrolled at all. Drives "Waiting for phone" vs the icon
// grid on an idle device.
bool anyEnrolled();

}  // namespace companionpeer
