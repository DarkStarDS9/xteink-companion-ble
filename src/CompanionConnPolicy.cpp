#include "CompanionConnPolicy.h"

CompanionConnPolicy::ParamRequest CompanionConnPolicy::paramsFor(ConnProfile profile) const {
  ParamRequest req{profile, kConnIntervalSessionUnits, kConnIntervalSessionMaxUnits, kConnLatencySession,
                   kConnTimeoutSessionUnits};
  switch (profile) {
    case ConnProfile::Session:
      req.intervalUnits = kConnIntervalSessionUnits;
      req.intervalMaxUnits = kConnIntervalSessionMaxUnits;
      req.latencyUnits = kConnLatencySession;
      req.timeoutUnits = kConnTimeoutSessionUnits;
      break;
    case ConnProfile::Idle:
      req.intervalUnits = kConnIntervalIdleUnits;
      req.intervalMaxUnits = kConnIntervalIdleMaxUnits;
      req.latencyUnits = kConnLatencyIdle;
      req.timeoutUnits = kConnTimeoutIdleUnits;
      break;
  }
  return req;
}

std::optional<CompanionConnPolicy::ParamRequest> CompanionConnPolicy::pump(uint32_t nowMs) {
  if (inFlight_) {
    // Still inside the window the central is allowed to take. Do not start a
    // second procedure: it would be the one that gets dropped.
    if (nowMs - requestedMs_ <= kConnParamsGraceMs) return std::nullopt;
    // The grace expired with no matching update. Either the central ignored
    // the request or the answer never came; either way the procedure is no
    // longer usefully outstanding, so allow a fresh attempt rather than
    // wedging here for the rest of the connection.
    inFlight_ = false;
  }

  if (desiredProfile_ == currentProfile_) return std::nullopt;

  const ConnProfile profile = desiredProfile_;
  const ParamRequest req = paramsFor(profile);
  // Deliberately NOT currentProfile_: that only moves when the central
  // actually grants the request (see onGrant()). Latching it here is what
  // made the firmware believe a dropped request had taken effect.
  requestedProfile_ = profile;
  inFlight_ = true;
  reqIntervalUnits_ = req.intervalUnits;
  reqIntervalMaxUnits_ = req.intervalMaxUnits;
  reqLatencyUnits_ = req.latencyUnits;
  reqTimeoutUnits_ = req.timeoutUnits;
  requestedMs_ = nowMs;
  matched_ = false;  // until onGrant() says otherwise
  divergenceLogged_ = false;
  return req;
}

std::optional<CompanionConnPolicy::ParamRequest> CompanionConnPolicy::onConnect(uint32_t nowMs) {
  noForegroundSinceMs_ = nowMs;
  desiredProfile_ = ConnProfile::Session;
  return pump(nowMs);
}

void CompanionConnPolicy::onDisconnect() {
  reqIntervalUnits_ = 0;
  reqIntervalMaxUnits_ = 0;
  // The next connect gets a fresh onConnect() request; reset to Idle (the
  // no-session state a fresh connection begins in) so a stale "already
  // there" doesn't suppress that request.
  currentProfile_ = ConnProfile::Idle;
  desiredProfile_ = ConnProfile::Idle;
  requestedProfile_ = ConnProfile::Idle;
  inFlight_ = false;
}

void CompanionConnPolicy::onGrant(uint16_t intervalUnits, uint16_t latencyUnits, uint16_t timeoutUnits,
                                   uint32_t /*nowMs*/) {
  // The request is a *range* (the profiles carry the guidelines' required
  // min/max spread), so a grant anywhere inside it is compliance, not
  // divergence. reqIntervalUnits_ == 0 (nothing ever requested) counts as a
  // vacuous match -- in practice unreachable, since onConnect() always
  // requests before the central's first update event, but the original code
  // guarded for it and this preserves that.
  matched_ = (reqIntervalUnits_ == 0 || (intervalUnits >= reqIntervalUnits_ && intervalUnits <= reqIntervalMaxUnits_ &&
                                          latencyUnits == reqLatencyUnits_ && timeoutUnits == reqTimeoutUnits_));
  // The link only counts as being in a profile once the central says so. An
  // update that does not match what we asked for is either the central's own
  // opening parameters or a request of ours it declined -- in both cases the
  // procedure is done, so stop treating ours as outstanding and let step()
  // decide whether to ask again.
  if (matched_) {
    currentProfile_ = requestedProfile_;
    inFlight_ = false;
  }
}

void CompanionConnPolicy::setForegroundActive(bool active, uint32_t nowMs) {
  if (active) {
    noForegroundSinceMs_ = 0;
    desiredProfile_ = ConnProfile::Session;
    return;
  }
  if (noForegroundSinceMs_ == 0) noForegroundSinceMs_ = nowMs;
  // Stay on Session through the handshake window and any brief handover;
  // only a sustained absence earns Idle.
  const bool unattended = nowMs - noForegroundSinceMs_ >= kIdleHoldoffMs;
  desiredProfile_ = unattended ? ConnProfile::Idle : ConnProfile::Session;
}

std::optional<CompanionConnPolicy::ParamRequest> CompanionConnPolicy::step(uint32_t nowMs) { return pump(nowMs); }

bool CompanionConnPolicy::divergenceDue(uint32_t nowMs) {
  if (matched_ || divergenceLogged_ || reqIntervalUnits_ == 0 || requestedMs_ == 0) return false;
  if (nowMs - requestedMs_ <= kConnParamsGraceMs) return false;
  divergenceLogged_ = true;
  return true;
}
