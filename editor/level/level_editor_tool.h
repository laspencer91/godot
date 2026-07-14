/**************************************************************************/
/*  level_editor_tool.h                                                   */
/**************************************************************************/
/*  G-Level S1: minimal shared modal-tool lifecycle and input contract.   */
/**************************************************************************/

#pragma once

#include "core/input/input_event.h"
#include "core/object/ref_counted.h"
#include "editor/level/tool_overlay.h"

class Camera3D;
class LevelEditorView;

class LevelEditorTool : public RefCounted {
	GDCLASS(LevelEditorTool, RefCounted);

	LevelEditorView *view = nullptr;
	ToolOverlay overlay;
	bool active = false;

protected:
	static void _bind_methods() {}

	virtual void _activate() {}
	virtual void _deactivate() {}
	virtual bool _handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) { return false; }
	virtual bool _commit_gesture() { return false; }
	virtual bool _has_active_gesture() const { return false; }
	virtual void _reset_gesture() {}
	virtual bool _handles_idle_escape() const { return false; }
	virtual void _escape_pressed() { exit_gesture(); }

	LevelEditorView *get_view() const { return view; }
	ToolOverlay &get_overlay() { return overlay; }
	bool commit_gesture();
	bool drag_started(const Vector2 &p_press_position, const Vector2 &p_current_position) const;

public:
	void initialize(LevelEditorView *p_view);
	void activate();
	void deactivate();
	bool handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);
	void exit_gesture();
	void set_view_visible(bool p_visible);

	bool is_active() const { return active; }
	bool has_active_gesture() const { return active && _has_active_gesture(); }
};
