#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace pkt4_mcu {

// Offline stock rules applied to individual UART samples. ESPHome supplies the
// presence/process preconditions; no output from this class controls the device.
class WeightReferenceObserver {
 public:
  struct Measurement {
    uint32_t raw;
    int32_t zero;
    float scale;
    float accounting;
    float control_residual;
    float clean_threshold;
    float pet_threshold;
  };
  struct State {
    bool known;
    bool pet_inside;
    bool busy;
    bool cover_absent;
    bool home_absent;
    bool clean_pending;
    bool ota;
  };
  enum class Decision { NONE, INITIALIZED, UPDATED, COVER_BLOCKED, BARREL_BLOCKED, UNCHANGED };
  struct Event {
    Decision decision{Decision::NONE};
    float actual{NAN};
    float before{NAN};
    float after{NAN};
    bool clean_pending{false};
    bool would_clear_threshold{false};
  };

  WeightReferenceObserver(uint32_t stock_zero = 0, uint32_t stock_mid = 0,
                          uint32_t stock_low_slope = 0, uint32_t stock_high_slope = 0)
      : stock_zero_(stock_zero), stock_mid_(stock_mid),
        stock_low_slope_(stock_low_slope), stock_high_slope_(stock_high_slope) {}

  bool observe(const Measurement &m, const State &s, uint32_t now) {
    this->seen_++;
    const bool gap = this->has_sample_ && uint32_t(now - this->last_sample_ms_) > FRESHNESS_MS;
    this->last_sample_ms_ = now;
    this->has_sample_ = true;
    this->counting_ = false;
    this->commit_allowed_ = false;
    this->valid_sample_ = false;
    if (!this->valid_(m)) {
      this->reset_window_();
      this->status_ = "Invalid sample or calibration";
      return false;
    }
    if (this->has_conversion_ && (m.zero != this->zero_ || m.scale != this->scale_)) {
      // A real user calibration invalidates the simulation's ADC references.
      this->initialized_ = false;
      this->retained_raw_ = this->barrel_baseline_raw_ = 0;
      this->barrel_out_ = false;
      this->barrel_count_ = 0;
      this->reset_window_();
    }
    this->has_conversion_ = true;
    this->valid_sample_ = true;
    this->zero_ = m.zero;
    this->scale_ = m.scale;
    this->accounting_ = m.accounting;
    this->raw_ = m.raw;
    if (gap) this->reset_window_();
    if (now >= 10000) this->boot_ready_ = true;
    if (!s.known || s.ota || !this->boot_ready_) {
      this->reset_window_();
      this->status_ = !s.known ? "Unknown states" : s.ota ? "OTA guard" : "Startup guard";
      return false;
    }

    const bool previous_valid = this->has_previous_;
    const int32_t delta = previous_valid ? this->difference_(m.raw, this->previous_raw_) : 0;
    const uint32_t magnitude = delta < 0 ? -delta : delta;
    // Stock +0x58: retain the current raw after >30 comparisons below 80 g.
    if (previous_valid && magnitude < 80) {
      if (this->initial_count_ < 31) this->initial_count_++;
      if (this->initial_count_ > 30) this->retained_raw_ = m.raw;
    } else {
      this->initial_count_ = 0;
    }

    // Stock tests this detector after the rebase block in the main task. The
    // current sample therefore uses the preceding detector result at commit.
    bool event = false;
    if (!this->initialized_) {
      this->status_ = "Waiting for initial reference";
      if (this->initial_count_ > 30 && !s.pet_inside && !s.busy &&
          !s.cover_absent && !this->barrel_out_) {
        this->reference_raw_ = m.raw;
        this->initialized_ = true;
        this->status_ = "Reference initialized";
        this->record_(Decision::INITIALIZED, m, s, NAN);
        event = true;
      }
    } else if (s.pet_inside) {
      this->samples_ = 0;
      this->status_ = "Presence: counter reset";
    } else if (previous_valid && magnitude >= 100) {
      this->samples_ = 0;
      this->status_ = "Variation >=100 g: counter reset";
    } else if (s.busy) {
      this->status_ = "Process busy: counter paused";
    } else if (previous_valid) {
      const int32_t residual = this->difference_(m.raw, this->reference_raw_);
      this->required_ = residual < -299 ? 3500 : 200;
      this->counting_ = true;
      this->commit_allowed_ = !s.cover_absent && !this->barrel_out_;
      this->status_ = s.cover_absent ? "Counting; cap blocks commit" :
                      this->barrel_out_ ? "Counting; barrel blocks commit" : "Counting";
      if (++this->samples_ > this->required_) {
        this->samples_ = 0;
        this->attempts_++;
        const float before = m.control_residual - this->correction();
        Decision decision;
        if (s.cover_absent) {
          decision = Decision::COVER_BLOCKED;
          this->blocked_++;
          this->status_ = "Attempt blocked: cap";
        } else if (this->barrel_out_) {
          decision = Decision::BARREL_BLOCKED;
          this->blocked_++;
          this->status_ = "Attempt blocked: barrel";
        } else if (m.raw == this->reference_raw_) {
          decision = Decision::UNCHANGED;
          this->status_ = "Attempt: raw unchanged";
        } else {
          decision = Decision::UPDATED;
          this->reference_raw_ = m.raw;
          this->updates_++;
          this->status_ = "Reference updated (simulation)";
        }
        this->record_(decision, m, s, before);
        event = true;
      }
    }
    this->detect_barrel_(m.raw, s);
    this->previous_raw_ = m.raw;
    this->has_previous_ = true;
    return event;
  }

  bool fresh(uint32_t now) const {
    return this->has_sample_ && this->valid_sample_ && uint32_t(now - this->last_sample_ms_) <= FRESHNESS_MS;
  }
  bool eligible(uint32_t now) const { return this->counting_ && this->fresh(now); }
  bool commit_allowed(uint32_t now) const { return this->eligible(now) && this->commit_allowed_; }
  bool initialized() const { return this->initialized_; }
  float reference() const {
    return this->initialized_ ? (int32_t(this->reference_raw_) - this->zero_) * this->scale_ : NAN;
  }
  float residual() const {
    return this->initialized_ ? (int32_t(this->raw_) - int32_t(this->reference_raw_)) * this->scale_ : NAN;
  }
  float correction() const { return this->reference() - this->accounting_; }
  uint32_t samples() const { return this->samples_; }
  uint32_t required() const { return this->required_; }
  uint32_t updates() const { return this->updates_; }
  uint32_t attempts() const { return this->attempts_; }
  uint32_t blocked() const { return this->blocked_; }
  uint32_t seen() const { return this->seen_; }
  bool barrel_out() const { return this->barrel_out_; }
  const Event &last_event() const { return this->event_; }
  const char *status(uint32_t now) const {
    if (this->has_sample_ && !this->valid_sample_) return this->status_;
    return this->fresh(now) ? this->status_ : "UART samples stale";
  }
  static const char *decision_name(Decision d) {
    switch (d) {
      case Decision::INITIALIZED: return "initialized";
      case Decision::UPDATED: return "rebase";
      case Decision::COVER_BLOCKED: return "cap_blocked";
      case Decision::BARREL_BLOCKED: return "barrel_blocked";
      case Decision::UNCHANGED: return "raw_unchanged";
      default: return "none";
    }
  }

 protected:
  static constexpr uint32_t FRESHNESS_MS = 5000;
  bool valid_(const Measurement &m) const {
    return m.raw > 0 && m.raw <= 0xFFFFFF && this->stock_zero_ > this->stock_mid_ &&
           this->stock_mid_ > 0 && this->stock_low_slope_ > 0 && this->stock_high_slope_ > 0 &&
           std::isfinite(m.scale) && m.scale < 0 && std::isfinite(m.accounting) && m.accounting >= 0;
  }
  // Stock 40125194: signed integer conversion, slope selected by current raw.
  int32_t difference_(uint32_t raw, uint32_t other) const {
    return (int32_t(raw) - int32_t(other)) /
           int32_t(raw < this->stock_mid_ ? this->stock_high_slope_ : this->stock_low_slope_);
  }
  int32_t absolute_grams_(uint32_t raw) const {
    if (raw >= this->stock_zero_) return 0;
    if (raw >= this->stock_mid_)
      return (this->stock_zero_ - raw) / this->stock_low_slope_;
    return 10000 + (this->stock_mid_ - raw) / this->stock_high_slope_;
  }
  void reset_window_() {
    this->samples_ = this->initial_count_ = 0;
    this->has_previous_ = false;
  }
  void detect_barrel_(uint32_t raw, const State &s) {
    if (s.busy) return;
    if (s.cover_absent && s.home_absent) {
      const bool removed = this->barrel_baseline_raw_ != 0
          ? this->difference_(raw, this->barrel_baseline_raw_) > 3000
          : this->absolute_grams_(raw) <= 999;
      if (removed) {
        if (this->barrel_count_ < 5) this->barrel_count_++;
        else if (!this->barrel_out_) {
          this->barrel_out_ = true;
          this->barrel_removed_raw_ = raw;
        }
      }
    } else if (!s.cover_absent) {
      this->barrel_count_ = 0;
      if ((this->barrel_out_ && this->difference_(this->barrel_removed_raw_, raw) > 3000) ||
          (this->barrel_baseline_raw_ == 0 && this->absolute_grams_(raw) > 1000))
        this->barrel_out_ = false;
      if (this->retained_raw_ != 0) this->barrel_baseline_raw_ = this->retained_raw_;
      else if (this->barrel_baseline_raw_ == 0) this->barrel_baseline_raw_ = raw;
    }
  }
  void record_(Decision decision, const Measurement &m, const State &s, float before) {
    this->event_ = {decision, m.control_residual, before,
                    m.control_residual - this->correction(), s.clean_pending, false};
    this->event_.would_clear_threshold = decision == Decision::UPDATED &&
        std::isfinite(m.control_residual) && m.control_residual > m.clean_threshold &&
        m.control_residual < m.pet_threshold && before > m.clean_threshold &&
        this->event_.after <= m.clean_threshold;
  }

  uint32_t stock_zero_, stock_mid_, stock_low_slope_, stock_high_slope_;
  bool initialized_{false}, has_sample_{false}, has_previous_{false}, has_conversion_{false}, valid_sample_{false};
  bool counting_{false}, commit_allowed_{false}, barrel_out_{false};
  bool boot_ready_{false};
  uint32_t last_sample_ms_{0}, raw_{0}, previous_raw_{0}, reference_raw_{0}, retained_raw_{0};
  uint32_t barrel_baseline_raw_{0}, barrel_removed_raw_{0}, barrel_count_{0}, initial_count_{0};
  uint32_t samples_{0}, required_{200}, updates_{0}, attempts_{0}, blocked_{0}, seen_{0};
  int32_t zero_{0};
  float scale_{NAN}, accounting_{NAN};
  const char *status_{"No samples"};
  Event event_{};
};

}  // namespace pkt4_mcu
}  // namespace esphome
