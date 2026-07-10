/**************************************************************************/
/*  box3d_ragdoll_profile_dialog.h                                        */
/**************************************************************************/

#pragma once

#include "../box3d_ragdoll.h"

#include "core/object/object_id.h"
#include "editor/editor_data.h"
#include "scene/gui/dialogs.h"

class EditorFileDialog;
class AnimationLibrary;
class AnimationMixer;
class Button;
class Label;
class MeshInstance3D;
class Node;
class OptionButton;
class Skeleton3D;
class SpinBox;

struct Box3DRagdollGenerationContext {
	ObjectID ragdoll_id;
	ObjectID scene_root_id;
	ObjectID skeleton_id;
	int scene_history_id = 0;
	String scene_path;
	String scene_title;

	bool capture_from_scenes(const Vector<EditorData::EditedScene> &p_scenes, Box3DRagdoll *p_ragdoll, String &r_error);
	bool matches_open_scene(const Vector<EditorData::EditedScene> &p_scenes) const;
};

class Box3DRagdollProfileGenerationDialog : public ConfirmationDialog {
	GDCLASS(Box3DRagdollProfileGenerationDialog, ConfirmationDialog);

	struct AnimationLibraryCandidate {
		ObjectID mixer_id;
		StringName library_name;
	};

	Box3DRagdollGenerationContext context;
	Vector<ObjectID> mesh_candidates;
	Vector<AnimationLibraryCandidate> animation_candidates;

	Label *binding_label = nullptr;
	Label *target_label = nullptr;
	OptionButton *mesh_option = nullptr;
	OptionButton *animation_option = nullptr;
	SpinBox *target_mass = nullptr;
	SpinBox *prune_bone_length = nullptr;
	SpinBox *vertex_weight_threshold = nullptr;
	SpinBox *animation_padding_degrees = nullptr;
	SpinBox *minimum_bone_length = nullptr;
	SpinBox *minimum_radius = nullptr;
	SpinBox *fallback_radius_ratio = nullptr;
	SpinBox *maximum_adjacent_mass_ratio = nullptr;
	SpinBox *maximum_swing_degrees = nullptr;
	Label *diagnostics_label = nullptr;
	Label *status_label = nullptr;
	Button *preview_button = nullptr;
	EditorFileDialog *save_dialog = nullptr;
	ConfirmationDialog *replace_confirmation = nullptr;

	Ref<Box3DRagdollProfile> pending_profile;
	String pending_save_path;

	bool _capture_context(Box3DRagdoll *p_ragdoll, String &r_error);
	bool _validate_context(Box3DRagdoll **r_ragdoll = nullptr, Node **r_scene_root = nullptr, String *r_error = nullptr) const;
	void _invalidate_context(const String &p_error);
	void _set_generation_controls_enabled(bool p_enabled);
	bool _discover_sources();
	void _collect_scene_sources(Node *p_node, Skeleton3D *p_skeleton, Node *p_scene_root);
	bool _library_matches_skeleton(const Ref<AnimationLibrary> &p_library, Skeleton3D *p_skeleton, AnimationMixer *p_mixer) const;
	MeshInstance3D *_selected_mesh() const;
	Ref<AnimationLibrary> _selected_animation_library() const;
	void _settings_changed();
	bool _generate_preview();
	void _preview_pressed();
	void _save_requested();
	void _save_path_selected(const String &p_path);
	void _save_profile();
	String _default_profile_directory() const;
	String _default_profile_filename() const;
	void _set_status(const String &p_text, bool p_error = false);

protected:
	void ok_pressed() override;
	void _notification(int p_what);

public:
	void popup_for_ragdoll(Box3DRagdoll *p_ragdoll);

	Box3DRagdollProfileGenerationDialog();
};
