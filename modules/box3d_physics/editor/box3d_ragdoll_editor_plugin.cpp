/**************************************************************************/
/*  box3d_ragdoll_editor_plugin.cpp                                       */
/**************************************************************************/

#include "box3d_ragdoll_editor_plugin.h"

#include "../box3d_ragdoll.h"

#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/tree.h"

static constexpr int RAGDOLL_CHAIN_COLOR_COUNT = 8;

Box3DRagdollGizmoPlugin::Box3DRagdollGizmoPlugin() {
	const Color colors[RAGDOLL_CHAIN_COLOR_COUNT] = {
		Color(0.23, 0.78, 0.95),
		Color(0.96, 0.53, 0.28),
		Color(0.45, 0.85, 0.48),
		Color(0.91, 0.38, 0.52),
		Color(0.76, 0.58, 0.96),
		Color(0.96, 0.78, 0.24),
		Color(0.34, 0.82, 0.73),
		Color(0.92, 0.47, 0.84),
	};
	chain_material_names.resize(RAGDOLL_CHAIN_COLOR_COUNT);
	for (int i = 0; i < RAGDOLL_CHAIN_COLOR_COUNT; i++) {
		chain_material_names[i] = vformat("chain_%d", i);
		create_material(chain_material_names[i], colors[i]);
	}
	line_generator.instantiate();
}

bool Box3DRagdollGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<Box3DRagdoll>(p_spatial) != nullptr;
}

String Box3DRagdollGizmoPlugin::get_gizmo_name() const {
	return "Box3DRagdoll";
}

int Box3DRagdollGizmoPlugin::get_priority() const {
	return -1;
}

void Box3DRagdollGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	Box3DRagdoll *ragdoll = Object::cast_to<Box3DRagdoll>(p_gizmo->get_node_3d());
	if (ragdoll == nullptr || ragdoll->get_profile().is_null()) {
		return;
	}
	Skeleton3D *skeleton = ragdoll->get_skeleton();
	if (skeleton == nullptr) {
		return;
	}

	const Dictionary groups = line_generator->get_gizmo_line_groups(ragdoll->get_profile(), skeleton);
	const Array group_names = groups.keys();
	const Transform3D node_inverse = ragdoll->get_global_transform().affine_inverse();
	for (int group_idx = 0; group_idx < group_names.size(); group_idx++) {
		const PackedVector3Array world_lines = groups[group_names[group_idx]];
		if (world_lines.is_empty()) {
			continue;
		}
		Vector<Vector3> local_lines;
		local_lines.resize(world_lines.size());
		for (int i = 0; i < world_lines.size(); i++) {
			local_lines.write[i] = node_inverse.xform(world_lines[i]);
		}
		const String &material_name = chain_material_names[group_idx % RAGDOLL_CHAIN_COLOR_COUNT];
		p_gizmo->add_lines(local_lines, get_material(material_name, p_gizmo));
		p_gizmo->add_collision_segments(local_lines);
	}
}

class Box3DRagdollSimulationControls : public HBoxContainer {
	GDCLASS(Box3DRagdollSimulationControls, HBoxContainer);

	ObjectID ragdoll_id;
	Button *simulate_button = nullptr;
	Button *reset_button = nullptr;

	Box3DRagdoll *_get_ragdoll() const {
		return Object::cast_to<Box3DRagdoll>(ObjectDB::get_instance(ragdoll_id));
	}

	void _simulation_toggled(bool p_enabled) {
		Box3DRagdoll *ragdoll = _get_ragdoll();
		if (ragdoll == nullptr) {
			return;
		}
		ragdoll->set_simulate_in_editor(p_enabled);
		simulate_button->set_pressed_no_signal(ragdoll->is_simulating_in_editor());
	}

	void _reset_pressed() {
		Box3DRagdoll *ragdoll = _get_ragdoll();
		if (ragdoll == nullptr) {
			return;
		}
		ragdoll->reset_simulation();
		simulate_button->set_pressed_no_signal(false);
	}

protected:
	void _notification(int p_what) {
		if (p_what == NOTIFICATION_ENTER_TREE) {
			simulate_button->set_button_icon(get_editor_theme_icon(SNAME("Play")));
			reset_button->set_button_icon(get_editor_theme_icon(SNAME("Reload")));
		}
	}

public:
	explicit Box3DRagdollSimulationControls(Box3DRagdoll *p_ragdoll) {
		ragdoll_id = p_ragdoll->get_instance_id();
		simulate_button = memnew(Button);
		simulate_button->set_text(TTR("Simulate"));
		simulate_button->set_toggle_mode(true);
		simulate_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		simulate_button->set_pressed_no_signal(p_ragdoll->is_simulating_in_editor());
		simulate_button->connect(SceneStringName(toggled), callable_mp(this, &Box3DRagdollSimulationControls::_simulation_toggled));
		add_child(simulate_button);

		reset_button = memnew(Button);
		reset_button->set_text(TTR("Reset"));
		reset_button->set_tooltip_text(TTR("Stop the private ragdoll preview and restore the captured skeleton pose."));
		reset_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DRagdollSimulationControls::_reset_pressed));
		add_child(reset_button);
	}
};

class Box3DRagdollInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(Box3DRagdollInspectorPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override {
		return Object::cast_to<Box3DRagdoll>(p_object) != nullptr;
	}

	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override {
		if (p_path != "simulate_in_editor") {
			return false;
		}
		Box3DRagdoll *ragdoll = Object::cast_to<Box3DRagdoll>(p_object);
		if (ragdoll != nullptr) {
			add_custom_control(memnew(Box3DRagdollSimulationControls(ragdoll)));
		}
		return true;
	}
};

class Box3DRagdollProfileBonesEditor : public VBoxContainer {
	GDCLASS(Box3DRagdollProfileBonesEditor, VBoxContainer);

	Ref<Box3DRagdollProfile> profile;
	Tree *tree = nullptr;

	void _item_edited() {
		TreeItem *item = tree->get_edited();
		const int column = tree->get_edited_column();
		if (item == nullptr || column < 1 || column > 6 || item->get_metadata(0).get_type() != Variant::INT) {
			return;
		}
		const int entry_index = item->get_metadata(0);
		TypedArray<Dictionary> old_bones = profile->get_bones();
		TypedArray<Dictionary> new_bones = old_bones.duplicate(true);
		if (entry_index < 0 || entry_index >= new_bones.size()) {
			return;
		}
		Dictionary entry = new_bones[entry_index];
		switch (column) {
			case 1:
				entry[SNAME("joint_type")] = (int)item->get_range(column);
				break;
			case 2:
				entry[SNAME("radius")] = item->get_range(column);
				break;
			case 3:
				entry[SNAME("height")] = item->get_range(column);
				break;
			case 4:
				entry[SNAME("swing_limit")] = Math::deg_to_rad(item->get_range(column));
				break;
			case 5:
				entry[SNAME("twist_lower")] = Math::deg_to_rad(item->get_range(column));
				break;
			case 6:
				entry[SNAME("twist_upper")] = Math::deg_to_rad(item->get_range(column));
				break;
		}
		new_bones[entry_index] = entry;

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Edit Box3D Ragdoll Bone"));
		undo_redo->add_do_method(profile.ptr(), "set_bones", new_bones);
		undo_redo->add_undo_method(profile.ptr(), "set_bones", old_bones);
		undo_redo->commit_action();
	}

	void _build_tree() {
		tree->clear();
		TreeItem *root = tree->create_item();
		const HashMap<StringName, StringName> bone_to_chain = profile->build_bone_to_chain_map();

		HashMap<StringName, TreeItem *> chain_items;
		const TypedArray<Dictionary> bones = profile->get_bones();
		for (int i = 0; i < bones.size(); i++) {
			const Dictionary entry = bones[i];
			const StringName bone_name = entry.get(SNAME("bone"), StringName());
			const StringName *found_chain = bone_to_chain.getptr(bone_name);
			const StringName chain_name = found_chain != nullptr ? *found_chain : Box3DRagdollProfile::ungrouped_chain_name();
			TreeItem **chain_item_ptr = chain_items.getptr(chain_name);
			TreeItem *chain_item = chain_item_ptr != nullptr ? *chain_item_ptr : nullptr;
			if (chain_item == nullptr) {
				chain_item = tree->create_item(root);
				chain_item->set_text(0, String(chain_name).capitalize());
				chain_item->set_collapsed(false);
				chain_items[chain_name] = chain_item;
			}

			TreeItem *row = tree->create_item(chain_item);
			row->set_text(0, String(bone_name));
			row->set_metadata(0, i);
			row->set_cell_mode(1, TreeItem::CELL_MODE_RANGE);
			row->set_range_config(1, 0, 2, 1);
			row->set_text(1, TTR("None") + "," + TTR("Spherical") + "," + TTR("Revolute"));
			const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(entry);
			row->set_range(1, (int)params.joint_type);
			row->set_editable(1, true);

			const real_t values[5] = {
				params.radius,
				params.height,
				Math::rad_to_deg(params.swing_limit),
				Math::rad_to_deg(params.twist_lower),
				Math::rad_to_deg(params.twist_upper),
			};
			for (int column = 2; column <= 6; column++) {
				row->set_cell_mode(column, TreeItem::CELL_MODE_RANGE);
				if (column <= 3) {
					row->set_range_config(column, column == 2 ? 0.01 : 0.02, column == 2 ? 2.0 : 5.0, 0.01);
					row->set_suffix(column, " m");
				} else {
					row->set_range_config(column, column == 4 ? 0.0 : -180.0, 180.0, 1.0);
					row->set_suffix(column, " deg");
				}
				row->set_range(column, values[column - 2]);
				row->set_editable(column, true);
			}
		}
		set_custom_minimum_size(Size2(0, CLAMP((int)bones.size() * 24 + (int)chain_items.size() * 24 + 56, 180, 420)));
	}

public:
	explicit Box3DRagdollProfileBonesEditor(const Ref<Box3DRagdollProfile> &p_profile) {
		profile = p_profile;
		Label *heading = memnew(Label);
		heading->set_text(TTR("Bone Chains"));
		heading->set_theme_type_variation(SNAME("HeaderSmall"));
		add_child(heading);

		tree = memnew(Tree);
		tree->set_columns(7);
		tree->set_hide_root(true);
		tree->set_column_titles_visible(true);
		const String titles[7] = { TTR("Bone"), TTR("Joint"), TTR("Radius"), TTR("Height"), TTR("Swing"), TTR("Twist Min"), TTR("Twist Max") };
		for (int i = 0; i < 7; i++) {
			tree->set_column_title(i, titles[i]);
			tree->set_column_custom_minimum_width(i, i == 0 ? 110 : 76);
		}
		tree->connect(SNAME("item_edited"), callable_mp(this, &Box3DRagdollProfileBonesEditor::_item_edited));
		add_child(tree);
		_build_tree();
	}
};

class Box3DRagdollProfileInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(Box3DRagdollProfileInspectorPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override {
		return Object::cast_to<Box3DRagdollProfile>(p_object) != nullptr;
	}

	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override {
		if (p_path != "bones") {
			return false;
		}
		Box3DRagdollProfile *profile_ptr = Object::cast_to<Box3DRagdollProfile>(p_object);
		if (profile_ptr != nullptr) {
			add_custom_control(memnew(Box3DRagdollProfileBonesEditor(Ref<Box3DRagdollProfile>(profile_ptr))));
		}
		return true;
	}
};

Box3DRagdollEditorPlugin::Box3DRagdollEditorPlugin() {
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);

	Ref<Box3DRagdollInspectorPlugin> ragdoll_inspector;
	ragdoll_inspector.instantiate();
	add_inspector_plugin(ragdoll_inspector);
	Ref<Box3DRagdollProfileInspectorPlugin> profile_inspector;
	profile_inspector.instantiate();
	add_inspector_plugin(profile_inspector);
}
