#include "components/pkt4_mcu/weight_reference_observer.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using Observer = esphome::pkt4_mcu::WeightReferenceObserver;
constexpr uint32_t BASE = 6000000;
static Observer::State idle{true, false, false, false, false, false, false};
static Observer::Measurement input(uint32_t raw = BASE) {
  return {raw, 6390000, -1.f / 65000, 6.f, 0.022f, 0.020f, 1.5f};
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.00002f; }
struct Simulation {
  Observer observer{7217189, 6557189, 66, 65};
  uint32_t now{10000};
  bool sample(const Observer::Measurement &m, Observer::State s = idle) {
    return observer.observe(m, s, now += 10);
  }
  void initialize() {
    for (unsigned i = 0; i < 31; ++i) assert(!sample(input()));
    assert(sample(input()));
    assert(observer.last_event().decision == Observer::Decision::INITIALIZED);
    assert(observer.updates() == 0 && near(observer.reference(), 6));
  }
};
int main() {
  Simulation drift;
  drift.initialize();
  auto plus22 = input(BASE - 22 * 65);
  for (unsigned i = 0; i < 200; ++i) assert(!drift.sample(plus22));
  assert(drift.sample(plus22));
  assert(drift.observer.updates() == 1 && near(drift.observer.correction(), 0.022f));
  assert(near(drift.observer.last_event().actual, .022f));
  assert(near(drift.observer.last_event().before, .022f));
  assert(near(drift.observer.last_event().after, 0));
  assert(drift.observer.last_event().would_clear_threshold);

  // One packet containing five samples must count as five observations.
  Simulation batch;
  batch.initialize();
  const auto count = batch.observer.seen();
  for (unsigned i = 0; i < 5; ++i) batch.sample(plus22);
  assert(batch.observer.samples() == 5 && batch.observer.seen() == count + 5);

  // Integer conversion, not a >0.299 kg floating-point comparison.
  Simulation boundary;
  boundary.initialize();
  auto below300 = input(BASE - (300 * 65 - 1));
  boundary.sample(below300); // >=100 g jump resets the window first.
  boundary.sample(below300);
  assert(boundary.observer.required() == 200);
  auto plus300 = input(BASE - 300 * 65);
  boundary.sample(plus300);
  assert(boundary.observer.required() == 3500);
  for (unsigned i = boundary.observer.samples(); i < 3500; ++i) assert(!boundary.sample(plus300));
  assert(boundary.sample(plus300));

  // Large decreases use the short counter; a >=100 g jump resets it.
  Simulation decrease;
  decrease.initialize();
  auto minus500 = input(BASE + 500 * 65);
  decrease.sample(minus500);
  assert(decrease.observer.samples() == 0);
  for (unsigned i = 0; i < 200; ++i) assert(!decrease.sample(minus500));
  assert(decrease.sample(minus500));
  assert(decrease.observer.required() == 200);

  // Busy pauses; presence resets. The previous raw still follows samples.
  Simulation pause;
  pause.initialize();
  for (unsigned i = 0; i < 200; ++i) pause.sample(plus22);
  auto busy = idle; busy.busy = true;
  assert(!pause.sample(plus22, busy));
  assert(pause.observer.samples() == 200 && !pause.observer.eligible(pause.now));
  assert(pause.sample(plus22));
  for (unsigned i = 0; i < 40; ++i) pause.sample(plus22);
  auto pet = idle; pet.pet_inside = true;
  assert(!pause.sample(plus22, pet));
  assert(pause.observer.samples() == 0);

  // Commit guards are late; a blocked attempt still consumes the window.
  Simulation cap;
  cap.initialize();
  auto open = idle; open.cover_absent = true;
  for (unsigned i = 0; i < 200; ++i) assert(!cap.sample(plus22, open));
  assert(cap.observer.eligible(cap.now) && !cap.observer.commit_allowed(cap.now));
  assert(cap.sample(plus22, open));
  assert(cap.observer.last_event().decision == Observer::Decision::COVER_BLOCKED);
  assert(cap.observer.blocked() == 1 && cap.observer.samples() == 0);
  assert(near(cap.observer.reference(), 6));
  assert(!cap.sample(plus22));
  assert(cap.observer.samples() == 1);

  // No 5 g deadband: even one raw count can update, but is not a success claim.
  Simulation tiny;
  tiny.initialize();
  auto one_count = input(BASE - 1); one_count.control_residual = .001f;
  for (unsigned i = 0; i < 200; ++i) tiny.sample(one_count);
  assert(tiny.sample(one_count));
  assert(tiny.observer.updates() == 1 && !tiny.observer.last_event().would_clear_threshold);
  for (unsigned i = 0; i < 200; ++i) tiny.sample(one_count);
  assert(tiny.sample(one_count));
  assert(tiny.observer.last_event().decision == Observer::Decision::UNCHANGED);
  assert(tiny.observer.attempts() == 2 && tiny.observer.updates() == 1);

  // Pending cleaning is recorded, not silently used as a stock counting gate.
  Simulation pending;
  pending.initialize();
  auto queued = idle; queued.clean_pending = true;
  for (unsigned i = 0; i < 200; ++i) pending.sample(plus22, queued);
  assert(pending.sample(plus22, queued));
  assert(pending.observer.last_event().clean_pending);
  assert(pending.observer.last_event().would_clear_threshold);

  // A later tiny rebase is not credited with a crossing already achieved.
  auto plus23 = input(BASE - 23 * 65);
  for (unsigned i = 0; i < 200; ++i) pending.sample(plus23);
  assert(pending.sample(plus23));
  assert(pending.observer.last_event().actual > .020f);
  assert(!pending.observer.last_event().would_clear_threshold);

  // The lower-weight ADC segment uses 66 counts/g, not 65.
  Simulation low_segment;
  auto low = input(7000000);
  for (unsigned i = 0; i < 32; ++i) low_segment.sample(low);
  low.raw -= 100 * 66 - 1;
  low_segment.sample(low);
  assert(low_segment.observer.samples() == 1); // 99 g after truncation.
  low.raw -= 100 * 66;
  low_segment.sample(low);
  assert(low_segment.observer.samples() == 0);

  // Startup is latched: millis wrap must not repeat the ten-second guard.
  Simulation wrapped;
  wrapped.initialize();
  wrapped.now = 0xFFFFFF00u;
  wrapped.sample(plus22); // Long gap resets the comparison window.
  for (unsigned i = 0; i < 200; ++i) wrapped.sample(plus22);
  assert(wrapped.sample(plus22));
  assert(wrapped.observer.updates() == 1);

  // Real accounting changes do not impersonate an OEM reference write.
  Simulation accounting;
  accounting.initialize();
  auto changed = input(); changed.accounting = 6.04f;
  accounting.sample(changed);
  assert(near(accounting.observer.reference(), 6));
  assert(near(accounting.observer.correction(), -.04f));

  // Stock software barrel detector: removal needs cap/home away + six reads.
  Simulation barrel;
  barrel.initialize();
  auto removed = input(BASE + 4000 * 65);
  auto apart = idle; apart.cover_absent = true; apart.home_absent = true;
  for (unsigned i = 0; i < 5; ++i) barrel.sample(removed, apart);
  assert(!barrel.observer.barrel_out());
  barrel.sample(removed, apart);
  assert(barrel.observer.barrel_out());
  auto closed = idle;
  for (unsigned i = 0; i < 201; ++i) barrel.sample(removed, closed);
  assert(barrel.observer.last_event().decision == Observer::Decision::BARREL_BLOCKED);
  barrel.sample(input(), closed);
  assert(!barrel.observer.barrel_out());

  // UART gaps, unknown state, OTA and invalid ADC fail closed diagnostically.
  Simulation guards;
  guards.initialize();
  guards.sample(plus22);
  assert(guards.observer.samples() == 1);
  guards.now += 6000;
  guards.sample(plus22);
  assert(guards.observer.samples() == 0);
  auto unknown = idle; unknown.known = false;
  guards.sample(plus22, unknown);
  assert(!guards.observer.eligible(guards.now));
  auto ota = idle; ota.ota = true;
  guards.sample(plus22, ota);
  assert(guards.observer.samples() == 0);
  guards.sample(input(0));
  assert(!guards.observer.fresh(guards.now));
  guards.sample(input(0x1000000));
  assert(!guards.observer.eligible(guards.now));
  auto invalid = input(); invalid.scale = std::numeric_limits<float>::quiet_NaN();
  guards.sample(invalid);
  assert(!guards.observer.fresh(guards.now));

  // A real calibration change restarts only the diagnostic model.
  Simulation calibration;
  calibration.initialize();
  auto new_calibration = input(); new_calibration.zero += 100;
  calibration.sample(new_calibration);
  assert(!calibration.observer.initialized());
  Observer missing_calibration;
  assert(!missing_calibration.observe(input(), idle, 20000));
  assert(!missing_calibration.initialized());

  // Missing control history is never called a would-clear-threshold event.
  Simulation unknown_control;
  unknown_control.initialize();
  auto missing = plus22; missing.control_residual = NAN;
  for (unsigned i = 0; i < 200; ++i) unknown_control.sample(missing);
  assert(unknown_control.sample(missing));
  assert(!unknown_control.observer.last_event().would_clear_threshold);
  assert(!unknown_control.observer.fresh(unknown_control.now + 5001));

  std::cout << "PASS: individual samples, integer thresholds, pause/reset/commit gates, "
               "barrel detector, decision snapshots, counterfactual threshold, accounting, "
               "invalid/stale inputs and calibration changes\n";
}
