/**************************************************************************/
/*  level_editor_tool.cpp                                                 */
/**************************************************************************/

#include "level_editor_tool.h"

#include "editor/editor_document.h"
#include "editor/level/level_editor_view.h"
#include "editor/themes/editor_scale.h"

void LevelEditorTool::initialize(LevelEditorView *p_view) {
	ERR_FAIL_COND_MSG(view != nullptr, "LevelEditorTool is already initialized.");
	ERR_FAIL_NULL(p_view);
	view = p_view;
	overlay.set_render_layer(view->get_gizmo_layer());
}

void LevelEditorTool::activate() {
	ERR_FAIL_NULL(view);
	if (active) {
		return;
	}
	active = true;
	LevelDocument *document = view->get_level_document();
	ERR_FAIL_NULL(document);
	overlay.set_scenario(document->get_scenario());
	overlay.set_view_visible(view->is_visible_in_tree());
	_activate();
}

void LevelEditorTool::deactivate() {
	if (!active) {
		return;
	}
	// Commit, Escape, and mode/view teardown all converge on this cleanup path.
	exit_gesture();
	_deactivate();
	active = false;
}

bool LevelEditorTool::handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	if (!active || !p_camera || p_event.is_null()) {
		return false;
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		if ((key->get_keycode() == Key::ESCAPE || key->get_physical_keycode() == Key::ESCAPE) &&
				(_has_active_gesture() || _handles_idle_escape())) {
			_escape_pressed();
			return true;
		}
		if (_has_active_gesture() && (key->get_keycode() == Key::ENTER || key->get_keycode() == Key::KP_ENTER ||
				key->get_physical_keycode() == Key::ENTER || key->get_physical_keycode() == Key::KP_ENTER)) {
			commit_gesture();
			return true;
		}
	}

	return _handle_input(p_camera, p_event);
}

bool LevelEditorTool::commit_gesture() {
	if (!_has_active_gesture() || !_commit_gesture()) {
		return false;
	}
	exit_gesture();
	return true;
}

void LevelEditorTool::exit_gesture() {
	overlay.clear();
	_reset_gesture();
}

void LevelEditorTool::set_view_visible(bool p_visible) {
	overlay.set_view_visible(p_visible);
}

bool LevelEditorTool::drag_started(const Vector2 &p_press_position, const Vector2 &p_current_position) const {
	const real_t drag_threshold = 4.0 * EDSCALE;
	return p_press_position.distance_squared_to(p_current_position) >= drag_threshold * drag_threshold;
}
