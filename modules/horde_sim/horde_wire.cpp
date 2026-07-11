/**************************************************************************/
/*  horde_wire.cpp                                                        */
/**************************************************************************/

#include "horde_wire.h"

#include "core/io/marshalls.h"
#include "core/object/class_db.h"

// --- HordeWireScheduler ------------------------------------------------------

int HordeWireScheduler::due_mask(int64_t p_server_tick) const {
	// client_phase staggers this client's send ticks off the others. Ticks are
	// non-negative and offsets are folded into range, so the raw modulo is safe.
	const int64_t t = p_server_tick + client_phase;
	int m = SEND_NONE;
	if (hot_period > 0 && (int)(t % hot_period) == _clamp_offset(hot_offset, hot_period)) {
		m |= SEND_HOT;
	}
	if (mid_period > 0 && (int)(t % mid_period) == _clamp_offset(mid_offset, mid_period)) {
		m |= SEND_MID;
	}
	if (far_period > 0 && (int)(t % far_period) == _clamp_offset(far_offset, far_period)) {
		m |= SEND_FAR;
	}
	return m;
}

void HordeWireScheduler::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_hot_period", "ticks"), &HordeWireScheduler::set_hot_period);
	ClassDB::bind_method(D_METHOD("get_hot_period"), &HordeWireScheduler::get_hot_period);
	ClassDB::bind_method(D_METHOD("set_mid_period", "ticks"), &HordeWireScheduler::set_mid_period);
	ClassDB::bind_method(D_METHOD("get_mid_period"), &HordeWireScheduler::get_mid_period);
	ClassDB::bind_method(D_METHOD("set_far_period", "ticks"), &HordeWireScheduler::set_far_period);
	ClassDB::bind_method(D_METHOD("get_far_period"), &HordeWireScheduler::get_far_period);
	ClassDB::bind_method(D_METHOD("set_hot_offset", "offset"), &HordeWireScheduler::set_hot_offset);
	ClassDB::bind_method(D_METHOD("get_hot_offset"), &HordeWireScheduler::get_hot_offset);
	ClassDB::bind_method(D_METHOD("set_mid_offset", "offset"), &HordeWireScheduler::set_mid_offset);
	ClassDB::bind_method(D_METHOD("get_mid_offset"), &HordeWireScheduler::get_mid_offset);
	ClassDB::bind_method(D_METHOD("set_far_offset", "offset"), &HordeWireScheduler::set_far_offset);
	ClassDB::bind_method(D_METHOD("get_far_offset"), &HordeWireScheduler::get_far_offset);
	ClassDB::bind_method(D_METHOD("set_client_phase", "phase"), &HordeWireScheduler::set_client_phase);
	ClassDB::bind_method(D_METHOD("get_client_phase"), &HordeWireScheduler::get_client_phase);

	ClassDB::bind_method(D_METHOD("due_mask", "server_tick"), &HordeWireScheduler::due_mask);

	BIND_ENUM_CONSTANT(SEND_NONE);
	BIND_ENUM_CONSTANT(SEND_HOT);
	BIND_ENUM_CONSTANT(SEND_MID);
	BIND_ENUM_CONSTANT(SEND_FAR);
}

// --- HordeWireCodec ----------------------------------------------------------

using namespace HordeWireFormat;

PackedByteArray HordeWireCodec::encode_record(int p_wire_id, const Vector3 &p_pos, float p_yaw, int p_state, const AABB &p_bounds) const {
	PackedByteArray out;
	out.resize(RECORD_BYTES);
	encode_uint64(HordeWireFormat::encode_record((uint32_t)p_wire_id, p_pos, p_yaw, (uint32_t)p_state, p_bounds), out.ptrw());
	return out;
}

Dictionary HordeWireCodec::decode_record(const PackedByteArray &p_bytes, int p_offset, const AABB &p_bounds) const {
	Dictionary d;
	ERR_FAIL_COND_V_MSG(p_offset < 0 || p_offset + RECORD_BYTES > p_bytes.size(), d, "decode_record: buffer too short for a record at this offset.");
	int id, state;
	Vector3 pos;
	float yaw;
	HordeWireFormat::decode_record(decode_uint64(p_bytes.ptr() + p_offset), p_bounds, id, pos, yaw, state);
	d["id"] = id;
	d["position"] = pos;
	d["yaw"] = yaw;
	d["state"] = state;
	return d;
}

Dictionary HordeWireCodec::decode_header(const PackedByteArray &p_bytes, int p_offset) const {
	Dictionary d;
	ERR_FAIL_COND_V_MSG(p_offset < 0 || p_offset + HEADER_BYTES > p_bytes.size(), d, "decode_header: buffer too short for a packet header at this offset.");
	const uint8_t *p = p_bytes.ptr() + p_offset;
	d["tick"] = (int64_t)decode_uint32(p);
	d["count"] = (int)decode_uint16(p + 4);
	return d;
}

// Reliable lifecycle events (R3.9). Positions/directions ride full float32 --
// these are rare reliable one-shots, not per-tick bandwidth, and R4.3 wants the
// death impulse direction to stay precise (the ragdoll hero-clip moment).
static constexpr int SPAWN_BYTES = 2 + 1 + 12; // id u16, archetype u8, pos 3xf32.
static constexpr int DEATH_BYTES = 2 + 2 + 12; // id u16, killer u16, impulse 3xf32.
static constexpr int DESPAWN_BYTES = 2; // id u16.
static constexpr uint16_t KILLER_NONE = 0xFFFF; // Sentinel for "no killer hint".

PackedByteArray HordeWireCodec::encode_spawn(int p_id, int p_archetype, const Vector3 &p_pos) const {
	PackedByteArray out;
	out.resize(SPAWN_BYTES);
	uint8_t *w = out.ptrw();
	encode_uint16((uint16_t)(p_id & 0xFFFF), w);
	w[2] = (uint8_t)(p_archetype & 0xFF);
	encode_float(p_pos.x, w + 3);
	encode_float(p_pos.y, w + 7);
	encode_float(p_pos.z, w + 11);
	return out;
}

Dictionary HordeWireCodec::decode_spawn(const PackedByteArray &p_bytes) const {
	Dictionary d;
	ERR_FAIL_COND_V_MSG(p_bytes.size() < SPAWN_BYTES, d, "decode_spawn: buffer too short.");
	const uint8_t *p = p_bytes.ptr();
	d["id"] = (int)decode_uint16(p);
	d["archetype"] = (int)p[2];
	d["position"] = Vector3(decode_float(p + 3), decode_float(p + 7), decode_float(p + 11));
	return d;
}

PackedByteArray HordeWireCodec::encode_death(int p_id, int p_killer_hint, const Vector3 &p_impulse_dir) const {
	PackedByteArray out;
	out.resize(DEATH_BYTES);
	uint8_t *w = out.ptrw();
	encode_uint16((uint16_t)(p_id & 0xFFFF), w);
	encode_uint16(p_killer_hint < 0 ? KILLER_NONE : (uint16_t)(p_killer_hint & 0xFFFF), w + 2);
	encode_float(p_impulse_dir.x, w + 4);
	encode_float(p_impulse_dir.y, w + 8);
	encode_float(p_impulse_dir.z, w + 12);
	return out;
}

Dictionary HordeWireCodec::decode_death(const PackedByteArray &p_bytes) const {
	Dictionary d;
	ERR_FAIL_COND_V_MSG(p_bytes.size() < DEATH_BYTES, d, "decode_death: buffer too short.");
	const uint8_t *p = p_bytes.ptr();
	const uint16_t killer = decode_uint16(p + 2);
	d["id"] = (int)decode_uint16(p);
	d["killer_hint"] = killer == KILLER_NONE ? -1 : (int)killer;
	d["impulse_dir"] = Vector3(decode_float(p + 4), decode_float(p + 8), decode_float(p + 12));
	return d;
}

PackedByteArray HordeWireCodec::encode_despawn(int p_id) const {
	PackedByteArray out;
	out.resize(DESPAWN_BYTES);
	encode_uint16((uint16_t)(p_id & 0xFFFF), out.ptrw());
	return out;
}

Dictionary HordeWireCodec::decode_despawn(const PackedByteArray &p_bytes) const {
	Dictionary d;
	ERR_FAIL_COND_V_MSG(p_bytes.size() < DESPAWN_BYTES, d, "decode_despawn: buffer too short.");
	d["id"] = (int)decode_uint16(p_bytes.ptr());
	return d;
}

void HordeWireCodec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("encode_record", "wire_id", "position", "yaw", "state", "bounds"), &HordeWireCodec::encode_record);
	ClassDB::bind_method(D_METHOD("decode_record", "bytes", "offset", "bounds"), &HordeWireCodec::decode_record);
	ClassDB::bind_method(D_METHOD("decode_header", "bytes", "offset"), &HordeWireCodec::decode_header);

	ClassDB::bind_method(D_METHOD("encode_spawn", "id", "archetype", "position"), &HordeWireCodec::encode_spawn);
	ClassDB::bind_method(D_METHOD("decode_spawn", "bytes"), &HordeWireCodec::decode_spawn);
	ClassDB::bind_method(D_METHOD("encode_death", "id", "killer_hint", "impulse_dir"), &HordeWireCodec::encode_death);
	ClassDB::bind_method(D_METHOD("decode_death", "bytes"), &HordeWireCodec::decode_death);
	ClassDB::bind_method(D_METHOD("encode_despawn", "id"), &HordeWireCodec::encode_despawn);
	ClassDB::bind_method(D_METHOD("decode_despawn", "bytes"), &HordeWireCodec::decode_despawn);

	BIND_ENUM_CONSTANT(RECORD_BYTES);
	BIND_ENUM_CONSTANT(HEADER_BYTES);
	BIND_ENUM_CONSTANT(MTU_PAYLOAD);
	BIND_ENUM_CONSTANT(MAX_RECORDS_PER_PACKET);
}
