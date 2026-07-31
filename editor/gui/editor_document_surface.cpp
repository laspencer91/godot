/**************************************************************************/
/*  editor_document_surface.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "editor_document_surface.h"

#include "core/object/callable_mp.h"
#include "core/templates/local_vector.h"
#include "editor/animation/animation_player_editor_plugin.h"
#include "editor/doc/editor_help.h"
#include "editor/docks/groups_dock.h"
#include "editor/docks/inspector_dock.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/docks/signals_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/document_bottom_dock.h"
#include "editor/gui/document_view.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/shader/shader_editor.h"
#include "editor/shader/shader_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/foldable_container.h"
#include "scene/gui/label.h"
#include "scene/gui/split_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/style_box_flat.h"

EditorDocumentSurfaceRegistry *EditorDocumentSurfaceRegistry::singleton = nullptr;

class EditorBuiltinDocumentSurfaceInstance : public EditorDocumentSurfaceInstance {
	GDCLASS(EditorBuiltinDocumentSurfaceInstance, EditorDocumentSurfaceInstance);

protected:
	DocumentView *host_view = nullptr;

	static void _bind_methods() {}

	void _park_script_chrome() {
		// G2 S7: if the shared chrome is mounted under this view, park it home before we die.
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->park_chrome_if_hosted_by(host_view);
		}
	}

public:
	virtual void notify_surface_closing() override {
		// G2 S7: user-close side effects are deliberately separate from teardown. This is
		// never called while a live view is merely detached and adopted by another pane.
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->notify_surface_closing(get_root_control());
		}
	}

	EditorBuiltinDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) {
		host_view = p_context.host_view;
	}
};

class EditorResourceDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorResourceDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

	InspectorDock *inspector_dock = nullptr;

protected:
	static void _bind_methods() {}

public:
	virtual void set_context_active(bool p_active) override {
		if (inspector_dock) {
			inspector_dock->set_context_active(p_active);
		}
	}

	virtual void pre_delete_cleanup() override {
		_park_script_chrome();
	}

	EditorResourceDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		ResourceDocument *rd = static_cast<ResourceDocument *>(p_context.document);
		EditorData &ed = EditorNode::get_editor_data();
		inspector_dock = memnew(InspectorDock(ed, false));
		inspector_dock->set_bound_document(p_context.document);
		inspector_dock->edit_resource_document(rd->get_resource());
		set_root_control(inspector_dock);
	}
};

class EditorScriptDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorScriptDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

	ScriptEditorBase *script_surface = nullptr;

protected:
	static void _bind_methods() {}

public:
	virtual void pre_delete_cleanup() override {
		_park_script_chrome();
		// G2 S4: if this view hosted a script surface, drop it from the ScriptEditor open-scripts
		// registry before it is freed (idempotent belt-and-suspenders with tree_exiting).
		if (script_surface) {
			if (ScriptEditor *se = ScriptEditor::get_singleton()) {
				se->release_editor_view(script_surface);
			}
		}
	}

	EditorScriptDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		// G2 S4: the per-script VIEW is minted by the ScriptEditor SERVICES singleton, fully wired to
		// menus / find-in-files / save-all / debugger. The singleton stays; only the view is per-tab.
		ScriptDocument *sd = static_cast<ScriptDocument *>(p_context.document);
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			script_surface = se->create_editor_view(sd->get_script_resource());
			set_root_control(script_surface);
		}
	}
};

class EditorShaderDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorShaderDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

	ShaderEditor *shader_surface = nullptr;

protected:
	static void _bind_methods() {}

public:
	virtual void pre_delete_cleanup() override {
		_park_script_chrome();
		// G-Shader: same for a shader surface - park the traveling File menu (if hosted here) and drop
		// the plugin's tracking entry before Node frees this view's children.
		if (shader_surface) {
			if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
				sep->release_editor_view(shader_surface);
			}
		}
	}

	EditorShaderDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		// G-Shader: the shader editor widget (text code editor or visual node graph) is minted by
		// the ShaderEditorPlugin SERVICES singleton via the shader-language factory. The plugin
		// stays; only the widget is per-tab (released on PREDELETE, above).
		ShaderDocument *shd = static_cast<ShaderDocument *>(p_context.document);
		if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
			shader_surface = sep->create_editor_view(shd->get_shader_resource());
			set_root_control(shader_surface);
		}
	}
};

class EditorHelpDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorHelpDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

protected:
	static void _bind_methods() {}

public:
	virtual void pre_delete_cleanup() override {
		_park_script_chrome();
	}

	EditorHelpDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		// G2 S6b: the view is minted by the ScriptEditor SERVICES singleton (wired to go_to_help
		// navigation + history, registered in the open-help registry). go_to_class needs the view
		// in the tree (theme + doc data), so defer it until after this DocumentView is parented.
		HelpDocument *hd = static_cast<HelpDocument *>(p_context.document);
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			EditorHelp *help = se->create_help_view(hd->get_class_name());
			callable_mp(help, &EditorHelp::go_to_class).call_deferred(hd->get_class_name());
			set_root_control(help);
		}
	}
};

class EditorScreenHostDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorScreenHostDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

	ScreenHostDocument *document = nullptr;
	Control *chrome_host = nullptr;
	Control *screen_stack = nullptr;

protected:
	static void _bind_methods() {}

public:
	virtual void pre_delete_cleanup() override {
		_park_script_chrome();
		// G2 S5.5: the screen-host view does not own the legacy main-screen stack - park it back under
		// EditorMainScreen's hidden holder so get_control() stays live (D11). If the holder is already
		// gone (whole-editor teardown, children die last-first), leave the stack to be freed with this
		// view, matching the stock lifetime where the vbox died with the main-screen tree.
		if (document && screen_stack && screen_stack->get_parent() == chrome_host) { // G2 S7: surfaces live under content_vbox now.
			Control *park = Object::cast_to<Control>(ObjectDB::get_instance(document->get_park_holder_id()));
			if (park) {
				chrome_host->remove_child(screen_stack);
				park->add_child(screen_stack);
				screen_stack = nullptr;
				set_root_control(nullptr); // No longer ours; Node's PREDELETE must not free it.
			}
		}
	}

	EditorScreenHostDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		// G2 S5.5: this view hosts the legacy main-screen stack ITSELF (seam #5). The stack is
		// EditorMainScreen's main_screen_vbox - never owned here; pre_delete_cleanup() parks it
		// back under the hidden holder so get_control() keeps returning the live vbox (D11).
		document = static_cast<ScreenHostDocument *>(p_context.document);
		chrome_host = p_context.chrome_host;
		screen_stack = document->get_screen_stack();
		if (screen_stack) {
			if (Node *stack_parent = screen_stack->get_parent()) {
				stack_parent->remove_child(screen_stack); // Un-park (re-summon after a tab close).
			}
			set_root_control(screen_stack);
		}
	}
};

// G2 styling: one "card" stylebox for a dock section. Only the fill color, header-vs-content role, and
// bottom rounding vary across the five styleboxes a section uses; radius and padding are fixed. Headers
// get the raised top-edge highlight + faint shadow (the "subtle gradient" lift); the panel is flat.
static Ref<StyleBoxFlat> _dock_card_sb(const Color &p_bg, bool p_header, bool p_round_bottom) {
	const int radius = 5 * EDSCALE;
	Ref<StyleBoxFlat> sb;
	sb.instantiate();
	sb->set_bg_color(p_bg);
	if (p_header) {
		sb->set_corner_radius(CORNER_TOP_LEFT, radius);
		sb->set_corner_radius(CORNER_TOP_RIGHT, radius);
	}
	if (p_round_bottom) {
		sb->set_corner_radius(CORNER_BOTTOM_LEFT, radius);
		sb->set_corner_radius(CORNER_BOTTOM_RIGHT, radius);
	}
	const int pad_v = (p_header ? 6 : 4) * EDSCALE;
	const int pad_h = (p_header ? 10 : 4) * EDSCALE;
	sb->set_content_margin(SIDE_LEFT, pad_h);
	sb->set_content_margin(SIDE_RIGHT, pad_h);
	sb->set_content_margin(SIDE_TOP, pad_v);
	sb->set_content_margin(SIDE_BOTTOM, pad_v);
	if (p_header) {
		// 1px top-edge highlight (light from above) + faint drop shadow lifts the header card off the
		// column - the "subtle gradient" pop without a baked texture (keeps the crisp corners).
		sb->set_border_width(SIDE_TOP, MAX(1, int(EDSCALE)));
		sb->set_border_color(p_bg.lightened(0.35));
		sb->set_shadow_color(Color(0, 0, 0, 0.16));
		sb->set_shadow_size(MAX(2, int(3 * EDSCALE)));
		sb->set_shadow_offset(Size2(0, MAX(1, int(EDSCALE))));
	}
	return sb;
}

static void _add_accordion_title_icon(FoldableContainer *p_section, const StringName &p_icon) {
	if (p_icon == StringName()) {
		return;
	}
	Control *gui_base = EditorNode::get_singleton()->get_gui_base();
	Ref<Texture2D> icon = gui_base->get_theme_icon(p_icon, EditorStringName(EditorIcons));
	if (icon.is_valid()) {
		TextureRect *rect = memnew(TextureRect);
		rect->set_texture(icon);
		rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		rect->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
		p_section->add_title_bar_control(rect);
	}
}

static String _get_inspector_target_name(Object *p_object) {
	if (!p_object) {
		return String();
	}
	if (p_object->has_method(SNAME("_get_editor_name"))) {
		const String editor_name = p_object->call(SNAME("_get_editor_name"));
		if (!editor_name.is_empty()) {
			return editor_name;
		}
	}
	if (Node *node = Object::cast_to<Node>(p_object)) {
		return node->get_name();
	}
	if (Resource *resource = Object::cast_to<Resource>(p_object)) {
		if (!resource->get_name().is_empty()) {
			return resource->get_name();
		}
		if (resource->get_path().is_resource_file()) {
			return resource->get_path().get_file();
		}
	}
	return p_object->get_class();
}

class EditorSceneDocumentSurfaceInstance : public EditorBuiltinDocumentSurfaceInstance {
	GDCLASS(EditorSceneDocumentSurfaceInstance, EditorBuiltinDocumentSurfaceInstance);

	EditorDocumentView *document_view = nullptr;
	EditorDocument *bound_scene_document = nullptr;
	Control *editor_surface = nullptr;
	Control *document_surface = nullptr;
	Control *scene_surface_stack = nullptr;
	Control *scene_surface_2d = nullptr;
	Control *scene_surface_3d = nullptr;
	SceneTreeDock *scene_tree_dock = nullptr;
	InspectorDock *inspector_dock = nullptr;
	SignalsDock *signals_dock = nullptr;
	GroupsDock *groups_dock = nullptr;
	HBoxContainer *toolbar_host = nullptr;
	DocumentBottomDockHost *bottom_dock_host = nullptr;
	AnimationPlayerEditor *animation_editor = nullptr;
	Label *inspector_target_label = nullptr;
	Button *inspector_lock_button = nullptr;
	bool scene_view_2d = false;
	bool context_active = false;
	bool scene_tree_selection_sync_pending = false;

	Control *_create_scene_surface(bool p_2d) {
		EditorDocument *document = document_view ? document_view->get_document() : nullptr;
		if (!document) {
			return nullptr;
		}
		if (p_2d) {
			CanvasItemEditor *canvas_editor = CanvasItemEditor::get_singleton();
			return canvas_editor ? canvas_editor->create_view_bound_to(document) : nullptr;
		}
		Node3DEditor *spatial_editor = Node3DEditor::get_singleton();
		return spatial_editor ? spatial_editor->create_view_bound_to(document) : nullptr;
	}

	Control *_ensure_scene_surface(bool p_2d) {
		Control *&surface = p_2d ? scene_surface_2d : scene_surface_3d;
		if (surface || !scene_surface_stack) {
			return surface;
		}
		surface = _create_scene_surface(p_2d);
		if (surface) {
			surface->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			surface->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			surface->hide();
			scene_surface_stack->add_child(surface);
		}
		return surface;
	}

	FoldableContainer *_add_accordion_section(VBoxContainer *p_column, Control *p_dock, const String &p_title, const StringName &p_icon, bool p_expanded) {
		// Build one GDStudio-style dock "card": a raised rounded header (leading icon + top-edge highlight)
		// over a recessed content panel. Colors are read from the editor theme at construction time - they
		// do NOT re-tint on a later theme change (accepted tradeoff until this moves to a DockSection theme
		// variation, which would also drop the per-pane stylebox rebuild).
		p_dock->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		FoldableContainer *section = memnew(FoldableContainer);
		section->set_title(p_title);
		section->add_child(p_dock);

		Control *gui_base = EditorNode::get_singleton()->get_gui_base();
		const Color base = gui_base->get_theme_color(SNAME("base_color"), EditorStringName(Editor));
		const Color header = base.lightened(0.10);
		const Color header_hover = base.lightened(0.16);
		section->add_theme_style_override(SNAME("title_panel"), _dock_card_sb(header, true, false));
		section->add_theme_style_override(SNAME("title_hover_panel"), _dock_card_sb(header_hover, true, false));
		section->add_theme_style_override(SNAME("title_collapsed_panel"), _dock_card_sb(header, true, true));
		section->add_theme_style_override(SNAME("title_collapsed_hover_panel"), _dock_card_sb(header_hover, true, true));
		section->add_theme_style_override(SNAME("panel"), _dock_card_sb(base.darkened(0.04), false, true));
		// The editor theme's FoldableContainer "focus" stylebox is an accent (blue) outline; it's square, so
		// it pokes out behind our rounded card corners when the header is focused. These fold headers are
		// mouse-driven - drop the ring so no blue shows behind the rounded tops.
		section->add_theme_style_override(SNAME("focus"), Ref<StyleBox>(memnew(StyleBoxEmpty)));

		_add_accordion_title_icon(section, p_icon);

		section->set_folded(!p_expanded);
		_on_section_folded(!p_expanded, section); // Single source for the fold-to-expand-flag rule (no signal yet).
		section->connect("folding_changed", callable_mp(this, &EditorSceneDocumentSurfaceInstance::_on_section_folded).bind(section));
		p_column->add_child(section);
		return section;
	}

	void _on_section_folded(bool p_folded, FoldableContainer *p_section) {
		// Responsive accordion: expanded sections share the column proportionally; collapsing one shrinks
		// it to just its header, freeing that space for the rest (so e.g. folding Inspector lifts Signals
		// and Groups up). No fixed heights, so nothing stays locked or clips.
		p_section->set_v_size_flags(p_folded ? Control::SIZE_FILL : Control::SIZE_EXPAND_FILL);
	}

	void _inspector_lock_toggled(bool p_locked) {
		if (!inspector_dock || !inspector_lock_button) {
			return;
		}
		if (p_locked && !inspector_dock->get_inspector()->get_edited_object()) {
			inspector_lock_button->set_pressed_no_signal(false);
			p_locked = false;
		}

		inspector_dock->set_selection_locked(p_locked);
		if (!p_locked && scene_tree_dock) {
			scene_tree_dock->sync_bound_inspector_to_selection();
		}
		_update_inspector_header();
	}

	void _update_inspector_header() {
		if (!inspector_dock || !inspector_target_label || !inspector_lock_button) {
			return;
		}
		if (!inspector_dock->is_selection_lock_target_valid()) {
			inspector_dock->set_selection_locked(false);
			inspector_lock_button->set_pressed_no_signal(false);
			inspector_dock->edit_bound(nullptr); // Clears a possibly stale raw pointer safely.
			if (scene_tree_dock) {
				callable_mp(scene_tree_dock, &SceneTreeDock::sync_bound_inspector_to_selection).call_deferred();
			}
			return;
		}
		Object *object = inspector_dock->get_inspector()->get_edited_object();
		if (!object && inspector_dock->is_selection_locked()) {
			inspector_dock->set_selection_locked(false);
			inspector_lock_button->set_pressed_no_signal(false);
			if (scene_tree_dock) {
				callable_mp(scene_tree_dock, &SceneTreeDock::sync_bound_inspector_to_selection).call_deferred();
			}
		}
		if (object && inspector_dock->is_selection_locked()) {
			// Explicit Inspector history/resource navigation is allowed while locked; that newly
			// inspected object becomes the target protected from subsequent Scene Tree selection.
			inspector_dock->set_selection_locked(true);
		}

		const String target_name = _get_inspector_target_name(object);
		inspector_target_label->set_text(target_name);
		inspector_target_label->set_tooltip_text(target_name);
		inspector_target_label->set_visible(!target_name.is_empty());
		inspector_lock_button->set_disabled(object == nullptr);
		inspector_lock_button->set_pressed_no_signal(inspector_dock->is_selection_locked());
		// Through gui_base (always in-tree): this runs during construction, where a
		// bare get_theme_icon() on the not-yet-added view warns "too early".
		inspector_lock_button->set_button_icon(EditorNode::get_singleton()->get_gui_base()->get_theme_icon(inspector_dock->is_selection_locked() ? SNAME("Lock") : SNAME("Unlock"), EditorStringName(EditorIcons)));
		inspector_lock_button->set_tooltip_text(inspector_dock->is_selection_locked() ? TTR("Unlock Inspector and follow the Scene Tree selection") : TTR("Lock Inspector to the current object"));
	}

	void _document_bottom_dock_toggled(StringName p_id, bool p_open) {
		if (p_id == SNAME("Animation") && animation_editor) {
			animation_editor->set_embedded_open(p_open);
			_store_animation_drawer_state();
		}
	}

	void _animation_drawer_visibility_requested(bool p_open) {
		if (bottom_dock_host) {
			bottom_dock_host->set_dock_open(SNAME("Animation"), p_open);
		}
	}

	void _store_animation_drawer_state() {
		if (!bound_scene_document || !animation_editor) {
			return;
		}
		bound_scene_document->set_contextual_editor_state(SNAME("Animation"), animation_editor->get_state());
	}

	void _bound_selection_changed() {
		if (!bound_scene_document) {
			return;
		}
		EditorSelection *selection = bound_scene_document->get_selection();
		if (!selection) {
			return;
		}
		List<Node *> nodes = selection->get_top_selected_node_list();
		Node *front = nodes.is_empty() ? nullptr : nodes.front()->get();
		// G3: the per-pane Signals + Groups docks follow the same selection.
		if (signals_dock) {
			signals_dock->set_object(front);
		}
		if (groups_dock) {
			Vector<Node *> node_vec;
			for (Node *n : nodes) {
				node_vec.push_back(n);
			}
			groups_dock->set_selection(node_vec);
		}
		_update_inspector_header();
		_queue_bound_scene_tree_selection_sync();
	}

	void _queue_bound_scene_tree_selection_sync() {
		if (!scene_tree_dock || !bound_scene_document || scene_tree_selection_sync_pending) {
			return;
		}

		scene_tree_selection_sync_pending = true;
		if (host_view->is_inside_tree()) {
			callable_mp(this, &EditorSceneDocumentSurfaceInstance::_sync_bound_scene_tree_selection).call_deferred();
		}
	}

	void _sync_bound_scene_tree_selection() {
		if (!scene_tree_selection_sync_pending || !host_view->is_inside_tree()) {
			return;
		}
		scene_tree_selection_sync_pending = false;

		if (!scene_tree_dock || !bound_scene_document) {
			return;
		}
		EditorSelection *selection = bound_scene_document->get_selection();
		if (!selection) {
			return;
		}

		const List<Node *> nodes = selection->get_top_selected_node_list();
		Node *reveal = nodes.is_empty() ? nullptr : nodes.back()->get();
		Node *root = bound_scene_document->get_root();
		if (reveal && (!root || (reveal != root && !root->is_ancestor_of(reveal)))) {
			reveal = nullptr;
		}

		// The EditorSelection signal updates row flags, while set_selected also updates SceneTreeEditor's
		// cursor and reveal state. Keep the last selected node as the active row for multi-selection, matching
		// the 3D viewport's active-node convention.
		scene_tree_dock->set_selected(reveal);
	}

protected:
	static void _bind_methods() {}

public:
	virtual void capture_view_state(Dictionary &r_state) const override {
		r_state[SNAME("scene_view_2d")] = scene_view_2d;
		if (const Node3DEditorView *spatial_view = Object::cast_to<Node3DEditorView>(scene_surface_3d)) {
			r_state[SNAME("node_3d")] = spatial_view->capture_view_state();
		}
	}

	virtual void apply_view_state(const Dictionary &p_state) override {
		if (p_state.has(SNAME("node_3d"))) {
			if (Node3DEditorView *spatial_view = Object::cast_to<Node3DEditorView>(_ensure_scene_surface(false))) {
				spatial_view->apply_view_state(p_state[SNAME("node_3d")]);
			}
		}
		if (p_state.has(SNAME("scene_view_2d"))) {
			set_scene_view_2d(p_state[SNAME("scene_view_2d")]);
		}
	}

	virtual SubViewport *get_scene_viewport() const override {
		if (CanvasItemEditorView *canvas_view = Object::cast_to<CanvasItemEditorView>(document_surface)) {
			return canvas_view->get_scene_viewport();
		}
		if (Node3DEditorView *spatial_view = Object::cast_to<Node3DEditorView>(document_surface)) {
			Node3DEditorViewport *editor_viewport = spatial_view->get_last_used_viewport();
			return editor_viewport ? editor_viewport->get_viewport_node() : nullptr;
		}
		return nullptr;
	}

	virtual Control *get_toolbar_host() const override {
		return toolbar_host;
	}

	virtual bool is_scene_view() const override {
		return bound_scene_document != nullptr;
	}

	virtual bool is_scene_view_2d() const override {
		return is_scene_view() && scene_view_2d;
	}

	virtual bool set_scene_view_2d(bool p_2d) override {
		if (!is_scene_view() || !scene_surface_stack) {
			return false;
		}
		if (scene_view_2d == p_2d) {
			return true;
		}

		Control *next_surface = _ensure_scene_surface(p_2d);
		if (!next_surface) {
			return false;
		}

		if (CanvasItemEditorView *canvas_view = Object::cast_to<CanvasItemEditorView>(document_surface)) {
			canvas_view->set_context_active(false);
		}
		document_surface->hide();
		document_surface = next_surface;
		scene_view_2d = p_2d;
		document_view->get_editor_states()[SNAME("scene_view_2d")] = scene_view_2d;
		document_surface->show();
		if (CanvasItemEditorView *canvas_view = Object::cast_to<CanvasItemEditorView>(document_surface)) {
			canvas_view->set_context_active(context_active);
		}

		if (context_active) {
			EditorNode::get_singleton()->update_scene_pane_toolbar(host_view);
		}
		return true;
	}

	virtual void set_context_active(bool p_active) override {
		context_active = p_active;
		if (scene_tree_dock) {
			scene_tree_dock->set_context_active(p_active);
		}
		if (CanvasItemEditorView *canvas_view = Object::cast_to<CanvasItemEditorView>(document_surface)) {
			canvas_view->set_context_active(p_active);
		}
		if (inspector_dock) {
			inspector_dock->set_context_active(p_active);
		}
		if (animation_editor) {
			if (!p_active) {
				_store_animation_drawer_state();
			}
			animation_editor->set_context_active(p_active);
		}
	}

	virtual void document_view_entered_tree() override {
		if (scene_tree_selection_sync_pending) {
			callable_mp(this, &EditorSceneDocumentSurfaceInstance::_sync_bound_scene_tree_selection).call_deferred();
		}
	}

	virtual void document_view_theme_changed() override {
		_update_inspector_header();
	}

	virtual void document_view_ready() override {
		// G2 D7a: re-point the embedded Scene Tree at its document's root now that we're in-tree. The
		// scene root can be assigned after a background (grab_focus=false) view is minted, so a refresh
		// here guarantees the tree shows the current tree rather than an empty one.
		if (scene_tree_dock && bound_scene_document) {
			scene_tree_dock->set_edited_scene(bound_scene_document->get_root());
		}
	}

	virtual void pre_delete_cleanup() override {
		if (scene_tree_dock) {
			scene_tree_dock->set_context_active(false);
			scene_tree_dock->set_bound_inspector(nullptr);
		}
		if (animation_editor) {
			_store_animation_drawer_state();
			if (AnimationPlayerEditorPlugin *plugin = AnimationPlayerEditorPlugin::get_singleton()) {
				plugin->release_editor_view(animation_editor);
			}
		}
		_park_script_chrome();
		// G2 M7.2a: same for the shared 2D/3D toolbar - it travels into the focused scene pane's
		// toolbar_host, and dying with this view would leave the editors' toolbar pointers dangling.
		if (toolbar_host) {
			if (Node3DEditor *sp = Node3DEditor::get_singleton()) {
				Control *tb = sp->get_shared_toolbar();
				if (tb && tb->get_parent() == toolbar_host) {
					sp->park_shared_toolbar();
				}
			}
			if (CanvasItemEditor *ci = CanvasItemEditor::get_singleton()) {
				Control *tb = ci->get_shared_toolbar();
				if (tb && tb->get_parent() == toolbar_host) {
					ci->park_shared_toolbar();
				}
			}
		}
	}

	EditorSceneDocumentSurfaceInstance(const EditorDocumentSurfaceContext &p_context) :
			EditorBuiltinDocumentSurfaceInstance(p_context) {
		document_view = p_context.document_view;

		// Host the editor surface for this document's kind, pointed at THIS document's isolated
		// world so the pane renders p_document's scene independently of the globally-active one.
		// 2D scenes initially get a CanvasItemEditorView; 3D and mixed scenes initially get a
		// Node3DEditorView. The other scene surface is minted lazily by the pane's 2D/3D selector.
		const EditorDocument::Type type = p_context.document ? p_context.document->get_type() : EditorDocument::TYPE_UNKNOWN;
		switch (type) {
			case EditorDocument::TYPE_SCENE_2D: {
				scene_view_2d = true;
				editor_surface = _create_scene_surface(true);
			} break;
			case EditorDocument::TYPE_SCENE_3D:
			case EditorDocument::TYPE_SCENE_MIXED: {
				scene_view_2d = false;
				editor_surface = _create_scene_surface(false);
			} break;
			default: {
				// An unclassified SceneDocument can be revealed before its root is assigned. It still gets
				// the scene switching surface; non-scene unknown documents retain the old 3D fallback.
				editor_surface = p_context.document && p_context.document->get_selection() ? _create_scene_surface(false) : (Node3DEditor::get_singleton() ? Node3DEditor::get_singleton()->create_view_bound_to(p_context.document) : nullptr);
			} break;
		}
		document_surface = editor_surface;
		// G2 D7a: a scene document's tab is a COMPOSITE - its own Scene Tree dock to the left of the
		// editor surface, bound to this document. The dock is a bound instance (is_global=false, D5, so it
		// never claims the SceneTreeDock singleton) reading this doc's own root/selection/history via the
		// D4 resolvers. Non-scene documents (script/help/screen-host/resource) have no selection, so they
		// are left as a plain surface. The composite becomes `editor_surface` (the split), which the
		// generic parent/stretch in DocumentView adds to content_vbox.
		if (editor_surface && p_context.document && p_context.document->get_selection()) {
			bound_scene_document = p_context.document;
			if (scene_view_2d) {
				scene_surface_2d = document_surface;
			} else {
				scene_surface_3d = document_surface;
			}
			document_view->get_editor_states()[SNAME("scene_view_2d")] = scene_view_2d;
			EditorData &ed = EditorNode::get_editor_data();

			// D7b: the per-pane dock column - a vertical accordion of FoldableContainer sections
			// (Scene Tree + Inspector expanded, Signals + Groups folded), on the RIGHT of the surface.
			VBoxContainer *dock_column = memnew(VBoxContainer);
			dock_column->set_custom_minimum_size(Size2(400 * EDSCALE, 0));
			dock_column->add_theme_constant_override("separation", 6 * EDSCALE); // Gaps make sections read as stacked cards.

			// Each section is a styled FoldableContainer "card" built by _add_accordion_section; only the dock
			// construction differs. Scene Tree + Inspector start expanded, Signals + Groups collapsed.
			scene_tree_dock = memnew(SceneTreeDock(p_context.document->get_root(), p_context.document->get_selection(), ed, false));
			scene_tree_dock->set_bound_document(p_context.document);
			scene_tree_dock->set_edited_scene(p_context.document->get_root());
			scene_tree_dock->set_document_shortcut_context(host_view);
			_add_accordion_section(dock_column, scene_tree_dock, TTR("Scene Tree"), SNAME("PackedScene"), true);

			inspector_dock = memnew(InspectorDock(ed, false));
			inspector_dock->set_bound_document(p_context.document);
			scene_tree_dock->set_bound_inspector(inspector_dock);
			FoldableContainer *inspector_section = _add_accordion_section(dock_column, inspector_dock, TTR("Inspector"), StringName(), true);

			inspector_target_label = memnew(Label);
			inspector_target_label->set_custom_minimum_size(Size2(96 * EDSCALE, 0));
			inspector_target_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
			inspector_target_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
			inspector_target_label->set_clip_text(true);
			inspector_target_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			inspector_target_label->set_modulate(Color(1, 1, 1, 0.58));
			inspector_section->add_title_bar_control(inspector_target_label);

			inspector_lock_button = memnew(Button);
			inspector_lock_button->set_name("InspectorLock");
			inspector_lock_button->set_toggle_mode(true);
			inspector_lock_button->set_flat(true);
			inspector_lock_button->set_focus_mode(Control::FOCUS_NONE);
			inspector_lock_button->set_accessibility_name(TTR("Lock Inspector"));
			inspector_lock_button->connect(SceneStringName(toggled), callable_mp(this, &EditorSceneDocumentSurfaceInstance::_inspector_lock_toggled));
			inspector_section->add_title_bar_control(inspector_lock_button);
			_add_accordion_title_icon(inspector_section, SNAME("Object"));
			inspector_dock->get_inspector()->connect(SNAME("edited_object_changed"), callable_mp(this, &EditorSceneDocumentSurfaceInstance::_update_inspector_header));

			signals_dock = memnew(SignalsDock(false));
			_add_accordion_section(dock_column, signals_dock, TTR("Signals"), SNAME("Signals"), false);

			groups_dock = memnew(GroupsDock(false));
			_add_accordion_section(dock_column, groups_dock, TTR("Groups"), SNAME("Groups"), false);

			// SceneTreeDock owns Inspector selection timing so a pressed node can be dragged into a property
			// of the previously inspected node. This connection keeps the pane's other selection presentation
			// synchronized without changing that Inspector timing.
			p_context.document->get_selection()->connect("selection_changed", callable_mp(this, &EditorSceneDocumentSurfaceInstance::_bound_selection_changed));
			List<Node *> initial_nodes = p_context.document->get_selection()->get_top_selected_node_list();
			inspector_dock->edit_bound(initial_nodes.is_empty() ? nullptr : initial_nodes.front()->get());
			_bound_selection_changed();
			_update_inspector_header();

			// Left side: [toolbar host | viewport]. M7.2a mounts the focused pane's 2D/3D toolbar into
			// toolbar_host; it sits empty (zero height) otherwise.
			toolbar_host = memnew(HBoxContainer);
			toolbar_host->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			VBoxContainer *surface_vbox = memnew(VBoxContainer);
			surface_vbox->add_theme_constant_override("separation", 0);
			surface_vbox->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			surface_vbox->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			surface_vbox->add_child(toolbar_host);
			document_surface->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			document_surface->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			scene_surface_stack = memnew(MarginContainer);
			scene_surface_stack->set_name("SceneSurfaceStack");
			scene_surface_stack->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			scene_surface_stack->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			scene_surface_stack->add_child(document_surface);

			const bool supports_animation_drawer = type == EditorDocument::TYPE_SCENE_2D || type == EditorDocument::TYPE_SCENE_3D || type == EditorDocument::TYPE_SCENE_MIXED;
			AnimationPlayerEditorPlugin *animation_plugin = supports_animation_drawer ? AnimationPlayerEditorPlugin::get_singleton() : nullptr;
			animation_editor = animation_plugin ? animation_plugin->create_editor_view(p_context.document, inspector_dock) : nullptr;
			if (animation_editor) {
				bottom_dock_host = memnew(DocumentBottomDockHost(toolbar_host));
				bottom_dock_host->set_surface(scene_surface_stack);
				bottom_dock_host->connect(SNAME("dock_toggled"), callable_mp(this, &EditorSceneDocumentSurfaceInstance::_document_bottom_dock_toggled));
				animation_editor->connect(SNAME("drawer_visibility_requested"), callable_mp(this, &EditorSceneDocumentSurfaceInstance::_animation_drawer_visibility_requested));
				bottom_dock_host->add_dock(SNAME("Animation"), TTR("Animation"), SNAME("Animation"), animation_editor);
				surface_vbox->add_child(bottom_dock_host);

				const Dictionary animation_state = p_context.document->get_contextual_editor_state(SNAME("Animation"));
				if (animation_state.is_empty()) {
					animation_editor->set_embedded_open(false);
				} else {
					animation_editor->set_state(animation_state);
				}
			} else {
				surface_vbox->add_child(scene_surface_stack);
			}

			// Compose: [toolbar|viewport] | dock column (docks on the right, per the design). The accordion
			// sizes responsively - expanded sections share the column via SIZE_EXPAND_FILL, collapsed ones
			// shrink to their header (see _add_accordion_section / _on_section_folded) and each section's
			// content scrolls internally, so no outer ScrollContainer is needed.
			HSplitContainer *scene_split = memnew(HSplitContainer);
			scene_split->add_child(surface_vbox);
			scene_split->add_child(dock_column);
			editor_surface = scene_split;
		}

		set_root_control(editor_surface);
	}
};

class EditorResourceDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("resource"); }

	virtual bool supports(EditorDocument *p_document) const override {
		return p_document && p_document->get_type() == EditorDocument::TYPE_RESOURCE && EditorNode::get_singleton();
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorResourceDocumentSurfaceInstance(p_context));
	}
};

class EditorScriptDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("script"); }

	virtual bool supports(EditorDocument *p_document) const override {
		return p_document && p_document->get_type() == EditorDocument::TYPE_SCRIPT && ScriptEditor::get_singleton();
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorScriptDocumentSurfaceInstance(p_context));
	}
};

class EditorShaderDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("shader"); }

	virtual bool supports(EditorDocument *p_document) const override {
		return p_document && p_document->get_type() == EditorDocument::TYPE_SHADER && ShaderEditorPlugin::get_singleton();
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorShaderDocumentSurfaceInstance(p_context));
	}
};

class EditorHelpDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("help"); }

	virtual bool supports(EditorDocument *p_document) const override {
		return p_document && p_document->get_type() == EditorDocument::TYPE_HELP && ScriptEditor::get_singleton();
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorHelpDocumentSurfaceInstance(p_context));
	}
};

class EditorScreenHostDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("screen_host"); }

	virtual bool supports(EditorDocument *p_document) const override {
		if (!p_document || p_document->get_type() != EditorDocument::TYPE_SCREEN_HOST) {
			return false;
		}
		return static_cast<ScreenHostDocument *>(p_document)->get_screen_stack() != nullptr;
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorScreenHostDocumentSurfaceInstance(p_context));
	}
};

class EditorSceneDocumentSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("scene"); }

	virtual bool supports(EditorDocument *p_document) const override {
		if (!p_document) {
			return Node3DEditor::get_singleton() != nullptr;
		}
		switch (p_document->get_type()) {
			case EditorDocument::TYPE_SCENE_2D:
				return CanvasItemEditor::get_singleton() != nullptr;
			case EditorDocument::TYPE_SCENE_3D:
			case EditorDocument::TYPE_SCENE_MIXED:
			case EditorDocument::TYPE_UNKNOWN:
				return Node3DEditor::get_singleton() != nullptr;
			default:
				return false;
		}
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		return memnew(EditorSceneDocumentSurfaceInstance(p_context));
	}
};

void EditorDocumentSurfaceRegistry::create() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = memnew(EditorDocumentSurfaceRegistry);
}

void EditorDocumentSurfaceRegistry::free() {
	if (!singleton) {
		return;
	}
	// CSG-3A: The registry borrows providers. Dropping its lookup table invalidates
	// resolution immediately without reaching into any already-created instance.
	singleton->providers.clear();
	memdelete(singleton);
	singleton = nullptr;
}

bool EditorDocumentSurfaceRegistry::register_provider(EditorDocumentSurfaceProvider *p_provider) {
	ERR_FAIL_NULL_V(p_provider, false);
	const StringName surface_id = p_provider->get_surface_id();
	ERR_FAIL_COND_V(surface_id.is_empty(), false);
	ERR_FAIL_COND_V_MSG(providers.has(surface_id), false, vformat("A document surface provider with ID '%s' is already registered.", surface_id));
	providers.insert(surface_id, p_provider);
	return true;
}

bool EditorDocumentSurfaceRegistry::unregister_provider(const StringName &p_surface_id, EditorDocumentSurfaceProvider *p_provider) {
	EditorDocumentSurfaceProvider **registered = providers.getptr(p_surface_id);
	if (!registered || (p_provider && *registered != p_provider)) {
		return false;
	}
	providers.erase(p_surface_id);
	return true;
}

EditorDocumentSurfaceProvider *EditorDocumentSurfaceRegistry::resolve_surface(const StringName &p_surface_id, EditorDocument *p_document) const {
	EditorDocumentSurfaceProvider *const *provider = providers.getptr(p_surface_id);
	if (!provider || !(*provider)->supports(p_document)) {
		return nullptr;
	}
	return *provider;
}

StringName EditorDocumentSurfaceRegistry::get_default_surface_id(EditorDocument *p_document) const {
	const EditorDocument::Type type = p_document ? p_document->get_type() : EditorDocument::TYPE_UNKNOWN;
	switch (type) {
		case EditorDocument::TYPE_RESOURCE:
			return SNAME("resource");
		case EditorDocument::TYPE_SCRIPT:
			return SNAME("script");
		case EditorDocument::TYPE_SHADER:
			return SNAME("shader");
		case EditorDocument::TYPE_HELP:
			return SNAME("help");
		case EditorDocument::TYPE_SCREEN_HOST:
			return SNAME("screen_host");
		case EditorDocument::TYPE_SCENE_2D:
		case EditorDocument::TYPE_SCENE_3D:
		case EditorDocument::TYPE_SCENE_MIXED:
		default:
			// CSG-3A: This default is intentional. The old DocumentView switch sent an
			// unclassified SceneDocument through the scene composite and a truly unknown
			// document through a bare 3D view; the scene instance retains that distinction.
			return SNAME("scene");
	}
}

EditorDocumentSurfaceProvider *EditorDocumentSurfaceRegistry::resolve_default_surface(EditorDocument *p_document) const {
	return resolve_surface(get_default_surface_id(p_document), p_document);
}

static LocalVector<EditorDocumentSurfaceProvider *> builtin_surface_providers;

void register_editor_document_surface_providers() {
	EditorDocumentSurfaceRegistry *registry = EditorDocumentSurfaceRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	ERR_FAIL_COND(!builtin_surface_providers.is_empty());

	builtin_surface_providers.push_back(memnew(EditorResourceDocumentSurfaceProvider));
	builtin_surface_providers.push_back(memnew(EditorScriptDocumentSurfaceProvider));
	builtin_surface_providers.push_back(memnew(EditorShaderDocumentSurfaceProvider));
	builtin_surface_providers.push_back(memnew(EditorHelpDocumentSurfaceProvider));
	builtin_surface_providers.push_back(memnew(EditorScreenHostDocumentSurfaceProvider));
	builtin_surface_providers.push_back(memnew(EditorSceneDocumentSurfaceProvider));

	for (uint32_t i = 0; i < builtin_surface_providers.size(); i++) {
		EditorDocumentSurfaceProvider *provider = builtin_surface_providers[i];
		if (!registry->register_provider(provider)) {
			for (uint32_t j = 0; j < i; j++) {
				EditorDocumentSurfaceProvider *registered = builtin_surface_providers[j];
				registry->unregister_provider(registered->get_surface_id(), registered);
			}
			for (EditorDocumentSurfaceProvider *allocated : builtin_surface_providers) {
				memdelete(allocated);
			}
			builtin_surface_providers.clear();
			ERR_FAIL_MSG("Failed to register the built-in document surface providers.");
		}
	}
}

void unregister_editor_document_surface_providers() {
	EditorDocumentSurfaceRegistry *registry = EditorDocumentSurfaceRegistry::get_singleton();
	for (int64_t i = int64_t(builtin_surface_providers.size()) - 1; i >= 0; i--) {
		EditorDocumentSurfaceProvider *provider = builtin_surface_providers[i];
		if (registry) {
			registry->unregister_provider(provider->get_surface_id(), provider);
		}
		memdelete(provider);
	}
	builtin_surface_providers.clear();
}
