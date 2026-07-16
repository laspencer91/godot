/**************************************************************************/
/*  selection_model.h                                                     */
/**************************************************************************/
/*  G-Level S4: document-owned stable-handle sub-object selection.        */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class LevelBlock;
class LevelDocument;
class LevelMesh;
class LevelMeshData;
class LevelMeshDiff;
class Node;
class SceneTree;

class SelectionModel : public Object {
public:
	enum Feature {
		FEATURE_VERTEX,
		FEATURE_EDGE,
		FEATURE_FACE,
		FEATURE_MAX,
	};

	enum Mode {
		MODE_VERTEX,
		MODE_EDGE,
		MODE_FACE,
		MODE_OBJECT,
	};

	enum Tier {
		TIER_POLYGROUP,
		TIER_TRIANGLE,
	};

	enum Operation {
		OP_REPLACE,
		OP_ADD,
		OP_TOGGLE,
		OP_SUBTRACT,
	};

	enum HandleKind {
		HANDLE_VERTEX,
		HANDLE_EDGE,
		HANDLE_FACE,
	};

	struct Element {
		ObjectID block_id;
		int64_t handle = -1;
		// Face selections use a face-local fan-triangle index. Synthetic
		// triangle diagonals use a packed pair of face-local corner indices.
		int64_t sub_index = -1;
		Feature feature = FEATURE_VERTEX;
		Tier tier = TIER_POLYGROUP;
		HandleKind handle_kind = HANDLE_VERTEX;

		bool operator==(const Element &p_other) const {
			return block_id == p_other.block_id && handle == p_other.handle &&
					sub_index == p_other.sub_index && feature == p_other.feature &&
					tier == p_other.tier && handle_kind == p_other.handle_kind;
		}
	};

	struct SelectionOp {
		Feature feature = FEATURE_VERTEX;
		Tier tier = TIER_POLYGROUP;
		Operation operation = OP_REPLACE;
		Vector<Element> elements;
	};

private:
	LevelDocument *document = nullptr; // Owned by EditorData; owns this model.
	SceneTree *scene_tree = nullptr;
	Vector<Element> selected[FEATURE_MAX];
	Element active[FEATURE_MAX];
	bool has_active[FEATURE_MAX] = {};
	Mode mode = MODE_FACE;
	Tier tier = TIER_POLYGROUP;
	uint64_t revision = 0;
	HashMap<ObjectID, Ref<LevelMeshData>> tracked_block_data;

	static bool _contains(const Vector<Element> &p_elements, const Element &p_element, int *r_index = nullptr);
	static void _append_dirty(Vector<ObjectID> &r_dirty, ObjectID p_block_id);
	void _emit_changed(const Vector<ObjectID> &p_dirty_blocks);
	void _emit_all_tracked_dirty();
	void _refresh_active(Feature p_feature);
	bool _belongs_to_document(LevelBlock *p_block) const;
	void _scan_node(Node *p_node);
	void _node_added(Node *p_node);
	void _node_removed(Node *p_node);
	void _track_block(LevelBlock *p_block);
	void _untrack_block(ObjectID p_block_id, bool p_drop_selection);
	void _block_baked(int64_t p_block_id);
	void _block_transform_changed(int64_t p_block_id);
	void _data_changed(int64_t p_block_id);
	void _mesh_diff_applied(const Ref<LevelMeshDiff> &p_diff, bool p_reverted, int64_t p_block_id);
	bool _drop_removed_handles(ObjectID p_block_id, const PackedInt64Array &p_vertex_handles,
			const PackedInt64Array &p_edge_handles, const PackedInt64Array &p_face_handles);

public:
	// WP7 transform/extrude handoff seam: operator tools snapshot these ordered
	// elements, then resolve each stable handle at use time before opening a diff.
	void bind_document(LevelDocument *p_document);
	LevelDocument *get_document() const { return document; }

	Mode get_mode() const { return mode; }
	Tier get_tier() const { return tier; }
	uint64_t get_revision() const { return revision; }
	void set_mode_and_tier(Mode p_mode, Tier p_tier);

	void apply(const SelectionOp &p_op);
	void clear();
	void revalidate(LevelBlock *p_block, const Ref<LevelMeshDiff> &p_diff, bool p_reverted);
	void mark_block_dirty(LevelBlock *p_block);

	const Vector<Element> &get_selected(Feature p_feature) const;
	int get_count(Feature p_feature) const;
	bool get_active(Feature p_feature, Element &r_element) const;
	bool is_active(const Element &p_element) const;
	bool is_block_tracked(ObjectID p_block_id) const;
	Vector<ObjectID> get_tracked_block_ids() const;
	bool resolve(const Element &p_element, LevelBlock *&r_block, Ref<LevelMesh> &r_mesh, int &r_slot) const;

	Array get_debug_entries() const;
	Dictionary get_debug_active() const;

	static int64_t encode_corner_pair(int p_corner_a, int p_corner_b);
	static bool decode_corner_pair(int64_t p_value, int &r_corner_a, int &r_corner_b);

	SelectionModel();
	~SelectionModel();
};
