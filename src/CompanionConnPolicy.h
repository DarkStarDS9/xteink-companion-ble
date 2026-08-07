#pragma once

#include <cstdint>
#include <optional>

// The connection-interval/peripheral-latency profile decision, extracted out
// of CompanionBle.cpp's pumpConnParams()/requestConnParamsForSessionState()/
// tick() so its rules can be covered by a host gtest suite instead of only
// hardware regressions (see test/companion_conn_policy/) -- this exact area
// has regressed on real hardware more than once (2026-08-06 dropped-request
// bug, 2026-08-07 handshake-window flapping and bursty-traffic latency).
//
// NOT thread-safe and NOT itself aware of NimBLE: every method takes the
// clock as a caller-supplied `nowMs` and returns plain data: CompanionBle.cpp
// still owns the NimBLEServer::updateConnParams() call, the g_mux-free single
// BLE host task calls this synchronously, and all logging stays where it was.
//
// Two link-layer profiles, requested via NimBLEServer::updateConnParams (a
// peripheral can only ever *request* new parameters -- the central, iOS
// here, grants or ignores it). "Session": a session currently holds the
// screen, so a push is likely at any moment and the link must be fully
// responsive -- 15 ms interval, zero peripheral latency. "Idle": nothing is
// going to push content to a screen no app owns, so the link may skip
// aggressively -- see the constants below for the full envelope reasoning
// (ported verbatim from CompanionBle.cpp).
class CompanionConnPolicy {
 public:
  enum class ConnProfile : uint8_t { Session, Idle };

  // "Session" -- interval min == max == 15 ms (the one equality Apple's
  // accessory-design-guidelines permit), zero latency: no skipping while a
  // session holds the screen. Reverted from latency 10 on 2026-08-07 -- this
  // protocol is bursty (START/chunks/END, several Write-With-Response round
  // trips, with a gap after each long enough for the controller to resume
  // skipping), so latency cost was paid on nearly every field: measured
  // batch commits at a 2919 ms median against 634 ms at latency 0, with the
  // 3 s batch safety net firing on 4 of 11 pushes. A safety-net commit
  // carries no pushId, so RENDER_STATUS never fires -- the panel updates but
  // the phone hears nothing and waits out its own render timeout, which in
  // SpokenFeeds delays the audio.
  static constexpr uint16_t kConnIntervalSessionUnits = 12;     // 15 ms
  static constexpr uint16_t kConnIntervalSessionMaxUnits = 12;  // 15 ms
  static constexpr uint16_t kConnLatencySession = 0;
  static constexpr uint16_t kConnTimeoutSessionUnits = 400;  // 4 s (10 ms units)

  // "Idle" -- 15-30 ms interval (iOS is measured to grant Interval Max, so
  // the link runs at 30 ms in practice), latency 30 (Apple's cap): effective
  // idle cadence 30 ms * (30+1) = 930 ms <= the 2 s guideline ceiling;
  // timeout 4000 ms > 930 ms * 3 = 2790 ms, satisfying the
  // timeout > maxInterval * (latency+1) * 3 supervision-timeout envelope.
  static constexpr uint16_t kConnIntervalIdleUnits = 12;     // 15 ms
  static constexpr uint16_t kConnIntervalIdleMaxUnits = 24;  // 30 ms
  static constexpr uint16_t kConnLatencyIdle = 30;
  static constexpr uint16_t kConnTimeoutIdleUnits = 400;  // 4 s

  // How long the central gets to honour a parameter request before it is
  // eligible for a retry (pumpConnParams()'s old grace) and before a still-
  // outstanding, unmatched request counts as diverged for diagnostics. A
  // connection-parameter update takes effect at an instant several
  // connection events out; measured round trip on real hardware is
  // 150-550 ms, so 3 s is far past any legitimate negotiation.
  static constexpr uint32_t kConnParamsGraceMs = 3000;

  // How long the link must go with nobody holding the screen before it drops
  // to Idle's deep latency. Not a power-tuning knob: "has a foreground
  // session" is false for the first couple of seconds of *every* connection
  // while the v6 handshake is still running, and briefly on every foreground
  // handover. Measured 2026-08-07 without this holdoff: every connection
  // went Session (at connect) -> Idle (+563 ms, handshake unfinished) ->
  // Session (+2316 ms), and a duplicate grant confused the match check, so
  // the link then sat at Idle's 930 ms cadence until +6552 ms -- right
  // through the first pushes. Median batch commit went 634 ms -> 2916 ms.
  static constexpr uint32_t kIdleHoldoffMs = 10000;

  struct ParamRequest {
    ConnProfile profile;
    uint16_t intervalUnits;
    uint16_t intervalMaxUnits;
    uint16_t latencyUnits;
    uint16_t timeoutUnits;
  };

  // A fresh link. Deliberately forces Session immediately (bypassing the
  // foreground-holdoff computation setForegroundActive() drives): no session
  // has claimed the foreground this early in a real connection, so asking by
  // session state would ask for Idle and then immediately ask again for
  // Session as soon as the handshake completes -- two negotiations where one
  // will do. Also starts the Idle holdoff clock, so the first step() call
  // during the handshake -- when no session holds the screen yet -- does not
  // immediately undo this. Returns the Session request to issue right away,
  // same as the original synchronous onConnect() -> requestConnParams(Session)
  // call.
  std::optional<ParamRequest> onConnect(uint32_t nowMs);

  // The link is gone. Resets to the same Idle/no-session baseline a fresh
  // connection begins from, so a stale "already there" can't suppress the
  // next connection's onConnect() request.
  void onDisconnect();

  // The central answered a request (NimBLEServerCallbacks::onConnParamsUpdate).
  // Units match the BLE spec directly, same as the request side: interval in
  // 1.25 ms units, latency as a skipped-event count, timeout in 10 ms units.
  // The profile only advances -- and the in-flight request only clears -- on
  // a matching grant: the request carries the guidelines' required min/max
  // spread, so a grant anywhere inside the range (not just at the minimum)
  // counts as compliance. A grant that doesn't match (the central's own
  // opening parameters, or a declined request) leaves the in-flight request
  // outstanding for step()'s grace-expiry retry to deal with.
  void onGrant(uint16_t intervalUnits, uint16_t latencyUnits, uint16_t timeoutUnits, uint32_t nowMs);

  // Whether some session currently holds the screen. Level-triggered --
  // call on every tick with the live state, same as the polling this
  // replaced (CompanionBle.cpp's old requestConnParamsForSessionState() read
  // g_foreground fresh every call); idempotent, so calling it every tick
  // costs nothing extra. Drives the desired profile: Session immediately
  // when true; Idle only once `false` has held for a full kIdleHoldoffMs
  // (the holdoff clock starts on the false->true->false edge, i.e. resets on
  // every reacquisition).
  void setForegroundActive(bool active, uint32_t nowMs);

  // Call every tick. Issues the request for the desired profile if the link
  // is not already there and no procedure is outstanding; safe and cheap to
  // call repeatedly, which is what turns a deferred request (one arriving
  // while another is still in flight) into a retried one rather than a
  // dropped one. Returns the request to forward to
  // NimBLEServer::updateConnParams(), or nullopt if nothing is due.
  std::optional<ParamRequest> step(uint32_t nowMs);

  // Diagnostic only: true the first time an outstanding, unmatched request
  // has sat past kConnParamsGraceMs without a matching grant -- latched so
  // it reports once per request, not every tick. Mirrors the old tick()
  // divergence check; CompanionBle.cpp still owns the actual LOG_ERR call
  // and the "what the link is actually at" half of the message (that stays
  // sourced from NimBLEConnInfo directly, independent of this class).
  bool divergenceDue(uint32_t nowMs);

  // The currently outstanding (or most recently requested) parameters, for
  // divergenceDue()'s log message.
  uint16_t requestedIntervalUnits() const { return reqIntervalUnits_; }
  uint16_t requestedIntervalMaxUnits() const { return reqIntervalMaxUnits_; }
  uint16_t requestedLatencyUnits() const { return reqLatencyUnits_; }
  uint16_t requestedTimeoutUnits() const { return reqTimeoutUnits_; }

  ConnProfile currentProfile() const { return currentProfile_; }
  ConnProfile requestedProfile() const { return requestedProfile_; }
  bool inFlight() const { return inFlight_; }

 private:
  ParamRequest paramsFor(ConnProfile profile) const;
  // Shared by onConnect() and step(): retries a grace-expired in-flight
  // request, then issues a fresh request if the desired profile differs
  // from the confirmed one and nothing is currently outstanding.
  std::optional<ParamRequest> pump(uint32_t nowMs);

  // What we *want* the link to be, as distinct from what it is (currentProfile_)
  // and what was last asked for (requestedProfile_). These are three separate
  // variables rather than one: a request for one profile can still be in
  // flight when the desired profile changes again (a push arrives mid-
  // handshake), and collapsing them let a dropped request go unretried --
  // the firmware believed the link was where it had asked for while the link
  // itself sat on the earlier profile's params, and nothing ever retried.
  ConnProfile desiredProfile_ = ConnProfile::Idle;
  ConnProfile currentProfile_ = ConnProfile::Idle;
  ConnProfile requestedProfile_ = ConnProfile::Idle;

  bool inFlight_ = false;
  uint16_t reqIntervalUnits_ = 0;
  uint16_t reqIntervalMaxUnits_ = 0;
  uint16_t reqLatencyUnits_ = 0;
  uint16_t reqTimeoutUnits_ = 0;
  uint32_t requestedMs_ = 0;
  bool matched_ = true;
  bool divergenceLogged_ = false;

  // millis()-equivalent since nobody has held the screen; 0 while some
  // session does. Set at connect so a fresh connection starts the holdoff
  // rather than counting as long-unattended from the first step().
  uint32_t noForegroundSinceMs_ = 0;
};
