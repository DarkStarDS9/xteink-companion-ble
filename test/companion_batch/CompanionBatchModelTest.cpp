#include "CompanionBatchModel.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using Resolution = CompanionBatchModel::Resolution;

const uint8_t* bytesOf(const char* s) { return reinterpret_cast<const uint8_t*>(s); }

std::string titleOf(const CompanionBatchModel& model) {
  return std::string(reinterpret_cast<const char*>(model.titleData()), model.titleLen());
}

std::string bodyOf(const CompanionBatchModel& model) {
  return std::string(reinterpret_cast<const char*>(model.bodyData()), model.bodyLen());
}

TEST(CompanionBatchModel, CleanBatchCommitsWithFinalFieldsPushId) {
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);
  model.onField(companionble::kFieldBody, bytesOf("Body"), 4, /*final=*/true, companionble::FieldOutcome::Complete,
                /*pushId=*/42, /*nowMs=*/1010);

  const auto result = model.poll(1020);

  EXPECT_EQ(result.resolution, Resolution::Commit);
  EXPECT_TRUE(result.hasTitle);
  EXPECT_TRUE(result.hasBody);
  EXPECT_EQ(result.pushId, 42);
  EXPECT_FALSE(result.timedOut);
  EXPECT_EQ(titleOf(model), "Title");
  EXPECT_EQ(bodyOf(model), "Body");
}

TEST(CompanionBatchModel, FinalNeverArrivesTimesOutWithZeroPushId) {
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);

  // Still inside the window: nothing to resolve yet.
  EXPECT_EQ(model.poll(1000 + CompanionBatchModel::kTimeoutMs).resolution, Resolution::None);

  const auto result = model.poll(1000 + CompanionBatchModel::kTimeoutMs + 1);

  EXPECT_EQ(result.resolution, Resolution::Commit);
  EXPECT_TRUE(result.timedOut);
  EXPECT_EQ(result.pushId, 0);
  EXPECT_TRUE(result.hasTitle);
}

TEST(CompanionBatchModel, SlowButSteadyBatchDoesNotTimeOut) {
  // Total span exceeds kTimeoutMs, but each field lands well within
  // kTimeoutMs of the previous one -- a live, if slow, client must not be
  // punished by the same budget as one that has gone silent.
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);

  // This poll lands after the first field's own kTimeoutMs window, but the
  // second field arrives before this check -- so it must not resolve yet.
  model.onField(companionble::kFieldBody, bytesOf("Body"), 4, /*final=*/true, companionble::FieldOutcome::Complete,
                /*pushId=*/88, /*nowMs=*/1000 + CompanionBatchModel::kTimeoutMs - 100);

  const auto result = model.poll(1000 + CompanionBatchModel::kTimeoutMs + 500);

  EXPECT_EQ(result.resolution, Resolution::Commit);
  EXPECT_FALSE(result.timedOut);
  EXPECT_EQ(result.pushId, 88);
  EXPECT_TRUE(result.hasTitle);
  EXPECT_TRUE(result.hasBody);
  // Diagnostic elapsed time still measures from the batch's first field to
  // resolution, even though the timeout itself no longer does.
  EXPECT_TRUE(result.elapsedValid);
  EXPECT_EQ(result.elapsedSinceFirstFieldMs, CompanionBatchModel::kTimeoutMs + 500);
}

TEST(CompanionBatchModel, TimesOutAfterSilenceSinceLastFieldNotFirst) {
  // The batch's first field is well inside the window on its own, but the
  // client then goes silent for kTimeoutMs after its *second* field -- the
  // clock must be judged against that second field, not the first.
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);
  model.onField(companionble::kFieldBody, bytesOf("Body"), 4, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1500);

  // Not yet kTimeoutMs since the body field.
  EXPECT_EQ(model.poll(1500 + CompanionBatchModel::kTimeoutMs).resolution, Resolution::None);

  const auto result = model.poll(1500 + CompanionBatchModel::kTimeoutMs + 1);
  EXPECT_EQ(result.resolution, Resolution::Commit);
  EXPECT_TRUE(result.timedOut);
  EXPECT_TRUE(result.hasTitle);
  EXPECT_TRUE(result.hasBody);
}

TEST(CompanionBatchModel, PoisonDoesNotLeakIntoTheNextBatch) {
  CompanionBatchModel model;
  // First field of the batch is dropped -- poisons it and still starts the clock.
  model.onField(companionble::kFieldTitle, nullptr, 0, /*final=*/false, companionble::FieldOutcome::Dropped,
                /*pushId=*/0, /*nowMs=*/1000);
  // The final-flagged field's END still carries a real pushId even though the batch is poisoned.
  model.onField(companionble::kFieldBody, bytesOf("Body"), 4, /*final=*/true, companionble::FieldOutcome::Complete,
                /*pushId=*/7, /*nowMs=*/1010);

  const auto discarded = model.poll(1020);
  EXPECT_EQ(discarded.resolution, Resolution::Discard);
  EXPECT_EQ(discarded.pushId, 7);
  EXPECT_FALSE(discarded.hasTitle);
  EXPECT_FALSE(discarded.hasBody);

  // The next batch starts clean -- no poison, no stale pushId.
  model.onField(companionble::kFieldTitle, bytesOf("Next"), 4, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/2000);
  model.onField(companionble::kFieldBody, bytesOf("Body2"), 5, /*final=*/true, companionble::FieldOutcome::Complete,
                /*pushId=*/9, /*nowMs=*/2010);

  const auto committed = model.poll(2020);
  EXPECT_EQ(committed.resolution, Resolution::Commit);
  EXPECT_EQ(committed.pushId, 9);
  EXPECT_TRUE(committed.hasTitle);
  EXPECT_TRUE(committed.hasBody);
  EXPECT_EQ(titleOf(model), "Next");
  EXPECT_EQ(bodyOf(model), "Body2");
}

TEST(CompanionBatchModel, PoisonOnlyBatchStillStartsTheTimeoutClock) {
  // wasIdle rule: a batch whose only surviving marker is the poison flag
  // (first field dropped, nothing ever buffered) must still start the clock,
  // or it would never resolve without a final flag that is never coming.
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, nullptr, 0, /*final=*/false, companionble::FieldOutcome::Dropped,
                /*pushId=*/0, /*nowMs=*/1000);

  EXPECT_EQ(model.poll(1000 + CompanionBatchModel::kTimeoutMs).resolution, Resolution::None);

  const auto result = model.poll(1000 + CompanionBatchModel::kTimeoutMs + 1);
  EXPECT_EQ(result.resolution, Resolution::Discard);
  EXPECT_TRUE(result.timedOut);
  EXPECT_EQ(result.pushId, 0);  // no final-flagged field ever arrived
}

TEST(CompanionBatchModel, DroppedFinalFieldLatchesPushIdAndSetsCommitReady) {
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);
  // The body is the field that gets lost, but its END still carried the final flag and a pushId.
  model.onField(companionble::kFieldBody, nullptr, 0, /*final=*/true, companionble::FieldOutcome::Dropped,
                /*pushId=*/55, /*nowMs=*/1010);

  const auto result = model.poll(1020);
  EXPECT_EQ(result.resolution, Resolution::Discard);
  EXPECT_EQ(result.pushId, 55);
}

TEST(CompanionBatchModel, TagStatePushedMidBatchDiesWithThePoisonedBatch) {
  CompanionBatchModel model;
  const uint8_t tagData[] = {0x01, 0x02, 0x03};
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);
  // Arrives while the title/body batch is already in flight -- bound to its fate.
  model.onField(companionble::kFieldTagState, tagData, sizeof(tagData), /*final=*/false,
                companionble::FieldOutcome::Complete, /*pushId=*/0, /*nowMs=*/1005);
  model.onField(companionble::kFieldBody, nullptr, 0, /*final=*/true, companionble::FieldOutcome::Dropped,
                /*pushId=*/77, /*nowMs=*/1010);

  const auto result = model.poll(1020);
  EXPECT_EQ(result.resolution, Resolution::Discard);
  EXPECT_EQ(result.pushId, 77);
  EXPECT_TRUE(result.tagStateInBatch);
  EXPECT_FALSE(result.hasTagState);  // suppressed -- went down with the poisoned batch
}

TEST(CompanionBatchModel, StandaloneTagStateSurvivesIndependently) {
  CompanionBatchModel model;
  const uint8_t tagData[] = {0x05, 0x06};
  // No title/body pending -- this is a standalone tag push (e.g. a plain
  // read-later toggle with no content change), final-flagged on its own.
  model.onField(companionble::kFieldTagState, tagData, sizeof(tagData), /*final=*/true,
                companionble::FieldOutcome::Complete, /*pushId=*/33, /*nowMs=*/1000);

  const auto result = model.poll(1010);
  EXPECT_EQ(result.resolution, Resolution::Commit);
  EXPECT_EQ(result.pushId, 33);
  EXPECT_FALSE(result.hasTitle);
  EXPECT_FALSE(result.hasBody);
  EXPECT_TRUE(result.hasTagState);
  EXPECT_FALSE(result.tagStateInBatch);
}

TEST(CompanionBatchModel, ResetClearsEverythingIncludingTheClock) {
  CompanionBatchModel model;
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);

  model.reset();

  // Had the clock survived, this would time out (>kTimeoutMs since 1000).
  const auto result = model.poll(1000 + CompanionBatchModel::kTimeoutMs + 1);
  EXPECT_EQ(result.resolution, Resolution::None);
}

TEST(CompanionBatchModel, ResetOnDisconnectPreservesAStandaloneTagPush) {
  CompanionBatchModel model;
  const uint8_t tagData[] = {0x09};
  // Standalone tag push, not yet drained by poll() when the link drops.
  model.onField(companionble::kFieldTagState, tagData, sizeof(tagData), /*final=*/true,
                companionble::FieldOutcome::Complete, /*pushId=*/5, /*nowMs=*/1000);

  model.resetOnDisconnect();

  const auto result = model.poll(1010);
  EXPECT_EQ(result.resolution, Resolution::None);  // commitReady was cleared with the rest of the batch state
  EXPECT_TRUE(result.hasTagState);                 // but the standalone tag push itself survives
  EXPECT_FALSE(result.tagStateInBatch);
}

TEST(CompanionBatchModel, ResetOnDisconnectDropsTagStateThatRodeTheBatch) {
  CompanionBatchModel model;
  const uint8_t tagData[] = {0x0A, 0x0B};
  model.onField(companionble::kFieldTitle, bytesOf("Title"), 5, /*final=*/false, companionble::FieldOutcome::Complete,
                /*pushId=*/0, /*nowMs=*/1000);
  model.onField(companionble::kFieldTagState, tagData, sizeof(tagData), /*final=*/false,
                companionble::FieldOutcome::Complete, /*pushId=*/0, /*nowMs=*/1005);

  model.resetOnDisconnect();

  const auto result = model.poll(1010);
  EXPECT_EQ(result.resolution, Resolution::None);
  EXPECT_FALSE(result.hasTagState);  // gone with the batch it rode
}

}  // namespace
