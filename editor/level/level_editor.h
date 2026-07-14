/**************************************************************************/
/*  level_editor.h                                                        */
/**************************************************************************/
/*  G-Level LE0: SERVICE state for the level-editor workspace seam.       */
/*  Per-pane render state belongs to LevelEditorView (ARCHITECTURE.md).    */
/**************************************************************************/

#pragma once

#include "core/math/dynamic_bvh.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/main/node.h"

class LevelBlock;
class LevelDocument;
class LevelEditorView;

class LevelEditor : public Node {
	GDCLASS(LevelEditor, Node);

public:
	enum ToolMode {
		TOOL_SELECT,
		TOOL_BLOCK,
	};
	static constexpr real_t DEFAULT_SNAP_STEP = 1.0;
	static constexpr real_t DEFAULT_BLOCK_HEIGHT = 3.0;
	static constexpr int MAJOR_GRID_MULTIPLE = 4;

private:
	static LevelEditor *singleton;
	ToolMode tool_mode = TOOL_SELECT;
	real_t snap_step = DEFAULT_SNAP_STEP;
	bool snap_enabled = true;
	real_t default_block_height = DEFAULT_BLOCK_HEIGHT;
	LocalVector<LevelEditorView *> views;
	HashMap<ObjectID, DynamicBVH::ID> block_bvh_ids;

	void _register_view(LevelEditorView *p_view);
	void _unregister_view(LevelEditorView *p_view);
	void _scan_node(Node *p_node);
	void _node_added(Node *p_node);
	void _node_removed(Node *p_node);
	void _track_block(LevelBlock *p_block);
	void _untrack_block(LevelBlock *p_block);
	void _register_block(ObjectID p_block_id);
	void _unregister_block(ObjectID p_block_id);
	void _block_world_entered(int64_t p_block_id);
	void _block_world_exiting(int64_t p_block_id);
	void _block_baked(int64_t p_block_id);
	void _block_transform_changed(int64_t p_block_id);
	bool _get_block_world_aabb(LevelBlock *p_block, AABB &r_aabb) const;

	friend class LevelEditorView;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static LevelEditor *get_singleton() { return singleton; }
	static real_t snap_step_or_default();

	ToolMode get_tool_mode() const { return tool_mode; }
	void set_tool_mode(ToolMode p_mode);
	real_t get_snap_step() const { return snap_step; }
	void set_snap_step(real_t p_step);
	bool is_snap_enabled() const { return snap_enabled; }
	void set_snap_enabled(bool p_enabled) { snap_enabled = p_enabled; }
	real_t get_default_block_height() const { return default_block_height; }

	// G-Level LE0: mint one unparented VIEW for the requesting document/pane.
	// DocumentView owns placement and lifetime; this service retains no render state.
	LevelEditorView *create_editor_view(LevelDocument *p_document);

	LevelEditor();
	~LevelEditor();
};

VARIANT_ENUM_CAST(LevelEditor::ToolMode);
