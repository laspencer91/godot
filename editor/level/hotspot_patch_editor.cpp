/**************************************************************************/
/*  hotspot_patch_editor.cpp                                              */
/**************************************************************************/
/*  G-Level WP21: HotspotAtlas resource-document view and interactions.   */
/**************************************************************************/

#include "hotspot_patch_editor.h"

#include "core/core_string_names.h"
#include "core/input/input_event.h"
#include "core/io/resource.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_toaster.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/level/level_editor.h"
#include "editor/level/texel_density_scanner.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/file_dialog.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/split_container.h"
#include "scene/main/timer.h"
#include "scene/resources/material.h"

#include "modules/level_kernel/hotspot_atlas.h"
#include "modules/level_kernel/hotspot_binding.h"

namespace {

constexpr int HANDLE_NONE = -1;
constexpr int HANDLE_TOP_LEFT = 0;
constexpr int HANDLE_TOP = 1;
constexpr int HANDLE_TOP_RIGHT = 2;
constexpr int HANDLE_RIGHT = 3;
constexpr int HANDLE_BOTTOM_RIGHT = 4;
constexpr int HANDLE_BOTTOM = 5;
constexpr int HANDLE_BOTTOM_LEFT = 6;
constexpr int HANDLE_LEFT = 7;

Rect2 rect_from_points(const Vector2 &p_a, const Vector2 &p_b) {
	const Vector2 begin(MIN(p_a.x, p_b.x), MIN(p_a.y, p_b.y));
	const Vector2 end(MAX(p_a.x, p_b.x), MAX(p_a.y, p_b.y));
	return Rect2(begin, end - begin);
}

String joined_param_names(const PackedStringArray &p_names) {
	String result;
	for (int i = 0; i < p_names.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += p_names[i];
	}
	return result;
}

PackedStringArray split_param_names(const String &p_text) {
	PackedStringArray result;
	const PackedStringArray pieces = p_text.split(",", false);
	for (const String &piece : pieces) {
		const String value = piece.strip_edges();
		if (!value.is_empty() && !result.has(value)) {
			result.push_back(value);
		}
	}
	return result;
}

} // namespace

class HotspotPatchCanvas : public Control {
	GDCLASS(HotspotPatchCanvas, Control);

	enum Gesture {
		GESTURE_NONE,
		GESTURE_CREATE,
		GESTURE_MOVE,
		GESTURE_RESIZE,
		GESTURE_PAN,
	};

	HotspotPatchEditor *editor = nullptr;
	Gesture gesture = GESTURE_NONE;
	int resize_handle = HANDLE_NONE;
	Vector2 pan;
	float zoom = 1.0f;
	Vector2 press_canvas;
	Vector2 press_texel;
	Rect2 initial_rect_px;
	Rect2 preview_rect_px;
	bool preview_visible = false;
	Vector<int> hover_hits;
	int hover_cycle = 0;

	Vector2 _texture_origin() const {
		const Size2i texture_size = editor ? editor->_texture_size() : Size2i();
		const Vector2 extent = texture_size.x > 0 && texture_size.y > 0 ? Vector2(texture_size) * zoom : Vector2();
		return get_size() * 0.5f + pan - extent * 0.5f;
	}

	Vector2 _canvas_to_texel(const Vector2 &p_position) const {
		return (p_position - _texture_origin()) / zoom;
	}

	Vector2 _texel_to_canvas(const Vector2 &p_position) const {
		return _texture_origin() + p_position * zoom;
	}

	Rect2 _rect_to_canvas(const Rect2 &p_rect_px) const {
		return Rect2(_texel_to_canvas(p_rect_px.position), p_rect_px.size * zoom);
	}

	Vector<Vector2> _handle_points(const Rect2 &p_rect_canvas) const {
		Vector<Vector2> points;
		points.push_back(p_rect_canvas.position);
		points.push_back(Vector2(p_rect_canvas.get_center().x, p_rect_canvas.position.y));
		points.push_back(Vector2(p_rect_canvas.get_end().x, p_rect_canvas.position.y));
		points.push_back(Vector2(p_rect_canvas.get_end().x, p_rect_canvas.get_center().y));
		points.push_back(p_rect_canvas.get_end());
		points.push_back(Vector2(p_rect_canvas.get_center().x, p_rect_canvas.get_end().y));
		points.push_back(Vector2(p_rect_canvas.position.x, p_rect_canvas.get_end().y));
		points.push_back(Vector2(p_rect_canvas.position.x, p_rect_canvas.get_center().y));
		return points;
	}

	int _handle_at(const Vector2 &p_position) const {
		if (!editor || editor->selected_patch < 0) {
			return HANDLE_NONE;
		}
		const Size2i texture_size = editor->_texture_size();
		if (texture_size.x <= 0 || texture_size.y <= 0) {
			return HANDLE_NONE;
		}
		const Rect2 rect_canvas = _rect_to_canvas(editor->atlas->get_patch_rect_px(editor->selected_patch));
		const Vector<Vector2> points = _handle_points(rect_canvas);
		const float radius = 7.0f * EDSCALE;
		for (int i = 0; i < points.size(); i++) {
			if (points[i].distance_to(p_position) <= radius) {
				return i;
			}
		}
		return HANDLE_NONE;
	}

	Vector<int> _hits_at(const Vector2 &p_position) const {
		Vector<int> hits;
		if (!editor || editor->atlas.is_null()) {
			return hits;
		}
		const Vector2 texel = _canvas_to_texel(p_position);
		const TypedArray<HotspotPatch> patches = editor->atlas->get_patches();
		for (int i = patches.size() - 1; i >= 0; i--) {
			Ref<HotspotPatch> patch = patches[i];
			if (patch.is_valid() && editor->atlas->get_patch_rect_px(i).has_point(texel)) {
				hits.push_back(i);
			}
		}
		return hits;
	}

	void _update_hover(const Vector2 &p_position) {
		const Vector<int> new_hits = _hits_at(p_position);
		if (new_hits != hover_hits) {
			hover_hits = new_hits;
			hover_cycle = 0;
			queue_redraw();
		}
	}

	Rect2 _resize_rect(const Vector2 &p_texel) const {
		Vector2 begin = initial_rect_px.position;
		Vector2 end = initial_rect_px.get_end();
		switch (resize_handle) {
			case HANDLE_TOP_LEFT: begin = p_texel; break;
			case HANDLE_TOP: begin.y = p_texel.y; break;
			case HANDLE_TOP_RIGHT: begin.y = p_texel.y; end.x = p_texel.x; break;
			case HANDLE_RIGHT: end.x = p_texel.x; break;
			case HANDLE_BOTTOM_RIGHT: end = p_texel; break;
			case HANDLE_BOTTOM: end.y = p_texel.y; break;
			case HANDLE_BOTTOM_LEFT: begin.x = p_texel.x; end.y = p_texel.y; break;
			case HANDLE_LEFT: begin.x = p_texel.x; break;
			default: break;
		}
		return rect_from_points(begin, end);
	}

	void _draw_checkerboard(const Rect2 &p_rect) {
		const float cell = 16.0f * EDSCALE;
		for (float y = p_rect.position.y; y < p_rect.get_end().y; y += cell) {
			for (float x = p_rect.position.x; x < p_rect.get_end().x; x += cell) {
				const int parity = int(Math::floor((x - p_rect.position.x) / cell)) + int(Math::floor((y - p_rect.position.y) / cell));
				draw_rect(Rect2(Vector2(x, y), Vector2(MIN(cell, p_rect.get_end().x - x), MIN(cell, p_rect.get_end().y - y))),
						(parity & 1) ? Color(0.18, 0.19, 0.21) : Color(0.28, 0.29, 0.31), true);
			}
		}
	}

	void _draw_canvas() {
		draw_rect(Rect2(Vector2(), get_size()), Color(0.055, 0.06, 0.07), true);
		if (!editor || editor->atlas.is_null()) {
			return;
		}
		const Size2i texture_size = editor->_texture_size();
		const Ref<Texture2D> texture = editor->atlas->get_reference_texture();
		if (texture_size.x <= 0 || texture_size.y <= 0 || texture.is_null()) {
			_draw_checkerboard(Rect2(Vector2(), get_size()));
			const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
			const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
			if (font.is_valid()) {
				draw_string(font, Vector2(18, 28) * EDSCALE, TTR("Set a reference texture to edit pixel rects."),
						HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.78, 0.80, 0.84));
			}
			return;
		}

		const Rect2 texture_rect(_texture_origin(), Vector2(texture_size) * zoom);
		_draw_checkerboard(texture_rect);
		draw_texture_rect(texture, texture_rect, false, Color(1, 1, 1, 1));

		if (editor->snap_enabled && editor->grid_step_px > 1 && editor->grid_step_px * zoom >= 6.0f) {
			const Color grid_color(0.75, 0.78, 0.84, 0.14);
			for (int x = editor->grid_step_px; x < texture_size.x; x += editor->grid_step_px) {
				const float canvas_x = _texel_to_canvas(Vector2(x, 0)).x;
				draw_line(Vector2(canvas_x, texture_rect.position.y), Vector2(canvas_x, texture_rect.get_end().y), grid_color);
			}
			for (int y = editor->grid_step_px; y < texture_size.y; y += editor->grid_step_px) {
				const float canvas_y = _texel_to_canvas(Vector2(0, y)).y;
				draw_line(Vector2(texture_rect.position.x, canvas_y), Vector2(texture_rect.get_end().x, canvas_y), grid_color);
			}
		}

		const Ref<Font> font = get_theme_font(SceneStringName(font), SNAME("Label"));
		const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("Label"));
		const TypedArray<HotspotPatch> patches = editor->atlas->get_patches();
		for (int i = 0; i < patches.size(); i++) {
			if (i == editor->selected_patch) {
				continue;
			}
			Ref<HotspotPatch> patch = patches[i];
			if (patch.is_null()) {
				continue;
			}
			const Rect2 rect = _rect_to_canvas(editor->atlas->get_patch_rect_px(i));
			const bool hovered = hover_hits.has(i);
			const Color outline = hovered ? Color(0.72, 0.80, 0.95, 0.72) : Color(0.58, 0.64, 0.72, 0.42);
			draw_rect(rect, outline, false, hovered ? 2.0f * EDSCALE : EDSCALE);
			if (font.is_valid()) {
				draw_string(font, rect.position + Vector2(3, font->get_ascent(font_size) + 2) * EDSCALE,
						String(patch->get_patch_name()), HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, rect.size.x - 6.0f * EDSCALE),
						font_size, Color(0.82, 0.85, 0.90, 0.56));
			}
		}

		if (editor->selected_patch >= 0 && editor->selected_patch < patches.size()) {
			Ref<HotspotPatch> patch = patches[editor->selected_patch];
			if (patch.is_valid()) {
				const Rect2 rect = _rect_to_canvas(preview_visible ? preview_rect_px : editor->atlas->get_patch_rect_px(editor->selected_patch));
				draw_rect(rect, Color(0.12, 0.56, 1.0, 0.12), true);
				draw_rect(rect, Color(0.28, 0.76, 1.0, 1.0), false, 2.5f * EDSCALE);
				if (font.is_valid()) {
					draw_string(font, rect.position + Vector2(4, font->get_ascent(font_size) + 3) * EDSCALE,
							String(patch->get_patch_name()), HORIZONTAL_ALIGNMENT_LEFT, MAX(0.0f, rect.size.x - 8.0f * EDSCALE),
							font_size, Color(0.96, 0.98, 1.0));
				}
				const Vector<Vector2> handles = _handle_points(rect);
				const Vector2 handle_size(8, 8);
				for (const Vector2 &point : handles) {
					const Rect2 handle(point - handle_size * 0.5f * EDSCALE, handle_size * EDSCALE);
					draw_rect(handle, Color(0.04, 0.08, 0.12, 0.96), true);
					draw_rect(handle, Color(0.36, 0.82, 1.0), false, EDSCALE);
				}
			}
		}

		if (gesture == GESTURE_CREATE && preview_visible) {
			const Rect2 rect = _rect_to_canvas(preview_rect_px);
			draw_rect(rect, Color(0.28, 1.0, 0.58, 0.12), true);
			draw_rect(rect, Color(0.36, 1.0, 0.64), false, 2.0f * EDSCALE);
		}
	}

protected:
	static void _bind_methods() {}

	void _notification(int p_what) {
		if (p_what == NOTIFICATION_DRAW) {
			_draw_canvas();
		}
	}

	void gui_input(const Ref<InputEvent> &p_event) override {
		if (!editor) {
			return;
		}
		Ref<InputEventKey> key = p_event;
		if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
			const Key code = key->get_keycode() != Key::NONE ? key->get_keycode() : key->get_physical_keycode();
			if (code == Key::BRACKETLEFT) {
				editor->_grid_smaller();
				accept_event();
				return;
			}
			if (code == Key::BRACKETRIGHT) {
				editor->_grid_larger();
				accept_event();
				return;
			}
		}

		Ref<InputEventMouseButton> button = p_event;
		if (button.is_valid()) {
			const MouseButton index = button->get_button_index();
			if (button->is_pressed() && (index == MouseButton::WHEEL_UP || index == MouseButton::WHEEL_DOWN)) {
				if (button->is_alt_pressed() && !hover_hits.is_empty()) {
					hover_cycle = (hover_cycle + (index == MouseButton::WHEEL_UP ? -1 : 1) + hover_hits.size()) % hover_hits.size();
					editor->_patch_selected(hover_hits[hover_cycle]);
				} else {
					const Vector2 pointer = button->get_position();
					const Vector2 texel_before = _canvas_to_texel(pointer);
					const float factor = index == MouseButton::WHEEL_UP ? 1.2f : (1.0f / 1.2f);
					zoom = CLAMP(zoom * factor, 0.03125f, 64.0f);
					const Vector2 desired_origin = pointer - texel_before * zoom;
					pan = desired_origin - get_size() * 0.5f + Vector2(editor->_texture_size()) * zoom * 0.5f;
					queue_redraw();
				}
				accept_event();
				return;
			}
			if (index == MouseButton::MIDDLE) {
				if (button->is_pressed()) {
					gesture = GESTURE_PAN;
					press_canvas = button->get_position();
				} else if (gesture == GESTURE_PAN) {
					gesture = GESTURE_NONE;
				}
				accept_event();
				return;
			}
			if (index == MouseButton::LEFT) {
				if (button->is_pressed()) {
					grab_focus();
					const Size2i texture_size = editor->_texture_size();
					if (texture_size.x <= 0 || texture_size.y <= 0) {
						editor->_show_status(TTR("A reference texture is required before editing patch rects."));
						accept_event();
						return;
					}
					press_canvas = button->get_position();
					press_texel = _canvas_to_texel(press_canvas);
					if (!Rect2(Vector2(), Vector2(texture_size)).has_point(press_texel)) {
						accept_event();
						return;
					}
					resize_handle = _handle_at(press_canvas);
					if (resize_handle != HANDLE_NONE && editor->selected_patch >= 0) {
						gesture = GESTURE_RESIZE;
						initial_rect_px = editor->atlas->get_patch_rect_px(editor->selected_patch);
						preview_rect_px = initial_rect_px;
						preview_visible = true;
					} else {
						const Vector<int> hits = _hits_at(press_canvas);
						if (!hits.is_empty()) {
							int picked = hits[0];
							if (button->is_alt_pressed()) {
								int current = hits.find(editor->selected_patch);
								if (current < 0) {
									current = 0;
								}
								picked = hits[(current + 1 + hits.size()) % hits.size()];
							}
							editor->_patch_selected(picked);
							gesture = GESTURE_MOVE;
							initial_rect_px = editor->atlas->get_patch_rect_px(picked);
							preview_rect_px = initial_rect_px;
							preview_visible = true;
						} else {
							gesture = GESTURE_CREATE;
							preview_rect_px = Rect2(press_texel, Vector2());
							preview_visible = true;
						}
					}
					queue_redraw();
				} else if (gesture == GESTURE_CREATE || gesture == GESTURE_MOVE || gesture == GESTURE_RESIZE) {
					const Gesture completed = gesture;
					gesture = GESTURE_NONE;
					preview_visible = false;
					if (preview_rect_px.size.x >= 1.0f && preview_rect_px.size.y >= 1.0f) {
						if (completed == GESTURE_CREATE) {
							editor->create_patch_px(preview_rect_px);
						} else {
							editor->set_patch_rect_px(editor->selected_patch, preview_rect_px,
									completed == GESTURE_MOVE ? TTR("Move Hotspot Patch") : TTR("Resize Hotspot Patch"));
						}
					}
					queue_redraw();
				}
				accept_event();
				return;
			}
		}

		Ref<InputEventMouseMotion> motion = p_event;
		if (motion.is_null()) {
			return;
		}
		if (gesture == GESTURE_PAN) {
			pan += motion->get_position() - press_canvas;
			press_canvas = motion->get_position();
			queue_redraw();
			accept_event();
			return;
		}
		const Vector2 texel = _canvas_to_texel(motion->get_position());
		if (gesture == GESTURE_CREATE) {
			preview_rect_px = editor->_snap_rect_px(rect_from_points(press_texel, texel));
			queue_redraw();
			accept_event();
			return;
		}
		if (gesture == GESTURE_MOVE) {
			preview_rect_px = initial_rect_px;
			preview_rect_px.position += texel - press_texel;
			preview_rect_px = editor->_snap_rect_px(preview_rect_px, true);
			queue_redraw();
			accept_event();
			return;
		}
		if (gesture == GESTURE_RESIZE) {
			preview_rect_px = editor->_snap_rect_px(_resize_rect(texel));
			queue_redraw();
			accept_event();
			return;
		}
		_update_hover(motion->get_position());
	}

public:
	void set_one_to_one() {
		zoom = 1.0f;
		pan = Vector2();
		queue_redraw();
	}

	explicit HotspotPatchCanvas(HotspotPatchEditor *p_editor) :
			editor(p_editor) {
		set_name("HotspotPatchCanvas");
		set_focus_mode(FOCUS_ALL);
		set_mouse_filter(MOUSE_FILTER_STOP);
		set_clip_contents(true);
		set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
		set_custom_minimum_size(Size2(420, 360) * EDSCALE);
	}
};

Ref<HotspotPatch> HotspotPatchEditor::_get_selected_patch() const {
	if (atlas.is_null() || selected_patch < 0) {
		return Ref<HotspotPatch>();
	}
	const TypedArray<HotspotPatch> patches = atlas->get_patches();
	return selected_patch < patches.size() ? Ref<HotspotPatch>(patches[selected_patch]) : Ref<HotspotPatch>();
}

Size2i HotspotPatchEditor::_texture_size() const {
	return atlas.is_valid() ? atlas->get_reference_texture_size() : Size2i();
}

Rect2 HotspotPatchEditor::_normalize_rect_px(const Rect2 &p_rect_px) const {
	const Size2i dimensions = _texture_size();
	if (dimensions.x <= 0 || dimensions.y <= 0) {
		return Rect2();
	}
	const Rect2 bounded = p_rect_px.intersection(Rect2(Vector2(), Vector2(dimensions)));
	return Rect2(bounded.position / Vector2(dimensions), bounded.size / Vector2(dimensions));
}

Rect2 HotspotPatchEditor::_snap_rect_px(const Rect2 &p_rect_px, bool p_preserve_size) const {
	const Size2i dimensions = _texture_size();
	if (dimensions.x <= 0 || dimensions.y <= 0) {
		return Rect2();
	}
	Rect2 result = p_rect_px;
	if (snap_enabled) {
		result.position = result.position.snappedf((real_t)grid_step_px);
		if (!p_preserve_size) {
			const Vector2 snapped_end = result.get_end().snappedf((real_t)grid_step_px);
			result.size = snapped_end - result.position;
		}
	}
	result.size.x = CLAMP(result.size.x, 1.0f, (float)dimensions.x);
	result.size.y = CLAMP(result.size.y, 1.0f, (float)dimensions.y);
	result.position.x = CLAMP(result.position.x, 0.0f, (float)dimensions.x - result.size.x);
	result.position.y = CLAMP(result.position.y, 0.0f, (float)dimensions.y - result.size.y);
	return result;
}

void HotspotPatchEditor::_mark_dirty() {
	if (document) {
		document->set_dirty(true);
		set_meta(StringName("_hotspot_document_dirty"), true);
	}
	if (atlas.is_valid()) {
		atlas->set_edited(true);
	}
}

void HotspotPatchEditor::_commit_atlas_property(const String &p_action, const StringName &p_setter,
		const Variant &p_before, const Variant &p_after) {
	if (atlas.is_null() || p_before == p_after) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action_for_history(p_action, document ? document->get_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(atlas.ptr(), p_setter, p_after);
	undo_redo->add_undo_method(atlas.ptr(), p_setter, p_before);
	undo_redo->commit_action();
	_mark_dirty();
}

void HotspotPatchEditor::_commit_patch_property(const String &p_action, const Ref<HotspotPatch> &p_patch,
		const StringName &p_setter, const Variant &p_before, const Variant &p_after) {
	if (p_patch.is_null() || p_before == p_after) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action_for_history(p_action, document ? document->get_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(p_patch.ptr(), p_setter, p_after);
	undo_redo->add_undo_method(p_patch.ptr(), p_setter, p_before);
	undo_redo->commit_action();
	_mark_dirty();
}

void HotspotPatchEditor::_commit_patch_set(const String &p_action, const TypedArray<HotspotPatch> &p_before,
		const TypedArray<HotspotPatch> &p_after, int p_select_after) {
	if (atlas.is_null() || p_before == p_after) {
		return;
	}
	selected_patch = p_select_after;
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action_for_history(p_action, document ? document->get_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(atlas.ptr(), SNAME("set_patches"), p_after);
	undo_redo->add_undo_method(atlas.ptr(), SNAME("set_patches"), p_before);
	undo_redo->commit_action();
	_mark_dirty();
}

void HotspotPatchEditor::_atlas_changed() {
	_refresh_all();
	_schedule_preview();
}

void HotspotPatchEditor::_resource_saved(const Ref<Resource> &p_resource) {
	if (atlas.is_valid() && p_resource == atlas && document) {
		document->set_dirty(false);
		set_meta(StringName("_hotspot_document_dirty"), false);
		_show_status(TTR("Hotspot atlas saved."));
	}
}

void HotspotPatchEditor::_refresh_patch_list() {
	if (!patch_list || atlas.is_null()) {
		return;
	}
	const TypedArray<HotspotPatch> patches = atlas->get_patches();
	selected_patch = patches.is_empty() ? -1 : CLAMP(selected_patch, 0, patches.size() - 1);
	patch_list->clear();
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		patch_list->add_item(patch.is_valid() ? String(patch->get_patch_name()) : vformat(TTR("Patch %d (invalid)"), i));
	}
	if (selected_patch >= 0) {
		patch_list->select(selected_patch);
	}
	set_meta(StringName("_hotspot_patch_count"), patches.size());
	set_meta(StringName("_hotspot_selected_patch"), selected_patch);
}

void HotspotPatchEditor::_refresh_patch_inspector() {
	const Ref<HotspotPatch> patch = _get_selected_patch();
	const bool enabled = patch.is_valid();
	patch_name_edit->set_editable(enabled);
	for (SpinBox *spin : rect_spins) {
		spin->set_editable(enabled);
	}
	allow_rotation->set_disabled(!enabled);
	allow_mirror_x->set_disabled(!enabled);
	allow_mirror_y->set_disabled(!enabled);
	allow_tiling->set_disabled(!enabled);
	tiling_axis->set_disabled(!enabled);
	inset_px->set_editable(enabled);
	if (!enabled) {
		patch_name_edit->clear();
		for (SpinBox *spin : rect_spins) {
			spin->set_value(0.0);
		}
		return;
	}
	patch_name_edit->set_text(String(patch->get_patch_name()));
	const Rect2 rect_px = atlas->get_patch_rect_px(selected_patch);
	const double values[4] = { rect_px.position.x, rect_px.position.y, rect_px.size.x, rect_px.size.y };
	for (int i = 0; i < 4; i++) {
		rect_spins[i]->set_value(values[i]);
	}
	allow_rotation->set_pressed_no_signal(patch->is_rotation_allowed());
	allow_mirror_x->set_pressed_no_signal(patch->is_mirror_x_allowed());
	allow_mirror_y->set_pressed_no_signal(patch->is_mirror_y_allowed());
	allow_tiling->set_pressed_no_signal(patch->is_tiling_allowed());
	tiling_axis->select(patch->get_tiling_axis());
	inset_px->set_value(patch->get_inset_px());
}

void HotspotPatchEditor::_refresh_atlas_inspector() {
	if (atlas.is_null()) {
		return;
	}
	reference_texture_picker->set_edited_resource(atlas->get_reference_texture());
	texel_density->set_value(atlas->get_texel_density_target());
	mapping_mode->select(atlas->get_default_mapping_mode());
	tiling_policy->select(atlas->get_tiling_policy());
	disallow_random->set_pressed_no_signal(atlas->is_random_disallowed());
	param_names_edit->set_text(joined_param_names(atlas->get_param_names()));
}

void HotspotPatchEditor::_refresh_bindings() {
	if (!binding_list) {
		return;
	}
	binding_list->clear();
	LevelEditor *level_editor = LevelEditor::get_singleton();
	const Ref<HotspotBinding> bindings = level_editor ? level_editor->get_hotspot_bindings() : Ref<HotspotBinding>();
	if (bindings.is_null()) {
		return;
	}
	const PackedStringArray keys = bindings->get_pattern_keys();
	for (const String &key : keys) {
		const String path = bindings->resolve_pattern(key);
		const int item = binding_list->add_item(vformat("%s  →  %s", key, path));
		binding_list->set_item_metadata(item, key);
		if (atlas.is_valid() && path == atlas->get_path()) {
			binding_list->set_item_custom_fg_color(item, Color(0.36, 0.82, 1.0));
		}
	}
}

void HotspotPatchEditor::_refresh_all() {
	if (updating_ui) {
		return;
	}
	updating_ui = true;
	_refresh_patch_list();
	_refresh_patch_inspector();
	_refresh_atlas_inspector();
	_refresh_bindings();
	grid_label->set_text(vformat(TTR("Grid: %d px"), grid_step_px));
	if (canvas) {
		canvas->queue_redraw();
	}
	updating_ui = false;
}

void HotspotPatchEditor::_patch_selected(int p_index) {
	if (atlas.is_null() || p_index < 0 || p_index >= atlas->get_patches().size()) {
		return;
	}
	selected_patch = p_index;
	_refresh_all();
}

bool HotspotPatchEditor::create_patch_px(const Rect2 &p_rect_px) {
	if (atlas.is_null() || _texture_size().x <= 0 || _texture_size().y <= 0) {
		return false;
	}
	const Rect2 snapped = _snap_rect_px(p_rect_px);
	if (snapped.size.x < 1.0f || snapped.size.y < 1.0f) {
		return false;
	}
	const TypedArray<HotspotPatch> before = atlas->get_patches();
	TypedArray<HotspotPatch> after = before.duplicate();
	Ref<HotspotPatch> patch;
	patch.instantiate();
	patch->set_patch_name(StringName("p" + itos(after.size())));
	patch->set_rect_uv(_normalize_rect_px(snapped));
	after.push_back(patch);
	_commit_patch_set(TTR("Create Hotspot Patch"), before, after, after.size() - 1);
	return true;
}

bool HotspotPatchEditor::set_patch_rect_px(int p_patch_index, const Rect2 &p_rect_px, const String &p_action) {
	if (atlas.is_null() || p_patch_index < 0 || p_patch_index >= atlas->get_patches().size()) {
		return false;
	}
	Ref<HotspotPatch> patch = atlas->get_patches()[p_patch_index];
	if (patch.is_null()) {
		return false;
	}
	const Rect2 normalized = _normalize_rect_px(_snap_rect_px(p_rect_px));
	if (!normalized.has_area()) {
		return false;
	}
	selected_patch = p_patch_index;
	_commit_patch_property(p_action.is_empty() ? TTR("Edit Hotspot Patch Rect") : p_action, patch,
			SNAME("set_rect_uv"), patch->get_rect_uv(), normalized);
	return true;
}

void HotspotPatchEditor::_add_patch_pressed() {
	const Size2i dimensions = _texture_size();
	if (dimensions.x <= 0 || dimensions.y <= 0) {
		_show_status(TTR("Set a reference texture before adding patches."));
		return;
	}
	const float width = MIN((float)dimensions.x, (float)MAX(grid_step_px, 64));
	const float height = MIN((float)dimensions.y, (float)MAX(grid_step_px, 64));
	create_patch_px(Rect2((Vector2(dimensions) - Vector2(width, height)) * 0.5f, Vector2(width, height)));
}

void HotspotPatchEditor::_remove_patch_pressed() {
	if (atlas.is_null() || selected_patch < 0) {
		return;
	}
	const TypedArray<HotspotPatch> before = atlas->get_patches();
	TypedArray<HotspotPatch> after = before.duplicate();
	after.remove_at(selected_patch);
	_commit_patch_set(TTR("Remove Hotspot Patch"), before, after,
			after.is_empty() ? -1 : MIN(selected_patch, after.size() - 1));
}

void HotspotPatchEditor::_move_patch(int p_delta) {
	if (atlas.is_null() || selected_patch < 0) {
		return;
	}
	const int target = selected_patch + p_delta;
	const TypedArray<HotspotPatch> before = atlas->get_patches();
	if (target < 0 || target >= before.size()) {
		return;
	}
	TypedArray<HotspotPatch> after = before.duplicate();
	const Variant moved = after[selected_patch];
	after.remove_at(selected_patch);
	after.insert(target, moved);
	_commit_patch_set(TTR("Reorder Hotspot Patches"), before, after, target);
}

void HotspotPatchEditor::_patch_name_committed() {
	if (updating_ui) {
		return;
	}
	Ref<HotspotPatch> patch = _get_selected_patch();
	if (patch.is_null()) {
		return;
	}
	String value = patch_name_edit->get_text().strip_edges();
	if (value.is_empty()) {
		value = "p" + itos(selected_patch);
	}
	_commit_patch_property(TTR("Rename Hotspot Patch"), patch, SNAME("set_patch_name"),
			patch->get_patch_name(), StringName(value));
}

void HotspotPatchEditor::_patch_name_submitted(const String &p_text) {
	(void)p_text;
	_patch_name_committed();
}

void HotspotPatchEditor::_rect_value_changed(double p_value, int p_component) {
	(void)p_value;
	(void)p_component;
	if (updating_ui || selected_patch < 0) {
		return;
	}
	set_patch_rect_px(selected_patch, Rect2(Vector2(rect_spins[0]->get_value(), rect_spins[1]->get_value()),
			Vector2(rect_spins[2]->get_value(), rect_spins[3]->get_value())));
}

void HotspotPatchEditor::_patch_bool_changed(bool p_value, StringName p_getter, StringName p_setter, String p_action) {
	if (updating_ui) {
		return;
	}
	Ref<HotspotPatch> patch = _get_selected_patch();
	if (patch.is_valid()) {
		_commit_patch_property(p_action, patch, p_setter, patch->call(p_getter), p_value);
	}
}

void HotspotPatchEditor::_tiling_axis_selected(int p_index) {
	if (!updating_ui) {
		Ref<HotspotPatch> patch = _get_selected_patch();
		if (patch.is_valid()) {
			_commit_patch_property(TTR("Set Hotspot Tiling Axis"), patch, SNAME("set_tiling_axis"), patch->get_tiling_axis(), p_index);
		}
	}
}

void HotspotPatchEditor::_inset_changed(double p_value) {
	if (!updating_ui) {
		Ref<HotspotPatch> patch = _get_selected_patch();
		if (patch.is_valid()) {
			_commit_patch_property(TTR("Set Hotspot Inset"), patch, SNAME("set_inset_px"), patch->get_inset_px(), p_value);
		}
	}
}

void HotspotPatchEditor::_reference_texture_changed(const Ref<Resource> &p_resource) {
	if (!updating_ui && atlas.is_valid()) {
		Ref<Texture2D> texture = p_resource;
		_commit_atlas_property(TTR("Set Hotspot Reference Texture"), SNAME("set_reference_texture"),
				atlas->get_reference_texture(), texture);
	}
}

void HotspotPatchEditor::_texel_density_changed(double p_value) {
	if (!updating_ui && atlas.is_valid()) {
		_commit_atlas_property(TTR("Set Hotspot Texel Density"), SNAME("set_texel_density_target"),
				atlas->get_texel_density_target(), p_value);
	}
}

void HotspotPatchEditor::_mapping_mode_selected(int p_index) {
	if (!updating_ui && atlas.is_valid()) {
		_commit_atlas_property(TTR("Set Hotspot Mapping Mode"), SNAME("set_default_mapping_mode"),
				atlas->get_default_mapping_mode(), p_index);
	}
}

void HotspotPatchEditor::_tiling_policy_selected(int p_index) {
	if (!updating_ui && atlas.is_valid()) {
		_commit_atlas_property(TTR("Set Hotspot Tiling Policy"), SNAME("set_tiling_policy"), atlas->get_tiling_policy(), p_index);
	}
}

void HotspotPatchEditor::_disallow_random_toggled(bool p_value) {
	if (!updating_ui && atlas.is_valid()) {
		_commit_atlas_property(TTR("Set Hotspot Random Policy"), SNAME("set_disallow_random"), atlas->is_random_disallowed(), p_value);
	}
}

void HotspotPatchEditor::_param_names_committed() {
	if (!updating_ui && atlas.is_valid()) {
		_commit_atlas_property(TTR("Set Hotspot Parameter Names"), SNAME("set_param_names"),
				atlas->get_param_names(), split_param_names(param_names_edit->get_text()));
	}
}

void HotspotPatchEditor::_param_names_submitted(const String &p_text) {
	(void)p_text;
	_param_names_committed();
}

void HotspotPatchEditor::_save_pressed() {
	save_resource();
}

void HotspotPatchEditor::save_resource() {
	if (atlas.is_valid() && EditorNode::get_singleton()) {
		EditorNode::get_singleton()->save_resource(atlas);
	}
}

void HotspotPatchEditor::_grid_smaller() {
	grid_step_px = MAX(1, grid_step_px / 2);
	_refresh_all();
}

void HotspotPatchEditor::_grid_larger() {
	grid_step_px = MIN(4096, grid_step_px * 2);
	_refresh_all();
}

void HotspotPatchEditor::_snap_toggled(bool p_enabled) {
	snap_enabled = p_enabled;
	set_meta(StringName("_hotspot_snap_enabled"), snap_enabled);
	if (canvas) {
		canvas->queue_redraw();
	}
}

void HotspotPatchEditor::_one_to_one_pressed() {
	if (canvas) {
		canvas->set_one_to_one();
	}
}

void HotspotPatchEditor::set_preview_enabled(bool p_enabled) {
	if (preview_toggle && preview_toggle->is_pressed() != p_enabled) {
		preview_toggle->set_pressed(p_enabled);
		return;
	}
	_preview_toggled(p_enabled);
}

void HotspotPatchEditor::set_debug_enabled(bool p_enabled) {
	if (debug_toggle && debug_toggle->is_pressed() != p_enabled) {
		debug_toggle->set_pressed(p_enabled);
		return;
	}
	_debug_toggled(p_enabled);
}

void HotspotPatchEditor::_preview_toggled(bool p_enabled) {
	preview_enabled = p_enabled;
	set_meta(StringName("_hotspot_preview_enabled"), preview_enabled);
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_hotspot_preview_request(this, atlas, preview_enabled, debug_enabled);
	}
}

void HotspotPatchEditor::_debug_toggled(bool p_enabled) {
	debug_enabled = p_enabled;
	set_meta(StringName("_hotspot_debug_enabled"), debug_enabled);
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->set_hotspot_preview_request(this, atlas, preview_enabled, debug_enabled);
	}
}

void HotspotPatchEditor::_schedule_preview() {
	if (preview_enabled && preview_debounce) {
		preview_debounce->start();
	}
}

void HotspotPatchEditor::_request_preview_refresh() {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->refresh_hotspot_preview_request(this);
	}
}

void HotspotPatchEditor::_import_pressed() {
	import_dialog->popup_file_dialog();
}

void HotspotPatchEditor::_export_pressed() {
	export_dialog->popup_file_dialog();
}

void HotspotPatchEditor::_import_file_selected(const String &p_path) {
	import_rect_path(p_path);
}

void HotspotPatchEditor::_export_file_selected(const String &p_path) {
	export_rect_path(p_path);
}

Error HotspotPatchEditor::import_rect_path(const String &p_path) {
	if (atlas.is_null()) {
		return ERR_UNCONFIGURED;
	}
	Ref<HotspotAtlas> parsed;
	parsed.instantiate();
	parsed->set_reference_texture(atlas->get_reference_texture());
	const Error error = parsed->import_rect(p_path);
	if (error != OK) {
		_show_error(parsed->get_last_rect_error().is_empty() ? vformat(TTR("Could not import %s (error %d)."), p_path, int(error)) : parsed->get_last_rect_error());
		return error;
	}
	const TypedArray<HotspotPatch> imported = parsed->get_patches();
	_commit_patch_set(TTR("Import Hotspot Rects"), atlas->get_patches(), imported, imported.is_empty() ? -1 : 0);
	_show_status(vformat(TTR("Imported %d patches from %s."), imported.size(), p_path.get_file()));
	return OK;
}

Error HotspotPatchEditor::export_rect_path(const String &p_path) {
	if (atlas.is_null()) {
		return ERR_UNCONFIGURED;
	}
	const Error error = atlas->export_rect(p_path);
	if (error != OK) {
		_show_error(atlas->get_last_rect_error().is_empty() ? vformat(TTR("Could not export %s (error %d)."), p_path, int(error)) : atlas->get_last_rect_error());
		return error;
	}
	_show_status(vformat(TTR("Exported hotspot rects to %s."), p_path));
	return OK;
}

void HotspotPatchEditor::_show_error(const String &p_message) {
	set_meta(StringName("_hotspot_last_error"), p_message);
	status_label->set_text(p_message);
	if (error_dialog) {
		error_dialog->set_text(p_message);
		error_dialog->popup_centered();
	}
}

void HotspotPatchEditor::_show_status(const String &p_message) {
	set_meta(StringName("_hotspot_status"), p_message);
	status_label->set_text(p_message);
}

Error HotspotPatchEditor::add_binding(const String &p_pattern_key) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (!level_editor || atlas.is_null() || atlas->get_path().is_empty() || p_pattern_key.strip_edges().is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const Ref<HotspotBinding> registry = level_editor->get_hotspot_bindings();
	const String old_path = registry.is_valid() ? registry->resolve_pattern(p_pattern_key) : String();
	if (old_path == atlas->get_path()) {
		return OK;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL_V(undo_redo, ERR_UNAVAILABLE);
	undo_redo->create_action_for_history(TTR("Add Hotspot Atlas Binding"), document ? document->get_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(level_editor, SNAME("set_hotspot_pattern_binding"), p_pattern_key, atlas->get_path());
	if (old_path.is_empty()) {
		undo_redo->add_undo_method(level_editor, SNAME("erase_hotspot_pattern_binding"), p_pattern_key);
	} else {
		undo_redo->add_undo_method(level_editor, SNAME("set_hotspot_pattern_binding"), p_pattern_key, old_path);
	}
	undo_redo->commit_action();
	_refresh_bindings();
	return OK;
}

Error HotspotPatchEditor::remove_binding(const String &p_pattern_key) {
	LevelEditor *level_editor = LevelEditor::get_singleton();
	if (!level_editor || atlas.is_null() || p_pattern_key.strip_edges().is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const Ref<HotspotBinding> registry = level_editor->get_hotspot_bindings();
	const String old_path = registry.is_valid() ? registry->resolve_pattern(p_pattern_key) : String();
	if (old_path.is_empty() || old_path != atlas->get_path()) {
		return ERR_DOES_NOT_EXIST;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	ERR_FAIL_NULL_V(undo_redo, ERR_UNAVAILABLE);
	undo_redo->create_action_for_history(TTR("Remove Hotspot Atlas Binding"), document ? document->get_history_id() : EditorUndoRedoManager::GLOBAL_HISTORY);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(level_editor, SNAME("erase_hotspot_pattern_binding"), p_pattern_key);
	undo_redo->add_undo_method(level_editor, SNAME("set_hotspot_pattern_binding"), p_pattern_key, old_path);
	undo_redo->commit_action();
	_refresh_bindings();
	return OK;
}

void HotspotPatchEditor::_binding_add_pressed() {
	const Error error = add_binding(binding_key_edit->get_text());
	if (error != OK) {
		_show_error(TTR("Enter a pattern key and save the atlas before adding a binding."));
	}
}

void HotspotPatchEditor::_binding_remove_pressed() {
	const Vector<int> selected = binding_list->get_selected_items();
	if (selected.is_empty()) {
		return;
	}
	const String key = binding_list->get_item_metadata(selected[0]);
	const Error error = remove_binding(key);
	if (error != OK) {
		_show_error(TTR("Only bindings that point to this atlas can be removed here."));
	}
}

void HotspotPatchEditor::_binding_material_changed(const Ref<Resource> &p_resource) {
	Ref<Material> picked_material = p_resource;
	LevelEditor *level_editor = LevelEditor::get_singleton();
	const Ref<TexelDensityScanner> scanner = level_editor ? level_editor->get_texel_density_scanner() : Ref<TexelDensityScanner>();
	if (picked_material.is_null() || scanner.is_null()) {
		return;
	}
	const std::optional<TexelDensityResult> result = scanner->scan(picked_material, picked_material->get_path());
	if (!result.has_value()) {
		_show_error(TTR("The chosen material has no recognized base-color texture."));
		return;
	}
	binding_key_edit->set_text(HotspotBinding::pattern_key_from_texture_path(result->texture_path));
}

void HotspotPatchEditor::set_context_active(bool p_active) {
	if (p_active && atlas.is_valid() && EditorNode::get_singleton()) {
		EditorNode::get_singleton()->edit_item(atlas.ptr(), this);
	}
}

void HotspotPatchEditor::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED && canvas) {
		canvas->queue_redraw();
	}
}

HotspotPatchEditor::HotspotPatchEditor(HotspotAtlasDocument *p_document) :
		document(p_document) {
	ERR_FAIL_NULL(document);
	atlas = document->get_resource();
	ERR_FAIL_COND(atlas.is_null());

	set_name("HotspotPatchEditor");
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	set_meta(StringName("_hotspot_document_type"), int(document->get_type()));
	set_meta(StringName("_hotspot_resource"), atlas);
	set_meta(StringName("_hotspot_specialized_document"), true);
	set_meta(StringName("_hotspot_document_dirty"), document->is_dirty());
	set_meta(StringName("_hotspot_preview_enabled"), false);
	set_meta(StringName("_hotspot_debug_enabled"), false);
	set_meta(StringName("_hotspot_snap_enabled"), true);

	VBoxContainer *root = memnew(VBoxContainer);
	root->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	root->set_h_size_flags(SIZE_EXPAND_FILL);
	root->set_v_size_flags(SIZE_EXPAND_FILL);
	root->add_theme_constant_override("separation", 0);
	add_child(root);

	HBoxContainer *toolbar = memnew(HBoxContainer);
	toolbar->set_name("HotspotPatchToolbar");
	toolbar->add_theme_constant_override("separation", 6 * EDSCALE);
	toolbar->add_child(memnew(Label(TTRC("Hotspot Atlas"))));
	toolbar->add_child(memnew(VSeparator));
	Button *one_to_one = memnew(Button(TTRC("1:1")));
	one_to_one->set_tooltip_text(TTRC("Show one texture pixel per canvas pixel"));
	one_to_one->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_one_to_one_pressed));
	toolbar->add_child(one_to_one);
	Button *grid_down = memnew(Button("["));
	grid_down->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_grid_smaller));
	toolbar->add_child(grid_down);
	grid_label = memnew(Label);
	toolbar->add_child(grid_label);
	Button *grid_up = memnew(Button("]"));
	grid_up->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_grid_larger));
	toolbar->add_child(grid_up);
	snap_toggle = memnew(CheckButton(TTRC("Snap")));
	snap_toggle->set_pressed(true);
	snap_toggle->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_snap_toggled));
	toolbar->add_child(snap_toggle);
	toolbar->add_child(memnew(VSeparator));
	Button *import_button = memnew(Button(TTRC("Import .rect")));
	import_button->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_import_pressed));
	toolbar->add_child(import_button);
	Button *export_button = memnew(Button(TTRC("Export .rect")));
	export_button->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_export_pressed));
	toolbar->add_child(export_button);
	Button *save_button = memnew(Button(TTRC("Save")));
	save_button->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_save_pressed));
	toolbar->add_child(save_button);
	Control *toolbar_spacer = memnew(Control);
	toolbar_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	toolbar->add_child(toolbar_spacer);
	preview_toggle = memnew(CheckButton(TTRC("Preview on selection")));
	preview_toggle->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_preview_toggled));
	toolbar->add_child(preview_toggle);
	debug_toggle = memnew(CheckButton(TTRC("Hotspot Fit Debug")));
	debug_toggle->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_debug_toggled));
	toolbar->add_child(debug_toggle);
	root->add_child(toolbar);

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_h_size_flags(SIZE_EXPAND_FILL);
	split->set_v_size_flags(SIZE_EXPAND_FILL);
	root->add_child(split);
	canvas = memnew(HotspotPatchCanvas(this));
	canvas->set_h_size_flags(SIZE_EXPAND_FILL);
	canvas->set_v_size_flags(SIZE_EXPAND_FILL);
	split->add_child(canvas);

	ScrollContainer *right_scroll = memnew(ScrollContainer);
	right_scroll->set_custom_minimum_size(Size2(390, 0) * EDSCALE);
	right_scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	split->add_child(right_scroll);
	VBoxContainer *right = memnew(VBoxContainer);
	right->set_h_size_flags(SIZE_EXPAND_FILL);
	right->add_theme_constant_override("separation", 5 * EDSCALE);
	right_scroll->add_child(right);

	auto add_row = [right](const String &p_label, Control *p_control) {
		HBoxContainer *row = memnew(HBoxContainer);
		Label *label = memnew(Label(p_label));
		label->set_custom_minimum_size(Size2(122, 0) * EDSCALE);
		row->add_child(label);
		p_control->set_h_size_flags(SIZE_EXPAND_FILL);
		row->add_child(p_control);
		right->add_child(row);
	};

	right->add_child(memnew(Label(TTRC("Atlas"))));
	reference_texture_picker = memnew(EditorResourcePicker);
	reference_texture_picker->set_base_type("Texture2D");
	reference_texture_picker->connect("resource_changed", callable_mp(this, &HotspotPatchEditor::_reference_texture_changed));
	add_row(TTRC("Reference"), reference_texture_picker);
	texel_density = memnew(SpinBox);
	texel_density->set_min(0.001);
	texel_density->set_max(1048576.0);
	texel_density->set_step(1.0);
	texel_density->set_suffix(" px/m");
	texel_density->connect(SceneStringName(value_changed), callable_mp(this, &HotspotPatchEditor::_texel_density_changed));
	add_row(TTRC("Texel density"), texel_density);
	mapping_mode = memnew(OptionButton);
	mapping_mode->add_item(TTRC("Automatic"));
	mapping_mode->add_item(TTRC("Square"));
	mapping_mode->add_item(TTRC("Conforming"));
	mapping_mode->add_item(TTRC("Follow Active Quads"));
	mapping_mode->connect(SceneStringName(item_selected), callable_mp(this, &HotspotPatchEditor::_mapping_mode_selected));
	add_row(TTRC("Mapping"), mapping_mode);
	tiling_policy = memnew(OptionButton);
	tiling_policy->add_item(TTRC("No tiling"));
	tiling_policy->add_item(TTRC("Allow tiling"));
	tiling_policy->add_item(TTRC("Tiling only"));
	tiling_policy->connect(SceneStringName(item_selected), callable_mp(this, &HotspotPatchEditor::_tiling_policy_selected));
	add_row(TTRC("Tiling policy"), tiling_policy);
	disallow_random = memnew(CheckButton(TTRC("Disallow random tie-break")));
	disallow_random->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_disallow_random_toggled));
	right->add_child(disallow_random);
	param_names_edit = memnew(LineEdit);
	param_names_edit->set_placeholder(TTRC("albedo_texture, BaseColor, ..."));
	param_names_edit->connect(SceneStringName(text_submitted), callable_mp(this, &HotspotPatchEditor::_param_names_submitted));
	param_names_edit->connect(SceneStringName(focus_exited), callable_mp(this, &HotspotPatchEditor::_param_names_committed));
	add_row(TTRC("Param names"), param_names_edit);

	right->add_child(memnew(HSeparator));
	right->add_child(memnew(Label(TTRC("Patches (top to bottom = priority)"))));
	patch_list = memnew(ItemList);
	patch_list->set_custom_minimum_size(Size2(0, 130) * EDSCALE);
	patch_list->set_select_mode(ItemList::SELECT_SINGLE);
	patch_list->connect(SceneStringName(item_selected), callable_mp(this, &HotspotPatchEditor::_patch_selected));
	right->add_child(patch_list);
	HBoxContainer *patch_buttons = memnew(HBoxContainer);
	Button *add_patch = memnew(Button(TTRC("Add")));
	add_patch->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_add_patch_pressed));
	patch_buttons->add_child(add_patch);
	Button *remove_patch = memnew(Button(TTRC("Remove")));
	remove_patch->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_remove_patch_pressed));
	patch_buttons->add_child(remove_patch);
	Button *move_up = memnew(Button(TTRC("Up")));
	move_up->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_move_patch).bind(-1));
	patch_buttons->add_child(move_up);
	Button *move_down = memnew(Button(TTRC("Down")));
	move_down->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_move_patch).bind(1));
	patch_buttons->add_child(move_down);
	right->add_child(patch_buttons);

	patch_name_edit = memnew(LineEdit);
	patch_name_edit->connect(SceneStringName(text_submitted), callable_mp(this, &HotspotPatchEditor::_patch_name_submitted));
	patch_name_edit->connect(SceneStringName(focus_exited), callable_mp(this, &HotspotPatchEditor::_patch_name_committed));
	add_row(TTRC("Name"), patch_name_edit);
	static const char *rect_labels[4] = { "X (px)", "Y (px)", "Width (px)", "Height (px)" };
	for (int i = 0; i < 4; i++) {
		rect_spins[i] = memnew(SpinBox);
		rect_spins[i]->set_min(0.0);
		rect_spins[i]->set_max(1048576.0);
		rect_spins[i]->set_step(1.0);
		rect_spins[i]->set_suffix(" px");
		rect_spins[i]->connect(SceneStringName(value_changed), callable_mp(this, &HotspotPatchEditor::_rect_value_changed).bind(i));
		add_row(TTRC(rect_labels[i]), rect_spins[i]);
	}
	allow_rotation = memnew(CheckButton(TTRC("Allow rotation")));
	allow_rotation->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_patch_bool_changed)
			.bind(SNAME("is_rotation_allowed"), SNAME("set_allow_rotation"), String(TTRC("Set Hotspot Rotation"))));
	right->add_child(allow_rotation);
	allow_mirror_x = memnew(CheckButton(TTRC("Allow mirror X")));
	allow_mirror_x->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_patch_bool_changed)
			.bind(SNAME("is_mirror_x_allowed"), SNAME("set_allow_mirror_x"), String(TTRC("Set Hotspot Mirror X"))));
	right->add_child(allow_mirror_x);
	allow_mirror_y = memnew(CheckButton(TTRC("Allow mirror Y")));
	allow_mirror_y->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_patch_bool_changed)
			.bind(SNAME("is_mirror_y_allowed"), SNAME("set_allow_mirror_y"), String(TTRC("Set Hotspot Mirror Y"))));
	right->add_child(allow_mirror_y);
	allow_tiling = memnew(CheckButton(TTRC("Allow tiling")));
	allow_tiling->connect(SceneStringName(toggled), callable_mp(this, &HotspotPatchEditor::_patch_bool_changed)
			.bind(SNAME("is_tiling_allowed"), SNAME("set_allow_tiling"), String(TTRC("Set Hotspot Tiling"))));
	right->add_child(allow_tiling);
	tiling_axis = memnew(OptionButton);
	tiling_axis->add_item("U");
	tiling_axis->add_item("V");
	tiling_axis->connect(SceneStringName(item_selected), callable_mp(this, &HotspotPatchEditor::_tiling_axis_selected));
	add_row(TTRC("Tiling axis"), tiling_axis);
	inset_px = memnew(SpinBox);
	inset_px->set_min(0.0);
	inset_px->set_max(1048576.0);
	inset_px->set_step(0.25);
	inset_px->set_suffix(" px");
	inset_px->connect(SceneStringName(value_changed), callable_mp(this, &HotspotPatchEditor::_inset_changed));
	add_row(TTRC("Inset"), inset_px);

	right->add_child(memnew(HSeparator));
	right->add_child(memnew(Label(TTRC("Pattern bindings"))));
	binding_list = memnew(ItemList);
	binding_list->set_custom_minimum_size(Size2(0, 110) * EDSCALE);
	right->add_child(binding_list);
	binding_key_edit = memnew(LineEdit);
	binding_key_edit->set_placeholder(TTRC("pattern/key"));
	add_row(TTRC("Pattern key"), binding_key_edit);
	binding_material_picker = memnew(EditorResourcePicker);
	binding_material_picker->set_base_type("Material");
	binding_material_picker->connect("resource_changed", callable_mp(this, &HotspotPatchEditor::_binding_material_changed));
	add_row(TTRC("Derive material"), binding_material_picker);
	HBoxContainer *binding_buttons = memnew(HBoxContainer);
	Button *add_binding_button = memnew(Button(TTRC("Bind this atlas")));
	add_binding_button->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_binding_add_pressed));
	binding_buttons->add_child(add_binding_button);
	Button *remove_binding_button = memnew(Button(TTRC("Remove selected")));
	remove_binding_button->connect(SceneStringName(pressed), callable_mp(this, &HotspotPatchEditor::_binding_remove_pressed));
	binding_buttons->add_child(remove_binding_button);
	right->add_child(binding_buttons);

	status_label = memnew(Label);
	status_label->set_name("HotspotPatchStatus");
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_text(TTRC("Drag empty texture space to create. Drag a patch or its handles to edit."));
	root->add_child(status_label);

	import_dialog = memnew(FileDialog);
	import_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
	import_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	import_dialog->add_filter("*.rect; Hotspot Rect");
	import_dialog->connect("file_selected", callable_mp(this, &HotspotPatchEditor::_import_file_selected));
	add_child(import_dialog);
	export_dialog = memnew(FileDialog);
	export_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	export_dialog->set_access(FileDialog::ACCESS_FILESYSTEM);
	export_dialog->add_filter("*.rect; Hotspot Rect");
	export_dialog->connect("file_selected", callable_mp(this, &HotspotPatchEditor::_export_file_selected));
	add_child(export_dialog);
	error_dialog = memnew(AcceptDialog);
	error_dialog->set_title(TTRC("Hotspot Atlas Error"));
	add_child(error_dialog);
	preview_debounce = memnew(Timer);
	preview_debounce->set_one_shot(true);
	preview_debounce->set_wait_time(0.15);
	preview_debounce->connect("timeout", callable_mp(this, &HotspotPatchEditor::_request_preview_refresh));
	add_child(preview_debounce);

	atlas->connect_changed(callable_mp(this, &HotspotPatchEditor::_atlas_changed));
	if (EditorNode::get_singleton()) {
		EditorNode::get_singleton()->connect("resource_saved", callable_mp(this, &HotspotPatchEditor::_resource_saved));
	}
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->register_hotspot_patch_editor(this);
	}
	_refresh_all();
}

HotspotPatchEditor::~HotspotPatchEditor() {
	if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
		level_editor->unregister_hotspot_patch_editor(this);
	}
	if (atlas.is_valid()) {
		const Callable changed = callable_mp(this, &HotspotPatchEditor::_atlas_changed);
		if (atlas->is_connected(CoreStringName(changed), changed)) {
			atlas->disconnect(CoreStringName(changed), changed);
		}
	}
	document = nullptr;
}
