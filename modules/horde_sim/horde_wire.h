/**************************************************************************/
/*  horde_wire.h                                                          */
/**************************************************************************/

#pragma once

#include "core/io/marshalls.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "core/variant/type_info.h"

// T3 horde wire format (NET R3.5-R3.9, P2.1).
//
// This header carries the single source of truth for the on-wire bit layout as
// _FORCE_INLINE_ free functions in namespace HordeWireFormat. Both the encoder
// (HordeAgents::pack_snapshot_into, which walks the SoA once per client per send)
// and the decoder (HordeWireCodec, used by tests and the P2.1b client) call the
// same primitives, so the two can never silently diverge.
//
// STRUCTURAL DECISIONS (streamlining review 2026-07-11, binding):
//  - Quantization is a per-client OUTPUT pass, never SoA storage: the SoA stays
//    float for tick determinism and multi-client keyframes (D-019). Encoding
//    happens here, in one native walk per client per send.
//  - Per-client relevance is NOT sim-LOD: the pack pass computes its own
//    client-relative distances (R3.7). HordeAgents::tier / nearest_any_dist_sq
//    are sim-LOD (nearest-to-ANY-player) and off-limits for wire relevance.
//
// R3.5 RECORD (8 bytes, little-endian uint64 bitfield):
//   bits [ 0..10]  id     11 bits  -- _make_id(): 10-bit slot | epoch parity<<10
//   bits [11..26]  qx     16 bits  -- horizontal X, unorm over the bounds AABB
//   bits [27..42]  qz     16 bits  -- horizontal Z, unorm over the bounds AABB
//   bits [43..51]  qy      9 bits  -- vertical Y, unorm over the bounds AABB
//   bits [52..59]  yaw     8 bits  -- heading, unorm over a full turn
//   bits [60..63]  state   4 bits  -- FSM state ordinal (<= 16 states)
// 16-bit horizontal over a <= 650 m map -> <= ~1 cm round-trip error (R3.5).
// 9-bit vertical (not 10) buys the 11th id bit: R3.5's "10-bit id" is a 10-bit
// slot plus the reuse-epoch parity bit, so the full _make_id rides the wire and
// the record stays exactly 8 bytes. R3.5 sanctions 8-10 bit vertical.
namespace HordeWireFormat {

constexpr int RECORD_BYTES = 8;
constexpr int HEADER_BYTES = 6; // [server_tick u32][count u16] (R3.6).
constexpr int MTU_PAYLOAD = 1200; // Target UDP payload to fill toward (R3.6).
constexpr int MAX_RECORDS_PER_PACKET = (MTU_PAYLOAD - HEADER_BYTES) / RECORD_BYTES; // 149.

constexpr uint32_t ID_MASK = 0x7FF; // 11 bits: slot (10) | epoch parity (1).
constexpr int QX_SHIFT = 11;
constexpr int QZ_SHIFT = 27;
constexpr int QY_SHIFT = 43;
constexpr int YAW_SHIFT = 52;
constexpr int STATE_SHIFT = 60;
constexpr uint32_t QXZ_MAX = 0xFFFF; // 16-bit horizontal quantum.
constexpr uint32_t QY_MAX = 0x1FF; // 9-bit vertical quantum.
constexpr uint32_t YAW_MAX = 0x100; // 256 headings (circular; 256 wraps to 0).
constexpr uint32_t STATE_MASK = 0xF;

// Unsigned-normalized quantize/dequantize over [p_min, p_min + p_size].
// Half-open clamp: values outside the AABB pin to the nearest edge quantum.
_FORCE_INLINE_ uint32_t quantize_unorm(float p_v, float p_min, float p_size, uint32_t p_max_q) {
	if (p_size <= 0.0f) {
		return 0;
	}
	const float t = (p_v - p_min) / p_size;
	if (t <= 0.0f) {
		return 0;
	}
	if (t >= 1.0f) {
		return p_max_q;
	}
	return (uint32_t)(t * (float)p_max_q + 0.5f);
}

_FORCE_INLINE_ float dequantize_unorm(uint32_t p_q, float p_min, float p_size, uint32_t p_max_q) {
	if (p_max_q == 0) {
		return p_min;
	}
	return p_min + ((float)p_q / (float)p_max_q) * p_size;
}

// Circular 8-bit heading. Any angle maps into [0, 256); 256 wraps to 0 so the
// seam at +/-PI is continuous.
_FORCE_INLINE_ uint32_t quantize_yaw(float p_yaw) {
	const float t = Math::fract(p_yaw / (float)Math::TAU + 0.5f); // [-PI, PI] -> [0, 1).
	const int q = (int)(t * (float)YAW_MAX + 0.5f);
	return (uint32_t)(q & 0xFF);
}

_FORCE_INLINE_ float dequantize_yaw(uint32_t p_q) {
	return ((float)p_q / (float)YAW_MAX) * (float)Math::TAU - (float)(Math::TAU * 0.5);
}

// Pack one R3.5 record. p_wire_id is HordeAgents::_make_id() (already 11-bit).
_FORCE_INLINE_ uint64_t encode_record(uint32_t p_wire_id, const Vector3 &p_pos, float p_yaw, uint32_t p_state, const AABB &p_bounds) {
	const uint64_t id = (uint64_t)(p_wire_id & ID_MASK);
	const uint64_t qx = quantize_unorm(p_pos.x, p_bounds.position.x, p_bounds.size.x, QXZ_MAX);
	const uint64_t qz = quantize_unorm(p_pos.z, p_bounds.position.z, p_bounds.size.z, QXZ_MAX);
	const uint64_t qy = quantize_unorm(p_pos.y, p_bounds.position.y, p_bounds.size.y, QY_MAX);
	const uint64_t qyaw = quantize_yaw(p_yaw);
	const uint64_t st = (uint64_t)(p_state & STATE_MASK);
	return id | (qx << QX_SHIFT) | (qz << QZ_SHIFT) | (qy << QY_SHIFT) | (qyaw << YAW_SHIFT) | (st << STATE_SHIFT);
}

_FORCE_INLINE_ void decode_record(uint64_t p_rec, const AABB &p_bounds, int &r_id, Vector3 &r_pos, float &r_yaw, int &r_state) {
	r_id = (int)(p_rec & ID_MASK);
	const uint32_t qx = (uint32_t)((p_rec >> QX_SHIFT) & QXZ_MAX);
	const uint32_t qz = (uint32_t)((p_rec >> QZ_SHIFT) & QXZ_MAX);
	const uint32_t qy = (uint32_t)((p_rec >> QY_SHIFT) & QY_MAX);
	const uint32_t qyaw = (uint32_t)((p_rec >> YAW_SHIFT) & 0xFF);
	const uint32_t st = (uint32_t)((p_rec >> STATE_SHIFT) & STATE_MASK);
	r_pos = Vector3(
			dequantize_unorm(qx, p_bounds.position.x, p_bounds.size.x, QXZ_MAX),
			dequantize_unorm(qy, p_bounds.position.y, p_bounds.size.y, QY_MAX),
			dequantize_unorm(qz, p_bounds.position.z, p_bounds.size.z, QXZ_MAX));
	r_yaw = dequantize_yaw(qyaw);
	r_state = (int)st;
}

// R3.6 packet header: [server_tick u32][count u16], HEADER_BYTES total. Shared
// by the pack pass (encoder) and HordeWireCodec (decoder) like the record above.
_FORCE_INLINE_ void encode_header(uint32_t p_server_tick, uint16_t p_count, uint8_t *p_dst) {
	encode_uint32(p_server_tick, p_dst);
	encode_uint16(p_count, p_dst + 4);
}

_FORCE_INLINE_ void decode_header(const uint8_t *p_src, uint32_t &r_server_tick, uint16_t &r_count) {
	r_server_tick = decode_uint32(p_src);
	r_count = decode_uint16(p_src + 4);
}

// R3.9 reliable lifecycle event layout (spawn/death/despawn). Positions and
// directions ride full float32 -- rare reliable one-shots, not per-tick
// bandwidth, and R4.3 wants the death impulse direction to stay precise (the
// ragdoll hero-clip moment).
constexpr int SPAWN_BYTES = 2 + 1 + 12; // id u16, archetype u8, pos 3xf32.
constexpr int DEATH_BYTES = 2 + 2 + 12; // id u16, killer u16, impulse 3xf32.
constexpr int DESPAWN_BYTES = 2; // id u16.
constexpr uint16_t KILLER_NONE = 0xFFFF; // Sentinel for "no killer hint".

} // namespace HordeWireFormat

// Per-client tiered send-rate scheduler (NET R3.7).
//
// One small helper object per client (state owned by the caller, per the ticket).
// due_mask(server_tick) reports which relevance tiers are due to send this tick.
// Periods are in 128 Hz ticks: hot ~20 Hz, mid ~10 Hz, far 4 Hz. The tier
// offsets are fixed constants, not knobs: they are chosen so hot (even ticks)
// and far (odd ticks) are parity-disjoint -- all three tiers never fall due on
// the same tick, so a client never bursts a full hot+mid+far send. client_phase
// staggers different clients off each other so their packets spread across
// ticks (host send-load smoothing) rather than co-firing.
class HordeWireScheduler : public RefCounted {
	GDCLASS(HordeWireScheduler, RefCounted);

public:
	enum SendBit {
		SEND_NONE = 0,
		SEND_HOT = 1,
		SEND_MID = 2,
		SEND_FAR = 4,
	};

private:
	// Fixed tier offsets (the parity-disjoint invariant above). Each must stay
	// within [0, period) for its default period.
	static constexpr int HOT_OFFSET = 0; // Even ticks (period 6).
	static constexpr int MID_OFFSET = 3;
	static constexpr int FAR_OFFSET = 5; // Odd ticks (period 32) -> disjoint from hot.

	int hot_period = 6; // 128/6 ~= 21.3 Hz (~20 Hz hot, R3.7).
	int mid_period = 13; // 128/13 ~= 9.8 Hz (~10 Hz mid).
	int far_period = 32; // 128/32 = 4 Hz far.
	int client_phase = 0;

	static void _bind_methods();

public:
	void set_hot_period(int p_ticks) { hot_period = p_ticks; }
	int get_hot_period() const { return hot_period; }
	void set_mid_period(int p_ticks) { mid_period = p_ticks; }
	int get_mid_period() const { return mid_period; }
	void set_far_period(int p_ticks) { far_period = p_ticks; }
	int get_far_period() const { return far_period; }
	void set_client_phase(int p_phase) { client_phase = p_phase; }
	int get_client_phase() const { return client_phase; }

	// Bitmask of SendBit tiers due to send at p_server_tick for this client.
	int due_mask(int64_t p_server_tick) const;

	HordeWireScheduler() {}
};

VARIANT_ENUM_CAST(HordeWireScheduler::SendBit);

// Stateless codec for the R3.5 record, the R3.6 packet header, and the R3.9
// reliable lifecycle events. Host and client share one implementation. The
// encoder half of the record is exercised natively by HordeAgents; this class
// exposes the symmetric encode/decode to GDScript and to the wire test suite.
class HordeWireCodec : public RefCounted {
	GDCLASS(HordeWireCodec, RefCounted);

public:
	enum Layout {
		RECORD_BYTES = HordeWireFormat::RECORD_BYTES,
		HEADER_BYTES = HordeWireFormat::HEADER_BYTES,
		MTU_PAYLOAD = HordeWireFormat::MTU_PAYLOAD,
		MAX_RECORDS_PER_PACKET = HordeWireFormat::MAX_RECORDS_PER_PACKET,
	};

private:
	static void _bind_methods();

public:
	// R3.5 record round-trip (p_wire_id is HordeAgents::_make_id()).
	PackedByteArray encode_record(int p_wire_id, const Vector3 &p_pos, float p_yaw, int p_state, const AABB &p_bounds) const;
	Dictionary decode_record(const PackedByteArray &p_bytes, int p_offset, const AABB &p_bounds) const;

	// R3.6 packet header.
	Dictionary decode_header(const PackedByteArray &p_bytes, int p_offset) const;

	// R3.9 reliable lifecycle events (ride game channel 2 as NetMessage types).
	PackedByteArray encode_spawn(int p_id, int p_archetype, const Vector3 &p_pos) const;
	Dictionary decode_spawn(const PackedByteArray &p_bytes) const;
	PackedByteArray encode_death(int p_id, int p_killer_hint, const Vector3 &p_impulse_dir) const;
	Dictionary decode_death(const PackedByteArray &p_bytes) const;
	PackedByteArray encode_despawn(int p_id) const;
	Dictionary decode_despawn(const PackedByteArray &p_bytes) const;

	HordeWireCodec() {}
};

VARIANT_ENUM_CAST(HordeWireCodec::Layout);
