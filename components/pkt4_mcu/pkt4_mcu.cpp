#include "pkt4_mcu.h"
#include <esphome/components/uart/uart_debugger.h>
#include <esphome/core/log.h>

namespace esphome {
namespace pkt4_mcu {

static const char *const TAG = "pkt4_mcu";

void PKT4MCUComponent::setup() {
	ESP_LOGCONFIG(TAG, "Setting up T4 MCU...");
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
						if (this->weight_sample_sensor_) this->weight_sample_sensor_->publish_state(NAN);
						break;
					}
					struct __attribute__((packed)) p7_0 {
						uint8_t count;
						uint32_t weight[];  // 24-bit
					} *data = (struct p7_0*)node->data;

					if (this->weight_sample_sensor_) {
						for (uint8_t i = 0; i < data->count; i++)
							this->weight_sample_sensor_->publish_state(data->weight[i]);
					}
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
			if (data->result != 2)
				if (auto *s = this->motor_stop_code_sensor_[node->node]) s->publish_state(data->scram);

			// Stock infers a full waste bin when the flap fails to close (MOTO_DOOR_CLOSE timeout -> WASTEBOX_FULL).
			if (node->node == 1 && data->result != 2 && this->tray_closing_) {
				this->tray_closing_ = false;
				if (data->pos != 9) ESP_LOGW(TAG, "Tray close failed: stopped at pos=%u scram=%u", data->pos, data->scram);
				if (this->tray_close_failed_sensor_) this->tray_close_failed_sensor_->publish_state(data->pos != 9);
			}
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

					uint8_t buf_40[] = {0x00, 0x00, 0x00, 0x00, 0xDC, 0x05, 0xC8, 0x00, 0x1E, 0x00, 0x14, 0x00};
					this->send_(0x4, buf_40, sizeof(buf_40));

					uint8_t buf_41[] = {0x01, 0x00, 0x00, 0x00, 0x14, 0x05, 0xC8, 0x00, 0x1E, 0x00, 0x14, 0x00};
					this->send_(0x4, buf_41, sizeof(buf_41));

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

void PKT4MCUComponent::deinit() {
	this->inited_ = false;
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
	if (motor < 2) buf.tag = this->command_id_[motor] = this->command_id_[motor] % 255 + 1;
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
		.seq = this->seq_++,
		.err = 0xFF,
	};

	if (len) memcpy(packet.payload, payload, len);
	*(uint16_t*)((uint8_t*)packet.payload + len) = crc16be((uint8_t*)&packet, (packet.len - sizeof(uint16_t)), 0xffff);

	ESP_LOGD(TAG, "> %X len=%u", packet.pid, (packet.len - offsetof(MCUPacket, payload) - sizeof(uint16_t)));
	this->write_array((uint8_t*)&packet, packet.len);
}

}  // namespace pkt4_mcu
}  // namespace esphome
