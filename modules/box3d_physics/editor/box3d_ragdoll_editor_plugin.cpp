/**************************************************************************/
/*  box3d_ragdoll_editor_plugin.cpp                                       */
/**************************************************************************/

#include "box3d_ragdoll_editor_plugin.h"
#include "box3d_ragdoll_profile_dialog.h"

#include "../box3d_ragdoll.h"
#include "../box3d_physics_server_3d.h"

#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
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

class Box3DRagdollAuthoringControls : public VBoxContainer {
	GDCLASS(Box3DRagdollAuthoringControls, VBoxContainer);

	ObjectID ragdoll_id;
	Box3DRagdollEditorPlugin *editor_plugin = nullptr;
	Label *scene_label = nullptr;
	Button *generate_button = nullptr;
	Button *simulate_button = nullptr;
	Button *reset_button = nullptr;

	Box3DRagdoll *_get_ragdoll() const {
		return Object::cast_to<Box3DRagdoll>(ObjectDB::get_instance(ragdoll_id));
	}

	void _generate_pressed() {
		Box3DRagdoll *ragdoll = _get_ragdoll();
		if (ragdoll != nullptr && editor_plugin != nullptr) {
			editor_plugin->popup_profile_generator(ragdoll);
		}
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
	Box3DRagdollAuthoringControls(Box3DRagdoll *p_ragdoll, Box3DRagdollEditorPlugin *p_editor_plugin) {
		ragdoll_id = p_ragdoll->get_instance_id();
		editor_plugin = p_editor_plugin;

		String scene_name = TTR("Unknown scene");
		String scene_path;
		EditorData &editor_data = EditorNode::get_editor_data();
		for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
			Node *root = editor_data.get_edited_scene_root(i);
			if (root == nullptr || (root != p_ragdoll && !root->is_ancestor_of(p_ragdoll))) {
				continue;
			}
			scene_path = editor_data.get_scene_path(i);
			scene_name = scene_path.is_empty() ? String(root->get_name()) : scene_path.get_file();
			break;
		}
		scene_label = memnew(Label(vformat(TTR("Bound scene: %s"), scene_name)));
		scene_label->set_theme_type_variation(SNAME("HeaderSmall"));
		scene_label->set_tooltip_text(scene_path);
		add_child(scene_label);

		generate_button = memnew(Button);
		generate_button->set_text(p_ragdoll->get_profile().is_valid() ? TTR("Regenerate Profile...") : TTR("Generate Profile..."));
		generate_button->set_tooltip_text(vformat(TTR("Generate a Box3D ragdoll profile for %s. The operation remains bound to this scene if pane focus changes."), scene_name));
		generate_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DRagdollAuthoringControls::_generate_pressed));
		add_child(generate_button);

		HBoxContainer *simulation_row = memnew(HBoxContainer);
		add_child(simulation_row);
		simulate_button = memnew(Button);
		simulate_button->set_text(TTR("Simulate"));
		simulate_button->set_toggle_mode(true);
		simulate_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		simulate_button->set_pressed_no_signal(p_ragdoll->is_simulating_in_editor());
		simulate_button->set_disabled(Box3DPhysicsServer3D::get_singleton() == nullptr || p_ragdoll->get_profile().is_null());
		if (Box3DPhysicsServer3D::get_singleton() == nullptr) {
			simulate_button->set_tooltip_text(TTR("Box3D must be the active PhysicsServer3D backend to preview this ragdoll."));
		}
		simulate_button->connect(SceneStringName(toggled), callable_mp(this, &Box3DRagdollAuthoringControls::_simulation_toggled));
		simulation_row->add_child(simulate_button);

		reset_button = memnew(Button);
		reset_button->set_text(TTR("Reset"));
		reset_button->set_tooltip_text(TTR("Stop the private ragdoll preview and restore the captured skeleton pose."));
		reset_button->set_disabled(Box3DPhysicsServer3D::get_singleton() == nullptr || p_ragdoll->get_profile().is_null());
		reset_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DRagdollAuthoringControls::_reset_pressed));
		simulation_row->add_child(reset_button);
	}
};

class Box3DRagdollInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(Box3DRagdollInspectorPlugin, EditorInspectorPlugin);
	Box3DRagdollEditorPlugin *editor_plugin = nullptr;

public:
	void set_editor_plugin(Box3DRagdollEditorPlugin *p_editor_plugin) { editor_plugin = p_editor_plugin; }

	bool can_handle(Object *p_object) override {
		return Object::cast_to<Box3DRagdoll>(p_object) != nullptr;
	}

	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override {
		if (p_path != "simulate_in_editor") {
			return false;
		}
		Box3DRagdoll *ragdoll = Object::cast_to<Box3DRagdoll>(p_object);
		if (ragdoll != nullptr) {
			add_custom_control(memnew(Box3DRagdollAuthoringControls(ragdoll, editor_plugin)));
		}
		return true;
	}
};

class Box3DRagdollProfileBonesEditor : public VBoxContainer {
	GDCLASS(Box3DRagdollProfileBonesEditor, VBoxContainer);

	Ref<Box3DRagdollProfile> profile;
	Tree *tree = nullptr;
	VBoxContainer *detail_panel = nullptr;
	Label *selected_bone_label = nullptr;
	CheckBox *enabled = nullptr;
	OptionButton *joint_type = nullptr;
	SpinBox *radius = nullptr;
	SpinBox *height = nullptr;
	SpinBox *density_scale = nullptr;
	SpinBox *swing_limit = nullptr;
	SpinBox *twist_lower = nullptr;
	SpinBox *twist_upper = nullptr;
	SpinBox *joint_friction_scale = nullptr;
	SpinBox *blend = nullptr;
	int selected_entry = -1;
	bool updating = false;

	static String _joint_type_name(Box3DRagdollProfile::JointType p_type) {
		switch (p_type) {
			case Box3DRagdollProfile::JOINT_TYPE_NONE:
				return TTR("None");
			case Box3DRagdollProfile::JOINT_TYPE_REVOLUTE:
				return TTR("Hinge");
			default:
				return TTR("Spherical");
		}
	}

	void _add_detail_control(GridContainer *p_grid, const String &p_label, Control *p_control) {
		p_grid->add_child(memnew(Label(p_label)));
		p_control->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		p_grid->add_child(p_control);
	}

	void _commit_entry_value(const StringName &p_key, const Variant &p_value, const String &p_action, UndoRedo::MergeMode p_merge_mode = UndoRedo::MERGE_DISABLE) {
		if (updating || selected_entry < 0) {
			return;
		}
		const TypedArray<Dictionary> old_bones = profile->get_bones();
		if (selected_entry >= old_bones.size()) {
			return;
		}
		const Dictionary old_entry = old_bones[selected_entry];
		if (old_entry.get(p_key, Variant()) == p_value) {
			return;
		}
		TypedArray<Dictionary> new_bones = old_bones.duplicate(true);
		Dictionary new_entry = new_bones[selected_entry];
		new_entry[p_key] = p_value;
		new_bones[selected_entry] = new_entry;

		TreeItem *selected_item = tree->get_selected();
		if (selected_item != nullptr) {
			if (p_key == SNAME("joint_type")) {
				selected_item->set_text(1, _joint_type_name((Box3DRagdollProfile::JointType)(int)p_value));
			} else if (p_key == SNAME("radius")) {
				selected_item->set_text(2, vformat("%.3f m", (double)p_value));
			}
		}

		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(p_action, p_merge_mode);
		undo_redo->add_do_method(profile.ptr(), "set_bones", new_bones);
		undo_redo->add_undo_method(profile.ptr(), "set_bones", old_bones);
		undo_redo->commit_action();
	}

	void _enabled_toggled(bool p_enabled) {
		_commit_entry_value(SNAME("enabled"), p_enabled, TTR("Toggle Box3D Ragdoll Bone"));
	}

	void _joint_type_selected(int p_type) {
		_commit_entry_value(SNAME("joint_type"), p_type, TTR("Change Box3D Ragdoll Joint Type"));
	}

	void _number_changed(double p_value, const StringName &p_key, const String &p_action, bool p_degrees) {
		_commit_entry_value(p_key, p_degrees ? Variant(Math::deg_to_rad(p_value)) : Variant(p_value), p_action, UndoRedo::MERGE_ENDS);
	}

	void _selection_changed() {
		TreeItem *item = tree->get_selected();
		if (item == nullptr || item->get_metadata(0).get_type() != Variant::INT) {
			selected_entry = -1;
			detail_panel->hide();
			return;
		}
		selected_entry = item->get_metadata(0);
		const TypedArray<Dictionary> bones = profile->get_bones();
		if (selected_entry < 0 || selected_entry >= bones.size()) {
			detail_panel->hide();
			return;
		}

		const Dictionary entry = bones[selected_entry];
		const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(entry);
		updating = true;
		selected_bone_label->set_text(vformat(TTR("Selected Bone: %s"), String(entry.get(SNAME("bone"), StringName()))));
		enabled->set_pressed_no_signal(params.enabled);
		joint_type->select((int)params.joint_type);
		radius->set_value(params.radius);
		height->set_value(params.height);
		density_scale->set_value(params.density_scale);
		swing_limit->set_value(Math::rad_to_deg(params.swing_limit));
		twist_lower->set_value(Math::rad_to_deg(params.twist_lower));
		twist_upper->set_value(Math::rad_to_deg(params.twist_upper));
		joint_friction_scale->set_value(params.joint_friction_scale);
		blend->set_value(params.blend);
		updating = false;
		detail_panel->show();
	}

	void _build_tree() {
		tree->clear();
		TreeItem *root = tree->create_item();
		const HashMap<StringName, StringName> bone_to_chain = profile->build_bone_to_chain_map();

		HashMap<StringName, TreeItem *> chain_items;
		TreeItem *first_bone_item = nullptr;
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
				chain_item->set_expand_right(0, true);
				chain_item->set_selectable(0, false);
				chain_item->set_collapsed(false);
				chain_items[chain_name] = chain_item;
			}

			TreeItem *row = tree->create_item(chain_item);
			row->set_text(0, String(bone_name));
			row->set_tooltip_text(0, String(bone_name));
			row->set_metadata(0, i);
			const Box3DRagdollProfile::BoneParams params = Box3DRagdollProfile::parse_bone_entry(entry);
			row->set_text(1, _joint_type_name(params.joint_type));
			row->set_tooltip_text(1, _joint_type_name(params.joint_type));
			row->set_text(2, vformat("%.3f m", params.radius));
			row->set_tooltip_text(2, vformat("Radius: %.3f m", params.radius));
			if (first_bone_item == nullptr) {
				first_bone_item = row;
			}
		}
		tree->set_custom_minimum_size(Size2(0, CLAMP((int)bones.size() * 24 + (int)chain_items.size() * 24 + 56, 160, 280)));
		if (first_bone_item != nullptr) {
			first_bone_item->select(0);
			_selection_changed();
		} else {
			detail_panel->hide();
		}
	}

public:
	explicit Box3DRagdollProfileBonesEditor(const Ref<Box3DRagdollProfile> &p_profile) {
		profile = p_profile;
		Label *heading = memnew(Label);
		heading->set_text(TTR("Bone Chains"));
		heading->set_theme_type_variation(SNAME("HeaderSmall"));
		add_child(heading);

		tree = memnew(Tree);
		tree->set_columns(3);
		tree->set_hide_root(true);
		tree->set_column_titles_visible(true);
		tree->set_h_scroll_enabled(false);
		const String titles[3] = { TTR("Bone"), TTR("Joint"), TTR("Radius") };
		const int expand_ratios[3] = { 3, 2, 2 };
		for (int i = 0; i < 3; i++) {
			tree->set_column_title(i, titles[i]);
			tree->set_column_expand(i, true);
			tree->set_column_expand_ratio(i, expand_ratios[i]);
			tree->set_column_clip_content(i, true);
			tree->set_column_custom_minimum_width(i, 52);
		}
		tree->connect(SNAME("item_selected"), callable_mp(this, &Box3DRagdollProfileBonesEditor::_selection_changed));
		add_child(tree);

		add_child(memnew(HSeparator));
		detail_panel = memnew(VBoxContainer);
		add_child(detail_panel);
		selected_bone_label = memnew(Label);
		selected_bone_label->set_theme_type_variation(SNAME("HeaderSmall"));
		detail_panel->add_child(selected_bone_label);
		GridContainer *detail_grid = memnew(GridContainer);
		detail_grid->set_columns(2);
		detail_panel->add_child(detail_grid);

		enabled = memnew(CheckBox(TTR("Simulate this bone")));
		enabled->connect(SceneStringName(toggled), callable_mp(this, &Box3DRagdollProfileBonesEditor::_enabled_toggled));
		_add_detail_control(detail_grid, TTR("Enabled"), enabled);

		joint_type = memnew(OptionButton);
		joint_type->add_item(TTR("None"), Box3DRagdollProfile::JOINT_TYPE_NONE);
		joint_type->add_item(TTR("Spherical"), Box3DRagdollProfile::JOINT_TYPE_SPHERICAL);
		joint_type->add_item(TTR("Hinge"), Box3DRagdollProfile::JOINT_TYPE_REVOLUTE);
		joint_type->connect(SceneStringName(item_selected), callable_mp(this, &Box3DRagdollProfileBonesEditor::_joint_type_selected));
		_add_detail_control(detail_grid, TTR("Joint Type"), joint_type);

		auto add_spin_box = [&](const String &p_label, double p_min, double p_max, double p_step, const String &p_suffix, const StringName &p_key, const String &p_action, bool p_degrees = false) {
			SpinBox *spin_box = memnew(SpinBox);
			spin_box->set_min(p_min);
			spin_box->set_max(p_max);
			spin_box->set_step(p_step);
			spin_box->set_suffix(p_suffix);
			spin_box->connect(SceneStringName(value_changed), callable_mp(this, &Box3DRagdollProfileBonesEditor::_number_changed).bind(p_key, p_action, p_degrees));
			_add_detail_control(detail_grid, p_label, spin_box);
			return spin_box;
		};
		radius = add_spin_box(TTR("Radius"), 0.01, 2.0, 0.005, " m", SNAME("radius"), TTR("Change Box3D Ragdoll Bone Radius"));
		height = add_spin_box(TTR("Height"), 0.02, 5.0, 0.01, " m", SNAME("height"), TTR("Change Box3D Ragdoll Bone Height"));
		density_scale = add_spin_box(TTR("Density Scale"), 0.0, 100.0, 0.01, String(), SNAME("density_scale"), TTR("Change Box3D Ragdoll Bone Density"));
		swing_limit = add_spin_box(TTR("Swing Limit"), 0.0, 180.0, 1.0, " deg", SNAME("swing_limit"), TTR("Change Box3D Ragdoll Swing Limit"), true);
		twist_lower = add_spin_box(TTR("Twist Minimum"), -180.0, 180.0, 1.0, " deg", SNAME("twist_lower"), TTR("Change Box3D Ragdoll Twist Minimum"), true);
		twist_upper = add_spin_box(TTR("Twist Maximum"), -180.0, 180.0, 1.0, " deg", SNAME("twist_upper"), TTR("Change Box3D Ragdoll Twist Maximum"), true);
		joint_friction_scale = add_spin_box(TTR("Joint Friction Scale"), 0.0, 10.0, 0.05, String(), SNAME("joint_friction_scale"), TTR("Change Box3D Ragdoll Joint Friction"));
		blend = add_spin_box(TTR("Physics Blend"), 0.0, 1.0, 0.01, String(), SNAME("blend"), TTR("Change Box3D Ragdoll Physics Blend"));
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
	ragdoll_inspector->set_editor_plugin(this);
	add_inspector_plugin(ragdoll_inspector);
	Ref<Box3DRagdollProfileInspectorPlugin> profile_inspector;
	profile_inspector.instantiate();
	add_inspector_plugin(profile_inspector);

	profile_generation_dialog = memnew(Box3DRagdollProfileGenerationDialog);
	add_child(profile_generation_dialog);
}

void Box3DRagdollEditorPlugin::popup_profile_generator(Box3DRagdoll *p_ragdoll) {
	ERR_FAIL_NULL(profile_generation_dialog);
	profile_generation_dialog->popup_for_ragdoll(p_ragdoll);
}
