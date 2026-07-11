/**************************************************************************/
/*  test_horde_wire.h                                                    */
/**************************************************************************/

#pragma once

#include "horde_test_helpers.h"

#include "../horde_wire.h"

// ===========================================================================
// T3 horde wire format (P2.1) — NET R3.5-R3.9, R8 budgets.
//
// Covers: R3.5 quantized record round-trip precision, R3.6 batching + header,
// R3.7 per-client interest + tiered send-rate phasing, R3.9 reliable lifecycle
// events, and the R8 pack-pass CPU budget (measured + printed).
// ===========================================================================

namespace TestHordeSim {

// Smallest signed angular difference, wrap-aware.
static float wire_angle_diff(float a, float b) {
	float d = a - b;
	while (d > (float)(Math::TAU * 0.5)) {
		d -= (float)Math::TAU;
	}
	while (d < -(float)(Math::TAU * 0.5)) {
		d += (float)Math::TAU;
	}
	return Math::abs(d);
}

// Walk a packed snapshot buffer (one or more concatenated packets) and return
// every decoded record id, ascending. Mirrors what the P2.1b client does.
static PackedInt32Array wire_collect_ids(const Ref<HordeWireCodec> &codec, const PackedByteArray &buf, const AABB &bounds) {
	PackedInt32Array ids;
	int off = 0;
	while (off + (int)HordeWireCodec::HEADER_BYTES <= buf.size()) {
		Dictionary h = codec->decode_header(buf, off);
		const int count = (int)h["count"];
		off += (int)HordeWireCodec::HEADER_BYTES;
		for (int r = 0; r < count; r++) {
			Dictionary rec = codec->decode_record(buf, off, bounds);
			ids.push_back((int)rec["id"]);
			off += (int)HordeWireCodec::RECORD_BYTES;
		}
	}
	ids.sort();
	return ids;
}

// ---------------------------------------------------------------------------
// 19. R3.5 record quantization round-trip precision.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Quantized record round-trips within <= 1 cm at 650 m bounds") {
	Ref<HordeWireCodec> codec;
	codec.instantiate();

	// A 650 m map (R3.5's stated worst case) with a 30 m vertical extent.
	const AABB bounds(Vector3(0, 0, 0), Vector3(650, 30, 650));

	// Horizontal precision: 16 bits over 650 m => half-step ~= 0.5 cm.
	const Vector3 samples[] = {
		Vector3(0, 0, 0),
		Vector3(650, 30, 650),
		Vector3(325, 15, 325),
		Vector3(123.45f, 7.7f, 600.01f),
		Vector3(1.0f, 0.05f, 649.0f),
	};
	for (const Vector3 &p : samples) {
		PackedByteArray rec = codec->encode_record(42, p, 0.0f, HordeAgents::STATE_ADVANCE, bounds);
		CHECK(rec.size() == (int)HordeWireCodec::RECORD_BYTES);
		Dictionary d = codec->decode_record(rec, 0, bounds);
		const Vector3 out = d["position"];
		CHECK(Math::abs(out.x - p.x) <= 0.01f); // <= 1 cm horizontal (R3.5).
		CHECK(Math::abs(out.z - p.z) <= 0.01f);
		CHECK(Math::abs(out.y - p.y) <= 0.06f); // 9-bit vertical: one step over 30 m.
	}

	// id: full 11-bit wire id (10-bit slot | epoch parity) survives intact.
	const int ids[] = { 0, 1, 1023, 1024, 2047 };
	for (int id : ids) {
		PackedByteArray rec = codec->encode_record(id, Vector3(10, 1, 10), 0.0f, 3, bounds);
		Dictionary d = codec->decode_record(rec, 0, bounds);
		CHECK((int)d["id"] == id);
	}

	// yaw: 8 bits over a full turn => <= 1.4 deg (0.0246 rad) error.
	const float yaws[] = { -3.0f, -1.5f, 0.0f, 0.9f, 1.5f, 3.0f };
	for (float y : yaws) {
		PackedByteArray rec = codec->encode_record(7, Vector3(10, 1, 10), y, 2, bounds);
		Dictionary d = codec->decode_record(rec, 0, bounds);
		CHECK(wire_angle_diff((float)d["yaw"], y) <= 0.0246f);
	}

	// state: every FSM ordinal (<= 16, 4 bits) round-trips.
	for (int s = 0; s < HordeAgents::STATE_MAX; s++) {
		PackedByteArray rec = codec->encode_record(1, Vector3(5, 1, 5), 0.0f, s, bounds);
		Dictionary d = codec->decode_record(rec, 0, bounds);
		CHECK((int)d["state"] == s);
	}
}

// ---------------------------------------------------------------------------
// 20. R3.6 batching: MTU-bounded packets with a [tick u32][count u16] header.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Snapshot batches into ~140-record MTU packets with headers") {
	Ref<HordeAgents> agents = make_agents(250);
	Ref<HordeWireCodec> codec;
	codec.instantiate();

	const AABB bounds(Vector3(0, 0, 0), Vector3(300, 20, 300));
	// 250 agents tightly clustered so a single client sees them all as hot.
	for (int i = 0; i < 250; i++) {
		agents->spawn(0, Vector3(100.0f + 0.01f * i, 0, 100.0f), HordeAgents::STATE_ADVANCE);
	}
	CHECK(agents->get_active_count() == 250);

	PackedByteArray buf;
	const uint32_t server_tick = 0xABCD1234u;
	const int packets = agents->pack_snapshot_into(server_tick, Vector3(100, 0, 100), bounds,
			/*hot*/ 1000.0f, /*mid*/ 2000.0f, HordeWireScheduler::SEND_HOT, buf);

	// 250 records at 149/packet => 2 packets (149 + 101).
	CHECK(packets == 2);
	const int expected_bytes = 250 * (int)HordeWireCodec::RECORD_BYTES + packets * (int)HordeWireCodec::HEADER_BYTES;
	CHECK(buf.size() == expected_bytes);

	// Walk the packets: verify headers, counts, per-packet MTU ceiling.
	int off = 0;
	int total_records = 0;
	int packet_index = 0;
	while (off < buf.size()) {
		Dictionary h = codec->decode_header(buf, off);
		CHECK((int64_t)h["tick"] == (int64_t)server_tick); // Every packet carries the server tick.
		const int count = (int)h["count"];
		CHECK(count <= (int)HordeWireCodec::MAX_RECORDS_PER_PACKET);
		const int packet_bytes = (int)HordeWireCodec::HEADER_BYTES + count * (int)HordeWireCodec::RECORD_BYTES;
		CHECK(packet_bytes <= (int)HordeWireCodec::MTU_PAYLOAD); // <= ~1200 B payload (R3.6).
		if (packet_index == 0) {
			CHECK(count == (int)HordeWireCodec::MAX_RECORDS_PER_PACKET); // First packet fills to the MTU.
		}
		total_records += count;
		off += packet_bytes;
		packet_index++;
	}
	CHECK(off == buf.size()); // Packets tile the buffer exactly.
	CHECK(total_records == 250);
	CHECK(packet_index == 2);
}

// ---------------------------------------------------------------------------
// 21. R3.7 per-client interest: two client positions get different hot sets.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Per-client relevance yields different hot sets") {
	Ref<HordeAgents> agents = make_agents(64);
	Ref<HordeWireCodec> codec;
	codec.instantiate();
	const AABB bounds(Vector3(0, 0, 0), Vector3(650, 20, 650));

	// Two clusters far apart: one by the roof stairs, one in the basement corner.
	PackedInt32Array cluster_a, cluster_b;
	for (int i = 0; i < 5; i++) {
		cluster_a.push_back(agents->spawn(0, Vector3(20.0f + i, 0, 20.0f), HordeAgents::STATE_ADVANCE));
	}
	for (int i = 0; i < 5; i++) {
		cluster_b.push_back(agents->spawn(0, Vector3(500.0f + i, 0, 500.0f), HordeAgents::STATE_ADVANCE));
	}
	cluster_a.sort();
	cluster_b.sort();

	PackedByteArray buf_a, buf_b;
	// hot radius 50 m: each client sees only its own cluster.
	agents->pack_snapshot_into(1, Vector3(22, 0, 22), bounds, 50.0f, 90.0f, HordeWireScheduler::SEND_HOT, buf_a);
	agents->pack_snapshot_into(1, Vector3(502, 0, 502), bounds, 50.0f, 90.0f, HordeWireScheduler::SEND_HOT, buf_b);

	PackedInt32Array ids_a = wire_collect_ids(codec, buf_a, bounds);
	PackedInt32Array ids_b = wire_collect_ids(codec, buf_b, bounds);

	// Client A's hot set is exactly cluster A; client B's is exactly cluster B.
	CHECK(ids_a == cluster_a);
	CHECK(ids_b == cluster_b);
	CHECK(ids_a != ids_b);
}

// ---------------------------------------------------------------------------
// 22. R3.7 tiered rate phasing: no tick fires hot+mid+far at once; each tier
// fires at its own cadence; client_phase staggers clients off each other.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Send-rate phases interleave without a full-tier burst") {
	Ref<HordeWireScheduler> sched;
	sched.instantiate();

	const int all = HordeWireScheduler::SEND_HOT | HordeWireScheduler::SEND_MID | HordeWireScheduler::SEND_FAR;
	int hot_fires = 0, mid_fires = 0, far_fires = 0;
	const int window = 960; // A whole number of hot (6) and far (32) periods.
	for (int t = 0; t < window; t++) {
		const int m = sched->due_mask(t);
		// The load-bearing invariant: a client never bursts a full hot+mid+far
		// send on one tick (hot lands on even ticks, far on odd -- disjoint).
		CHECK((m & all) != all);
		const bool hot_and_far = (m & HordeWireScheduler::SEND_HOT) && (m & HordeWireScheduler::SEND_FAR);
		CHECK_FALSE(hot_and_far);
		if (m & HordeWireScheduler::SEND_HOT) {
			hot_fires++;
		}
		if (m & HordeWireScheduler::SEND_MID) {
			mid_fires++;
		}
		if (m & HordeWireScheduler::SEND_FAR) {
			far_fires++;
		}
	}
	// Cadence ordering holds: hot (~20 Hz) > mid (~10 Hz) > far (4 Hz).
	CHECK(hot_fires > mid_fires);
	CHECK(mid_fires > far_fires);
	CHECK(far_fires == window / 32); // 4 Hz over 128 Hz = exactly window/32.
	CHECK(hot_fires == window / 6); // ~20 Hz.

	// client_phase staggers a second client so their hot sends don't co-fire
	// every tick (host send-load smoothing).
	Ref<HordeWireScheduler> sched2;
	sched2.instantiate();
	sched2->set_client_phase(1);
	bool differs = false;
	for (int t = 0; t < window && !differs; t++) {
		if ((sched->due_mask(t) & HordeWireScheduler::SEND_HOT) != (sched2->due_mask(t) & HordeWireScheduler::SEND_HOT)) {
			differs = true;
		}
	}
	CHECK(differs);
}

// ---------------------------------------------------------------------------
// 23. Integration: HordeAgents.pack records decode back to live agent state.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Packed records round-trip to live agent position/state") {
	Ref<HordeAgents> agents = make_agents(8);
	Ref<HordeWireCodec> codec;
	codec.instantiate();
	const AABB bounds(Vector3(0, 0, 0), Vector3(200, 10, 200));

	const int id0 = agents->spawn(0, Vector3(30.5f, 1.5f, 40.25f), HordeAgents::STATE_CHASE);
	const int id1 = agents->spawn(1, Vector3(60.0f, 2.0f, 12.0f), HordeAgents::STATE_ADVANCE);

	PackedByteArray buf;
	agents->pack_snapshot_into(99, Vector3(45, 0, 30), bounds, 1000.0f, 1000.0f, HordeWireScheduler::SEND_HOT, buf);

	Dictionary h = codec->decode_header(buf, 0);
	REQUIRE((int)h["count"] == 2);

	// Records are emitted in ascending slot order (id0 then id1).
	Dictionary r0 = codec->decode_record(buf, (int)HordeWireCodec::HEADER_BYTES, bounds);
	Dictionary r1 = codec->decode_record(buf, (int)HordeWireCodec::HEADER_BYTES + (int)HordeWireCodec::RECORD_BYTES, bounds);

	CHECK((int)r0["id"] == id0);
	CHECK((int)r0["state"] == HordeAgents::STATE_CHASE);
	const Vector3 p0 = r0["position"];
	CHECK(p0.distance_to(agents->get_agent_position(id0)) <= 0.02f);

	CHECK((int)r1["id"] == id1);
	CHECK((int)r1["state"] == HordeAgents::STATE_ADVANCE);
	const Vector3 p1 = r1["position"];
	CHECK(p1.distance_to(agents->get_agent_position(id1)) <= 0.02f);
}

// ---------------------------------------------------------------------------
// 24. R3.9 reliable lifecycle events: spawn / death / despawn round-trip.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] Lifecycle event encode/decode round-trips") {
	Ref<HordeWireCodec> codec;
	codec.instantiate();

	// spawn(id, archetype, pos).
	{
		Dictionary d = codec->decode_spawn(codec->encode_spawn(1234, 3, Vector3(12.5f, 3.25f, -7.5f)));
		CHECK((int)d["id"] == 1234);
		CHECK((int)d["archetype"] == 3);
		const Vector3 p = d["position"];
		CHECK(p == Vector3(12.5f, 3.25f, -7.5f)); // Full float32: exact.
	}

	// death(id, killer_hint, impulse_dir) -- with a killer, then without.
	{
		Dictionary d = codec->decode_death(codec->encode_death(77, 2, Vector3(0.5f, 1.0f, -0.25f)));
		CHECK((int)d["id"] == 77);
		CHECK((int)d["killer_hint"] == 2);
		const Vector3 imp = d["impulse_dir"];
		CHECK(imp == Vector3(0.5f, 1.0f, -0.25f)); // Hero-clip impulse stays precise (R4.3).

		Dictionary d2 = codec->decode_death(codec->encode_death(77, -1, Vector3(1, 0, 0)));
		CHECK((int)d2["killer_hint"] == -1); // Sentinel for "no killer hint".
	}

	// despawn(id).
	{
		Dictionary d = codec->decode_despawn(codec->encode_despawn(2047));
		CHECK((int)d["id"] == 2047);
	}
}

// ---------------------------------------------------------------------------
// 25. R8 budget: pack pass for 250 agents x 1 client, measured + printed.
// ---------------------------------------------------------------------------
TEST_CASE("[HordeSim][Wire] 250-agent pack pass stays within the dev budget") {
	Ref<HordeAgents> agents = make_agents(250);
	const AABB bounds(Vector3(0, 0, 0), Vector3(650, 30, 650));

	// A realistic spread across the map so the client-relevance classify sees a
	// full mix of hot/mid/far distances (worst case: everything due this tick).
	for (int i = 0; i < 250; i++) {
		const float wx = (float)((i * 13) % 640) + 1.0f;
		const float wz = (float)((i * 29) % 640) + 1.0f;
		agents->spawn(i & 1, Vector3(wx, 0, wz), HordeAgents::STATE_ADVANCE);
	}
	CHECK(agents->get_active_count() == 250);

	const Vector3 client(40, 0, 40);
	PackedByteArray buf; // Reused across iterations: zero per-call heap on the walk.
	const int all = HordeWireScheduler::SEND_HOT | HordeWireScheduler::SEND_MID | HordeWireScheduler::SEND_FAR;

	// Warm up, then take the min pack time over N runs (measures the algorithm,
	// not test-box scheduling noise -- D-016 discipline).
	for (int w = 0; w < 8; w++) {
		agents->pack_snapshot_into((uint32_t)w, client, bounds, 30.0f, 70.0f, all, buf);
	}
	uint64_t usec = UINT64_MAX;
	int packets = 0;
	for (int t = 0; t < 16; t++) {
		packets = agents->pack_snapshot_into((uint32_t)t, client, bounds, 30.0f, 70.0f, all, buf);
		usec = MIN(usec, agents->get_pack_time_usec());
	}
	print_line(vformat("[HordeSim] pack pass 250 agents x 1 client: %d us (%.3f ms) | %d packets, %d bytes",
			(int64_t)usec, usec / 1000.0, packets, buf.size()));

	// Dev-build (/Od-class) gate. The R8 release ceiling is <= 2 ms/tick to build
	// replication for ALL clients; one client's pack must sit far under 1 ms here.
	CHECK(usec < 1000);
}

} // namespace TestHordeSim
