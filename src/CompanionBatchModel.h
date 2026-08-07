#pragma once

#include <cstddef>
#include <cstdint>

#include "CompanionBle.h"

// The title/body/tag-state atomic-batch state machine, extracted out of
// CompanionModeActivity.cpp's onContentField()/loop() so its rules can be
// covered by a host-side gtest suite instead of only hardware regressions
// (see test/companion_batch/).
//
// NOT thread-safe: every method here assumes the caller already holds
// whatever lock serializes it against the NimBLE host task (CompanionModeActivity
// keeps g_mux around every call, exactly as before this extraction). No
// FreeRTOS/millis() call lives inside this class -- the clock is always the
// caller-supplied `nowMs`, which is what makes this host-buildable.
//
// A "batch" is one or more of {title, body, tag-state} fields pushed under a
// single kFinalFieldFlag-marked END. The device owes exactly one commit/discard
// resolution per batch, decided the moment the final-flagged field's END
// arrives (onField(..., final=true, ...)) or, failing that, kTimeoutMs after
// the batch's first field -- see poll()'s doc comment.
class CompanionBatchModel {
 public:
  // How long a batch is allowed to sit with a field pending and no
  // final-flagged END before poll() gives up waiting and applies/discards
  // whatever arrived anyway. Safety net for an app crash or a disconnect
  // mid-push that leaves the screen stuck on stale content indefinitely.
  static constexpr uint32_t kTimeoutMs = 3000;

  enum class Resolution : uint8_t {
    None,     // nothing to resolve this poll
    Commit,   // apply whatever survived (hasTitle/hasBody/hasTagState say what)
    Discard,  // a field was lost to a CHUNK sequence gap; nothing in this batch applies
  };

  struct PollResult {
    Resolution resolution = Resolution::None;

    // Only meaningful when resolution == Commit.
    bool hasTitle = false;
    bool hasBody = false;
    bool timedOut = false;  // resolved via kTimeoutMs rather than a final-flagged END

    // Tag state resolves independently of the title/body commit/discard
    // outcome -- a standalone tag push (not riding a title/body batch) must
    // keep applying immediately even while some unrelated batch is in
    // trouble, so this is drained on every poll() regardless of `resolution`.
    // The one exception: tag state that DID ride a batch that resolves to
    // Discard in this same poll goes down with it (see poll()'s doc comment)
    // -- hasTagState is forced false in that case.
    bool hasTagState = false;
    bool tagStateInBatch = false;

    // The pushId of the final-flagged field's END. Valid for both Commit and
    // Discard; 0 ("no answer wanted") when the timeout safety net fired
    // before any field of the batch was ever final-flagged.
    uint8_t pushId = 0;

    // nowMs minus the batch's first-field timestamp, i.e. how long it sat
    // between its first field landing and this resolution -- diagnostic only
    // (push-to-visible time nothing else accounts for). Valid (elapsedValid
    // true) whenever the batch actually had a text field staged before it
    // resolved; false for e.g. a standalone tag-only push, which never starts
    // the batch clock.
    uint32_t elapsedSinceFirstFieldMs = 0;
    bool elapsedValid = false;
  };

  // Records one field's arrival. `field` is companionble::kFieldTitle /
  // kFieldBody / kFieldTagState (kFinalFieldFlag already stripped); `final`
  // mirrors kFinalFieldFlag from that field's START packet. `outcome` ==
  // Dropped means `data`/`len` do not describe a field (a v10 CHUNK sequence
  // gap cost this field entirely) -- the batch is poisoned rather than
  // copying anything.
  void onField(uint8_t field, const uint8_t* data, size_t len, bool final, companionble::FieldOutcome outcome,
               uint8_t pushId, uint32_t nowMs);

  // Called every loop() tick. Resolves the batch (Commit or Discard) once its
  // final-flagged field's END has arrived or kTimeoutMs has elapsed since its
  // first field, and separately reports any tag state that has arrived
  // (whether or not the title/body batch resolves this call) -- see
  // PollResult's doc comments for exactly how those two outcomes compose.
  PollResult poll(uint32_t nowMs);

  // Full clear: every buffer, flag and the batch clock. For activity
  // teardown (onExit()), where nothing pending should survive.
  void reset();

  // Partial clear, for a link drop mid-batch: title/body/commit/poison/pushId
  // and the batch clock are cleared unconditionally (nobody is left to answer
  // them), but a tag-state push that is NOT bound to this batch (a standalone
  // push, not yet drained by poll()) is left alone so it can still apply on
  // the next tick even though the connection that carried it just died. Tag
  // state that WAS bound to the batch the link just killed goes with it --
  // otherwise it would surface on the next loop() as if it were a standalone
  // push, marking content that has nothing to do with it.
  void resetOnDisconnect();

  const uint8_t* titleData() const { return titleBuf_; }
  uint16_t titleLen() const { return titleLen_; }
  const uint8_t* bodyData() const { return bodyBuf_; }
  uint16_t bodyLen() const { return bodyLen_; }
  const uint8_t* tagStateData() const { return tagStateBuf_; }
  uint8_t tagStateLen() const { return tagStateLen_; }

 private:
  uint8_t titleBuf_[companionble::kMaxFieldLen];
  uint16_t titleLen_ = 0;
  bool titleReady_ = false;

  uint8_t bodyBuf_[companionble::kMaxFieldLen];
  uint16_t bodyLen_ = 0;
  bool bodyReady_ = false;

  uint8_t tagStateBuf_[1 + 2 * companionble::kMaxTags];
  uint8_t tagStateLen_ = 0;
  bool tagStateReady_ = false;
  // Whether the pending tag state arrived *inside* a title/body batch (clients
  // push it last, under the same final flag) or on its own -- see
  // resetOnDisconnect()'s doc comment and PollResult::tagStateInBatch.
  bool tagStateInBatch_ = false;

  // millis()-equivalent timestamp (caller-supplied nowMs) of the first pending
  // field of the current batch; 0 when idle.
  uint32_t batchStartMs_ = 0;

  // Set when any title/body field of the current batch arrived as
  // FieldOutcome::Dropped. A batch that lost a field fails as a whole --
  // committing the rest paints new content under stale framing.
  bool poisoned_ = false;

  // The pushId of this batch, as chosen by the client -- latched only from
  // the final-flagged field's END (see onField()'s doc comment).
  uint8_t batchPushId_ = 0;

  // Set once a field's END arrives with kFinalFieldFlag set.
  bool commitReady_ = false;
};
