#pragma once

#include <cstddef>
#include <cstdint>

#include "CompanionBle.h"
#include "CompanionTodoDocument.h"

// Streams a kFieldListDoc push to lists.json's JSON shape as
// companiontodo::parseDocument() walks it -- extracted out of
// CompanionPeerStore.cpp (which pulls in ArduinoJson/HalStorage and cannot be
// host-built, see that file's own note) so the buffering and escaping logic
// here has a host gtest suite (test/companion_list_json_writer/), the same
// split CompanionUiDeclaration.h and CompanionTodoDocument.h already use for
// their own codecs. The only thing this header depends on beyond
// <cstdint>/<cstddef> is CompanionBle.h (kMaxListItems) and
// CompanionTodoDocument.h (the Visitor interface), both themselves
// host-buildable.
//
// This does NOT depend on HalFile/HalStorage: it writes through the
// JsonWriteSink interface below instead, so CompanionPeerStore.cpp's
// production code wraps a HalFile in a one-line adapter and a host test can
// use a Sink that appends to a std::string.
namespace companionpeer {

// Sink a JsonListWriter flushes its buffer to. Production wraps a HalFile
// (see CompanionPeerStore.cpp); tests use an in-memory one.
class JsonWriteSink {
 public:
  virtual ~JsonWriteSink() = default;
  // Returns false on any failure. A false return latches JsonListWriter::ok()
  // false for the rest of the document -- see that class's comment.
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

// Visits a companiontodo document (wire push or, in principle, any other
// source that drives this Visitor interface) and streams it to `sink` as
// JSON, matching lists.json's shape exactly:
//   { "revision": N, "lists": [ { "listId", "title", "groups": [
//     { "groupId", "label", "items": [ { "itemId", "text", "checked" } ] }
//   ] } ] }
//
// Buffers output in a small fixed-size chunk and flushes to `sink` only when
// full (or at finish()) rather than issuing one `sink->write()` per byte.
// This matters because a production `sink->write()` is a HalFile::write(),
// which takes HalStorage's StorageLock mutex per call (HalStorage.cpp) --
// on the NimBLE host task, which every other task touching storage also
// contends with. A byte-at-a-time writer costs a 16 KB document on the order
// of 16,000 locked calls; batching into kBufferSize chunks cuts that by
// roughly two orders of magnitude.
//
// Also enforces companionble::kMaxListItems: companiontodo::parseDocument()
// has no early-stop signal (see its own header comment), so a document over
// the cap is still walked to completion and its JSON still buffered/flushed
// like any other -- but overCap() reports true once onItem() has fired more
// than kMaxListItems times, and the caller (CompanionPeerStore.cpp's
// storeListDocument()) discards the output rather than committing it. This
// is what keeps the read-back path (loadListDocument(), which
// re-materializes the whole document into an ArduinoJson JsonDocument)
// bounded -- see kMaxListItems's own comment in CompanionBle.h for the
// measurement behind the 512 figure.
class JsonListWriter : public companiontodo::Visitor {
 public:
  // Chosen to keep this class's own stack/member footprint small -- it is
  // instantiated as a local in storeListDocument(), which runs on the
  // NimBLE host task alongside the already-resident 16 KB g_activeBuf
  // reassembly buffer -- while still cutting a 16 KB document's write-call
  // count by roughly two orders of magnitude versus one call per byte.
  static constexpr size_t kBufferSize = 256;

  explicit JsonListWriter(JsonWriteSink& sink) : sink_(sink) {}

  // False if any underlying write failed. Checked by the caller instead of
  // trusting parseDocument()'s own Ok/Malformed verdict alone -- a full SD
  // card can fail a write in the middle of an otherwise well-formed
  // document, and that must not look like Stored either.
  bool ok() const { return ok_; }

  // True once more than companionble::kMaxListItems onItem() calls have
  // fired. See the class comment above for why the caller must check this
  // in addition to ok() and parseDocument()'s own verdict.
  bool overCap() const { return itemCount_ > companionble::kMaxListItems; }

  void onDocument(uint32_t revision) override;
  void onListStart(uint16_t listId, const char* title, uint8_t titleLen) override;
  void onGroupStart(uint16_t groupId, const char* label, uint8_t labelLen) override;
  void onItem(uint16_t itemId, bool checked, const char* text, uint8_t textLen) override;
  void onGroupEnd(uint16_t groupId) override;
  void onListEnd(uint16_t listId) override;

  // Closes the document and flushes any remaining buffered bytes. Always
  // safe to call, even on a document that never got past onDocument() (or
  // was never called at all) -- the caller discards this output entirely
  // unless parseDocument() returned Ok and ok()/overCap() both check out, so
  // an unbalanced close on a document that failed early is harmless. Must be
  // called before reading ok() for the last time: a failure in this final
  // flush is exactly the kind of late failure ok() exists to catch.
  void finish();

 private:
  void bufferByte(uint8_t b);
  void flush();
  void writeRaw(const char* s);
  void writeUint(uint32_t v);
  // Wire text is arbitrary bytes the phone chose (parseDocument() validates
  // structure, never UTF-8 well-formedness), so only what JSON itself
  // requires is escaped: quote, backslash, and control characters. Anything
  // else -- including multi-byte UTF-8 sequences -- passes through verbatim.
  void writeJsonString(const char* s, uint8_t len);

  JsonWriteSink& sink_;
  bool ok_ = true;
  size_t listIndex_ = 0;
  size_t groupIndex_ = 0;
  size_t itemIndex_ = 0;
  size_t itemCount_ = 0;
  uint8_t buf_[kBufferSize];
  size_t bufLen_ = 0;
};

}  // namespace companionpeer
