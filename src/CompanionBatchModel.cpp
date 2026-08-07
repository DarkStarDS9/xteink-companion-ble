#include "CompanionBatchModel.h"

#include <cstring>

void CompanionBatchModel::onField(uint8_t field, const uint8_t* data, size_t len, bool final,
                                   companionble::FieldOutcome outcome, uint8_t pushId, uint32_t nowMs) {
  const bool isTextField = field == companionble::kFieldTitle || field == companionble::kFieldBody;
  // "Idle" has to include the poison flag, or a batch whose only surviving
  // marker is the poison (first field dropped, nothing buffered yet) would look
  // like a fresh batch to the next field and restart the timeout clock.
  const bool wasIdle = !titleReady_ && !bodyReady_ && !poisoned_;
  // Only the final-flagged field's END names the batch. This still runs on
  // the Dropped path below: a dropped field's END still carried a real
  // pushId, and a poisoned batch is answered with it same as a clean one
  // (see poll()'s Discard resolution).
  if (final) batchPushId_ = pushId;
  if (outcome == companionble::FieldOutcome::Dropped) {
    // No data to copy — just poison the batch. The clock still has to start
    // here: if this is the batch's first field and the final flag never
    // arrives, poll()'s timeout is what clears the poison again.
    if (isTextField) poisoned_ = true;
    if (wasIdle && isTextField) batchStartMs_ = nowMs;
    if (final) commitReady_ = true;
    return;
  }
  if (field == companionble::kFieldTitle) {
    const size_t n = len > sizeof(titleBuf_) ? sizeof(titleBuf_) : len;
    memcpy(titleBuf_, data, n);
    titleLen_ = static_cast<uint16_t>(n);
    titleReady_ = true;
  } else if (field == companionble::kFieldTagState) {
    const size_t n = len > sizeof(tagStateBuf_) ? sizeof(tagStateBuf_) : len;
    memcpy(tagStateBuf_, data, n);
    tagStateLen_ = static_cast<uint8_t>(n);
    tagStateReady_ = true;
    // A title/body batch already in flight (or already poisoned) means this
    // tag state belongs to it; an idle handoff means it is a standalone tag
    // push and owes the batch machinery nothing.
    tagStateInBatch_ = !wasIdle;
  } else if (field == companionble::kFieldBody) {
    const size_t n = len > sizeof(bodyBuf_) ? sizeof(bodyBuf_) : len;
    memcpy(bodyBuf_, data, n);
    bodyLen_ = static_cast<uint16_t>(n);
    bodyReady_ = true;
  }
  if (wasIdle && isTextField) {
    batchStartMs_ = nowMs;
  }
  if (final) commitReady_ = true;
}

CompanionBatchModel::PollResult CompanionBatchModel::poll(uint32_t nowMs) {
  PollResult result;

  if (commitReady_) {
    result.resolution = Resolution::Commit;
  } else if ((titleReady_ || bodyReady_ || poisoned_) && batchStartMs_ != 0 && nowMs - batchStartMs_ > kTimeoutMs) {
    // Safety net: the final-flagged field's END never arrived in time (e.g.
    // the app crashed or lost the connection mid-push). Apply whatever we
    // have rather than leaving the screen stuck on stale content
    // indefinitely. A poisoned batch resolves here too -- it still has to be
    // *cleared*, or the poison would leak into the next batch; it is just
    // discarded rather than applied (see the poisoned_ handling below).
    result.resolution = Resolution::Commit;
    result.timedOut = true;
  }

  if (result.resolution == Resolution::Commit) {
    result.pushId = batchPushId_;
    if (batchStartMs_ != 0) {
      result.elapsedValid = true;
      result.elapsedSinceFirstFieldMs = nowMs - batchStartMs_;
    }
    if (poisoned_) {
      // A field of this batch was lost to a CHUNK sequence gap. Applying the
      // survivors would mix this content's body with the last one's title, so
      // the whole batch is discarded rather than committed.
      result.resolution = Resolution::Discard;
    } else {
      result.hasTitle = titleReady_;
      result.hasBody = bodyReady_;
    }
    titleReady_ = false;
    bodyReady_ = false;
    commitReady_ = false;
    batchStartMs_ = 0;
    poisoned_ = false;
    batchPushId_ = 0;
  }

  // Tag state resolves independently of the title/body outcome above -- see
  // PollResult::hasTagState's doc comment.
  if (tagStateReady_) {
    result.hasTagState = true;
    result.tagStateInBatch = tagStateInBatch_;
    tagStateReady_ = false;
    tagStateInBatch_ = false;
  }

  if (result.resolution == Resolution::Discard && result.tagStateInBatch) {
    // Tag state that rode this batch goes with it. Letting it through would
    // be the worst of the three outcomes: the stale content left on screen
    // would wear the *new* content's tags, i.e. a mark that belongs to
    // content the caller cannot see. A standalone tag push (tagStateInBatch
    // false) is untouched.
    result.hasTagState = false;
  }

  return result;
}

void CompanionBatchModel::reset() {
  titleReady_ = false;
  bodyReady_ = false;
  commitReady_ = false;
  batchStartMs_ = 0;
  poisoned_ = false;
  batchPushId_ = 0;
  tagStateReady_ = false;
  tagStateInBatch_ = false;
}

void CompanionBatchModel::resetOnDisconnect() {
  titleReady_ = false;
  bodyReady_ = false;
  commitReady_ = false;
  batchStartMs_ = 0;
  poisoned_ = false;
  batchPushId_ = 0;
  // Tag state belonging to the batch the link just killed goes with it —
  // otherwise it would surface on the next loop as if it were a standalone
  // tag push, marking whatever content is still on screen. A genuinely
  // standalone pending tag push is left alone.
  if (tagStateInBatch_) {
    tagStateReady_ = false;
    tagStateInBatch_ = false;
  }
}
