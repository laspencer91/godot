/**************************************************************************/
/*  hotspot_patch_editor.h                                                */
/**************************************************************************/
/*  G-Level WP21: pane-owned HotspotAtlas resource editor view.           */
/**************************************************************************/

#pragma once

#include "core/math/rect2.h"
#include "core/variant/typed_array.h"
#include "core/variant/variant.h"
#include "scene/gui/control.h"

class AcceptDialog;
class Button;
class CheckButton;
class EditorResourcePicker;
class FileDialog;
class HotspotAtlas;
class HotspotAtlasDocument;
class HotspotPatch;
class HotspotPatchCanvas;
class ItemList;
class Label;
class LineEdit;
class OptionButton;
class Resource;
class SpinBox;
class Timer;

// VIEW STATE: one canvas/inspector per pane presenting a HotspotAtlasDocument.
// The document owns identity/dirty state; the Ref owns the edited resource;
// every mutation is committed through the document history.
class HotspotPatchEditor : public Control {
	GDCLASS(HotspotPatchEditor, Control);

	HotspotAtlasDocument *document = nullptr; // EditorData-owned.
	Ref<HotspotAtlas> atlas;
	HotspotPatchCanvas *canvas = nullptr;
	ItemList *patch_list = nullptr;
	LineEdit *patch_name_edit = nullptr;
	SpinBox *rect_spins[4] = {};
	CheckButton *allow_rotation = nullptr;
	CheckButton *allow_mirror_x = nullptr;
	CheckButton *allow_mirror_y = nullptr;
	CheckButton *allow_tiling = nullptr;
	OptionButton *tiling_axis = nullptr;
	SpinBox *inset_px = nullptr;
	EditorResourcePicker *reference_texture_picker = nullptr;
	SpinBox *texel_density = nullptr;
	OptionButton *mapping_mode = nullptr;
	OptionButton *tiling_policy = nullptr;
	CheckButton *disallow_random = nullptr;
	LineEdit *param_names_edit = nullptr;
	Label *grid_label = nullptr;
	CheckButton *snap_toggle = nullptr;
	CheckButton *preview_toggle = nullptr;
	CheckButton *debug_toggle = nullptr;
	ItemList *binding_list = nullptr;
	LineEdit *binding_key_edit = nullptr;
	EditorResourcePicker *binding_material_picker = nullptr;
	Label *status_label = nullptr;
	AcceptDialog *error_dialog = nullptr;
	FileDialog *import_dialog = nullptr;
	FileDialog *export_dialog = nullptr;
	Timer *preview_debounce = nullptr;

	int selected_patch = -1;
	int grid_step_px = 1;
	bool snap_enabled = true;
	bool preview_enabled = false;
	bool debug_enabled = false;
	bool updating_ui = false;

	Ref<HotspotPatch> _get_selected_patch() const;
	Size2i _texture_size() const;
	Rect2 _normalize_rect_px(const Rect2 &p_rect_px) const;
	Rect2 _snap_rect_px(const Rect2 &p_rect_px, bool p_preserve_size = false) const;
	void _mark_dirty();
	void _commit_atlas_property(const String &p_action, const StringName &p_setter,
			const Variant &p_before, const Variant &p_after);
	void _commit_patch_property(const String &p_action, const Ref<HotspotPatch> &p_patch,
			const StringName &p_setter, const Variant &p_before, const Variant &p_after);
	void _commit_patch_set(const String &p_action, const TypedArray<HotspotPatch> &p_before,
			const TypedArray<HotspotPatch> &p_after, int p_select_after);

	void _atlas_changed();
	void _resource_saved(const Ref<Resource> &p_resource);
	void _refresh_all();
	void _refresh_patch_list();
	void _refresh_patch_inspector();
	void _refresh_atlas_inspector();
	void _refresh_bindings();
	void _patch_selected(int p_index);
	void _add_patch_pressed();
	void _remove_patch_pressed();
	void _move_patch(int p_delta);
	void _patch_name_committed();
	void _patch_name_submitted(const String &p_text);
	void _rect_value_changed(double p_value, int p_component);
	void _patch_bool_changed(bool p_value, StringName p_getter, StringName p_setter, String p_action);
	void _tiling_axis_selected(int p_index);
	void _inset_changed(double p_value);
	void _reference_texture_changed(const Ref<Resource> &p_resource);
	void _texel_density_changed(double p_value);
	void _mapping_mode_selected(int p_index);
	void _tiling_policy_selected(int p_index);
	void _disallow_random_toggled(bool p_value);
	void _param_names_committed();
	void _param_names_submitted(const String &p_text);
	void _save_pressed();

	void _grid_smaller();
	void _grid_larger();
	void _snap_toggled(bool p_enabled);
	void _one_to_one_pressed();
	void _preview_toggled(bool p_enabled);
	void _debug_toggled(bool p_enabled);
	void _schedule_preview();
	void _request_preview_refresh();

	void _import_pressed();
	void _export_pressed();
	void _import_file_selected(const String &p_path);
	void _export_file_selected(const String &p_path);
	void _show_error(const String &p_message);
	void _show_status(const String &p_message);

	void _binding_add_pressed();
	void _binding_remove_pressed();
	void _binding_material_changed(const Ref<Resource> &p_resource);

	friend class HotspotPatchCanvas;

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	const Ref<HotspotAtlas> &get_atlas() const { return atlas; }
	HotspotAtlasDocument *get_hotspot_document() const { return document; }
	int get_selected_patch() const { return selected_patch; }
	int get_grid_step_px() const { return grid_step_px; }
	bool is_snap_enabled() const { return snap_enabled; }
	bool is_preview_enabled() const { return preview_enabled; }
	bool is_debug_enabled() const { return debug_enabled; }

	// Shared handlers used by both UI gestures and the additive editor smoke.
	bool create_patch_px(const Rect2 &p_rect_px);
	bool set_patch_rect_px(int p_patch_index, const Rect2 &p_rect_px, const String &p_action = String());
	Error import_rect_path(const String &p_path);
	Error export_rect_path(const String &p_path);
	Error add_binding(const String &p_pattern_key);
	Error remove_binding(const String &p_pattern_key);
	void set_preview_enabled(bool p_enabled);
	void set_debug_enabled(bool p_enabled);
	void set_context_active(bool p_active);
	void save_resource();

	HotspotPatchEditor(HotspotAtlasDocument *p_document);
	~HotspotPatchEditor();
};
