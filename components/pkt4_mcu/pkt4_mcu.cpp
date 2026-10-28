#include "pkt4_mcu.h"
#include <esphome/components/uart/uart_debugger.h>
#include <esphome/core/log.h>

namespace esphome {
namespace pkt4_mcu {

static const char *const TAG = "pkt4_mcu";
static const char *const MOTOR_NAMES[2] = {"Drum", "Tray"};

// Motor parameter block (type 4) sent at init as stock (FUN_401272e8); meaning not established (thresholds?).
struct __attribute__((packed)) MotorParams {
	uint8_t unk[3];
	uint16_t value[4];
};
static const MotorParams MOTOR_PARAMS[2] = {
	{{0, 0, 0}, {1500, 200, 30, 20}},
	{{0, 0, 0}, {1300, 200, 30, 20}},
};

void PKT4MCUComponent::setup() {
	ESP_LOGCONFIG(TAG, "Setting up T4 MCU...");
	this->last_rx_ms_ = millis();
	this->init();
}

void PKT4MCUComponent::dump_config() {
	ESP_LOGCONFIG(TAG, "T4 MCU:");
	this->check_uart_settings(115200, 1, uart::UART_CONFIG_PARITY_NONE, 8);
	ESP_LOGCONFIG(TAG, "  Inited: %s", YESNO(this->inited_));
	ESP_LOGCONFIG(TAG, "  RX bytes dropped: %" PRIu32, this->rx_dropped_total_ + this->rx_dropped_);
	if (!this->inited_) return;
	ESP_LOGCONFIG(TAG, "  HW ver: %d", this->hw_ver_);
	ESP_LOGCONFIG(TAG, "  SW ver: %d", this->sw_ver_);
}

//void PKT4MCUComponent::update() {}

void PKT4MCUComponent::loop() {
	// Drain every complete frame already buffered: handling one per loop() let the RX buffer overflow under load.
	while (this->read_packet_())
		this->handle_packet_();
	this->flush_tx_();

	// Stock raises its `mcu` error when no frame was received for more than 15 s (0x4012635c).
	const bool lost = millis() - this->last_rx_ms_ > LINK_TIMEOUT_MS;
	if (lost != this->link_lost_) {
		this->link_lost_ = lost;
		if (lost) ESP_LOGE(TAG, "No frame from the MCU for %" PRIu32 " s", LINK_TIMEOUT_MS / 1000);
		else ESP_LOGI(TAG, "MCU link back");
	}
	if (this->link_lost_sensor_) this->link_lost_sensor_->publish_state(lost);
}

// Accumulates bytes into packet_ across loop() calls without blocking, resyncing byte by byte on the magic.
bool PKT4MCUComponent::read_packet_() {
	uint8_t *raw = (uint8_t*)&this->packet_, byte;
	while (this->available() && this->read_byte(&byte)) {
		if (this->rx_len_ < sizeof(MAGIC) && byte != uint8_t(MAGIC >> (8 * this->rx_len_))) {
			this->rx_dropped_ += this->rx_len_;
			this->rx_len_ = 0;
			if (byte != uint8_t(MAGIC)) {
				this->rx_dropped_++;
				continue;
			}
		}
		raw[this->rx_len_++] = byte;
		if (this->rx_len_ <= offsetof(MCUPacket, len)) continue;

		if (this->packet_.len < offsetof(MCUPacket, payload) + sizeof(uint16_t)) {  // impossible length
			this->rx_dropped_ += this->rx_len_;
			this->rx_len_ = 0;
			continue;
		}
		if (this->rx_len_ < this->packet_.len) continue;

		this->rx_len_ = 0;
		if (this->rx_dropped_) {
			ESP_LOGW(TAG, "Resynced after dropping %" PRIu32 " bytes", this->rx_dropped_);
			this->rx_dropped_total_ += this->rx_dropped_;
			this->rx_dropped_ = 0;
		}
		return true;
	}
	return false;
}

void PKT4MCUComponent::handle_packet_() {
	uint16_t crc_in = *(uint16_t*)((uint8_t*)&this->packet_ + this->packet_.len - sizeof(crc_in)),
	         crc_act = crc16be((uint8_t*)&this->packet_, (this->packet_.len - sizeof(crc_in)), 0xffff);

	if (crc_in != crc_act) {
		ESP_LOGW(TAG, "CRC mismatch: %04x != %04x", crc_act, crc_in);
		uart::UARTDebug::log_hex(uart::UART_DIRECTION_RX, std::vector<uint8_t>((uint8_t*)&this->packet_, ((uint8_t*)&this->packet_ + this->packet_.len)), ' ');
		return;
	}
	this->last_rx_ms_ = millis();

	if (this->packet_.pid != 0x0 &&
	    this->packet_.pid != 0x1 &&
	    //(this->packet_.pid != 0x1 || this->packet_.payload[0] != 0x81) &&
	    this->packet_.pid != 0x2 &&
	    this->packet_.pid != 0x3 &&
	    this->packet_.pid != 0x7) {
		ESP_LOGD(TAG, "< %X seq=%u len=%u crc=%04x", this->packet_.pid, this->packet_.seq, (this->packet_.len - offsetof(MCUPacket, payload) - sizeof(crc_in)), crc_in);
		uart::UARTDebug::log_hex(uart::UART_DIRECTION_RX, std::vector<uint8_t>(this->packet_.payload, (this->packet_.payload + (this->packet_.len - offsetof(MCUPacket, payload)) - sizeof(crc_in))), ' ');
	}

	struct __attribute__((packed)) node {
		uint8_t node :4;
		bool unk0 :1,
		     intr :1,
		     unk2 :1,
		     flag :1;
		uint8_t data[];
	} *node = (struct node*)this->packet_.payload;

	switch (this->packet_.pid) {
		case 0x0: {
			if (this->packet_.len != offsetof(MCUPacket, payload) + 2 + sizeof(crc_in)) {
				ESP_LOGW(TAG, "Invalid heartbeat length: %u", this->packet_.len);
				break;
			}
			if (node->node != 0x0) {
				ESP_LOGW(TAG, "Unknown heartbeat node: %x (raw %02x)", node->node, this->packet_.payload[0]);
				break;
			}
			ESP_LOGV(TAG, "MCU heartbeat: seq=%u value=0x%02X", this->packet_.seq, node->data[0]);
			// Inferred from the stock capture: 00 = waiting for the ESP, 01 = handshake (type 0 {00}) acknowledged.
			if (node->data[0] != this->heartbeat_) {
				this->heartbeat_ = node->data[0];
				ESP_LOGI(TAG, "MCU heartbeat %02X: %s", this->heartbeat_,
				         this->heartbeat_ == 0x01 ? "handshake acknowledged" : this->heartbeat_ == 0x00 ? "waiting for the handshake" : "unknown value");
			}
			if (this->handshake_sensor_) this->handshake_sensor_->publish_state(this->heartbeat_ == 0x01);
		}; break;

		case 0x1: {
			switch (node->node) {
				case 0x0: {
					struct __attribute__((packed)) p1_0 {
						bool unk0 :1,  // all bits are inverted
						     approached :1,
						     cover_present :1,
						     drum_level :1,
						     drum_up :1,
						     drum_down :1,
						     tray_open :1,
						     tray_closed :1,
						     bin_present :1;
						uint16_t unk :7;
					} *data = (struct p1_0*)node->data;  // repeated twice in payload

					// Right after an MCU reboot the status bits are not settled yet (Drum Down seen on while the drum was level).
					if (this->rebooting_) {
						if (millis() - this->rebooted_ms_ < REBOOT_SETTLE_MS) break;
						this->rebooting_ = false;
					}

					if (data->unk0) ESP_LOGW(TAG, "0x1.0 unk0 bit set!");
					if (this->approach_sensor_) this->approach_sensor_->publish_state(data->approached);
					if (this->cover_sensor_) this->cover_sensor_->publish_state(data->cover_present);
					if (this->drum_level_sensor_) this->drum_level_sensor_->publish_state(data->drum_level);
					if (this->drum_up_sensor_) this->drum_up_sensor_->publish_state(!data->drum_up);
					if (this->drum_down_sensor_) this->drum_down_sensor_->publish_state(!data->drum_down);
					if (this->tray_sensor_) {
						if (!data->tray_open && !!data->tray_closed) this->tray_sensor_->publish_state(true);
						else if (!data->tray_closed && !!data->tray_open) this->tray_sensor_->publish_state(false);
						else this->tray_sensor_->invalidate_state();
					}
					if (this->bin_sensor_) this->bin_sensor_->publish_state(data->bin_present);
					if (data->unk0) ESP_LOGW(TAG, "0x1.0 has unknown bits set: %X", data->unk);
				}; break;

				case 0x1: {
					struct __attribute__((packed)) p1_1 {
						uint8_t unk[10];
						uint16_t distance;
					} *data = (struct p1_1*)node->data;

					if (this->distance_sensor_) this->distance_sensor_->publish_state(data->distance);
				}; break;

				default: ESP_LOGW(TAG, "Unknown node for %x: %x", this->packet_.pid, this->packet_.payload[0]);
			}
		}; break;

		case 0x7: {
			switch (node->node) {
				case 0x0: {
					const size_t header_size = offsetof(MCUPacket, payload) + 2 + sizeof(crc_in);
					if (this->packet_.len < header_size ||
					    this->packet_.len != header_size + size_t(node->data[0]) * sizeof(uint32_t)) {
						ESP_LOGW(TAG, "Invalid weight packet length: %u", this->packet_.len);
						break;
					}
					struct __attribute__((packed)) p7_0 {
						uint8_t count;
						uint32_t weight[];  // 24-bit
					} *data = (struct p7_0*)node->data;

					if (data->count) {
						uint32_t weight = data->weight[0];
						for (uint8_t i = 1; i < data->count; i++)
							weight += data->weight[i];  // int32 fits a sum of 128x uint24, while a packet can fit only 61 of such.
						weight /= data->count;

						if (this->weight_sensor_) this->weight_sensor_->publish_state(weight);
					}
				}; break;

				default: ESP_LOGW(TAG, "Unknown node for %x: %x", this->packet_.pid, this->packet_.payload[0]);
			}
		}; break;

		case 0x2:  // echo of the motor configuration; it may still be the previous one, so motor status (0x3) is authoritative
			break;

		case 0x3: {
			struct __attribute__((packed)) p3 {
				uint8_t result,  // 2: running, 3: stopped
				        scram,   // stop cause (1: stop command, 4/5/…: MCU-side protection)
				        act,
				        speed,
				        pos;     // 1 level, 3 dump, 5 smooth, 7 tray open, 9 tray closed; even values in between
				uint16_t step,
				         cur,
				         cur_max,
				         runtime;
				uint8_t tag;
			} *data = (struct p3*)node->data;

			if (node->node > 1 || this->packet_.len != offsetof(MCUPacket, payload) + 1 + sizeof(p3) + sizeof(crc_in)) {
				ESP_LOGW(TAG, "Invalid motor status: node %u, len %u", node->node, this->packet_.len);
				break;
			}
			ESP_LOGD(TAG, "Motor %u: result=%u scram=%u pos=%u act=%u cur=%u/%u runtime=%u tag=%u", node->node,
			         data->result, data->scram, data->pos, data->act, data->cur, data->cur_max, data->runtime, data->tag);

			// Stock (FUN_4011d42c) ignores statuses carrying another command's ID; 0 (no ID echoed) is still accepted.
			if (data->tag && data->tag != this->command_id_[node->node]) {
				ESP_LOGD(TAG, "Motor %u: status of command %u ignored (last sent %u)", node->node, data->tag, this->command_id_[node->node]);
				break;
			}

			// A stopped status with no runtime only acknowledges a stop sent while already idle.
			if (data->result != 2 && !data->runtime) break;
			if (auto *s = this->motor_position_sensor_[node->node]) s->publish_state(data->pos);
			if (auto *s = this->motor_current_sensor_[node->node]) s->publish_state(data->cur);
			if (data->result == 2) break;

			// The MCU repeats the stopped status (with the position still changing if the motor is pushed back):
			// the stop is reported once per command, so its reactions run once.
			if (this->stop_reported_[node->node]) break;
			this->stop_reported_[node->node] = true;

			// Stock infers a full waste bin when the flap fails to close (MOTO_DOOR_CLOSE timeout -> WASTEBOX_FULL).
			// Published before the stop code, whose reactions may already send the next tray command.
			if (node->node == 1 && this->tray_closing_) {
				this->tray_closing_ = false;
				if (data->pos != 9) ESP_LOGW(TAG, "Tray close failed: stopped at pos=%u scram=%u", data->pos, data->scram);
				if (this->tray_close_failed_sensor_) this->tray_close_failed_sensor_->publish_state(data->pos != 9);
			}
			if (auto *s = this->motor_stop_code_sensor_[node->node]) s->publish_state(data->scram);
		}; break;

		case 0x4: {  // the MCU sends back the motor parameter block set at init
			if (node->node > 1 || this->packet_.len != offsetof(MCUPacket, payload) + 1 + sizeof(MotorParams) + sizeof(crc_in)) {
				ESP_LOGW(TAG, "Invalid motor parameters: node %u, len %u", node->node, this->packet_.len);
				break;
			}
			const MotorParams *data = (const MotorParams*)node->data;
			const char *name = MOTOR_NAMES[node->node];
			if (!memcmp(data, &MOTOR_PARAMS[node->node], sizeof(MotorParams)))
				ESP_LOGD(TAG, "%s motor parameters confirmed by the MCU", name);
			else
				ESP_LOGW(TAG, "%s motor parameters differ from those sent: %u %u %u %u (sent %u %u %u %u)", name,
				         data->value[0], data->value[1], data->value[2], data->value[3],
				         MOTOR_PARAMS[node->node].value[0], MOTOR_PARAMS[node->node].value[1],
				         MOTOR_PARAMS[node->node].value[2], MOTOR_PARAMS[node->node].value[3]);
		}; break;

		case 0x9: {
			switch (node->node) {
				case 0x0: {
					struct __attribute__((packed)) p9_0 {
						uint8_t unk,
						        hw_ver,
						        sw_ver;
					} *data = (struct p9_0*)node->data;

					this->hw_ver_ = data->hw_ver;
					this->sw_ver_ = data->sw_ver;

					if (this->inited_) return;

					uint8_t buf_20[] = {0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x64, 0x00, 0x00};
					this->send_(0x2, buf_20, sizeof(buf_20));

					uint8_t buf_21[] = {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x64, 0x00, 0x00};
					this->send_(0x2, buf_21, sizeof(buf_21));

					uint8_t buf_C0[] = {0x00, 0x0A, 0xC8, 0x0F, 0x0A, 0x0A, 0x0A, 0x05, 0x00};
					this->send_(0xC, buf_C0, sizeof(buf_C0));

					uint8_t buf_C1[] = {0x01, 0xC8, 0x00, 0xC8, 0x00, 0xC8, 0x00, 0xC8, 0x00};
					this->send_(0xC, buf_C1, sizeof(buf_C1));

					uint8_t buf_C2[] = {0x02, 0xFC, 0x01, 0x1F, 0x00, 0x4C, 0x70, 0x01};
					this->send_(0xC, buf_C2, sizeof(buf_C2));

					uint8_t buf_D0[] = {0x00, 0x02, 0xFF, 0x0A, 0x00, 0x1E, 0x00, 0x06};
					this->send_(0xD, buf_D0, sizeof(buf_D0));

					uint8_t buf_D1[] = {0x01, 0x02, 0xFF, 0x0A, 0x00, 0x1E, 0x00, 0x06};
					this->send_(0xD, buf_D1, sizeof(buf_D1));

					for (uint8_t motor = 0; motor < 2; motor++) {
						uint8_t buf_4[1 + sizeof(MotorParams)] = {motor};
						memcpy(buf_4 + 1, &MOTOR_PARAMS[motor], sizeof(MotorParams));
						this->send_(0x4, buf_4, sizeof(buf_4));
					}

					this->send_(0x0, (uint8_t*)(uint8_t[]){0x00, 0}, 2);

					struct __attribute__((packed)) {
						uint8_t sid;
						uint8_t rate[3];
					} buf_1 = {
						.sid = 0,
						.rate = {10, 10, 10},
					};
					this->send_(0x1, (uint8_t*)&buf_1, sizeof(buf_1));

					/*struct __attribute__((packed)) {
						uint8_t sid;
						uint8_t samples;
						uint16_t rate[2];
					} buf_7 = {
						.sid = 0,
						.samples = 5,
						.rate = {10, 10},
					};
					this->send_(0x7, (uint8_t*)&buf_7, sizeof(buf_7));*/

					this->inited_ = true;
					ESP_LOGI(TAG, "Inited ver hw: %d sw: %d", this->hw_ver_, this->sw_ver_);
				}; break;

				default: ESP_LOGW(TAG, "Unknown node for %x: %x", this->packet_.pid, this->packet_.payload[0]);
			}
		}; break;

		default:
			ESP_LOGD(TAG, "< %X seq=%u len=%u crc=%04x", this->packet_.pid, this->packet_.seq, (this->packet_.len - offsetof(MCUPacket, payload) - sizeof(crc_in)), crc_in);
			ESP_LOGW(TAG, "Unknown packet: %x (node %x, raw %02x)", this->packet_.pid, node->node, this->packet_.payload[0]);
			uart::UARTDebug::log_hex(uart::UART_DIRECTION_RX, std::vector<uint8_t>((uint8_t*)&this->packet_, ((uint8_t*)&this->packet_ + this->packet_.len)), ' ');
	}
}

void PKT4MCUComponent::init() {
	this->send_(0x9, (uint8_t*)(uint8_t[]){0x80}, 1);
}

// Stock reboots the MCU once a week (type 0xA, node 0, no data; FUN_401273a8). The MCU then announces its
// version again, which runs the whole init sequence.
void PKT4MCUComponent::reboot() {
	ESP_LOGW(TAG, "Rebooting the MCU");
	this->send_(0xA, (uint8_t*)(uint8_t[]){0x00}, 1);
	this->rebooting_ = true;
	this->rebooted_ms_ = millis();
	this->deinit();
}

void PKT4MCUComponent::deinit() {
	this->inited_ = false;
	this->heartbeat_ = 0xFF;  // log the next heartbeat again
	ESP_LOGW(TAG, "Deinited T4 MCU");
}

void PKT4MCUComponent::motor(uint8_t motor, uint8_t mode, uint8_t direction, uint8_t speed, uint16_t duration, uint16_t timeout, uint8_t pos) {
	struct __attribute__((packed)) {
		uint8_t motor,
		        mode,
		        c1,
		        pos;  // stock logs it as pos/Dpos: destination at which the MCU itself stops the motor (0: none)

		uint8_t direction,
		        speed;

		uint16_t duration;
		uint16_t timeout;
		uint8_t tag;
	} buf = {
		.motor = motor,
		.mode = mode,
		.c1 = 0b0,
		.pos = pos,
		.direction = direction,
		.speed = speed,
		.duration = duration,
		.timeout = timeout,
		.tag = 0,
	};
	if (motor < 2) {
		buf.tag = this->command_id_[motor] = this->command_id_[motor] % 255 + 1;
		this->stop_reported_[motor] = false;
		if (mode != 2) this->last_direction_[motor] = direction;
	}
	// Stop commands (mode 2) don't end the tracked close: the closed-position hall edge itself sends one.
	if (motor == 1 && mode != 2) this->tray_closing_ = (direction == 2);
	this->send_(0x2, (uint8_t*)&buf, sizeof(buf));
}

void PKT4MCUComponent::send_(uint8_t pid, uint8_t payload[], uint8_t len) {
	if (len && !payload) {
		ESP_LOGE(TAG, "send_() payload == NULL");
		return;
	}

	if (len > sizeof(MCUPacket{}.payload)) {
		ESP_LOGE(TAG, "send_() len > max");
		return;
	}

	MCUPacket packet = {
		.magic = MAGIC,
		.len = (uint8_t)(offsetof(MCUPacket, payload) + len + sizeof(uint16_t)),
		.pid = pid,
		.seq = ++this->seq_[pid & 0xF],  // one counter per type, from 1, as stock (FUN_4012e084)
		.err = 0xFF,
	};

	if (len) memcpy(packet.payload, payload, len);
	*(uint16_t*)((uint8_t*)packet.payload + len) = crc16be((uint8_t*)&packet, (packet.len - sizeof(uint16_t)), 0xffff);

	ESP_LOGD(TAG, "> %X len=%u", packet.pid, (packet.len - offsetof(MCUPacket, payload) - sizeof(uint16_t)));
	this->tx_queue_.emplace_back((uint8_t*)&packet, (uint8_t*)&packet + packet.len);
	this->flush_tx_();
}

// Stock waits 2 ticks (20 ms) between the frames of its init sequence; the frames sent back to back used to reach the
// MCU in one burst. Every frame goes through this queue, so a lone command still leaves at once.
void PKT4MCUComponent::flush_tx_() {
	if (this->tx_queue_.empty() || millis() - this->last_tx_ms_ < TX_GAP_MS) return;
	this->write_array(this->tx_queue_.front());
	this->tx_queue_.pop_front();
	this->last_tx_ms_ = millis();
}

}  // namespace pkt4_mcu
}  // namespace esphome
