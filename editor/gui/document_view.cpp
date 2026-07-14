/**************************************************************************/
/*  document_view.cpp                                                     */
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

#include "document_view.h"

#include "core/object/callable_mp.h"
#include "editor/doc/editor_help.h"
#include "editor/docks/groups_dock.h"
#include "editor/docks/inspector_dock.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/docks/signals_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/level/level_editor.h"
#include "editor/level/level_editor_view.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/shader/shader_editor.h"
#include "editor/shader/shader_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/foldable_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/style_box_flat.h"

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
		// column — the "subtle gradient" pop without a baked texture (keeps the crisp corners).
		sb->set_border_width(SIDE_TOP, MAX(1, int(EDSCALE)));
		sb->set_border_color(p_bg.lightened(0.35));
		sb->set_shadow_color(Color(0, 0, 0, 0.16));
		sb->set_shadow_size(MAX(2, int(3 * EDSCALE)));
		sb->set_shadow_offset(Size2(0, MAX(1, int(EDSCALE))));
	}
	return sb;
}

void DocumentView::_add_accordion_section(VBoxContainer *p_column, Control *p_dock, const String &p_title, const StringName &p_icon, bool p_expanded) {
	// Build one GDStudio-style dock "card": a raised rounded header (leading icon + top-edge highlight)
	// over a recessed content panel. Colors are read from the editor theme at construction time — they
	// do NOT re-tint on a later theme change (accepted tradeoff until this moves to a DockSection theme
	// variation, which would also drop the per-pane stylebox rebuild).
	p_dock->set_v_size_flags(SIZE_EXPAND_FILL);
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
	// mouse-driven — drop the ring so no blue shows behind the rounded tops.
	section->add_theme_style_override(SNAME("focus"), Ref<StyleBox>(memnew(StyleBoxEmpty)));

	if (p_icon != StringName()) {
		Ref<Texture2D> icon = gui_base->get_theme_icon(p_icon, EditorStringName(EditorIcons));
		if (icon.is_valid()) {
			TextureRect *rect = memnew(TextureRect);
			rect->set_texture(icon);
			rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
			rect->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
			section->add_title_bar_control(rect);
		}
	}

	section->set_folded(!p_expanded);
	_on_section_folded(!p_expanded, section); // single source for the fold→expand-flag rule (no signal yet).
	section->connect("folding_changed", callable_mp(this, &DocumentView::_on_section_folded).bind(section));
	p_column->add_child(section);
}

void DocumentView::_on_section_folded(bool p_folded, FoldableContainer *p_section) {
	// Responsive accordion: expanded sections share the column proportionally; collapsing one shrinks
	// it to just its header, freeing that space for the rest (so e.g. folding Inspector lifts Signals
	// and Groups up). No fixed heights, so nothing stays locked or clips.
	p_section->set_v_size_flags(p_folded ? SIZE_FILL : SIZE_EXPAND_FILL);
}

DocumentView::DocumentView(EditorDocument *p_document) {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("margin_left", 0);
	add_theme_constant_override("margin_right", 0);
	add_theme_constant_override("margin_top", 0);
	add_theme_constant_override("margin_bottom", 0);

	// Bind the model side: this view presents p_document.
	doc_view = memnew(EditorDocumentView);
	doc_view->set_document(p_document);

	// G2 S7 (seam #8): a vertical stack so shared chrome (menu strip / find bar) can mount
	// above/below the surface while this tab is current.
	content_vbox = memnew(VBoxContainer);
	content_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
	content_vbox->set_v_size_flags(SIZE_EXPAND_FILL);
	content_vbox->add_theme_constant_override("separation", 0);
	add_child(content_vbox);

	// Host the editor surface for this document's kind, pointed at THIS document's isolated
	// world so the pane renders p_document's scene independently of the globally-active one.
	// 2D scenes get a CanvasItemEditorView (the real 2D editor view, minted per-document); 3D (and
	// mixed/unknown, until the ⑤b 2D/3D toggle) get a Node3DEditorView. Script/help documents get a
	// ScriptTextEditor/EditorHelp view. Symmetric factory calls per kind.
	const EditorDocument::Type type = p_document ? p_document->get_type() : EditorDocument::TYPE_UNKNOWN;
	switch (type) {
		case EditorDocument::TYPE_RESOURCE: {
			ResourceDocument *rd = static_cast<ResourceDocument *>(p_document);
			EditorData &ed = EditorNode::get_editor_data();
			inspector_dock = memnew(InspectorDock(ed, false));
			inspector_dock->set_bound_document(p_document);
			inspector_dock->edit_resource_document(rd->get_resource());
			editor_surface = inspector_dock;
		} break;
		case EditorDocument::TYPE_SCRIPT: {
			// G2 S4: the per-script VIEW is minted by the ScriptEditor SERVICES singleton, fully wired to
			// menus / find-in-files / save-all / debugger. The singleton stays; only the view is per-tab.
			ScriptDocument *sd = static_cast<ScriptDocument *>(p_document);
			if (ScriptEditor *se = ScriptEditor::get_singleton()) {
				editor_surface = se->create_editor_view(sd->get_script_resource());
			}
		} break;
		case EditorDocument::TYPE_SHADER: {
			// G-Shader: the shader editor widget (text code editor or visual node graph) is minted by
			// the ShaderEditorPlugin SERVICES singleton via the shader-language factory. The plugin
			// stays; only the widget is per-tab (released on PREDELETE, below).
			ShaderDocument *shd = static_cast<ShaderDocument *>(p_document);
			if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
				editor_surface = sep->create_editor_view(shd->get_shader_resource());
			}
		} break;
		case EditorDocument::TYPE_LEVEL: {
			// G-Level LE0: the singleton owns SERVICES/tool state only. It mints this pane's
			// camera/viewport/grid surface against the LevelDocument's explicit world.
			LevelDocument *ld = static_cast<LevelDocument *>(p_document);
			if (LevelEditor *level_editor = LevelEditor::get_singleton()) {
				editor_surface = level_editor->create_editor_view(ld);
			}
		} break;
		case EditorDocument::TYPE_HELP: {
			// G2 S6b: the view is minted by the ScriptEditor SERVICES singleton (wired to go_to_help
			// navigation + history, registered in the open-help registry). go_to_class needs the view
			// in the tree (theme + doc data), so defer it until after this DocumentView is parented.
			HelpDocument *hd = static_cast<HelpDocument *>(p_document);
			if (ScriptEditor *se = ScriptEditor::get_singleton()) {
				EditorHelp *help = se->create_help_view(hd->get_class_name());
				callable_mp(help, &EditorHelp::go_to_class).call_deferred(hd->get_class_name());
				editor_surface = help;
			}
		} break;
		case EditorDocument::TYPE_SCREEN_HOST: {
			// G2 S5.5: this view hosts the legacy main-screen stack ITSELF (seam #5). The stack is
			// EditorMainScreen's main_screen_vbox — never owned here; NOTIFICATION_PREDELETE parks it
			// back under the hidden holder so get_control() keeps returning the live vbox (D11).
			ScreenHostDocument *shd = static_cast<ScreenHostDocument *>(p_document);
			Control *stack = shd->get_screen_stack();
			if (stack) {
				if (Node *stack_parent = stack->get_parent()) {
					stack_parent->remove_child(stack); // Un-park (re-summon after a tab close).
				}
				editor_surface = stack;
			}
		} break;
		case EditorDocument::TYPE_SCENE_2D: {
			if (CanvasItemEditor *canvas_editor = CanvasItemEditor::get_singleton()) {
				editor_surface = canvas_editor->create_view_bound_to(p_document);
			}
		} break;
		default: {
			if (Node3DEditor *spatial = Node3DEditor::get_singleton()) {
				editor_surface = spatial->create_view_bound_to(p_document);
			}
		} break;
	}
	// G2 D7a: a scene document's tab is a COMPOSITE — its own Scene Tree dock to the left of the
	// editor surface, bound to this document. The dock is a bound instance (is_global=false, D5, so it
	// never claims the SceneTreeDock singleton) reading this doc's own root/selection/history via the
	// D4 resolvers. Non-scene documents (script/help/screen-host/resource) have no selection, so they
	// are left as a plain surface. The composite becomes `editor_surface` (the split), which the
	// generic parent/stretch below adds to content_vbox.
	if (editor_surface && p_document && p_document->get_selection()) {
		bound_scene_document = p_document;
		EditorData &ed = EditorNode::get_editor_data();

		// D7b: the per-pane dock column — a vertical accordion of FoldableContainer sections
		// (Scene Tree + Inspector expanded, Signals + Groups folded), on the RIGHT of the surface.
		VBoxContainer *dock_column = memnew(VBoxContainer);
		dock_column->set_custom_minimum_size(Size2(400 * EDSCALE, 0));
		dock_column->add_theme_constant_override("separation", 6 * EDSCALE); // gaps → sections read as stacked cards.

		// Each section is a styled FoldableContainer "card" built by _add_accordion_section; only the dock
		// construction differs. Scene Tree + Inspector start expanded, Signals + Groups collapsed.
		scene_tree_dock = memnew(SceneTreeDock(p_document->get_root(), p_document->get_selection(), ed, false));
		scene_tree_dock->set_bound_document(p_document);
		scene_tree_dock->set_edited_scene(p_document->get_root());
		_add_accordion_section(dock_column, scene_tree_dock, TTR("Scene Tree"), SNAME("PackedScene"), true);

		inspector_dock = memnew(InspectorDock(ed, false));
		inspector_dock->set_bound_document(p_document);
		scene_tree_dock->set_bound_inspector(inspector_dock);
		_add_accordion_section(dock_column, inspector_dock, TTR("Inspector"), SNAME("Object"), true);

		signals_dock = memnew(SignalsDock(false));
		_add_accordion_section(dock_column, signals_dock, TTR("Signals"), SNAME("Signals"), false);

		groups_dock = memnew(GroupsDock(false));
		_add_accordion_section(dock_column, groups_dock, TTR("Groups"), SNAME("Groups"), false);

		// SceneTreeDock owns Inspector selection timing so a pressed node can be dragged into a property
		// of the previously inspected node. This connection only keeps Signals and Groups synchronized.
		p_document->get_selection()->connect("selection_changed", callable_mp(this, &DocumentView::_bound_selection_changed));
		List<Node *> initial_nodes = p_document->get_selection()->get_top_selected_node_list();
		inspector_dock->edit_bound(initial_nodes.is_empty() ? nullptr : initial_nodes.front()->get());
		_bound_selection_changed();

		// Left side: [toolbar host | viewport]. M7.2a mounts the focused pane's 2D/3D toolbar into
		// toolbar_host; it sits empty (zero height) otherwise.
		toolbar_host = memnew(HBoxContainer);
		toolbar_host->set_h_size_flags(SIZE_EXPAND_FILL);
		if (LevelEditorView *level_view = Object::cast_to<LevelEditorView>(editor_surface)) {
			level_view->mount_top_strip(toolbar_host);
		}
		VBoxContainer *surface_vbox = memnew(VBoxContainer);
		surface_vbox->add_theme_constant_override("separation", 0);
		surface_vbox->set_h_size_flags(SIZE_EXPAND_FILL);
		surface_vbox->set_v_size_flags(SIZE_EXPAND_FILL);
		surface_vbox->add_child(toolbar_host);
		editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
		editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
		surface_vbox->add_child(editor_surface);

		// Compose: [toolbar|viewport] | dock column (docks on the right, per the design). The accordion
		// sizes responsively — expanded sections share the column via SIZE_EXPAND_FILL, collapsed ones
		// shrink to their header (see _add_accordion_section / _on_section_folded) and each section's
		// content scrolls internally, so no outer ScrollContainer is needed.
		HSplitContainer *scene_split = memnew(HSplitContainer);
		scene_split->add_child(surface_vbox);
		scene_split->add_child(dock_column);
		editor_surface = scene_split;
	}

	// Parent + stretch the minted surface identically regardless of kind.
	if (editor_surface) {
		content_vbox->add_child(editor_surface);
		editor_surface->set_h_size_flags(SIZE_EXPAND_FILL);
		editor_surface->set_v_size_flags(SIZE_EXPAND_FILL);
	}
}

void DocumentView::_bound_selection_changed() {
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
}

Control *DocumentView::get_chrome_host() const {
	return content_vbox;
}

void DocumentView::set_context_active(bool p_active) {
	if (context_active == p_active) {
		return;
	}
	context_active = p_active;
	if (doc_view) {
		doc_view->set_active(p_active);
	}
	if (inspector_dock) {
		inspector_dock->set_context_active(p_active);
	}
}

void DocumentView::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		// G2 D7a: re-point the embedded Scene Tree at its document's root now that we're in-tree. The
		// scene root can be assigned after a background (grab_focus=false) view is minted, so a refresh
		// here guarantees the tree shows the current tree rather than an empty one.
		if (scene_tree_dock && bound_scene_document) {
			scene_tree_dock->set_edited_scene(bound_scene_document->get_root());
		}
		return;
	}
	if (p_what != NOTIFICATION_PREDELETE) {
		return;
	}
	set_context_active(false);
	if (scene_tree_dock) {
		scene_tree_dock->set_bound_inspector(nullptr);
	}
	// PREDELETE dispatches derived-first, so this runs BEFORE Node's handler frees the children —
	// the last moment editor_surface is guaranteed alive (the destructor is too late: children are
	// already freed by then).
	// G2 S7: if the shared chrome is mounted under this view, park it home before we die.
	if (ScriptEditor *se = ScriptEditor::get_singleton()) {
		se->park_chrome_if_hosted_by(this);
	}
	// G2 M7.2a: same for the shared 2D/3D toolbar — it travels into the focused scene pane's
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
	if (!editor_surface) {
		return;
	}
	// G2 S4: if this view hosted a script surface, drop it from the ScriptEditor open-scripts
	// registry before it is freed (idempotent belt-and-suspenders with tree_exiting).
	if (ScriptEditorBase *seb = Object::cast_to<ScriptEditorBase>(editor_surface)) {
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->release_editor_view(seb);
		}
	}
	// G-Shader: same for a shader surface — park the traveling File menu (if hosted here) and drop
	// the plugin's tracking entry before Node frees this view's children.
	if (ShaderEditor *she = Object::cast_to<ShaderEditor>(editor_surface)) {
		if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
			sep->release_editor_view(she);
		}
	}
	// G2 S5.5: the screen-host view does not own the legacy main-screen stack — park it back under
	// EditorMainScreen's hidden holder so get_control() stays live (D11). If the holder is already
	// gone (whole-editor teardown, children die last-first), leave the stack to be freed with this
	// view, matching the stock lifetime where the vbox died with the main-screen tree.
	EditorDocument *doc = doc_view ? doc_view->get_document() : nullptr;
	if (doc && doc->get_type() == EditorDocument::TYPE_SCREEN_HOST && editor_surface->get_parent() == content_vbox) { // G2 S7: surfaces live under content_vbox now.
		ScreenHostDocument *shd = static_cast<ScreenHostDocument *>(doc);
		Control *park = Object::cast_to<Control>(ObjectDB::get_instance(shd->get_park_holder_id()));
		if (park) {
			content_vbox->remove_child(editor_surface);
			park->add_child(editor_surface);
			editor_surface = nullptr; // No longer ours; Node's PREDELETE must not free it.
		}
	}
}

DocumentView::~DocumentView() {
	// doc_view is a plain C++ object we own; the children (editor_surface) were already freed by
	// Node's PREDELETE — surface-detach work lives in _notification(NOTIFICATION_PREDELETE) above.
	if (doc_view) {
		memdelete(doc_view);
		doc_view = nullptr;
	}
}
