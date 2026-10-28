#pragma once

#include <esphome/core/component.h>
#include <esphome/components/binary_sensor/binary_sensor.h>
#include <esphome/components/sensor/sensor.h>
#include <esphome/components/uart/uart.h>

#include <deque>
#include <vector>

namespace esphome {
namespace pkt4_mcu {

static const uint16_t MAGIC = 0xA55A;
static const uint32_t LINK_TIMEOUT_MS = 15000;
static const uint32_t TX_GAP_MS = 20;  // between two frames sent to the MCU
static const uint32_t REBOOT_SETTLE_MS = 2000;  // status bits ignored after an MCU reboot

// Meaning of a motor stop code (scram), from the stock handling (FUN_4011d300, FUN_4011e008).
inline const char *stop_reason(int code) {
	switch (code) {
		case 0: return "Destination reached";
		case 1: return "Stopped";
		case 4: return "Drum stall";
		case 5: return "Flap stall";
		case 17: case 18: return "Timeout";
		default: return "Unknown";
	}
}

struct __attribute__((packed)) MCUPacket {
	uint16_t magic;
	uint8_t len,
	        pid,
	        seq,
	        err;
	uint8_t payload[0xFF - 6];
};

class PKT4MCUComponent: public Component, public uart::UARTDevice {
	public:
		void setup() override;
		//void update() override;
		void loop() override;
		void dump_config() override;
		float get_setup_priority() const { return setup_priority::LATE; }

		void set_distance_sensor(sensor::Sensor *distance_sensor) { this->distance_sensor_ = distance_sensor; }
		void set_weight_sensor(sensor::Sensor *weight_sensor) { this->weight_sensor_ = weight_sensor; }
		void set_approach_sensor(binary_sensor::BinarySensor *approach_sensor) { this->approach_sensor_ = approach_sensor; }
		void set_bin_sensor(binary_sensor::BinarySensor *bin_sensor) { this->bin_sensor_ = bin_sensor; }
		void set_cover_sensor(binary_sensor::BinarySensor *cover_sensor) { this->cover_sensor_ = cover_sensor; }
		void set_drum_up_sensor(binary_sensor::BinarySensor *drum_up_sensor) { this->drum_up_sensor_ = drum_up_sensor; }
		void set_drum_down_sensor(binary_sensor::BinarySensor *drum_down_sensor) { this->drum_down_sensor_ = drum_down_sensor; }
		void set_drum_level_sensor(binary_sensor::BinarySensor *drum_level_sensor) { this->drum_level_sensor_ = drum_level_sensor; }
		void set_tray_sensor(binary_sensor::BinarySensor *tray_sensor) { this->tray_sensor_ = tray_sensor; }
		void set_tray_close_failed_sensor(binary_sensor::BinarySensor *sensor) { this->tray_close_failed_sensor_ = sensor; }
		void set_link_lost_sensor(binary_sensor::BinarySensor *sensor) { this->link_lost_sensor_ = sensor; }
		void set_handshake_sensor(binary_sensor::BinarySensor *sensor) { this->handshake_sensor_ = sensor; }
		// Motor status (type 0x3) feedback, per motor: 0 = drum, 1 = tray.
		void set_motor_position_sensor(uint8_t motor, sensor::Sensor *sensor) { this->motor_position_sensor_[motor] = sensor; }
		void set_motor_stop_code_sensor(uint8_t motor, sensor::Sensor *sensor) { this->motor_stop_code_sensor_[motor] = sensor; }
		void set_motor_current_sensor(uint8_t motor, sensor::Sensor *sensor) { this->motor_current_sensor_[motor] = sensor; }

		void init(void);
		void deinit(void);
		void reboot(void);
		bool link_lost() const { return this->link_lost_; }
		void motor(uint8_t motor, uint8_t mode, uint8_t direction, uint8_t speed, uint16_t duration, uint16_t timeout, uint8_t pos = 0);
		// Direction of the last move sent to a motor (1 = down/open, 2 = up/close), to back off the way it came.
		uint8_t last_direction(uint8_t motor) const { return motor < 2 ? this->last_direction_[motor] : 0; }

	protected:
		uint8_t hw_ver_,
		        sw_ver_;
		bool inited_{false};
		MCUPacket packet_;
		uint8_t rx_len_{0};  // bytes of packet_ received so far
		uint32_t rx_dropped_{0},
		         rx_dropped_total_{0};
		uint8_t seq_[16]{};  // per frame type
		std::deque<std::vector<uint8_t>> tx_queue_;
		uint32_t last_tx_ms_{0};
		uint8_t heartbeat_{0xFF};  // last heartbeat value from the MCU

		bool read_packet_();
		void handle_packet_();

		sensor::Sensor *distance_sensor_{nullptr},
		               *weight_sensor_{nullptr};
		sensor::Sensor *motor_position_sensor_[2]{},
		               *motor_stop_code_sensor_[2]{},
		               *motor_current_sensor_[2]{};
		binary_sensor::BinarySensor *approach_sensor_{nullptr},
		                            *bin_sensor_{nullptr},
		                            *cover_sensor_{nullptr},
		                            *drum_up_sensor_{nullptr},
		                            *drum_down_sensor_{nullptr},
		                            *drum_level_sensor_{nullptr},
		                            *tray_sensor_{nullptr},
		                            *tray_close_failed_sensor_{nullptr},
		                            *link_lost_sensor_{nullptr},
		                            *handshake_sensor_{nullptr};
		uint32_t last_rx_ms_{0};  // last valid frame, any type
		bool link_lost_{false};
		bool rebooting_{false};  // status bits are ignored until REBOOT_SETTLE_MS after rebooted_ms_
		uint32_t rebooted_ms_{0};
		bool tray_closing_{false};  // a tray close command is in progress
		uint8_t command_id_[2]{};  // last ctrl_ID sent per motor, 1..255 as stock (FUN_4011d284)
		uint8_t last_direction_[2]{};
		bool stop_reported_[2]{};  // the stop of the last command was already reported

		void send_(uint8_t pid, uint8_t payload[], uint8_t len);
		void flush_tx_();
};

template<typename... Ts> class InitAction: public Action<Ts...>, public Parented<PKT4MCUComponent> {
	public:
		void play(const Ts &...x) override { this->parent_->init(); }
};

template<typename... Ts> class DeinitAction: public Action<Ts...>, public Parented<PKT4MCUComponent> {
	public:
		void play(const Ts &...x) override { this->parent_->deinit(); }
};

template<typename... Ts> class RebootAction: public Action<Ts...>, public Parented<PKT4MCUComponent> {
	public:
		void play(const Ts &...x) override { this->parent_->reboot(); }
};

template<typename... Ts> class MotorAction: public Action<Ts...>, public Parented<PKT4MCUComponent> {
	public:
		void set_motor(uint8_t motor) { this->motor_ = motor; }
		void set_mode(uint8_t mode) { this->mode_ = mode; }
		void set_direction(uint8_t direction) { this->direction_ = direction; }
		void set_speed(uint8_t speed) { this->speed_ = speed; }
		void set_duration(uint16_t duration) { this->duration_ = duration; }
		void set_timeout(uint16_t timeout) { this->timeout_ = timeout; }
		void set_pos(uint8_t pos) { this->pos_ = pos; }

		void play(const Ts &...x) override { this->parent_->motor(this->motor_, this->mode_, this->direction_, this->speed_, this->duration_, this->timeout_, this->pos_); }

	protected:
		uint8_t motor_, mode_, direction_, speed_{0}, pos_{0};
		uint16_t duration_{0}, timeout_{0};
};

}  // namespace pkt4_mcu
}  // namespace esphome
