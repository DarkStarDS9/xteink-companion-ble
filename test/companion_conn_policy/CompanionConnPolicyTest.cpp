#include "CompanionConnPolicy.h"

#include <gtest/gtest.h>

namespace {

using ConnProfile = CompanionConnPolicy::ConnProfile;

TEST(CompanionConnPolicy, ConnectRequestsSessionOnceAndDoesNotSpamWithinGrace) {
  CompanionConnPolicy policy;

  const auto req = policy.onConnect(1000);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->profile, ConnProfile::Session);
  EXPECT_EQ(req->intervalUnits, CompanionConnPolicy::kConnIntervalSessionUnits);
  EXPECT_EQ(req->intervalMaxUnits, CompanionConnPolicy::kConnIntervalSessionMaxUnits);
  EXPECT_EQ(req->latencyUnits, CompanionConnPolicy::kConnLatencySession);
  EXPECT_EQ(req->timeoutUnits, CompanionConnPolicy::kConnTimeoutSessionUnits);
  EXPECT_TRUE(policy.inFlight());

  // No grant has landed yet -- every tick inside the grace window must stay
  // quiet rather than starting a second procedure (which would be the one
  // that gets dropped).
  for (uint32_t t = 1001; t <= 1000 + CompanionConnPolicy::kConnParamsGraceMs; t += 250) {
    EXPECT_FALSE(policy.step(t).has_value()) << "t=" << t;
  }
}

TEST(CompanionConnPolicy, DesiredChangeWhileInFlightIsDeferredThenRetriedNotDropped) {
  // Shape of the 2026-08-06 bug: a relax request and a tighten request
  // overlapped, and the second was silently dropped because only one
  // procedure may be in flight at a time. The fix keeps desired/current/
  // requested as three separate variables so a change that arrives mid-flight
  // surfaces as soon as the outstanding one resolves, instead of being lost.
  CompanionConnPolicy policy;

  const auto sessionReq = policy.onConnect(1000);
  ASSERT_TRUE(sessionReq.has_value());
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 1010);
  ASSERT_EQ(policy.currentProfile(), ConnProfile::Session);

  // Nobody holds the screen for a full holdoff -- desired flips to Idle.
  // setForegroundActive() is level-triggered (called every tick with the
  // live nowMs, same as CompanionBle.cpp's tick() does), so the second call
  // here is what actually observes the holdoff having elapsed.
  policy.setForegroundActive(false, 2000);
  policy.setForegroundActive(false, 2000 + CompanionConnPolicy::kIdleHoldoffMs);
  const auto idleReq = policy.step(2000 + CompanionConnPolicy::kIdleHoldoffMs);
  ASSERT_TRUE(idleReq.has_value());
  EXPECT_EQ(idleReq->profile, ConnProfile::Idle);
  ASSERT_TRUE(policy.inFlight());

  // A foreground reacquisition arrives while that Idle request is still
  // outstanding. Must be deferred, not dropped.
  const uint32_t midFlightMs = 2000 + CompanionConnPolicy::kIdleHoldoffMs + 100;
  policy.setForegroundActive(true, midFlightMs);
  EXPECT_FALSE(policy.step(midFlightMs).has_value());

  // The outstanding Idle request finally gets its grant.
  policy.onGrant(idleReq->intervalUnits, idleReq->latencyUnits, idleReq->timeoutUnits, midFlightMs + 50);
  ASSERT_EQ(policy.currentProfile(), ConnProfile::Idle);

  // The deferred Session desire surfaces on the very next step() -- retried,
  // not lost.
  const auto retried = policy.step(midFlightMs + 60);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(retried->profile, ConnProfile::Session);
}

TEST(CompanionConnPolicy, NoIdleUntilTheHoldoffFullyElapses) {
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(0);
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 5);

  policy.setForegroundActive(false, 1000);

  policy.setForegroundActive(false, 1000 + CompanionConnPolicy::kIdleHoldoffMs - 1);
  EXPECT_FALSE(policy.step(1000 + CompanionConnPolicy::kIdleHoldoffMs - 1).has_value());

  policy.setForegroundActive(false, 1000 + CompanionConnPolicy::kIdleHoldoffMs);
  const auto idleReq = policy.step(1000 + CompanionConnPolicy::kIdleHoldoffMs);
  ASSERT_TRUE(idleReq.has_value());
  EXPECT_EQ(idleReq->profile, ConnProfile::Idle);
}

TEST(CompanionConnPolicy, ForegroundReacquisitionResetsTheHoldoffClock) {
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(0);
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 5);

  policy.setForegroundActive(false, 1000);
  // Reacquired just before the old clock would have earned Idle.
  policy.setForegroundActive(true, 1000 + CompanionConnPolicy::kIdleHoldoffMs - 1);
  // Lost again -- the clock restarts here.
  policy.setForegroundActive(false, 1000 + CompanionConnPolicy::kIdleHoldoffMs);

  // The ORIGINAL clock would have expired by now; the reset one has not.
  const uint32_t almostThere = 1000 + CompanionConnPolicy::kIdleHoldoffMs + CompanionConnPolicy::kIdleHoldoffMs - 1;
  policy.setForegroundActive(false, almostThere);
  EXPECT_FALSE(policy.step(almostThere).has_value());

  policy.setForegroundActive(false, 1000 + 2 * CompanionConnPolicy::kIdleHoldoffMs);
  const auto idleReq = policy.step(1000 + 2 * CompanionConnPolicy::kIdleHoldoffMs);
  ASSERT_TRUE(idleReq.has_value());
  EXPECT_EQ(idleReq->profile, ConnProfile::Idle);
}

TEST(CompanionConnPolicy, HoldoffClockStartsAtConnectNotAtFirstObservedAbsence) {
  // 81f0947c's specific anti-flap point: the handshake window has no
  // foreground session either, so if the clock only started on the first
  // setForegroundActive(false, ...) call, a connection would earn Idle at
  // exactly the same wall-clock moment as one that started the clock at
  // connect -- the two are indistinguishable unless the clock is checked
  // against the CONNECT timestamp specifically.
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(1000);  // holdoff clock starts here
  ASSERT_TRUE(sessionReq.has_value());
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 1010);

  // tick() calls setForegroundActive() every tick regardless of whether a
  // session has ever held the screen -- the first call here happens well
  // after connect, simulating a slow first tick.
  const uint32_t firstTickMs = 1000 + CompanionConnPolicy::kIdleHoldoffMs - 1;
  policy.setForegroundActive(false, firstTickMs);
  EXPECT_FALSE(policy.step(firstTickMs).has_value());

  policy.setForegroundActive(false, 1000 + CompanionConnPolicy::kIdleHoldoffMs);
  const auto idleReq = policy.step(1000 + CompanionConnPolicy::kIdleHoldoffMs);
  ASSERT_TRUE(idleReq.has_value());
  EXPECT_EQ(idleReq->profile, ConnProfile::Idle);
}

TEST(CompanionConnPolicy, GrantAnywhereInTheRequestedRangeSettlesTheProfile) {
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(0);
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 5);
  ASSERT_EQ(policy.currentProfile(), ConnProfile::Session);

  policy.setForegroundActive(false, 1000);
  policy.setForegroundActive(false, 1000 + CompanionConnPolicy::kIdleHoldoffMs);
  const auto idleReq = policy.step(1000 + CompanionConnPolicy::kIdleHoldoffMs);
  ASSERT_TRUE(idleReq.has_value());
  ASSERT_LT(idleReq->intervalUnits, idleReq->intervalMaxUnits);  // Idle actually has a spread to test range-match on

  // iOS is measured to grant Interval Max in practice -- a grant at the max
  // of the requested range, not just the min, must still count as a match.
  policy.onGrant(idleReq->intervalMaxUnits, idleReq->latencyUnits, idleReq->timeoutUnits,
                 1000 + CompanionConnPolicy::kIdleHoldoffMs + 50);
  EXPECT_EQ(policy.currentProfile(), ConnProfile::Idle);
  EXPECT_FALSE(policy.inFlight());
}

TEST(CompanionConnPolicy, ForeignGrantDoesNotSettleTheProfile) {
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(0);
  ASSERT_TRUE(sessionReq.has_value());

  // e.g. the central's own opening parameters, unrelated to what was asked.
  policy.onGrant(48, 5, 999, 5);
  EXPECT_EQ(policy.currentProfile(), ConnProfile::Idle);  // unchanged from the fresh-connection baseline
  EXPECT_TRUE(policy.inFlight());
}

TEST(CompanionConnPolicy, SettledLinkEmitsNoFurtherRequests) {
  CompanionConnPolicy policy;
  const auto sessionReq = policy.onConnect(0);
  policy.onGrant(sessionReq->intervalUnits, sessionReq->latencyUnits, sessionReq->timeoutUnits, 5);
  policy.setForegroundActive(true, 10);

  for (uint32_t t = 10; t < 100000; t += 5000) {
    EXPECT_FALSE(policy.step(t).has_value()) << "t=" << t;
  }
}

TEST(CompanionConnPolicy, BothProfilesSatisfyTheSupervisionTimeoutEnvelope) {
  // timeout(ms) > maxInterval(ms) * (latency+1) * 3 -- the 0x22-risk envelope
  // (HCI 0x22 LMP/LL response timeout has been directly observed on this
  // hardware from colliding LL procedures; a timeout too tight against the
  // granted cadence is the other way to trip a supervision timeout).
  auto envelopeHolds = [](uint16_t intervalMaxUnits, uint16_t latencyUnits, uint16_t timeoutUnits) {
    const double intervalMaxMs = intervalMaxUnits * 1.25;
    const double timeoutMs = timeoutUnits * 10.0;
    return timeoutMs > intervalMaxMs * (latencyUnits + 1) * 3;
  };

  EXPECT_TRUE(envelopeHolds(CompanionConnPolicy::kConnIntervalSessionMaxUnits, CompanionConnPolicy::kConnLatencySession,
                            CompanionConnPolicy::kConnTimeoutSessionUnits));
  EXPECT_TRUE(envelopeHolds(CompanionConnPolicy::kConnIntervalIdleMaxUnits, CompanionConnPolicy::kConnLatencyIdle,
                            CompanionConnPolicy::kConnTimeoutIdleUnits));
}

TEST(CompanionConnPolicy, SessionProfileHasZeroLatency) {
  // Reverted from latency 10 on 2026-08-07 -- this protocol's bursty
  // traffic pattern pays the latency cost on nearly every field. See
  // CompanionConnPolicy.h's kConnLatencySession doc comment.
  EXPECT_EQ(CompanionConnPolicy::kConnLatencySession, 0);
}

}  // namespace
