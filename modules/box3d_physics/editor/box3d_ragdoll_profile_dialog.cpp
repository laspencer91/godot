/**************************************************************************/
/*  box3d_ragdoll_profile_dialog.cpp                                      */
/**************************************************************************/

#include "box3d_ragdoll_profile_dialog.h"

#include "../box3d_physics_server_3d.h"

#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/variant/variant_utility.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_mixer.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"

static void _add_labeled_control(GridContainer *p_grid, const String &p_label, Control *p_control) {
	Label *label = memnew(Label(p_label));
	p_grid->add_child(label);
	p_control->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_grid->add_child(p_control);
}

bool Box3DRagdollGenerationContext::capture_from_scenes(const Vector<EditorData::EditedScene> &p_scenes, Box3DRagdoll *p_ragdoll, String &r_error) {
	*this = Box3DRagdollGenerationContext();
	ERR_FAIL_NULL_V(p_ragdoll, false);
	Skeleton3D *skeleton = p_ragdoll->get_skeleton();
	if (skeleton == nullptr) {
		r_error = TTR("The Box3DRagdoll must be a child of a Skeleton3D.");
		return false;
	}

	for (const EditorData::EditedScene &scene : p_scenes) {
		Node *root = scene.root;
		if (root == nullptr || (root != p_ragdoll && !root->is_ancestor_of(p_ragdoll))) {
			continue;
		}
		ragdoll_id = p_ragdoll->get_instance_id();
		scene_root_id = root->get_instance_id();
		skeleton_id = skeleton->get_instance_id();
		scene_history_id = scene.history_id;
		scene_path = scene.path;
		scene_title = scene_path.is_empty() ? String(root->get_name()) : scene_path.get_file();
		return true;
	}

	r_error = TTR("The ragdoll does not belong to an open scene document.");
	return false;
}

bool Box3DRagdollGenerationContext::matches_open_scene(const Vector<EditorData::EditedScene> &p_scenes) const {
	Node *scene_root = Object::cast_to<Node>(ObjectDB::get_instance(scene_root_id));
	if (scene_root == nullptr) {
		return false;
	}
	for (const EditorData::EditedScene &scene : p_scenes) {
		if (scene.root == scene_root && scene.history_id == scene_history_id) {
			return true;
		}
	}
	return false;
}

bool Box3DRagdollProfileGenerationDialog::_capture_context(Box3DRagdoll *p_ragdoll, String &r_error) {
	return context.capture_from_scenes(EditorNode::get_editor_data().get_edited_scenes(), p_ragdoll, r_error);
}

bool Box3DRagdollProfileGenerationDialog::_validate_context(Box3DRagdoll **r_ragdoll, Node **r_scene_root, String *r_error) const {
	Box3DRagdoll *ragdoll = Object::cast_to<Box3DRagdoll>(ObjectDB::get_instance(context.ragdoll_id));
	Node *scene_root = Object::cast_to<Node>(ObjectDB::get_instance(context.scene_root_id));
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(context.skeleton_id));
	if (ragdoll == nullptr || scene_root == nullptr || skeleton == nullptr) {
		if (r_error != nullptr) {
			*r_error = TTR("The bound scene was closed or reloaded. Reopen the generator from the ragdoll inspector.");
		}
		return false;
	}
	if (ragdoll->get_skeleton() != skeleton || (scene_root != ragdoll && !scene_root->is_ancestor_of(ragdoll))) {
		if (r_error != nullptr) {
			*r_error = TTR("The ragdoll moved outside its bound scene or changed skeleton. Reopen the generator.");
		}
		return false;
	}

	if (!context.matches_open_scene(EditorNode::get_editor_data().get_edited_scenes())) {
		if (r_error != nullptr) {
			*r_error = TTR("The bound scene document is no longer open. Reopen the generator from its inspector.");
		}
		return false;
	}

	if (r_ragdoll != nullptr) {
		*r_ragdoll = ragdoll;
	}
	if (r_scene_root != nullptr) {
		*r_scene_root = scene_root;
	}
	return true;
}

void Box3DRagdollProfileGenerationDialog::_invalidate_context(const String &p_error) {
	pending_profile.unref();
	pending_save_path.clear();
	diagnostics_label->set_text(TTR("Preview unavailable because the bound scene context is no longer valid."));
	_set_generation_controls_enabled(false);
	_set_status(p_error, true);
	save_dialog->hide();
	replace_confirmation->hide();
	set_process(false);
}

void Box3DRagdollProfileGenerationDialog::_set_generation_controls_enabled(bool p_enabled) {
	mesh_option->set_disabled(!p_enabled);
	animation_option->set_disabled(!p_enabled);
	target_mass->set_editable(p_enabled);
	prune_bone_length->set_editable(p_enabled);
	vertex_weight_threshold->set_editable(p_enabled);
	animation_padding_degrees->set_editable(p_enabled);
	minimum_bone_length->set_editable(p_enabled);
	minimum_radius->set_editable(p_enabled);
	fallback_radius_ratio->set_editable(p_enabled);
	maximum_adjacent_mass_ratio->set_editable(p_enabled);
	maximum_swing_degrees->set_editable(p_enabled);
	preview_button->set_disabled(!p_enabled);
	get_ok_button()->set_disabled(!p_enabled);
}

bool Box3DRagdollProfileGenerationDialog::_library_matches_skeleton(const Ref<AnimationLibrary> &p_library, Skeleton3D *p_skeleton, AnimationMixer *p_mixer) const {
	if (p_library.is_null() || p_skeleton == nullptr || p_mixer == nullptr) {
		return false;
	}
	Node *animation_root = p_mixer->get_node_or_null(p_mixer->get_root_node());
	if (animation_root == nullptr) {
		return false;
	}
	LocalVector<StringName> animation_names;
	p_library->get_animation_list(&animation_names);
	for (const StringName &animation_name : animation_names) {
		Ref<Animation> animation = p_library->get_animation(animation_name);
		if (animation.is_null()) {
			continue;
		}
		for (int track = 0; track < animation->get_track_count(); track++) {
			if (animation->track_get_type(track) != Animation::TYPE_ROTATION_3D) {
				continue;
			}
			const NodePath path = animation->track_get_path(track);
			Node *track_target = animation_root->get_node_or_null(NodePath(path.get_concatenated_names()));
			if (track_target == p_skeleton && p_skeleton->find_bone(path.get_concatenated_subnames()) >= 0) {
				return true;
			}
		}
	}
	return false;
}

void Box3DRagdollProfileGenerationDialog::_collect_scene_sources(Node *p_node, Skeleton3D *p_skeleton, Node *p_scene_root) {
	if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node)) {
		Node *resolved_skeleton = mesh->get_node_or_null(mesh->get_skeleton_path());
		if (resolved_skeleton == p_skeleton && mesh->get_mesh().is_valid()) {
			mesh_candidates.push_back(mesh->get_instance_id());
			mesh_option->add_item(String(p_scene_root->get_path_to(mesh)));
		}
	}
	if (AnimationMixer *mixer = Object::cast_to<AnimationMixer>(p_node)) {
		LocalVector<StringName> libraries;
		mixer->get_animation_library_list(&libraries);
		for (const StringName &library_name : libraries) {
			Ref<AnimationLibrary> library = mixer->get_animation_library(library_name);
			if (!_library_matches_skeleton(library, p_skeleton, mixer)) {
				continue;
			}
			AnimationLibraryCandidate candidate;
			candidate.mixer_id = mixer->get_instance_id();
			candidate.library_name = library_name;
			animation_candidates.push_back(candidate);
			const String display_name = library_name == StringName() ? TTR("Default Library") : String(library_name);
			animation_option->add_item(vformat("%s :: %s", String(p_scene_root->get_path_to(mixer)), display_name));
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_scene_sources(p_node->get_child(i), p_skeleton, p_scene_root);
	}
}

bool Box3DRagdollProfileGenerationDialog::_discover_sources() {
	mesh_candidates.clear();
	animation_candidates.clear();
	mesh_option->clear();
	animation_option->clear();
	mesh_option->add_item(TTR("None (bone-length fallback)"));
	animation_option->add_item(TTR("None (default joint limits)"));

	Node *scene_root = nullptr;
	String error;
	if (!_validate_context(nullptr, &scene_root, &error)) {
		_invalidate_context(error);
		return false;
	}
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(context.skeleton_id));
	ERR_FAIL_NULL_V(skeleton, false);
	_collect_scene_sources(scene_root, skeleton, scene_root);
	if (!mesh_candidates.is_empty()) {
		mesh_option->select(1);
	}
	if (!animation_candidates.is_empty()) {
		animation_option->select(1);
	}
	return true;
}

MeshInstance3D *Box3DRagdollProfileGenerationDialog::_selected_mesh() const {
	const int selected = mesh_option->get_selected() - 1;
	if (selected < 0 || selected >= mesh_candidates.size()) {
		return nullptr;
	}
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(mesh_candidates[selected]));
	Node *scene_root = Object::cast_to<Node>(ObjectDB::get_instance(context.scene_root_id));
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(context.skeleton_id));
	if (mesh == nullptr || scene_root == nullptr || skeleton == nullptr || (scene_root != mesh && !scene_root->is_ancestor_of(mesh)) || mesh->get_mesh().is_null() || mesh->get_node_or_null(mesh->get_skeleton_path()) != skeleton) {
		return nullptr;
	}
	return mesh;
}

Ref<AnimationLibrary> Box3DRagdollProfileGenerationDialog::_selected_animation_library() const {
	const int selected = animation_option->get_selected() - 1;
	if (selected < 0 || selected >= animation_candidates.size()) {
		return Ref<AnimationLibrary>();
	}
	const AnimationLibraryCandidate &candidate = animation_candidates[selected];
	AnimationMixer *mixer = Object::cast_to<AnimationMixer>(ObjectDB::get_instance(candidate.mixer_id));
	Node *scene_root = Object::cast_to<Node>(ObjectDB::get_instance(context.scene_root_id));
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(context.skeleton_id));
	if (mixer == nullptr || scene_root == nullptr || skeleton == nullptr || (scene_root != mixer && !scene_root->is_ancestor_of(mixer))) {
		return Ref<AnimationLibrary>();
	}
	const Ref<AnimationLibrary> library = mixer->get_animation_library(candidate.library_name);
	return _library_matches_skeleton(library, skeleton, mixer) ? library : Ref<AnimationLibrary>();
}

void Box3DRagdollProfileGenerationDialog::_settings_changed() {
	pending_profile.unref();
	diagnostics_label->set_text(TTR("Settings changed. Preview again to update diagnostics."));
}

bool Box3DRagdollProfileGenerationDialog::_generate_preview() {
	Box3DRagdoll *ragdoll = nullptr;
	String error;
	if (!_validate_context(&ragdoll, nullptr, &error)) {
		_invalidate_context(error);
		return false;
	}
	Skeleton3D *skeleton = ragdoll->get_skeleton();
	ERR_FAIL_NULL_V(skeleton, false);
	MeshInstance3D *selected_mesh = _selected_mesh();
	if (mesh_option->get_selected() > 0 && selected_mesh == nullptr) {
		_discover_sources();
		_set_status(TTR("The selected mesh changed or was removed. Sources were refreshed; review the selection and preview again."), true);
		return false;
	}
	const Ref<AnimationLibrary> selected_library = _selected_animation_library();
	if (animation_option->get_selected() > 0 && selected_library.is_null()) {
		_discover_sources();
		_set_status(TTR("The selected animation library changed or was removed. Sources were refreshed; review the selection and preview again."), true);
		return false;
	}

	Ref<Box3DRagdollProfileGenerator> generator;
	generator.instantiate();
	generator->set_target_total_mass(target_mass->get_value());
	generator->set_prune_bone_length(prune_bone_length->get_value());
	generator->set_vertex_weight_threshold(vertex_weight_threshold->get_value());
	generator->set_animation_padding(Math::deg_to_rad(animation_padding_degrees->get_value()));
	generator->set_minimum_bone_length(minimum_bone_length->get_value());
	generator->set_minimum_radius(minimum_radius->get_value());
	generator->set_fallback_radius_ratio(fallback_radius_ratio->get_value());
	generator->set_maximum_adjacent_mass_ratio(maximum_adjacent_mass_ratio->get_value());
	generator->set_maximum_swing_limit(Math::deg_to_rad(maximum_swing_degrees->get_value()));

	pending_profile = generator->generate_profile(skeleton, selected_mesh, selected_library);
	if (pending_profile.is_null()) {
		_set_status(TTR("Profile generation failed. Check the editor output for details."), true);
		return false;
	}
	const Dictionary analysis = generator->analyze_profile(pending_profile);
	String diagnostics = vformat(TTR("Bodies: %d    Total mass: %.1f kg\nBone mass: %.2f-%.2f kg    Maximum ratio: %.1f:1"),
			(int)analysis.get(SNAME("bone_count"), 0),
			(double)analysis.get(SNAME("total_mass"), 0.0),
			(double)analysis.get(SNAME("minimum_bone_mass"), 0.0),
			(double)analysis.get(SNAME("maximum_bone_mass"), 0.0),
			(double)analysis.get(SNAME("maximum_mass_ratio"), 0.0));
	const PackedStringArray warnings = generator->get_warnings();
	if (!warnings.is_empty()) {
		diagnostics += "\n\n" + TTR("Warnings:") + "\n- " + String("\n- ").join(warnings);
	}
	diagnostics_label->set_text(diagnostics);
	_set_status(TTR("Preview generated in memory. Save and assign when the result looks correct."));
	get_ok_button()->set_disabled(false);
	return true;
}

void Box3DRagdollProfileGenerationDialog::_preview_pressed() {
	_generate_preview();
}

String Box3DRagdollProfileGenerationDialog::_default_profile_directory() const {
	return context.scene_path.is_empty() ? String("res://") : context.scene_path.get_base_dir();
}

String Box3DRagdollProfileGenerationDialog::_default_profile_filename() const {
	String base = context.scene_path.is_empty() ? String(context.scene_title).to_snake_case() : context.scene_path.get_file().get_basename().to_snake_case();
	if (base.is_empty()) {
		base = "ragdoll";
	}
	return base + "_ragdoll_profile.tres";
}

void Box3DRagdollProfileGenerationDialog::_save_requested() {
	if (!_generate_preview()) {
		return;
	}
	save_dialog->set_current_dir(_default_profile_directory());
	save_dialog->set_current_file(_default_profile_filename());
	save_dialog->popup_centered_ratio();
}

void Box3DRagdollProfileGenerationDialog::_save_path_selected(const String &p_path) {
	pending_save_path = p_path;
	Box3DRagdoll *ragdoll = nullptr;
	String error;
	if (!_validate_context(&ragdoll, nullptr, &error)) {
		_invalidate_context(error);
		return;
	}
	const Ref<Box3DRagdollProfile> current = ragdoll->get_profile();
	if (current.is_valid() && current->get_path() == pending_save_path && FileAccess::exists(pending_save_path)) {
		replace_confirmation->set_text(TTR("This path is the profile currently assigned to the bound ragdoll. Regeneration replaces the profile on disk, including all hand-tuned bone entries and profile settings. This disk write cannot be undone.\n\nOverwrite it anyway?"));
		replace_confirmation->popup_centered();
		return;
	}
	_save_profile();
}

void Box3DRagdollProfileGenerationDialog::_save_profile() {
	Box3DRagdoll *ragdoll = nullptr;
	String error;
	if (!_validate_context(&ragdoll, nullptr, &error)) {
		_invalidate_context(error);
		return;
	}
	if (pending_profile.is_null() || pending_save_path.is_empty()) {
		_set_status(TTR("Generate a preview and choose a save path first."), true);
		return;
	}

	const Ref<Box3DRagdollProfile> previous_profile = ragdoll->get_profile();
	const Error save_error = ResourceSaver::save(pending_profile, pending_save_path);
	if (save_error != OK) {
		_set_status(vformat(TTR("Could not save the ragdoll profile: %s"), VariantUtilityFunctions::error_string(save_error)), true);
		return;
	}
	// Give the generated resource the saved external path only after a successful
	// write. Taking over is required when regenerating the currently loaded file.
	pending_profile->set_path(pending_save_path, true);
	if (EditorFileSystem::get_singleton() != nullptr) {
		EditorFileSystem::get_singleton()->update_file(pending_save_path);
	}

	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action_for_history(TTR("Assign Box3D Ragdoll Profile"), context.scene_history_id);
	undo_redo->add_do_method(ragdoll, "set_profile", pending_profile);
	undo_redo->add_undo_method(ragdoll, "set_profile", previous_profile);
	undo_redo->commit_action();

	_set_status(vformat(TTR("Saved and assigned %s to %s."), pending_save_path.get_file(), context.scene_title));
	hide();
}

void Box3DRagdollProfileGenerationDialog::_set_status(const String &p_text, bool p_error) {
	status_label->set_text(p_text);
	status_label->add_theme_color_override(SNAME("font_color"), p_error ? get_theme_color(SNAME("error_color"), SNAME("Editor")) : get_theme_color(SNAME("font_color"), SNAME("Label")));
}

void Box3DRagdollProfileGenerationDialog::ok_pressed() {
	_save_requested();
}

void Box3DRagdollProfileGenerationDialog::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		set_process(is_visible());
	} else if (p_what == NOTIFICATION_PROCESS && is_visible()) {
		String error;
		if (!_validate_context(nullptr, nullptr, &error)) {
			_invalidate_context(error);
		}
	}
}

void Box3DRagdollProfileGenerationDialog::popup_for_ragdoll(Box3DRagdoll *p_ragdoll) {
	String error;
	if (!_capture_context(p_ragdoll, error)) {
		context = Box3DRagdollGenerationContext();
		set_title(TTR("Generate Box3D Ragdoll Profile"));
		binding_label->set_text(TTR("Bound scene: unavailable"));
		target_label->set_text(TTR("Target: unavailable"));
		_invalidate_context(error);
		popup_centered_clamped(Size2(720, 640), 0.9);
		return;
	}

	Node *scene_root = Object::cast_to<Node>(ObjectDB::get_instance(context.scene_root_id));
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(ObjectDB::get_instance(context.skeleton_id));
	ERR_FAIL_NULL(scene_root);
	ERR_FAIL_NULL(skeleton);
	binding_label->set_text(vformat(TTR("Bound scene: %s"), context.scene_title));
	binding_label->set_tooltip_text(context.scene_path);
	target_label->set_text(vformat(TTR("Target: %s / %s"), String(scene_root->get_path_to(skeleton)), String(scene_root->get_path_to(p_ragdoll))));
	_set_generation_controls_enabled(true);
	target_mass->set_value(p_ragdoll->get_profile().is_valid() ? p_ragdoll->get_profile()->estimate_total_mass() : 0.0);
	prune_bone_length->set_value(0.06);
	vertex_weight_threshold->set_value(0.5);
	animation_padding_degrees->set_value(10.0);
	minimum_bone_length->set_value(0.08);
	minimum_radius->set_value(0.04);
	fallback_radius_ratio->set_value(0.2);
	maximum_adjacent_mass_ratio->set_value(10.0);
	maximum_swing_degrees->set_value(80.0);
	pending_profile.unref();
	pending_save_path.clear();
	if (!_discover_sources()) {
		popup_centered_clamped(Size2(720, 640), 0.9);
		return;
	}
	diagnostics_label->set_text(TTR("Preview the profile to see body, mass, and warning diagnostics."));
	_set_status(Box3DPhysicsServer3D::get_singleton() == nullptr ? TTR("Generation is available. Box3D must be the active PhysicsServer3D backend to simulate the result.") : TTR("Ready to generate for the bound scene."));
	set_title(p_ragdoll->get_profile().is_valid() ? TTR("Regenerate Box3D Ragdoll Profile") : TTR("Generate Box3D Ragdoll Profile"));
	popup_centered_clamped(Size2(720, 640), 0.9);
}

Box3DRagdollProfileGenerationDialog::Box3DRagdollProfileGenerationDialog() {
	set_hide_on_ok(false);
	set_ok_button_text(TTR("Save & Assign..."));
	set_cancel_button_text(TTR("Cancel"));

	ScrollContainer *content_scroll = memnew(ScrollContainer);
	content_scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	add_child(content_scroll);
	VBoxContainer *main_vbox = memnew(VBoxContainer);
	main_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	content_scroll->add_child(main_vbox);

	binding_label = memnew(Label);
	binding_label->set_theme_type_variation(SNAME("HeaderSmall"));
	main_vbox->add_child(binding_label);
	target_label = memnew(Label);
	target_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	main_vbox->add_child(target_label);
	main_vbox->add_child(memnew(HSeparator));

	GridContainer *source_grid = memnew(GridContainer);
	source_grid->set_columns(2);
	main_vbox->add_child(source_grid);
	mesh_option = memnew(OptionButton);
	_add_labeled_control(source_grid, TTR("Skinned Mesh"), mesh_option);
	animation_option = memnew(OptionButton);
	_add_labeled_control(source_grid, TTR("Animation Library"), animation_option);

	Label *settings_heading = memnew(Label(TTR("Generation Settings")));
	settings_heading->set_theme_type_variation(SNAME("HeaderSmall"));
	main_vbox->add_child(settings_heading);
	GridContainer *settings_grid = memnew(GridContainer);
	settings_grid->set_columns(2);
	main_vbox->add_child(settings_grid);

	target_mass = memnew(SpinBox);
	target_mass->set_min(0.0);
	target_mass->set_max(500.0);
	target_mass->set_step(0.5);
	target_mass->set_suffix(TTR(" kg (0 = automatic)"));
	_add_labeled_control(settings_grid, TTR("Target Mass"), target_mass);
	prune_bone_length = memnew(SpinBox);
	prune_bone_length->set_min(0.0);
	prune_bone_length->set_max(1.0);
	prune_bone_length->set_step(0.005);
	prune_bone_length->set_suffix(" m");
	_add_labeled_control(settings_grid, TTR("Prune Bone Length"), prune_bone_length);
	vertex_weight_threshold = memnew(SpinBox);
	vertex_weight_threshold->set_min(0.0);
	vertex_weight_threshold->set_max(1.0);
	vertex_weight_threshold->set_step(0.01);
	_add_labeled_control(settings_grid, TTR("Vertex Weight Threshold"), vertex_weight_threshold);
	animation_padding_degrees = memnew(SpinBox);
	animation_padding_degrees->set_min(0.0);
	animation_padding_degrees->set_max(180.0);
	animation_padding_degrees->set_step(1.0);
	animation_padding_degrees->set_suffix(" deg");
	_add_labeled_control(settings_grid, TTR("Animation Padding"), animation_padding_degrees);
	minimum_bone_length = memnew(SpinBox);
	minimum_bone_length->set_min(0.001);
	minimum_bone_length->set_max(10.0);
	minimum_bone_length->set_step(0.001);
	minimum_bone_length->set_suffix(" m");
	_add_labeled_control(settings_grid, TTR("Minimum Bone Length"), minimum_bone_length);
	minimum_radius = memnew(SpinBox);
	minimum_radius->set_min(0.001);
	minimum_radius->set_max(2.0);
	minimum_radius->set_step(0.001);
	minimum_radius->set_suffix(" m");
	_add_labeled_control(settings_grid, TTR("Minimum Radius"), minimum_radius);
	fallback_radius_ratio = memnew(SpinBox);
	fallback_radius_ratio->set_min(0.01);
	fallback_radius_ratio->set_max(2.0);
	fallback_radius_ratio->set_step(0.01);
	_add_labeled_control(settings_grid, TTR("Fallback Radius Ratio"), fallback_radius_ratio);
	maximum_adjacent_mass_ratio = memnew(SpinBox);
	maximum_adjacent_mass_ratio->set_min(1.0);
	maximum_adjacent_mass_ratio->set_max(100.0);
	maximum_adjacent_mass_ratio->set_step(0.5);
	maximum_adjacent_mass_ratio->set_suffix(":1");
	_add_labeled_control(settings_grid, TTR("Maximum Adjacent Mass Ratio"), maximum_adjacent_mass_ratio);
	maximum_swing_degrees = memnew(SpinBox);
	maximum_swing_degrees->set_min(0.0);
	maximum_swing_degrees->set_max(180.0);
	maximum_swing_degrees->set_step(1.0);
	maximum_swing_degrees->set_suffix(" deg");
	maximum_swing_degrees->set_tooltip_text(TTR("Upper bound for animation-derived swing cones. The union of a whole animation library can span nearly the full sphere, which leaves joints effectively unlimited."));
	_add_labeled_control(settings_grid, TTR("Maximum Swing Limit"), maximum_swing_degrees);

	Label *diagnostics_heading = memnew(Label(TTR("Preview Diagnostics")));
	diagnostics_heading->set_theme_type_variation(SNAME("HeaderSmall"));
	main_vbox->add_child(diagnostics_heading);
	diagnostics_label = memnew(Label);
	diagnostics_label->set_custom_minimum_size(Size2(0, 110));
	diagnostics_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	diagnostics_label->set_vertical_alignment(VERTICAL_ALIGNMENT_TOP);
	main_vbox->add_child(diagnostics_label);
	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	main_vbox->add_child(status_label);

	preview_button = add_button(TTR("Preview"), false);
	preview_button->connect(SceneStringName(pressed), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_preview_pressed));
	mesh_option->connect(SceneStringName(item_selected), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_settings_changed).unbind(1));
	animation_option->connect(SceneStringName(item_selected), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_settings_changed).unbind(1));
	for (SpinBox *spin_box : { target_mass, prune_bone_length, vertex_weight_threshold, animation_padding_degrees, minimum_bone_length, minimum_radius, fallback_radius_ratio, maximum_adjacent_mass_ratio, maximum_swing_degrees }) {
		spin_box->connect(SceneStringName(value_changed), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_settings_changed).unbind(1));
	}

	save_dialog = memnew(EditorFileDialog);
	save_dialog->set_access(EditorFileDialog::ACCESS_RESOURCES);
	save_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	save_dialog->add_filter("*.tres", TTR("Text Resource"));
	save_dialog->add_filter("*.res", TTR("Binary Resource"));
	save_dialog->set_title(TTR("Save Box3D Ragdoll Profile"));
	save_dialog->connect(SNAME("file_selected"), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_save_path_selected));
	add_child(save_dialog);

	replace_confirmation = memnew(ConfirmationDialog);
	replace_confirmation->set_title(TTR("Overwrite Tuned Ragdoll Profile?"));
	replace_confirmation->set_ok_button_text(TTR("Overwrite"));
	replace_confirmation->connect(SceneStringName(confirmed), callable_mp(this, &Box3DRagdollProfileGenerationDialog::_save_profile));
	add_child(replace_confirmation);
}
