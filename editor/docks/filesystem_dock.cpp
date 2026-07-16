/**************************************************************************/
/*  filesystem_dock.cpp                                                   */
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

#include "filesystem_dock.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/templates/list.h"
#include "editor/docks/import_dock.h"
#include "editor/docks/resource_inspector_dock.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/dependency_editor.h"
#include "editor/file_system/editor_asset_description.h"
#include "editor/gui/create_dialog.h"
#include "editor/gui/directory_create_dialog.h"
#include "editor/gui/editor_dir_dialog.h"
#include "editor/gui/editor_simple_markdown.h"
#include "editor/import/3d/scene_import_settings.h"
#include "editor/inspector/editor_context_menu_plugin.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/inspector/editor_resource_tooltip_plugins.h"
#include "editor/plugins/editor_resource_conversion_plugin.h"
#include "editor/scene/editor_scene_tabs.h"
#include "editor/scene/scene_create_dialog.h"
#include "editor/settings/editor_command_palette.h"
#include "editor/settings/editor_feature_profile.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/editor_settings_dialog.h"
#include "editor/shader/shader_create_dialog.h"
#include "editor/themes/editor_scale.h"
#include "editor/themes/editor_theme_manager.h"
#include "scene/gui/box_container.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/progress_bar.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/packed_scene.h"
#include "servers/display/display_server.h"

namespace {

constexpr int DESCRIPTION_TREE_BUTTON_ID = 2001;
constexpr int MAX_DESCRIPTION_SIZE_BYTES = 32 * 1024;

struct ExploreCategoryIcon {
	const char *theme_icon;
	const char *display_name;
	bool tintable;
};

// Keep this list intentionally small and stable: values stored in project.godot are the theme_icon IDs.
static const ExploreCategoryIcon explore_category_icons[] = {
	{ "Folder", TTRC("Folder"), true },
	{ "PackedScene", TTRC("Packed Scene"), true },
	{ "AudioStream", TTRC("Audio Stream"), false },
	{ "WorldEnvironment", TTRC("World Environment"), false },
	{ "StandardMaterial3D", TTRC("Standard Material 3D"), false },
	{ "Texture2D", TTRC("Texture 2D"), true },
	{ "Mesh", TTRC("Mesh"), true },
	{ "Script", TTRC("Script"), true },
	{ "Tools", TTRC("Tools"), true },
	{ "Help", TTRC("Help"), true },
};

const ExploreCategoryIcon *get_explore_category_icon(const String &p_id) {
	for (const ExploreCategoryIcon &option : explore_category_icons) {
		if (p_id == option.theme_icon) {
			return &option;
		}
	}
	return &explore_category_icons[0];
}

} // namespace

Control *FileSystemTree::make_custom_tooltip(const String &p_text) const {
	if (p_text == TTR("View asset description")) {
		Label *label = memnew(Label);
		label->set_text(p_text);
		return label;
	}
	TreeItem *item = get_item_at_position(get_local_mouse_position());
	if (!item) {
		return nullptr;
	}
	return FileSystemDock::get_singleton()->create_tooltip_for_path(item->get_metadata(0));
}

Control *FileSystemList::make_custom_tooltip(const String &p_text) const {
	int idx = get_item_at_position(get_local_mouse_position(), true);
	if (idx == -1) {
		return nullptr;
	}
	const String action_icon_tooltip = get_item_action_icon_tooltip(idx);
	if (!action_icon_tooltip.is_empty() && p_text == action_icon_tooltip) {
		Label *label = memnew(Label);
		label->set_text(p_text);
		return label;
	}
	return FileSystemDock::get_singleton()->create_tooltip_for_path(get_item_metadata(idx));
}

void FileSystemList::_line_editor_submit(const String &p_text) {
	if (popup_edit_committed) {
		return; // Already processed by _text_editor_popup_modal_close
	}

	if (popup_editor->get_hide_reason() == Popup::HIDE_REASON_CANCELED) {
		return; // ESC pressed, app focus lost, or forced close from code.
	}

	popup_edit_committed = true; // End edit popup processing.
	popup_editor->hide();

	emit_signal(SNAME("item_edited"));
	queue_redraw();
}

bool FileSystemList::edit_selected() {
	ERR_FAIL_COND_V_MSG(!is_anything_selected(), false, "No item selected.");
	int s = get_current();
	ERR_FAIL_COND_V_MSG(s < 0, false, "No current item selected.");
	ensure_current_is_visible();

	Rect2 rect;
	Rect2 popup_rect;
	Vector2 ofs;

	Vector2 icon_size = get_fixed_icon_size() * get_icon_scale();

	// Handles the different icon modes (TOP/LEFT).
	switch (get_icon_mode()) {
		case ItemList::ICON_MODE_LEFT:
			rect = get_item_rect(s, true);
			if (get_v_scroll_bar()->is_visible()) {
				rect.position.y -= get_v_scroll_bar()->get_value();
			}
			if (get_h_scroll_bar()->is_visible()) {
				rect.position.x -= get_h_scroll_bar()->get_value();
			}
			ofs = Vector2(0, Math::floor((MAX(line_editor->get_minimum_size().height, rect.size.height) - rect.size.height) / 2));
			popup_rect.position = rect.position - ofs;
			popup_rect.size = rect.size;

			// Adjust for icon position and size.
			popup_rect.size.x -= MAX(theme_cache.h_separation, 0) / 2 + icon_size.x;
			popup_rect.position.x += MAX(theme_cache.h_separation, 0) / 2 + icon_size.x;
			break;
		case ItemList::ICON_MODE_TOP:
			rect = get_item_rect(s, false);
			if (get_v_scroll_bar()->is_visible()) {
				rect.position.y -= get_v_scroll_bar()->get_value();
			}
			if (get_h_scroll_bar()->is_visible()) {
				rect.position.x -= get_h_scroll_bar()->get_value();
			}
			popup_rect.position = rect.position;
			popup_rect.size = rect.size;

			// Adjust for icon position and size.
			popup_rect.size.y -= MAX(theme_cache.v_separation, 0) / 2 + theme_cache.icon_margin + icon_size.y;
			popup_rect.position.y += MAX(theme_cache.v_separation, 0) / 2 + theme_cache.icon_margin + icon_size.y;
			break;
	}
	if (is_layout_rtl()) {
		popup_rect.position.x = get_size().width - popup_rect.position.x - popup_rect.size.x;
	}
	popup_rect.position += get_screen_position();

	popup_editor->set_position(popup_rect.position);
	popup_editor->set_size(popup_rect.size);

	String name = get_item_text(s);
	line_editor->set_text(name);
	line_editor->select(0, name.rfind_char('.'));

	popup_edit_committed = false; // Start edit popup processing.
	popup_editor->popup();
	popup_editor->child_controls_changed();
	line_editor->grab_focus();
	return true;
}

String FileSystemList::get_edit_text() {
	return line_editor->get_text();
}

void FileSystemList::_text_editor_popup_modal_close() {
	if (popup_edit_committed) {
		return; // Already processed by _text_editor_popup_modal_close
	}

	if (popup_editor->get_hide_reason() == Popup::HIDE_REASON_CANCELED) {
		return; // ESC pressed, app focus lost, or forced close from code.
	}

	_line_editor_submit(line_editor->get_text());
}

void FileSystemList::_bind_methods() {
	ADD_SIGNAL(MethodInfo("item_edited"));
}

FileSystemList::FileSystemList() {
	set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);

	popup_editor = memnew(Popup);
	add_child(popup_editor);

	popup_editor_vb = memnew(VBoxContainer);
	popup_editor_vb->add_theme_constant_override("separation", 0);
	popup_editor_vb->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	popup_editor->add_child(popup_editor_vb);

	line_editor = memnew(LineEdit);
	line_editor->set_v_size_flags(SIZE_EXPAND_FILL);
	popup_editor_vb->add_child(line_editor);
	line_editor->connect(SceneStringName(text_submitted), callable_mp(this, &FileSystemList::_line_editor_submit));
	popup_editor->connect("popup_hide", callable_mp(this, &FileSystemList::_text_editor_popup_modal_close));
}

Ref<Texture2D> FileSystemDock::_get_tree_item_icon(bool p_is_valid, const String &p_file_type, const String &p_icon_path) {
	if (!p_icon_path.is_empty()) {
		Ref<Texture2D> icon = ResourceLoader::load(p_icon_path);
		if (icon.is_valid()) {
			return icon;
		}
	}

	if (!p_is_valid) {
		return get_editor_theme_icon(SNAME("ImportFail"));
	} else if (has_theme_icon(p_file_type, EditorStringName(EditorIcons))) {
		return get_editor_theme_icon(p_file_type);
	} else {
		return get_editor_theme_icon(SNAME("File"));
	}
}

void FileSystemDock::_add_category_root_badge(TreeItem *p_item, const String &p_color_key) {
	const ExploreCategoryIcon *category_icon = get_explore_category_icon(_get_color_icon_id(p_color_key));
	p_item->add_button(0, get_editor_theme_icon(category_icon->theme_icon), -1, true, vformat(TTR("Category root: %s"), _get_color_label(p_color_key)));
	if (category_icon->tintable) {
		const Color category_color = folder_colors[p_color_key];
		p_item->set_button_color(0, p_item->get_button_count(0) - 1, editor_is_dark_icon_and_font ? category_color : category_color * ITEM_COLOR_SCALE);
	}
}

void FileSystemDock::_create_tree(TreeItem *p_parent, EditorFileSystemDirectory *p_dir, const Vector<String> &p_uncollapsed_paths, const Vector<String> &p_selected_paths, const String &p_inherited_color) {
	// Create a tree item for the subdirectory.
	String dname = p_dir->get_name();
	String lpath = p_dir->get_path();
	if (_is_color_collection_active() && !category_visible_paths.has(lpath)) {
		return;
	}

	TreeItem *subdirectory_item = tree->create_item(p_parent);

	if (dname.is_empty()) {
		dname = "res://";
		resources_item = subdirectory_item;
	}

	// Set custom folder color (if applicable).
	const String explicit_color_key = assigned_folder_colors.get(lpath, String());
	const bool has_custom_color = folder_colors.has(explicit_color_key);
	const String effective_color_key = has_custom_color ? explicit_color_key : p_inherited_color;
	Color custom_color = has_custom_color ? folder_colors[explicit_color_key] : Color();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);

	if (has_custom_color) {
		subdirectory_item->set_icon_modulate(0, editor_is_dark_icon_and_font ? custom_color : custom_color * ITEM_COLOR_SCALE);
		subdirectory_item->set_custom_bg_color(0, Color(custom_color, editor_is_dark_icon_and_font ? ITEM_ALPHA_MIN : ITEM_ALPHA_MAX));
	} else {
		TreeItem *parent = subdirectory_item->get_parent();
		if (parent) {
			Color parent_bg_color = parent->get_custom_bg_color(0);
			if (parent_bg_color != Color()) {
				const String parent_color_key = assigned_folder_colors.get(parent->get_metadata(0), String());
				bool parent_has_custom_color = folder_colors.has(parent_color_key);
				subdirectory_item->set_custom_bg_color(0, parent_has_custom_color ? parent_bg_color.darkened(ITEM_BG_DARK_SCALE) : parent_bg_color);
				subdirectory_item->set_icon_modulate(0, parent->get_icon_modulate(0));
			} else {
				subdirectory_item->set_icon_modulate(0, get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog")));
			}
		}
	}

	subdirectory_item->set_text(0, dname);
	subdirectory_item->set_structured_text_bidi_override(0, TextServer::STRUCTURED_TEXT_FILE);
	subdirectory_item->set_icon(0, get_editor_theme_icon(SNAME("Folder")));
	if (da->is_link(lpath)) {
		subdirectory_item->set_icon_overlay(0, get_editor_theme_icon(SNAME("LinkOverlay")));
		subdirectory_item->set_tooltip_text(0, vformat(TTR("Link to: %s"), da->read_link(lpath)));
	}
	subdirectory_item->set_selectable(0, true);
	subdirectory_item->set_metadata(0, lpath);
	if (has_custom_color) {
		_add_category_root_badge(subdirectory_item, explicit_color_key);
	}
	folder_map[lpath] = subdirectory_item;

	const bool category_scope_selected = _is_color_collection_active() && !category_scope_path.is_empty() && category_scope_path == lpath;
	const bool normal_selected = !_is_color_collection_active() && (current_path == lpath || p_selected_paths.has(lpath) || ((display_mode != DISPLAY_MODE_TREE_ONLY) && (current_path.get_base_dir() == lpath)));
	if (category_scope_selected || normal_selected) {
		subdirectory_item->select(0, category_scope_selected || current_path == lpath);
	}

	subdirectory_item->set_collapsed(_is_color_collection_active() ? false : !p_uncollapsed_paths.has(lpath));

	// Create items for all subdirectories.
	bool reversed = file_sort == FileSortOption::FILE_SORT_NAME_REVERSE;
	for (int i = reversed ? p_dir->get_subdir_count() - 1 : 0;
			reversed ? i >= 0 : i < p_dir->get_subdir_count();
			reversed ? i-- : i++) {
		_create_tree(subdirectory_item, p_dir->get_subdir(i), p_uncollapsed_paths, p_selected_paths, effective_color_key);
	}

	// Create all items for the files in the subdirectory.
	if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		// Build the list of the files to display.
		List<FileInfo> file_list;
		const bool include_directory_files = !_is_color_collection_active() || active_color_filter.has(effective_color_key);
		for (int i = 0; include_directory_files && i < p_dir->get_file_count(); i++) {
			if (!searched_tokens.is_empty() && !_matches_all_search_tokens(p_dir->get_file(i))) {
				continue;
			}
			String file_type = p_dir->get_file_type(i);
			if (_is_file_type_disabled_by_feature_profile(file_type)) {
				// If type is disabled, file won't be displayed.
				continue;
			}

			FileInfo file_info;
			file_info.name = p_dir->get_file(i);
			file_info.type = p_dir->get_file_type(i);
			file_info.icon_path = p_dir->get_file_icon_path(i);
			file_info.import_broken = !p_dir->get_file_import_is_valid(i);
			file_info.modified_time = p_dir->get_file_modified_time(i);

			file_list.push_back(file_info);
		}

		// Sort the file list if needed.
		sort_file_info_list(file_list, file_sort);

		// Build the tree.
		const int icon_size = get_theme_constant(SNAME("class_icon_size"), EditorStringName(Editor));

		for (const FileInfo &file_info : file_list) {
			TreeItem *file_item = tree->create_item(subdirectory_item);
			const String file_metadata = lpath.path_join(file_info.name);
			file_item->set_text(0, file_info.name);
			file_item->set_structured_text_bidi_override(0, TextServer::STRUCTURED_TEXT_FILE);
			file_item->set_icon(0, _get_tree_item_icon(!file_info.import_broken, file_info.type, file_info.icon_path));
			if (da->is_link(file_metadata)) {
				file_item->set_icon_overlay(0, get_editor_theme_icon(SNAME("LinkOverlay")));
				// TRANSLATORS: This is a tooltip for a file that is a symbolic link to another file.
				file_item->set_tooltip_text(0, vformat(TTR("Link to: %s"), da->read_link(file_metadata)));
			}
			file_item->set_icon_max_width(0, icon_size);
			Color parent_bg_color = subdirectory_item->get_custom_bg_color(0);
			if (has_custom_color) {
				file_item->set_custom_bg_color(0, parent_bg_color.darkened(ITEM_BG_DARK_SCALE));
			} else if (parent_bg_color != Color()) {
				file_item->set_custom_bg_color(0, parent_bg_color);
			}
			file_item->set_metadata(0, file_metadata);
			if (_asset_has_description(file_metadata)) {
				_set_tree_description_indicator(file_item, true);
			}
			file_item->set_accept_children(false);
			if (current_path == file_metadata || p_selected_paths.has(file_metadata)) {
				file_item->select(0, current_path == file_metadata);
			}
			if (main_scene_path == file_metadata) {
				file_item->set_custom_color(0, get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
			}
			EditorResourcePreview::get_singleton()->queue_resource_preview(file_metadata, callable_mp(this, &FileSystemDock::_tree_thumbnail_done).bind(tree_update_id, file_item->get_instance_id()));
		}
	} else if (!_is_color_collection_active() && lpath.get_base_dir() == current_path.get_base_dir()) {
		subdirectory_item->select(0);
	}
}

Vector<String> FileSystemDock::get_uncollapsed_paths() const {
	Vector<String> uncollapsed_paths;
	TreeItem *root = tree->get_root();
	if (root) {
		if (!favorites_item->is_collapsed()) {
			uncollapsed_paths.push_back(favorites_item->get_metadata(0));
		}

		// BFS to find all uncollapsed paths of the resource directory.
		TreeItem *res_subtree = root->get_first_child()->get_next();
		if (res_subtree) {
			List<TreeItem *> queue;
			queue.push_back(res_subtree);

			while (!queue.is_empty()) {
				TreeItem *ti = queue.back()->get();
				queue.pop_back();
				if (!ti->is_collapsed() && ti->get_child_count() > 0) {
					Variant path = ti->get_metadata(0);
					if (path) {
						uncollapsed_paths.push_back(path);
					}
				}
				for (int i = 0; i < ti->get_child_count(); i++) {
					queue.push_back(ti->get_child(i));
				}
			}
		}
	}
	return uncollapsed_paths;
}

void FileSystemDock::_update_tree(const Vector<String> &p_uncollapsed_paths, bool p_uncollapse_root, bool p_scroll_to_selected, const Vector<String> &p_override_selection) {
	const Vector<String> previous_selection = p_override_selection.is_empty() ? _tree_get_selected(false) : p_override_selection;
	if (_is_color_collection_active()) {
		_build_category_visible_paths();
	}

	// Recreate the tree.
	tree->clear();
	tree_update_id++;
	updating_tree = true;
	TreeItem *root = tree->create_item();
	root->set_accept_children(false);
	folder_map.clear();

	// Handles the favorites.
	favorites_item = tree->create_item(root);
	favorites_item->set_icon(0, get_editor_theme_icon(SNAME("Favorites")));
	favorites_item->set_text(0, TTRC("Favorites"));
	favorites_item->set_auto_translate_mode(0, AUTO_TRANSLATE_MODE_ALWAYS);
	favorites_item->set_metadata(0, "Favorites");
	favorites_item->set_collapsed(!p_uncollapsed_paths.has("Favorites"));
	favorites_item->set_visible(!_is_color_collection_active());

	Vector<String> favorite_paths = EditorSettings::get_singleton()->get_favorites();

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	bool fav_changed = false;
	for (int i = favorite_paths.size() - 1; i >= 0; i--) {
		if (da->dir_exists(favorite_paths[i]) || da->file_exists(favorite_paths[i])) {
			continue;
		}
		favorite_paths.remove_at(i);
		fav_changed = true;
	}
	if (fav_changed) {
		EditorSettings::get_singleton()->set_favorites(favorite_paths);
		// Setting favorites causes the tree to update, so continuing is redundant.
		return;
	}

	Ref<Texture2D> folder_icon = get_editor_theme_icon(SNAME("Folder"));
	const Color default_folder_color = get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog"));

	const int icon_size = get_theme_constant(SNAME("class_icon_size"), EditorStringName(Editor));
	for (const String &favorite : favorite_paths) {
		if (_is_color_collection_active()) {
			break;
		}
		if (!favorite.begins_with("res://")) {
			continue;
		}

		String text;
		Ref<Texture2D> icon;
		Color color;
		if (favorite == "res://") {
			text = "/";
			icon = folder_icon;
			color = default_folder_color;
		} else if (favorite.ends_with("/")) {
			text = favorite.substr(0, favorite.length() - 1).get_file();
			icon = folder_icon;
			color = FileSystemDock::get_dir_icon_color(favorite, default_folder_color);
		} else {
			text = favorite.get_file();
			int index;
			EditorFileSystemDirectory *dir = EditorFileSystem::get_singleton()->find_file(favorite, &index);
			if (dir) {
				icon = _get_tree_item_icon(dir->get_file_import_is_valid(index), dir->get_file_type(index), dir->get_file_icon_path(index));
			} else {
				icon = get_editor_theme_icon(SNAME("File"));
			}
			color = Color(1, 1, 1);
		}

		TreeItem *ti = tree->create_item(favorites_item);
		ti->set_text(0, text);
		ti->set_icon(0, icon);
		ti->set_icon_modulate(0, color);
		ti->set_icon_max_width(0, icon_size);
		ti->set_tooltip_text(0, favorite);
		ti->set_selectable(0, true);
		ti->set_metadata(0, favorite);
		ti->set_accept_children(false);
		if (!favorite.ends_with("/") && _asset_has_description(favorite)) {
			_set_tree_description_indicator(ti, true);
		}
		if (favorite.ends_with("/")) {
			const String explicit_color_key = assigned_folder_colors.get(favorite, String());
			if (folder_colors.has(explicit_color_key)) {
				_add_category_root_badge(ti, explicit_color_key);
			}
		}

		if (favorite == main_scene_path) {
			ti->set_custom_color(0, get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
		}

		if (!favorite.ends_with("/")) {
			EditorResourcePreview::get_singleton()->queue_resource_preview(favorite, callable_mp(this, &FileSystemDock::_tree_thumbnail_done).bind(tree_update_id, ti->get_instance_id()));
		}
	}

	Vector<String> uncollapsed_paths = p_uncollapsed_paths;
	if (p_uncollapse_root && !uncollapsed_paths.has("res://")) {
		uncollapsed_paths.push_back("res://");
	}

	// Create the remaining of the tree.
	_create_tree(root, EditorFileSystem::get_singleton()->get_filesystem(), uncollapsed_paths, previous_selection);
	if (!searched_tokens.is_empty() && !_is_color_collection_active()) {
		_update_filtered_items();
	}
	if (_is_color_collection_active() && display_mode == DISPLAY_MODE_TREE_ONLY) {
		category_collection_stats = CategoryCollectionStats();
		_gather_color_collection(EditorFileSystem::get_singleton()->get_filesystem(), "res://", String(), nullptr, &category_collection_stats);
		_update_category_empty_state();
	}

	if (p_scroll_to_selected) {
		tree->ensure_cursor_is_visible();
	}

	updating_tree = false;
}

void FileSystemDock::set_display_mode(DisplayMode p_display_mode) {
	display_mode = p_display_mode;
	_update_display_mode(false);
}

void FileSystemDock::_update_display_mode(bool p_force) {
	if (!p_force && old_display_mode == display_mode) {
		return;
	}

	// Preserve the selection when switching modes.
	Vector<String> selected_paths;
	if (old_display_mode != display_mode && old_display_mode != DISPLAY_MODE_VSPLIT) {
		selected_paths = get_selected_paths();
	}

	switch (display_mode) {
		case DISPLAY_MODE_TREE_ONLY: {
			button_toggle_display_mode->set_button_icon(get_editor_theme_icon(SNAME("Panels1")));
			tree->show();
			tree->set_v_size_flags(SIZE_EXPAND_FILL);
			tree->set_theme_type_variation("");
			if (horizontal) {
				toolbar2_hbc->hide();

				tree->set_scroll_hint_mode(touches_bottom ? Tree::SCROLL_HINT_MODE_TOP : Tree::SCROLL_HINT_MODE_BOTH);
				tree_mc->set_theme_type_variation("NoBorderHorizontal");
			} else {
				toolbar2_hbc->show();

				tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_TOP);
				tree_mc->set_theme_type_variation("NoBorderHorizontalBottom");
			}
			button_file_list_display_mode->hide();

			_update_tree(get_uncollapsed_paths(), false, true, selected_paths);
			file_list_vb->hide();
		} break;

		case DISPLAY_MODE_HSPLIT:
		case DISPLAY_MODE_VSPLIT: {
			const bool is_vertical = display_mode == DISPLAY_MODE_VSPLIT;
			split_box->set_vertical(is_vertical);

			const int actual_offset = is_vertical ? split_box_offset_v : split_box_offset_h;
			split_box->set_split_offset(actual_offset);
			const StringName icon = is_vertical ? SNAME("Panels2") : SNAME("Panels2Alt");
			button_toggle_display_mode->set_button_icon(get_editor_theme_icon(icon));

			tree->show();
			tree->set_v_size_flags(SIZE_EXPAND_FILL);
			tree->set_theme_type_variation("TreeSecondary");
			tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_DISABLED);
			tree_mc->set_theme_type_variation("");

			files->set_theme_type_variation("ItemListSecondary");
			files->set_scroll_hint_mode(ItemList::SCROLL_HINT_MODE_DISABLED);
			files_mc->set_theme_type_variation("");

			toolbar2_hbc->hide();
			button_file_list_display_mode->show();
			file_list_vb->show();

			if (old_display_mode == DISPLAY_MODE_TREE_ONLY) {
				// Properly allocate the selections between the views.
				Vector<String> selected_files;
				for (int i = 0; i < selected_paths.size(); i++) {
					const String &path = selected_paths[i];
					if (!path.ends_with("/")) {
						selected_files.append(path);
						selected_paths.remove_at(i);
						i--;
					}
				}

				_update_tree(get_uncollapsed_paths(), false, true, selected_paths);
				_update_file_list(!selected_files.is_empty(), selected_files);
			} else {
				tree->ensure_cursor_is_visible();

				// Always update to avoid broken icons, as previous updates
				// could have happened before the dock was inside the tree.
				update_all();
			}
		} break;
	}

	tree_mc->show();
	_update_category_empty_state();

	old_display_mode = display_mode;
}

void FileSystemDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			EditorFeatureProfileManager::get_singleton()->connect("current_feature_profile_changed", callable_mp(this, &FileSystemDock::_feature_profile_changed));
			EditorFileSystem::get_singleton()->connect("filesystem_changed", callable_mp(this, &FileSystemDock::_fs_changed));
			EditorResourcePreview::get_singleton()->connect("preview_invalidated", callable_mp(this, &FileSystemDock::_preview_invalidated));

			button_file_list_display_mode->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_toggle_file_display));
			files->connect("item_activated", callable_mp(this, &FileSystemDock::_file_list_activate_file));
			button_hist_next->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_fw_history));
			button_hist_prev->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_bw_history));
			file_list_popup->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_file_list_rmb_option));
			tree_popup->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_tree_rmb_option));
			current_path_line_edit->connect(SceneStringName(text_submitted), callable_mp(this, &FileSystemDock::_navigate_to_path).bind(false, true));

			always_show_folders = bool(EDITOR_GET("docks/filesystem/always_show_folders"));
			thumbnail_size_setting = EDITOR_GET("docks/filesystem/thumbnail_size");

			set_file_list_display_mode(FileSystemDock::FILE_LIST_DISPLAY_LIST);

			_rebuild_category_rail();
			_update_responsive_layout();
			_update_display_mode();

			if (EditorFileSystem::get_singleton()->is_scanning()) {
				_set_scanning_mode();
			} else {
				_update_tree(Vector<String>(), true);
			}
		} break;

		case NOTIFICATION_RESIZED: {
			_update_responsive_layout();
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_queue_visible_scene_previews_update();
		} break;

		case NOTIFICATION_PROCESS: {
			if (EditorFileSystem::get_singleton()->is_scanning()) {
				scanning_progress->set_value(EditorFileSystem::get_singleton()->get_scanning_progress() * 100.0f);
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (details_update_waiting_for_mouse_release && !filesystem_drag_preserves_details && !Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
				details_update_waiting_for_mouse_release = false;
				set_process_internal(false);
				_update_import_dock();
			}
		} break;

		case NOTIFICATION_DRAG_BEGIN: {
			Dictionary dd = get_viewport()->gui_get_drag_data();
			if (dd.has("type")) {
				const String drag_type = dd["type"];
				Object *drag_from_object = dd.get("from", Variant());
				Control *drag_from = Object::cast_to<Control>(drag_from_object);
				if ((drag_type == "files" || drag_type == "files_and_dirs") && (drag_from == tree || drag_from == files)) {
					// A FileSystem drag may target a Resource field in the adjacent Inspector. Keep the
					// pre-drag detail context alive instead of replacing it with the dragged source.
					filesystem_drag_preserves_details = true;
				}

				if (!tree->is_visible_in_tree()) {
					break;
				}
				if (dd.has("favorite")) {
					if ((String(dd["favorite"]) == "all")) {
						tree->set_drop_mode_flags(Tree::DROP_MODE_INBETWEEN);
					}
				} else if (drag_type == "files" || drag_type == "files_and_dirs") {
					tree->set_drop_mode_flags(Tree::DROP_MODE_ON_ITEM | Tree::DROP_MODE_INBETWEEN);
				} else if (drag_type == "nodes" || drag_type == "resource") {
					holding_branch = true;
					TreeItem *item = tree->get_next_selected(tree->get_root());
					while (item) {
						tree_items_selected_on_drag_begin.push_back(item);
						item = tree->get_next_selected(item);
					}
					list_items_selected_on_drag_begin = files->get_selected_items();
				}
			}
		} break;

		case NOTIFICATION_DRAG_END: {
			tree->set_drop_mode_flags(0);

			if (filesystem_drag_preserves_details) {
				filesystem_drag_preserves_details = false;
				details_update_waiting_for_mouse_release = false;
				import_dock_needs_update = false;
				set_process_internal(false);
			}

			if (holding_branch) {
				holding_branch = false;
				_reselect_items_selected_on_drag_begin(true);
			}
		} break;

		case NOTIFICATION_TRANSLATION_CHANGED:
		case NOTIFICATION_LAYOUT_DIRECTION_CHANGED:
		case NOTIFICATION_THEME_CHANGED: {
			_update_display_mode(true);

			StringName mode_icon = "Panels1";
			if (display_mode == DISPLAY_MODE_VSPLIT) {
				mode_icon = "Panels2";
			} else if (display_mode == DISPLAY_MODE_HSPLIT) {
				mode_icon = "Panels2Alt";
			}
			button_toggle_display_mode->set_button_icon(get_editor_theme_icon(mode_icon));

			if (file_list_display_mode == FILE_LIST_DISPLAY_LIST) {
				button_file_list_display_mode->set_button_icon(get_editor_theme_icon(SNAME("FileThumbnail")));
			} else {
				button_file_list_display_mode->set_button_icon(get_editor_theme_icon(SNAME("FileList")));
			}

			tree_search_box->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			tree_button_sort->set_button_icon(get_editor_theme_icon(SNAME("Sort")));

			file_list_search_box->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			file_list_button_sort->set_button_icon(get_editor_theme_icon(SNAME("Sort")));
			description_text_edit->add_theme_font_override(SceneStringName(font), get_theme_font(SNAME("source"), EditorStringName(EditorFonts)));
			description_text_edit->add_theme_font_size_override(SceneStringName(font_size), get_theme_font_size(SNAME("source_size"), EditorStringName(EditorFonts)));

			_rebuild_category_rail();
			for (const KeyValue<String, MenuButton *> &E : color_icon_buttons) {
				_update_color_icon_button(E.key);
			}

			if (is_layout_rtl()) {
				button_hist_next->set_button_icon(get_editor_theme_icon(SNAME("Back")));
				button_hist_prev->set_button_icon(get_editor_theme_icon(SNAME("Forward")));
			} else {
				button_hist_next->set_button_icon(get_editor_theme_icon(SNAME("Forward")));
				button_hist_prev->set_button_icon(get_editor_theme_icon(SNAME("Back")));
			}

			overwrite_dialog_scroll->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SceneStringName(panel), "Tree"));
			_refresh_all_description_indicators();
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			// Update editor dark theme & always show folders states from editor settings, redraw if needed.
			bool do_redraw = false;

			bool new_editor_is_dark_icon_and_font = EditorThemeManager::is_dark_icon_and_font();
			if (new_editor_is_dark_icon_and_font != editor_is_dark_icon_and_font) {
				editor_is_dark_icon_and_font = new_editor_is_dark_icon_and_font;
				do_redraw = true;
			}

			bool new_always_show_folders = bool(EDITOR_GET("docks/filesystem/always_show_folders"));
			if (new_always_show_folders != always_show_folders) {
				always_show_folders = new_always_show_folders;
				do_redraw = true;
			}

			int new_thumbnail_size_setting = EDITOR_GET("docks/filesystem/thumbnail_size");
			if (new_thumbnail_size_setting != thumbnail_size_setting) {
				thumbnail_size_setting = new_thumbnail_size_setting;
				do_redraw = true;
			}

			if (do_redraw) {
				update_all();
			}

			if (EditorThemeManager::is_generated_theme_outdated()) {
				// Change full tree mode.
				_update_display_mode();
			}
		} break;
	}
}

void FileSystemDock::_tree_multi_selected(Object *p_item, int p_column, bool p_selected) {
	// Update the import dock.
	import_dock_needs_update = true;
	callable_mp(this, &FileSystemDock::_update_import_dock).call_deferred();

	// Return if we don't select something new.
	if (!p_selected) {
		return;
	}

	// Tree item selected.
	TreeItem *selected = tree->get_selected();
	if (!selected) {
		return;
	}
	const String selected_path = selected->get_metadata(0);
	if (_is_color_collection_active() && !updating_tree) {
		if (selected_path.ends_with("/")) {
			category_scope_path = selected_path;
		}
		current_path = selected_path;
		_set_current_path_line_edit_text(current_path);
		if (display_mode != DISPLAY_MODE_TREE_ONLY) {
			_update_file_list(true);
		}
		return;
	}

	if (selected->get_parent() == favorites_item && !String(selected->get_metadata(0)).ends_with("/")) {
		// Go to the favorites if we click in the favorites and the path has changed.
		current_path = "Favorites";
	} else {
		current_path = selected_path;
		// Note: the "Favorites" item also leads to this path.
	}

	// Display the current path.
	_set_current_path_line_edit_text(current_path);
	_push_to_history();

	// Update the file list.
	if (!updating_tree && display_mode != DISPLAY_MODE_TREE_ONLY) {
		_update_file_list(true);
	}
}

Vector<String> FileSystemDock::get_selected_paths() const {
	Vector<String> selected_tree = _tree_get_selected(false);
	// Use the old mode to help preserve selection between modes.
	// That variable also gets updated shortly after, so it shouldn't cause issues.
	if (old_display_mode == DISPLAY_MODE_TREE_ONLY) {
		return _tree_get_selected(false);
	}

	Vector<String> selected_files = _file_list_get_selected();
	for (const String &file : selected_files) {
		if (!selected_tree.has(file)) {
			selected_tree.append(file);
		}
	}

	return selected_tree;
}

String FileSystemDock::get_current_path() const {
	return current_path;
}

String FileSystemDock::get_current_directory() const {
	if (current_path.ends_with("/")) {
		return current_path;
	} else {
		return current_path.get_base_dir();
	}
}

void FileSystemDock::_set_current_path_line_edit_text(const String &p_path) {
	if (p_path == "Favorites") {
		current_path_line_edit->set_text(TTR("Favorites"));
	} else {
		current_path_line_edit->set_text(p_path);
	}
}

void FileSystemDock::_navigate_to_path(const String &p_path, bool p_select_in_favorites, bool p_grab_focus) {
	String target_path = p_path;
	bool is_directory = false;

	if (p_path.is_empty()) {
		target_path = "res://";
		is_directory = true;
	} else if (p_path != "Favorites") {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		if (da->dir_exists(p_path)) {
			is_directory = true;
			if (!p_path.ends_with("/")) {
				target_path += "/";
			}
		} else if (!da->file_exists(p_path)) {
			ERR_FAIL_MSG(vformat("Cannot navigate to '%s' as it has not been found in the file system!", p_path));
		}
	}

	current_path = target_path;
	_set_current_path_line_edit_text(current_path);
	_push_to_history();

	String base_dir_path = target_path.get_base_dir();
	if (base_dir_path != "res://") {
		base_dir_path += "/";
	}

	TreeItem **directory_ptr = folder_map.getptr(base_dir_path);
	if (!directory_ptr) {
		return;
	}

	// Unfold all folders along the path.
	TreeItem *ti = *directory_ptr;
	while (ti) {
		ti->set_collapsed(false);
		ti = ti->get_parent();
	}

	// Select the file or directory in the tree.
	tree->deselect_all();
	if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		// Either search for 'folder/' or '/file.ext'.
		const String file_name = is_directory ? target_path.trim_suffix("/").get_file() + "/" : "/" + target_path.get_file();
		TreeItem *item = is_directory ? *directory_ptr : (*directory_ptr)->get_first_child();
		while (item) {
			if (item->get_metadata(0).operator String().ends_with(file_name)) {
				item->select(0);
				break;
			}
			item = item->get_next();
		}
		if (p_grab_focus) {
			tree->grab_focus(true);
		}
	} else {
		(*directory_ptr)->select(0);
		_update_file_list(false);
		if (p_grab_focus) {
			files->grab_focus(true);
		}
	}
	tree->ensure_cursor_is_visible();
}

bool FileSystemDock::_update_filtered_items(TreeItem *p_tree_item) {
	TreeItem *item = p_tree_item;
	if (!item) {
		item = tree->get_root();
	}
	ERR_FAIL_NULL_V(item, false);

	bool keep_visible = false;
	for (TreeItem *child = item->get_first_child(); child; child = child->get_next()) {
		keep_visible = _update_filtered_items(child) || keep_visible;
	}

	if (searched_tokens.is_empty()) {
		item->set_visible(true);
		// Always uncollapse root (the hidden item above res:// and favorites).
		item->set_collapsed(item != tree->get_root() && !uncollapsed_paths_before_search.has(item->get_metadata(0)));
		return true;
	}

	if (keep_visible) {
		item->set_collapsed(false);
	} else {
		// res:// and favorites are always visible.
		keep_visible = item == resources_item || item == favorites_item;
		keep_visible = keep_visible || _matches_all_search_tokens(item->get_text(0));
	}
	item->set_visible(keep_visible);
	return keep_visible;
}

void FileSystemDock::navigate_to_path(const String &p_path) {
	if (_is_color_collection_active()) {
		active_color_filter.clear();
		_end_category_filter();
		_rebuild_category_rail();
		_update_color_filter_view();
	}
	file_list_search_box->clear();
	// G4: FileSystem is hosted by the workspace drawer and intentionally isn't registered with
	// EditorDockManager. Asking the manager to focus it reports an "unknown dock" error; reveal its
	// actual host instead.
	EditorNode::get_singleton()->open_file_drawer();
	_navigate_to_path(p_path, false, is_visible_in_tree());

	import_dock_needs_update = true;
	// Programmatic navigation can rebuild the split-mode file list while Tree/ItemList selection
	// notifications are still settling. Match interactive selection and update its detail panels once.
	callable_mp(this, &FileSystemDock::_update_import_dock).call_deferred();
}

Error FileSystemDock::open_scene_in_level_editor(const String &p_path) {
	ERR_FAIL_COND_V_MSG(
			p_path.get_extension().to_lower() != "tscn" || ResourceLoader::get_resource_type(p_path) != "PackedScene",
			ERR_INVALID_PARAMETER,
			"Only .tscn scene files can be opened in the Level Editor.");
	ERR_FAIL_NULL_V(EditorNode::get_singleton(), ERR_UNAVAILABLE);
	return EditorNode::get_singleton()->open_scene_in_level_editor(p_path);
}

void FileSystemDock::_file_list_thumbnail_done(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, int p_index, const String &p_filename) {
	if (p_preview.is_valid()) {
		if (p_index < files->get_item_count() && files->get_item_text(p_index) == p_filename && files->get_item_metadata(p_index) == p_path) {
			Ref<Texture2D> thumbnail;

			if (file_list_display_mode == FILE_LIST_DISPLAY_LIST) {
				thumbnail = p_small_preview;
			} else {
				thumbnail = p_preview;
			}

			if (thumbnail.is_valid()) {
				files->set_item_icon(p_index, _apply_thumbnail_filter(thumbnail, p_path));
			}
		}
	}
}

void FileSystemDock::_tree_thumbnail_done(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview, int p_update_id, ObjectID p_item) {
	TreeItem *item = ObjectDB::get_instance<TreeItem>(p_item);
	if (item && tree_update_id == p_update_id && p_small_preview.is_valid()) {
		item->set_icon(0, _apply_thumbnail_filter(p_small_preview, p_path));
	}
}

void FileSystemDock::_cancel_visible_scene_previews() {
	EditorResourcePreview *preview = EditorResourcePreview::get_singleton();
	if (preview) {
		for (const KeyValue<String, Callable> &request : visible_scene_preview_requests) {
			preview->cancel_scene_preview(request.key, request.value);
		}
	}
	visible_scene_preview_requests.clear();
}

void FileSystemDock::_queue_visible_scene_previews_update() {
	if (scene_preview_visibility_update_queued) {
		return;
	}
	scene_preview_visibility_update_queued = true;
	callable_mp(this, &FileSystemDock::_update_visible_scene_previews).call_deferred();
}

void FileSystemDock::_file_list_scroll_changed(double) {
	_queue_visible_scene_previews_update();
}

void FileSystemDock::_update_visible_scene_previews() {
	scene_preview_visibility_update_queued = false;
	if (!files->is_visible_in_tree() || !file_list_vb->is_visible_in_tree()) {
		_cancel_visible_scene_previews();
		return;
	}

	files->force_update_list_size();
	VScrollBar *vertical_scroll = files->get_v_scroll_bar();
	HScrollBar *horizontal_scroll = files->get_h_scroll_bar();
	const Vector2 scroll_position(horizontal_scroll->get_value(), vertical_scroll->get_value());
	const Vector2 visible_size(
			horizontal_scroll->is_visible() ? horizontal_scroll->get_page() : files->get_size().x,
			vertical_scroll->is_visible() ? vertical_scroll->get_page() : files->get_size().y);
	const Rect2 visible_rect(scroll_position, visible_size);
	HashSet<String> visible_scenes;

	for (int i = 0; i < files->get_item_count(); i++) {
		const String path = files->get_item_metadata(i);
		if (path.ends_with("/") || EditorFileSystem::get_singleton()->get_file_type(path) != SNAME("PackedScene") || !files->get_item_rect(i).intersects(visible_rect)) {
			continue;
		}
		visible_scenes.insert(path);
		if (visible_scene_preview_requests.has(path)) {
			continue;
		}

		const Callable callback = callable_mp(this, &FileSystemDock::_file_list_thumbnail_done).bind(i, files->get_item_text(i));
		visible_scene_preview_requests.insert(path, callback);
		EditorResourcePreview::get_singleton()->queue_scene_preview(path, callback, EditorResourcePreview::PREVIEW_PRIORITY_HIGH);
	}

	Vector<String> requests_to_cancel;
	for (const KeyValue<String, Callable> &request : visible_scene_preview_requests) {
		if (!visible_scenes.has(request.key)) {
			requests_to_cancel.push_back(request.key);
		}
	}
	for (const String &path : requests_to_cancel) {
		EditorResourcePreview::get_singleton()->cancel_scene_preview(path, visible_scene_preview_requests[path]);
		visible_scene_preview_requests.erase(path);
	}
}

Ref<Texture2D> FileSystemDock::_apply_thumbnail_filter(const Ref<Texture2D> &p_thumbnail, const String &p_file_path) const {
	if (!p_file_path.is_empty()) {
		int index;
		EditorFileSystemDirectory *dir = EditorFileSystem::get_singleton()->find_file(p_file_path, &index);

		if (dir) {
			if (dir->get_file_import_is_valid(index)) {
				const StringName &file_type = dir->get_file_type(index);

				if (file_type == SNAME("CompressedTexture2D") || file_type == SNAME("Image")) {
					const String extension = p_file_path.get_extension();

					if (extension != "svg" && extension != "svgz") {
						Ref<CanvasTexture> thumbnail_wrapped;
						thumbnail_wrapped.instantiate();
						thumbnail_wrapped->set_diffuse_texture(p_thumbnail);
						thumbnail_wrapped->set_texture_filter(CanvasItem::TextureFilter::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
						return thumbnail_wrapped;
					}
				}
			}
		}
	}

	return p_thumbnail;
}

void FileSystemDock::_toggle_file_display() {
	_set_file_display(file_list_display_mode != FILE_LIST_DISPLAY_LIST);
	emit_signal(SNAME("display_mode_changed"));
}

void FileSystemDock::_set_file_display(bool p_active) {
	if (p_active) {
		file_list_display_mode = FILE_LIST_DISPLAY_LIST;
		button_file_list_display_mode->set_button_icon(get_editor_theme_icon(SNAME("FileThumbnail")));
		button_file_list_display_mode->set_tooltip_text(TTRC("View items as a grid of thumbnails."));
	} else {
		file_list_display_mode = FILE_LIST_DISPLAY_THUMBNAILS;
		button_file_list_display_mode->set_button_icon(get_editor_theme_icon(SNAME("FileList")));
		button_file_list_display_mode->set_tooltip_text(TTRC("View items as a list."));
	}

	_update_file_list(true);
}

bool FileSystemDock::_is_file_type_disabled_by_feature_profile(const StringName &p_class) {
	Ref<EditorFeatureProfile> profile = EditorFeatureProfileManager::get_singleton()->get_current_profile();
	if (profile.is_null() || !ClassDB::class_exists(p_class)) {
		return false;
	}

	StringName class_name = p_class;

	while (class_name != StringName()) {
		if (profile->is_class_disabled(class_name)) {
			return true;
		}
		class_name = ClassDB::get_parent_class(class_name);
	}

	return false;
}

void FileSystemDock::_search(EditorFileSystemDirectory *p_path, List<FileInfo> *matches, int p_max_items) {
	if (matches->size() > p_max_items) {
		return;
	}

	for (int i = 0; i < p_path->get_subdir_count(); i++) {
		_search(p_path->get_subdir(i), matches, p_max_items);
	}

	for (int i = 0; i < p_path->get_file_count(); i++) {
		String file = p_path->get_file(i);

		if (_matches_all_search_tokens(file)) {
			FileInfo file_info;
			file_info.name = file;
			file_info.type = p_path->get_file_type(i);
			file_info.path = p_path->get_file_path(i);
			file_info.import_broken = !p_path->get_file_import_is_valid(i);
			file_info.modified_time = p_path->get_file_modified_time(i);

			if (_is_file_type_disabled_by_feature_profile(file_info.type)) {
				// This type is disabled, will not appear here.
				continue;
			}

			matches->push_back(file_info);
			if (matches->size() > p_max_items) {
				return;
			}
		}
	}
}

void FileSystemDock::_update_file_list(bool p_keep_selection, const Vector<String> &p_override_selection) {
	_cancel_visible_scene_previews();

	// Register the previously selected items.
	Vector<String> previous_selection;
	if (p_keep_selection) {
		previous_selection = p_override_selection.is_empty() ? _file_list_get_selected() : p_override_selection;
	}

	HashSet<int> valid_selection;

	files->clear();

	_set_current_path_line_edit_text(current_path);

	String directory = current_path;
	String file = "";

	int thumbnail_size = thumbnail_size_setting * EDSCALE;
	Ref<Texture2D> folder_thumbnail;
	Ref<Texture2D> file_thumbnail;
	Ref<Texture2D> file_thumbnail_broken;

	bool use_thumbnails = (file_list_display_mode == FILE_LIST_DISPLAY_THUMBNAILS);

	if (use_thumbnails) {
		// Thumbnails mode.
		files->set_max_columns(0);
		files->set_icon_mode(ItemList::ICON_MODE_TOP);
		files->set_fixed_column_width(thumbnail_size * 3 / 2);
		files->set_max_text_lines(2);
		files->set_fixed_icon_size(Size2(thumbnail_size, thumbnail_size));

		const int icon_size = get_theme_constant(SNAME("class_icon_size"), EditorStringName(Editor));
		files->set_fixed_tag_icon_size(Size2(icon_size, icon_size));

		if (thumbnail_size < 64) {
			folder_thumbnail = get_editor_theme_icon(SNAME("FolderMediumThumb"));
			file_thumbnail = get_editor_theme_icon(SNAME("FileMediumThumb"));
			file_thumbnail_broken = get_editor_theme_icon(SNAME("FileDeadMediumThumb"));
		} else {
			folder_thumbnail = get_editor_theme_icon(SNAME("FolderBigThumb"));
			file_thumbnail = get_editor_theme_icon(SNAME("FileBigThumb"));
			file_thumbnail_broken = get_editor_theme_icon(SNAME("FileDeadBigThumb"));
		}
	} else {
		// No thumbnails.
		files->set_icon_mode(ItemList::ICON_MODE_LEFT);
		files->set_max_columns(1);
		files->set_max_text_lines(1);
		files->set_fixed_column_width(0);
		const int icon_size = get_theme_constant(SNAME("class_icon_size"), EditorStringName(Editor));
		files->set_fixed_icon_size(Size2(icon_size, icon_size));
	}

	Ref<Texture2D> folder_icon = (use_thumbnails) ? folder_thumbnail : get_theme_icon(SNAME("folder"), SNAME("FileDialog"));
	const Color default_folder_color = get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog"));

	// Build the FileInfo list.
	List<FileInfo> file_list;
	if (_is_color_collection_active()) {
		// Categories initially gather a project-wide union. A directory selected in the pruned tree adds
		// a real res:// scope without replacing the underlying All Assets navigation state.
		category_collection_stats = CategoryCollectionStats();
		_gather_color_collection(EditorFileSystem::get_singleton()->get_filesystem(), "res://", String(), &file_list, &category_collection_stats);
	} else if (current_path == "Favorites") {
		// Display the favorites.
		Vector<String> favorites_list = EditorSettings::get_singleton()->get_favorites();
		for (const String &favorite : favorites_list) {
			if (!favorite.begins_with("res://")) {
				continue;
			}
			String text;
			Ref<Texture2D> icon;
			if (favorite == "res://") {
				text = "/";
				icon = folder_icon;
				if (searched_tokens.is_empty() || _matches_all_search_tokens(text)) {
					files->add_item(text, icon, true);
					files->set_item_metadata(-1, favorite);
				}
			} else if (favorite.ends_with("/")) {
				text = favorite.substr(0, favorite.length() - 1).get_file();
				icon = folder_icon;
				if (searched_tokens.is_empty() || _matches_all_search_tokens(text)) {
					files->add_item(text, icon, true);
					files->set_item_metadata(-1, favorite);

					const Color folder_color = FileSystemDock::get_dir_icon_color(favorite, default_folder_color);
					if (!editor_is_dark_icon_and_font && folder_color != default_folder_color) {
						files->set_item_icon_modulate(-1, folder_color * ITEM_COLOR_SCALE);
					} else {
						files->set_item_icon_modulate(-1, folder_color);
					}
				}
			} else {
				int index;
				EditorFileSystemDirectory *efd = EditorFileSystem::get_singleton()->find_file(favorite, &index);

				FileInfo file_info;
				file_info.name = favorite.get_file();
				file_info.path = favorite;
				if (efd) {
					file_info.type = efd->get_file_type(index);
					file_info.icon_path = efd->get_file_icon_path(index);
					file_info.import_broken = !efd->get_file_import_is_valid(index);
					file_info.modified_time = efd->get_file_modified_time(index);
				} else {
					file_info.type = "";
					file_info.import_broken = true;
					file_info.modified_time = 0;
				}

				if (searched_tokens.is_empty() || _matches_all_search_tokens(file_info.name)) {
					file_list.push_back(file_info);
				}
			}
		}
	} else {
		if (!directory.begins_with("res://")) {
			directory = "res://" + directory;
		}
		// Get infos on the directory + file.
		if (directory.ends_with("/") && directory != "res://") {
			directory = directory.substr(0, directory.length() - 1);
		}
		EditorFileSystemDirectory *efd = EditorFileSystem::get_singleton()->get_filesystem_path(directory);
		if (!efd) {
			directory = current_path.get_base_dir();
			file = current_path.get_file();
			efd = EditorFileSystem::get_singleton()->get_filesystem_path(directory);
		}
		if (!efd) {
			return;
		}

		if (!searched_tokens.is_empty()) {
			// Display the search results.
			// Limit the number of results displayed to avoid an infinite loop.
			_search(EditorFileSystem::get_singleton()->get_filesystem(), &file_list, 10000);
		} else {
			if (display_mode == DISPLAY_MODE_TREE_ONLY || always_show_folders) {
				// Check for a folder color to inherit (if one is assigned).
				const Color inherited_folder_color = FileSystemDock::get_dir_icon_color(directory, default_folder_color);

				// Display folders in the list.
				if (directory != "res://") {
					files->add_item("..", folder_icon, true);

					String bd = directory.get_base_dir();
					if (bd != "res://" && !bd.ends_with("/")) {
						bd += "/";
					}

					files->set_item_metadata(-1, bd);
					files->set_item_selectable(-1, false);
					if (!editor_is_dark_icon_and_font && inherited_folder_color != default_folder_color) {
						files->set_item_icon_modulate(-1, inherited_folder_color * ITEM_COLOR_SCALE);
					} else {
						files->set_item_icon_modulate(-1, inherited_folder_color);
					}
				}

				bool reversed = file_sort == FileSortOption::FILE_SORT_NAME_REVERSE;
				for (int i = reversed ? efd->get_subdir_count() - 1 : 0;
						reversed ? i >= 0 : i < efd->get_subdir_count();
						reversed ? i-- : i++) {
					String dname = efd->get_subdir(i)->get_name();
					String dpath = directory.path_join(dname) + "/";
					bool has_custom_color = assigned_folder_colors.has(dpath);

					files->add_item(dname, folder_icon, true);
					files->set_item_metadata(-1, dpath);
					Color this_folder_color = has_custom_color ? folder_colors[assigned_folder_colors[dpath]] : inherited_folder_color;
					if (!editor_is_dark_icon_and_font && this_folder_color != default_folder_color) {
						this_folder_color *= ITEM_COLOR_SCALE;
					}
					files->set_item_icon_modulate(-1, this_folder_color);

					if (previous_selection.has(dpath)) {
						files->select(files->get_item_count() - 1, false);
						valid_selection.insert(files->get_item_count() - 1);
					}
				}
			}

			// Display the folder content.
			for (int i = 0; i < efd->get_file_count(); i++) {
				FileInfo file_info;
				file_info.name = efd->get_file(i);
				file_info.path = directory.path_join(file_info.name);
				file_info.type = efd->get_file_type(i);
				file_info.icon_path = efd->get_file_icon_path(i);
				file_info.import_broken = !efd->get_file_import_is_valid(i);
				file_info.modified_time = efd->get_file_modified_time(i);

				file_list.push_back(file_info);
			}
		}
	}

	// Sort the file list if needed.
	sort_file_info_list(file_list, file_sort);

	// Fills the ItemList control node from the FileInfos.
	for (FileInfo &E : file_list) {
		FileInfo *finfo = &(E);
		String fname = finfo->name;
		String fpath = finfo->path;

		Ref<Texture2D> type_icon;
		Ref<Texture2D> big_icon;

		String tooltip = fpath;

		// Select the icons.
		type_icon = _get_tree_item_icon(!finfo->import_broken, finfo->type, finfo->icon_path);
		if (!finfo->import_broken) {
			big_icon = file_thumbnail;
		} else {
			big_icon = file_thumbnail_broken;
			tooltip += "\n" + TTR("Status: Import of file failed. Please fix file and reimport manually.");
		}

		// Add the item to the ItemList.
		int item_index;
		if (use_thumbnails) {
			files->add_item(fname, big_icon, true);
			item_index = files->get_item_count() - 1;
			files->set_item_metadata(item_index, fpath);
			files->set_item_tag_icon(item_index, type_icon);

		} else {
			files->add_item(fname, type_icon, true);
			item_index = files->get_item_count() - 1;
			files->set_item_metadata(item_index, fpath);
		}
		if (_asset_has_description(fpath)) {
			_set_file_list_description_icon(item_index, true, fname);
		}

		if (fpath == main_scene_path) {
			files->set_item_custom_fg_color(item_index, get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
		}

		// Generate the preview.
		if (!finfo->import_broken) {
			EditorResourcePreview::get_singleton()->queue_resource_preview(fpath, callable_mp(this, &FileSystemDock::_file_list_thumbnail_done).bind(item_index, fname));
		}

		// Select the items.
		if (previous_selection.has(fpath)) {
			files->select(item_index, false);
			if (current_path == fpath) {
				files->set_current(item_index);
			}
			valid_selection.insert(item_index);
		}

		if (!p_keep_selection && !file.is_empty() && fname == file) {
			files->select(item_index, true);
			files->ensure_current_is_visible();
		}

		// Tooltip.
		if (finfo->sources.size()) {
			for (int j = 0; j < finfo->sources.size(); j++) {
				tooltip += "\nSource: " + finfo->sources[j];
			}
		}
		files->set_item_tooltip(item_index, tooltip);
	}

	// If we have any selected items retained, one must be set as the current one.
	if (files->get_current() == -1 && !valid_selection.is_empty()) {
		files->set_current(*valid_selection.begin());
	}

	_update_category_empty_state();
	_queue_visible_scene_previews_update();
}

HashSet<String> FileSystemDock::_get_valid_conversions_for_file_paths(const Vector<String> &p_paths) {
	HashSet<String> all_valid_conversion_to_targets;
	for (const String &fpath : p_paths) {
		if (fpath.is_empty() || fpath == "res://" || !FileAccess::exists(fpath) || FileAccess::exists(fpath + ".import")) {
			return HashSet<String>();
		}

		Vector<Ref<EditorResourceConversionPlugin>> conversions = EditorNode::get_singleton()->find_resource_conversion_plugin_for_type_name(EditorFileSystem::get_singleton()->get_file_type(fpath));

		if (conversions.is_empty()) {
			// This resource can't convert to anything, so return an empty list.
			return HashSet<String>();
		}

		// Get a list of all potential conversion-to targets.
		HashSet<String> current_valid_conversion_to_targets;
		for (const Ref<EditorResourceConversionPlugin> &E : conversions) {
			const String what = E->converts_to();
			current_valid_conversion_to_targets.insert(what);
		}

		if (all_valid_conversion_to_targets.is_empty()) {
			// If we have no existing valid conversions, this is the first one, so copy them directly.
			all_valid_conversion_to_targets = current_valid_conversion_to_targets;
		} else {
			// Check existing conversion targets and remove any which are not in the current list.
			for (const String &S : all_valid_conversion_to_targets) {
				if (!current_valid_conversion_to_targets.has(S)) {
					all_valid_conversion_to_targets.erase(S);
				}
			}
			// We have no more remaining valid conversions, so break the loop.
			if (all_valid_conversion_to_targets.is_empty()) {
				break;
			}
		}
	}

	return all_valid_conversion_to_targets;
}

void FileSystemDock::_select_file(const String &p_path, bool p_select_in_favorites, bool p_navigate) {
	String fpath = p_path;
	if (fpath.ends_with("/")) {
		// Ignore a directory.
	} else if (fpath != "Favorites") {
		if (FileAccess::exists(fpath + ".import")) {
			Ref<ConfigFile> config;
			config.instantiate();
			Error err = config->load(fpath + ".import");
			if (err == OK) {
				if (config->has_section_key("remap", "importer")) {
					String importer = config->get_value("remap", "importer");
					if (importer == "keep" || importer == "skip") {
						EditorNode::get_singleton()->show_warning(TTRC("Importing has been disabled for this file, so it can't be opened for editing."));
						return;
					}
				}
			}
		}

		String resource_type = ResourceLoader::get_resource_type(fpath);
		if (resource_type == "PackedScene" || resource_type == "AnimationLibrary") {
			bool is_imported = false;
			{
				List<String> importer_exts;
				ResourceImporterScene::get_scene_importer_extensions(&importer_exts);
				String extension = fpath.get_extension();
				for (const String &E : importer_exts) {
					if (extension.nocasecmp_to(E) == 0) {
						is_imported = true;
						break;
					}
				}
			}

			if (is_imported) {
				SceneImportSettingsDialog::get_singleton()->open_settings(p_path, resource_type);
			} else {
				EditorNode::get_singleton()->load_scene_or_resource(fpath);
			}
		} else if (ResourceLoader::is_imported(fpath)) {
			// If the importer has advanced settings, show them.
			int order;
			bool can_threads;
			String name;
			Error err = ResourceFormatImporter::get_singleton()->get_import_order_threads_and_importer(fpath, order, can_threads, name);
			bool used_advanced_settings = false;
			if (err == OK) {
				Ref<ResourceImporter> importer = ResourceFormatImporter::get_singleton()->get_importer_by_name(name);
				if (importer.is_valid() && importer->has_advanced_options()) {
					importer->show_advanced_options(fpath);
					used_advanced_settings = true;
				}
			}

			if (!used_advanced_settings) {
				EditorNode::get_singleton()->load_resource(fpath);
			}
		} else {
			EditorNode::get_singleton()->load_resource(fpath);
		}
	}
	if (p_navigate) {
		_navigate_to_path(fpath, p_select_in_favorites);
	}
}

void FileSystemDock::_tree_activate_file() {
	TreeItem *selected = tree->get_selected();
	if (selected) {
		String file_path = selected->get_metadata(0);
		TreeItem *parent = selected->get_parent();
		bool is_favorite = parent != nullptr && parent->get_metadata(0) == "Favorites";
		bool is_folder = file_path.ends_with("/");

		if ((!is_favorite && is_folder) || file_path == "Favorites") {
			bool collapsed = selected->is_collapsed();
			selected->set_collapsed(!collapsed);
		} else {
			_select_file(file_path, is_favorite && !is_folder, is_favorite && is_folder);
		}
	}
}

void FileSystemDock::_file_list_activate_file(int p_idx) {
	_select_file(files->get_item_metadata(p_idx));
}

void FileSystemDock::_file_list_description_action_clicked(int p_idx, const Vector2 &p_position, MouseButton p_button) {
	if (p_button != MouseButton::LEFT || p_idx < 0 || p_idx >= files->get_item_count()) {
		return;
	}
	_show_description(files->get_item_metadata(p_idx));
}

void FileSystemDock::_tree_description_button_clicked(TreeItem *p_item, int p_column, int p_id, MouseButton p_button) {
	if (!p_item || p_column != 0 || p_id != DESCRIPTION_TREE_BUTTON_ID || p_button != MouseButton::LEFT) {
		return;
	}
	_show_description(p_item->get_metadata(0));
}

bool FileSystemDock::_asset_has_description(const String &p_path) {
	if (!EditorAssetDescription::is_supported(p_path)) {
		return false;
	}
	const uint64_t modified_time = EditorAssetDescription::get_cache_modified_time(p_path);
	HashMap<String, DescriptionCacheEntry>::Iterator cached = description_cache.find(p_path);
	if (cached && cached->value.modified_time == modified_time) {
		return cached->value.has_description;
	}

	DescriptionCacheEntry entry;
	entry.modified_time = modified_time;
	entry.has_description = EditorAssetDescription::has_description_bounded(p_path);
	description_cache.insert(p_path, entry);
	return entry.has_description;
}

void FileSystemDock::_set_tree_description_indicator(TreeItem *p_item, bool p_has_description) {
	ERR_FAIL_NULL(p_item);
	const int button_index = p_item->get_button_by_id(0, DESCRIPTION_TREE_BUTTON_ID);
	if (!p_has_description) {
		if (button_index >= 0) {
			p_item->erase_button(0, button_index);
		}
		return;
	}

	const String tooltip = TTR("View asset description");
	if (button_index >= 0) {
		p_item->set_button(0, button_index, get_editor_theme_icon(SNAME("Info")));
		p_item->set_button_tooltip_text(0, button_index, tooltip);
		p_item->set_button_description(0, button_index, tooltip);
	} else {
		p_item->add_button(0, get_editor_theme_icon(SNAME("Info")), DESCRIPTION_TREE_BUTTON_ID, false, tooltip, tooltip);
	}
}

void FileSystemDock::_set_file_list_description_icon(int p_item_index, bool p_has_description, const String &p_file_name) {
	files->set_item_action_icon(p_item_index, p_has_description ? get_editor_theme_icon(SNAME("Info")) : Ref<Texture2D>());
	files->set_item_action_icon_tooltip(p_item_index, p_has_description ? TTR("View asset description") : String());
	files->set_item_action_icon_accessibility_text(p_item_index, p_has_description ? vformat(TTR("View description for %s"), p_file_name) : String());
}

void FileSystemDock::_refresh_description_indicator(const String &p_path) {
	description_cache.erase(p_path);
	const bool has_description = _asset_has_description(p_path);
	for (int item_index = 0; item_index < files->get_item_count(); item_index++) {
		if (files->get_item_metadata(item_index) != p_path) {
			continue;
		}
		_set_file_list_description_icon(item_index, has_description, p_path.get_file());
	}

	for (TreeItem *item = tree->get_root(); item; item = item->get_next_in_tree()) {
		if (item->get_metadata(0) == p_path) {
			_set_tree_description_indicator(item, has_description);
		}
	}
}

void FileSystemDock::_refresh_all_description_indicators() {
	if (!files || !tree) {
		return;
	}
	for (int item_index = 0; item_index < files->get_item_count(); item_index++) {
		const String path = files->get_item_metadata(item_index);
		const bool has_description = !path.ends_with("/") && _asset_has_description(path);
		_set_file_list_description_icon(item_index, has_description, path.get_file());
	}
	for (TreeItem *item = tree->get_root(); item; item = item->get_next_in_tree()) {
		const String path = item->get_metadata(0);
		_set_tree_description_indicator(item, !path.ends_with("/") && _asset_has_description(path));
	}
}

void FileSystemDock::_show_description_error(const String &p_message) {
	description_error_dialog->set_text(p_message);
	description_error_dialog->popup_centered();
}

void FileSystemDock::_show_description(const String &p_path) {
	String description;
	String error_message;
	if (EditorAssetDescription::read_description(p_path, description, error_message) != OK) {
		_show_description_error(error_message);
		return;
	}

	description_viewer_path = p_path;
	description_viewer->set_title(p_path.get_file());
	EditorSimpleMarkdown::render(description_viewer_text, description, get_theme_font_size(SNAME("main_size"), EditorStringName(EditorFonts)));
	description_viewer->popup_centered(Size2(700, 500) * EDSCALE);
}

void FileSystemDock::_edit_description(const String &p_path) {
	if (!EditorAssetDescription::is_supported(p_path)) {
		return;
	}
	String description;
	String error_message;
	if (EditorAssetDescription::read_description(p_path, description, error_message) != OK) {
		_show_description_error(error_message);
		return;
	}

	description_editor_path = p_path;
	description_editor->set_title(vformat(TTR("Edit Description — %s"), p_path.get_file()));
	description_path_type_label->set_text(vformat(TTR("Path: %s\nType: %s"), p_path, EditorAssetDescription::get_asset_kind_name(EditorAssetDescription::get_asset_kind(p_path))));
	description_text_edit->set_text(description);
	description_text_edit->set_caret_line(0);
	description_text_edit->set_caret_column(0);
	description_editor->popup_centered(Size2(720, 520) * EDSCALE);
	description_text_edit->grab_focus();
}

void FileSystemDock::_edit_viewed_description() {
	if (description_viewer_path.is_empty()) {
		return;
	}
	description_viewer->hide();
	_edit_description(description_viewer_path);
}

void FileSystemDock::_save_description() {
	const String description = description_text_edit->get_text();
	const CharString utf8 = description.utf8();
	String utf8_round_trip;
	if (utf8_round_trip.append_utf8(utf8.get_data(), utf8.length()) != OK || utf8_round_trip != description) {
		_show_description_error(TTR("The description is not valid UTF-8 and cannot be saved."));
		return;
	}
	if (utf8.length() > MAX_DESCRIPTION_SIZE_BYTES) {
		_show_description_error(vformat(TTR("The description is %d bytes. Descriptions are limited to 32 KiB (32768 bytes)."), utf8.length()));
		return;
	}

	String error_message;
	if (EditorAssetDescription::write_description(description_editor_path, description, error_message) != OK) {
		_show_description_error(error_message);
		return;
	}

	description_editor->hide();
	_refresh_description_indicator(description_editor_path);
	if (description_viewer->is_visible() && description_viewer_path == description_editor_path) {
		EditorSimpleMarkdown::render(description_viewer_text, description, get_theme_font_size(SNAME("main_size"), EditorStringName(EditorFonts)));
	}
}

void FileSystemDock::_description_meta_clicked(const Variant &p_meta) {
	if (p_meta.get_type() != Variant::STRING) {
		return;
	}
	const String link = p_meta;
	if (!EditorSimpleMarkdown::is_safe_http_link(link)) {
		return;
	}
	description_pending_link = link;
	description_link_dialog->set_text(vformat(TTR("Open this external link in your default browser?\n\n%s"), link));
	description_link_dialog->popup_centered();
}

void FileSystemDock::_open_description_link() {
	if (!EditorSimpleMarkdown::is_safe_http_link(description_pending_link)) {
		return;
	}
	const Error error = OS::get_singleton()->shell_open(description_pending_link);
	if (error != OK) {
		_show_description_error(TTR("The external link could not be opened."));
	}
	description_pending_link.clear();
}

void FileSystemDock::_preview_invalidated(const String &p_path) {
	if (Callable *callback = visible_scene_preview_requests.getptr(p_path)) {
		EditorResourcePreview::get_singleton()->cancel_scene_preview(p_path, *callback);
		visible_scene_preview_requests.erase(p_path);
	}
	if (EditorFileSystem::get_singleton()->get_file_type(p_path) == SNAME("PackedScene")) {
		_queue_visible_scene_previews_update();
		return;
	}
	if (file_list_display_mode == FILE_LIST_DISPLAY_THUMBNAILS && p_path.get_base_dir() == current_path && searched_tokens.is_empty() && file_list_vb->is_visible_in_tree()) {
		for (int i = 0; i < files->get_item_count(); i++) {
			if (files->get_item_metadata(i) == p_path) {
				// Re-request preview.
				EditorResourcePreview::get_singleton()->queue_resource_preview(p_path, callable_mp(this, &FileSystemDock::_file_list_thumbnail_done).bind(i, files->get_item_text(i)));
				break;
			}
		}
	}
}

void FileSystemDock::_fs_changed() {
	button_hist_prev->set_disabled(history_pos == 0);
	button_hist_next->set_disabled(history_pos == history.size() - 1);
	scanning_vb->hide();
	category_wide_split->set_visible(responsive_layout != RESPONSIVE_LAYOUT_NARROW);
	category_narrow_split->set_visible(responsive_layout == RESPONSIVE_LAYOUT_NARROW);
	split_box->show();

	update_all();

	if (!select_after_scan.is_empty()) {
		_navigate_to_path(select_after_scan);
		select_after_scan.clear();
		import_dock_needs_update = true;
		callable_mp(this, &FileSystemDock::_update_import_dock).call_deferred();
	}

	set_process(false);
	if (had_focus) {
		had_focus->grab_focus();
		had_focus = nullptr;
	}
}

void FileSystemDock::_set_scanning_mode() {
	button_hist_prev->set_disabled(true);
	button_hist_next->set_disabled(true);
	category_wide_split->hide();
	category_narrow_split->hide();
	scanning_vb->show();
	set_process(true);
	if (EditorFileSystem::get_singleton()->is_scanning()) {
		scanning_progress->set_value(EditorFileSystem::get_singleton()->get_scanning_progress() * 100);
	} else {
		scanning_progress->set_value(0);
	}
}

void FileSystemDock::_fw_history() {
	if (history_pos < history.size() - 1) {
		history_pos++;
	}

	_update_history();
}

void FileSystemDock::_bw_history() {
	if (history_pos > 0) {
		history_pos--;
	}

	_update_history();
}

void FileSystemDock::_update_history() {
	current_path = history[history_pos];
	_set_current_path_line_edit_text(current_path);

	if (tree->is_visible()) {
		_update_tree(get_uncollapsed_paths());
		tree->grab_focus(true);
	}

	if (file_list_vb->is_visible()) {
		_update_file_list(false);
	}

	button_hist_prev->set_disabled(history_pos == 0);
	button_hist_next->set_disabled(history_pos == history.size() - 1);
}

void FileSystemDock::_push_to_history() {
	if (history[history_pos] != current_path) {
		history.resize(history_pos + 1);
		history.push_back(current_path);
		history_pos++;

		if (history.size() > history_max_size) {
			history.remove_at(0);
			history_pos = history_max_size - 1;
		}
	}

	button_hist_prev->set_disabled(history_pos == 0);
	button_hist_next->set_disabled(history_pos == history.size() - 1);
}

void FileSystemDock::_get_all_items_in_dir(EditorFileSystemDirectory *p_efsd, Vector<String> &r_files, Vector<String> &r_folders) const {
	if (p_efsd == nullptr) {
		return;
	}

	for (int i = 0; i < p_efsd->get_subdir_count(); i++) {
		r_folders.push_back(p_efsd->get_subdir(i)->get_path());
		_get_all_items_in_dir(p_efsd->get_subdir(i), r_files, r_folders);
	}
	for (int i = 0; i < p_efsd->get_file_count(); i++) {
		r_files.push_back(p_efsd->get_file_path(i));
	}
}

void FileSystemDock::_find_file_owners(EditorFileSystemDirectory *p_efsd, const HashSet<String> &p_renames, HashSet<String> &r_file_owners) const {
	for (int i = 0; i < p_efsd->get_subdir_count(); i++) {
		_find_file_owners(p_efsd->get_subdir(i), p_renames, r_file_owners);
	}
	for (int i = 0; i < p_efsd->get_file_count(); i++) {
		Vector<String> deps = p_efsd->get_file_deps(i);
		for (int j = 0; j < deps.size(); j++) {
			if (p_renames.has(deps[j])) {
				r_file_owners.insert(p_efsd->get_file_path(i));
				break;
			}
		}
	}
}

void FileSystemDock::_try_move_item(const FileOrFolder &p_item, const String &p_new_path,
		HashMap<String, String> &p_file_renames, HashMap<String, String> &p_folder_renames) {
	// Ensure folder paths end with "/".
	String old_path = (p_item.is_file || p_item.path.ends_with("/")) ? p_item.path : (p_item.path + "/");
	String new_path = (p_item.is_file || p_new_path.ends_with("/")) ? p_new_path : (p_new_path + "/");

	if (new_path == old_path) {
		return;
	} else if (old_path == "res://") {
		EditorNode::get_singleton()->add_io_error(TTR("Cannot move/rename resources root."));
		return;
	} else if (!p_item.is_file && new_path.begins_with(old_path)) {
		// This check doesn't erroneously catch renaming to a longer name as folder paths always end with "/".
		EditorNode::get_singleton()->add_io_error(TTR("Cannot move a folder into itself.") + "\n" + old_path + "\n");
		return;
	}

	// Build a list of files which will have new paths as a result of this operation.
	Vector<String> file_changed_paths;
	Vector<String> folder_changed_paths;
	if (p_item.is_file) {
		file_changed_paths.push_back(old_path);
	} else {
		folder_changed_paths.push_back(old_path);
		_get_all_items_in_dir(EditorFileSystem::get_singleton()->get_filesystem_path(old_path), file_changed_paths, folder_changed_paths);
	}

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	print_verbose("Moving " + old_path + " -> " + new_path);
	Error err = da->rename(old_path, new_path);
	if (err == OK) {
		// Move/Rename any corresponding import settings too.
		if (p_item.is_file && FileAccess::exists(old_path + ".import")) {
			err = da->rename(old_path + ".import", new_path + ".import");
			if (err != OK) {
				EditorNode::get_singleton()->add_io_error(TTR("Error moving:") + "\n" + old_path + ".import\n");
			}
		}

		if (p_item.is_file && FileAccess::exists(old_path + ".uid")) {
			err = da->rename(old_path + ".uid", new_path + ".uid");
			if (err != OK) {
				EditorNode::get_singleton()->add_io_error(TTR("Error moving:") + "\n" + old_path + ".uid\n");
			}
		}

		// Update scene if it is open.
		for (int i = 0; i < file_changed_paths.size(); ++i) {
			String new_item_path = p_item.is_file ? new_path : file_changed_paths[i].replace_first(old_path, new_path);
			if (ResourceLoader::get_resource_type(new_item_path) == "PackedScene" && EditorNode::get_singleton()->is_scene_open(file_changed_paths[i])) {
				EditorData *ed = &EditorNode::get_editor_data();
				for (int j = 0; j < ed->get_edited_scene_count(); j++) {
					if (ed->get_scene_path(j) == file_changed_paths[i]) {
						ed->get_edited_scene_root(j)->set_scene_file_path(new_item_path);
						EditorNode::get_singleton()->save_editor_layout_delayed();
						break;
					}
				}
			}
		}

		// Only treat as a changed dependency if it was successfully moved.
		for (int i = 0; i < file_changed_paths.size(); ++i) {
			p_file_renames[file_changed_paths[i]] = file_changed_paths[i].replace_first(old_path, new_path);
			print_verbose("  Remap: " + file_changed_paths[i] + " -> " + p_file_renames[file_changed_paths[i]]);
			emit_signal(SNAME("files_moved"), file_changed_paths[i], p_file_renames[file_changed_paths[i]]);
		}
		for (int i = 0; i < folder_changed_paths.size(); ++i) {
			p_folder_renames[folder_changed_paths[i]] = folder_changed_paths[i].replace_first(old_path, new_path);
			emit_signal(SNAME("folder_moved"), folder_changed_paths[i], p_folder_renames[folder_changed_paths[i]].substr(0, p_folder_renames[folder_changed_paths[i]].length() - 1));
		}
	} else {
		EditorNode::get_singleton()->add_io_error(TTR("Error moving:") + "\n" + old_path + "\n");
	}
}

void FileSystemDock::_try_duplicate_item(const FileOrFolder &p_item, const String &p_new_path) const {
	// Ensure folder paths end with "/".
	String old_path = (p_item.is_file || p_item.path.ends_with("/")) ? p_item.path : (p_item.path + "/");
	String new_path = (p_item.is_file || p_new_path.ends_with("/")) ? p_new_path : (p_new_path + "/");

	if (new_path == old_path) {
		return;
	} else if (old_path == "res://") {
		EditorNode::get_singleton()->add_io_error(TTR("Cannot move/rename resources root."));
		return;
	} else if (!p_item.is_file && new_path.begins_with(old_path)) {
		// This check doesn't erroneously catch renaming to a longer name as folder paths always end with "/".
		EditorNode::get_singleton()->add_io_error(TTR("Cannot move a folder into itself.") + "\n" + old_path + "\n");
		return;
	}

	if (p_item.is_file) {
		print_verbose("Duplicating " + old_path + " -> " + new_path);

		// Create the directory structure.
		EditorFileSystem::get_singleton()->make_dir_recursive(p_new_path.get_base_dir());

		Error err = EditorFileSystem::get_singleton()->copy_file(old_path, new_path);
		if (err != OK) {
			EditorNode::get_singleton()->add_io_error(TTR("Error duplicating:") + "\n" + old_path + U" → " + new_path + ": " + TTR(error_names[err]) + "\n");
		}
	} else {
		Error err = EditorFileSystem::get_singleton()->copy_directory(old_path, new_path);
		if (err != OK) {
			EditorNode::get_singleton()->add_io_error(TTR("Error duplicating directory:") + "\n" + old_path + U" → " + new_path + ": " + TTR(error_names[err]) + "\n");
		}
	}
}

void FileSystemDock::_update_resource_paths_after_move(const HashMap<String, String> &p_renames) const {
	// Rename all resources loaded, be it subresources or actual resources.
	List<Ref<Resource>> cached;
	ResourceCache::get_cached_resources(&cached);

	for (Ref<Resource> &r : cached) {
		String base_path = r->get_path();
		String extra_path;
		int sep_pos = r->get_path().find("::");
		if (sep_pos >= 0) {
			extra_path = base_path.substr(sep_pos);
			base_path = base_path.substr(0, sep_pos);
		}

		if (p_renames.has(base_path)) {
			base_path = p_renames[base_path];
			r->set_path(base_path + extra_path);
		}
	}

	Vector<String> files_to_update;
	for (const KeyValue<String, String> &E : p_renames) {
		if (!files_to_update.has(E.key)) {
			files_to_update.push_back(E.key);
		}
		if (!files_to_update.has(E.value)) {
			files_to_update.push_back(E.value);
		}
	}
	print_verbose("FileSystem: updating file infos.");
	EditorFileSystem::get_singleton()->update_files(files_to_update);
}

void FileSystemDock::_update_dependencies_after_move(const HashMap<String, String> &p_renames, const HashSet<String> &p_file_owners) const {
	// The following code assumes that the following holds:
	// 1) EditorFileSystem contains the old paths/folder structure from before the rename/move.
	// 2) ResourceLoader can use the new paths without needing to call rescan.

	// The currently edited scene should be reloaded first, so get it's path (GH-82652).
	const String &edited_scene_path = EditorNode::get_editor_data().get_scene_path(EditorNode::get_editor_data().get_edited_scene());
	List<String> scenes_to_reload;
	for (const String &E : p_file_owners) {
		// Because we haven't called a rescan yet the found remap might still be an old path itself.
		const HashMap<String, String>::ConstIterator I = p_renames.find(E);
		const String file = I ? I->value : E;
		print_verbose("Remapping dependencies for: " + file);
		const Error err = ResourceLoader::rename_dependencies(file, p_renames);
		if (err == OK) {
			if (ResourceLoader::get_resource_type(file) == "PackedScene" && EditorNode::get_editor_data().get_edited_scene_from_path(file) != -1) {
				if (file == edited_scene_path) {
					scenes_to_reload.push_front(file);
				} else {
					scenes_to_reload.push_back(file);
				}
			}
		} else {
			EditorNode::get_singleton()->add_io_error(TTR("Unable to update dependencies for:") + "\n" + E + "\n");
		}
	}

	for (const String &E : scenes_to_reload) {
		EditorNode::get_singleton()->reload_scene(E);
	}
}

void FileSystemDock::_update_project_settings_after_move(const HashMap<String, String> &p_renames, const HashMap<String, String> &p_folders_renames) {
	// Find all project settings of type FILE and replace them if needed.
	const HashMap<StringName, PropertyInfo> prop_info(ProjectSettings::get_singleton()->get_custom_property_info());
	for (const KeyValue<StringName, PropertyInfo> &E : prop_info) {
		if (E.value.hint == PROPERTY_HINT_FILE || E.value.hint == PROPERTY_HINT_FILE_PATH) {
			String old_path = GLOBAL_GET(E.key);
			if (p_renames.has(old_path)) {
				ProjectSettings::get_singleton()->set_setting(E.key, p_renames[old_path]);
			}
		};
	}

	// Also search for the file in autoload, as they are stored differently from normal files.
	List<PropertyInfo> property_list;
	ProjectSettings::get_singleton()->get_property_list(&property_list);
	for (const PropertyInfo &E : property_list) {
		if (E.name.begins_with("autoload/")) {
			// If the autoload resource paths has a leading "*", it indicates that it is a Singleton,
			// so we have to handle both cases when updating.
			String autoload = GLOBAL_GET(E.name);
			String autoload_singleton = autoload.substr(1);
			if (p_renames.has(autoload)) {
				ProjectSettings::get_singleton()->set_setting(E.name, p_renames[autoload]);
			} else if (autoload.begins_with("*") && p_renames.has(autoload_singleton)) {
				ProjectSettings::get_singleton()->set_setting(E.name, "*" + p_renames[autoload_singleton]);
			}
		}
	}

	if (p_renames.has(main_scene_path)) {
		main_scene_path = p_renames[main_scene_path];
	}

	// Update folder colors.
	for (const KeyValue<String, String> &rename : p_folders_renames) {
		if (assigned_folder_colors.has(rename.key)) {
			assigned_folder_colors[rename.value] = assigned_folder_colors[rename.key];
			assigned_folder_colors.erase(rename.key);
		}
	}
	ProjectSettings::get_singleton()->save();
}

String FileSystemDock::_get_unique_name(const FileOrFolder &p_entry, const String &p_at_path) {
	String new_path;
	String new_path_base;

	if (p_entry.is_file) {
		new_path = p_at_path.path_join(p_entry.path.get_file());
		new_path_base = new_path.get_basename() + " (%d)." + new_path.get_extension();
	} else {
		PackedStringArray path_split = p_entry.path.split("/");
		new_path = p_at_path.path_join(path_split[path_split.size() - 2]);
		new_path_base = new_path + " (%d)";
	}

	int exist_counter = 1;
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	while (da->file_exists(new_path) || da->dir_exists(new_path)) {
		exist_counter++;
		new_path = vformat(new_path_base, exist_counter);
	}

	return new_path;
}

void FileSystemDock::_update_favorites_after_move(const HashMap<String, String> &p_files_renames, const HashMap<String, String> &p_folders_renames) const {
	Vector<String> favorite_files = EditorSettings::get_singleton()->get_favorites();
	Vector<String> new_favorite_files;
	for (const String &old_path : favorite_files) {
		if (p_folders_renames.has(old_path)) {
			new_favorite_files.push_back(p_folders_renames[old_path]);
		} else if (p_files_renames.has(old_path)) {
			new_favorite_files.push_back(p_files_renames[old_path]);
		} else {
			new_favorite_files.push_back(old_path);
		}
	}
	EditorSettings::get_singleton()->set_favorites(new_favorite_files);

	HashMap<String, PackedStringArray> favorite_properties = EditorSettings::get_singleton()->get_favorite_properties();
	for (const KeyValue<String, String> &KV : p_files_renames) {
		if (favorite_properties.has(KV.key)) {
			favorite_properties.replace_key(KV.key, KV.value);
		}
	}
	EditorSettings::get_singleton()->set_favorite_properties(favorite_properties);
}

void FileSystemDock::_make_scene_confirm() {
	const String scene_path = make_scene_dialog->get_scene_path();

	int idx = EditorNode::get_singleton()->new_scene();
	EditorNode::get_editor_data().set_scene_path(idx, scene_path);
	EditorNode::get_singleton()->set_edited_scene(make_scene_dialog->create_scene_root());
	EditorNode::get_singleton()->save_scene_if_open(scene_path);
}

void FileSystemDock::_resource_removed(const Ref<Resource> &p_resource) {
	const Ref<Script> &scr = p_resource;
	if (scr.is_valid()) {
		ScriptServer::remove_global_class_by_path(scr->get_path());
		EditorNode::get_editor_data().script_class_save_global_classes();
		EditorFileSystem::get_singleton()->emit_signal(SNAME("script_classes_updated"));
	}
	emit_signal(SNAME("resource_removed"), p_resource);
}

void FileSystemDock::_file_removed(const String &p_file) {
	emit_signal(SNAME("file_removed"), p_file);

	// Find the closest parent directory available, in case multiple items were deleted along the same path.
	current_path = p_file.get_base_dir();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	while (!da->dir_exists(current_path)) {
		current_path = current_path.get_base_dir();
	}

	current_path_line_edit->set_text(current_path);
}

void FileSystemDock::_folder_removed(const String &p_folder) {
	emit_signal(SNAME("folder_removed"), p_folder);

	// Find the closest parent directory available, in case multiple items were deleted along the same path.
	current_path = p_folder.get_base_dir();
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	while (!da->dir_exists(current_path)) {
		current_path = current_path.get_base_dir();
	}

	// Remove assigned folder color for all subfolders.
	bool folder_colors_updated = false;
	for (const Variant &E : assigned_folder_colors.get_key_list()) {
		const String &path = E;
		// These folder paths are guaranteed to end with a "/".
		if (path.begins_with(p_folder)) {
			assigned_folder_colors.erase(path);
			folder_colors_updated = true;
		}
	}
	if (folder_colors_updated) {
		_update_folder_colors_setting();
	}

	current_path_line_edit->set_text(current_path);
	EditorFileSystemDirectory *efd = EditorFileSystem::get_singleton()->get_filesystem_path(current_path);
	if (efd) {
		efd->force_update();
	}
}

void FileSystemDock::_rename_operation_confirm() {
	String new_name;
	TreeItem *ti = tree->get_edited();
	int col_index = tree->get_edited_column();

	if (ti) {
		new_name = ti->get_text(col_index).strip_edges();
	} else {
		new_name = files->get_edit_text().strip_edges();
	}
	String old_name = to_rename.is_file ? to_rename.path.get_file() : to_rename.path.left(-1).get_file();

	bool rename_error = false;
	if (new_name.length() == 0) {
		EditorNode::get_singleton()->show_warning(TTRC("No name provided."));
		rename_error = true;
	} else if (new_name.contains_char('/') || new_name.contains_char('\\') || new_name.contains_char(':')) {
		EditorNode::get_singleton()->show_warning(TTRC("Name contains invalid characters."));
		rename_error = true;
	} else if (new_name[0] == '.') {
		EditorNode::get_singleton()->show_warning(TTRC("This filename begins with a dot rendering the file invisible to the editor.\nIf you want to rename it anyway, use your operating system's file manager."));
		rename_error = true;
	} else if (to_rename.is_file && to_rename.path.get_extension() != new_name.get_extension()) {
		if (!EditorFileSystem::get_singleton()->get_valid_extensions().find(new_name.get_extension())) {
			unrecognized_ext_dialog->popup_centered_clamped();
			rename_error = true;
		}
	}

	// Restore original name.
	if (rename_error) {
		if (ti) {
			ti->set_text(col_index, old_name);
		}
		return;
	}

	String old_path = to_rename.path.ends_with("/") ? to_rename.path.left(-1) : to_rename.path;
	String new_path = old_path.get_base_dir().path_join(new_name);
	if (old_path == new_path) {
		return;
	}

	if (EditorFileSystem::get_singleton()->is_group_file(old_path)) {
		EditorFileSystem::get_singleton()->move_group_file(old_path, new_path);
	}

	// Present a more user friendly warning for name conflict.
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);

	bool new_exist = (da->file_exists(new_path) || da->dir_exists(new_path));
	if (!da->is_case_sensitive(new_path.get_base_dir())) {
		new_exist = new_exist && (new_path.to_lower() != old_path.to_lower());
	}
	if (new_exist) {
		EditorNode::get_singleton()->show_warning(TTRC("A file or folder with this name already exists."));
		if (ti) {
			ti->set_text(col_index, old_name);
		}
		return;
	}

	HashSet<String> file_owners; // The files that use these moved/renamed resource files.
	_before_move(file_owners);

	HashMap<String, String> file_renames;
	HashMap<String, String> folder_renames;
	_try_move_item(to_rename, new_path, file_renames, folder_renames);

	int current_tab = EditorSceneTabs::get_singleton()->get_current_tab();
	_update_resource_paths_after_move(file_renames);
	_update_dependencies_after_move(file_renames, file_owners);
	_update_project_settings_after_move(file_renames, folder_renames);
	_update_favorites_after_move(file_renames, folder_renames);

	EditorSceneTabs::get_singleton()->set_current_tab(current_tab);

	if (ti) {
		current_path = new_path;
		current_path_line_edit->set_text(current_path);
	}

	print_verbose("FileSystem: calling rescan.");
	_rescan();
}

void FileSystemDock::_duplicate_operation_confirm(const String &p_path) {
	const String base_dir = p_path.trim_suffix("/").get_base_dir();
	if (!DirAccess::dir_exists_absolute(base_dir)) {
		Error err = EditorFileSystem::get_singleton()->make_dir_recursive(base_dir);
		if (err != OK) {
			EditorNode::get_singleton()->show_warning(vformat(TTR("Could not create base directory: %s"), TTR(error_names[err])));
			return;
		}
	}
	_try_duplicate_item(to_duplicate, p_path);
}

void FileSystemDock::_move_confirm() {
	if (confirm_before_move_checkbox->is_pressed()) {
		EditorSettings::get_singleton()->set("docks/filesystem/ask_before_moving_files", false);
		confirm_before_move_checkbox->set_pressed(false);
	}

	_move_operation_confirm(confirm_move_to_dir, confirm_to_copy);
}

void FileSystemDock::_overwrite_dialog_action(bool p_overwrite) {
	overwrite_dialog->hide();
	_move_operation_confirm(to_move_path, to_move_or_copy, p_overwrite ? OVERWRITE_REPLACE : OVERWRITE_RENAME);
}

void FileSystemDock::_convert_dialog_action() {
	Vector<Ref<Resource>> selected_resources;
	for (const String &S : to_convert) {
		Ref<Resource> res = ResourceLoader::load(S);
		ERR_FAIL_COND(res.is_null());
		selected_resources.push_back(res);
	}

	Vector<Ref<Resource>> converted_resources;
	HashSet<Ref<Resource>> resources_to_erase_history_for;
	for (Ref<Resource> res : selected_resources) {
		Vector<Ref<EditorResourceConversionPlugin>> conversions = EditorNode::get_singleton()->find_resource_conversion_plugin_for_resource(res);
		for (const Ref<EditorResourceConversionPlugin> &conversion : conversions) {
			int conversion_id = 0;
			for (const String &target : cached_valid_conversion_targets) {
				if (conversion_id == selected_conversion_id && conversion->converts_to() == target) {
					Ref<Resource> converted_res = conversion->convert(res);
					ERR_FAIL_COND(converted_res.is_null());
					ERR_FAIL_COND(res.is_null());
					converted_resources.push_back(converted_res);
					resources_to_erase_history_for.insert(res);
					break;
				}
				conversion_id++;
			}
		}
	}

	// Clear history for the objects being replaced.
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	for (Ref<Resource> res : resources_to_erase_history_for) {
		undo_redo->clear_history(true, undo_redo->get_history_id_for_object(res.ptr()));
	}

	// Updates all the resources existing as node properties.
	EditorNode::get_singleton()->replace_resources_in_scenes(selected_resources, converted_resources);

	// Overwrite the old resources.
	for (int i = 0; i < converted_resources.size(); i++) {
		Ref<Resource> original_resource = selected_resources.get(i);
		Ref<Resource> new_resource = converted_resources.get(i);

		// Notify plugins that the original resource is removed.
		emit_signal(SNAME("file_removed"), original_resource->get_path());
		// Overwrite the path.
		new_resource->set_path(original_resource->get_path(), true);

		ResourceSaver::save(new_resource);
	}
}

Vector<String> FileSystemDock::_check_existing() {
	Vector<String> conflicting_items;
	for (const FileOrFolder &item : to_move) {
		String old_path = item.path.trim_suffix("/");
		String new_path = to_move_path.path_join(old_path.get_file());

		if ((item.is_file && FileAccess::exists(new_path)) || (!item.is_file && DirAccess::exists(new_path))) {
			conflicting_items.push_back(old_path);
		}
	}
	return conflicting_items;
}

void FileSystemDock::_move_operation_confirm(const String &p_to_path, bool p_copy, Overwrite p_overwrite) {
	if (p_overwrite == OVERWRITE_UNDECIDED) {
		to_move_path = p_to_path;
		to_move_or_copy = p_copy;

		Vector<String> conflicting_items = _check_existing();
		if (!conflicting_items.is_empty()) {
			// Ask to do something.
			overwrite_dialog_header->set_text(vformat(
					TTR("The following files or folders conflict with items in the target location '%s':"), to_move_path));
			overwrite_dialog_file_list->set_text(String("\n").join(conflicting_items));
			overwrite_dialog_footer->set_text(
					p_copy ? TTRC("Do you wish to overwrite them or rename the copied files?")
						   : TTRC("Do you wish to overwrite them or rename the moved files?"));
			overwrite_dialog->popup_centered_ratio(0.6);
			return;
		}
	}

	Vector<String> new_paths;
	new_paths.resize(to_move.size());
	for (int i = 0; i < to_move.size(); i++) {
		if (p_overwrite == OVERWRITE_RENAME) {
			new_paths.write[i] = _get_unique_name(to_move[i], p_to_path);
		} else {
			new_paths.write[i] = p_to_path.path_join(to_move[i].path.trim_suffix("/").get_file());
		}
	}

	if (p_copy) {
		for (int i = 0; i < to_move.size(); i++) {
			if (to_move[i].path != new_paths[i]) {
				_try_duplicate_item(to_move[i], new_paths[i]);
				select_after_scan = new_paths[i];
			}
		}
	} else {
		// Check groups.
		for (int i = 0; i < to_move.size(); i++) {
			if (to_move[i].is_file && EditorFileSystem::get_singleton()->is_group_file(to_move[i].path)) {
				EditorFileSystem::get_singleton()->move_group_file(to_move[i].path, new_paths[i]);
			}
		}

		HashSet<String> file_owners; // The files that use these moved/renamed resource files.
		_before_move(file_owners);

		bool is_moved = false;
		HashMap<String, String> file_renames;
		HashMap<String, String> folder_renames;

		for (int i = 0; i < to_move.size(); i++) {
			if (to_move[i].path != new_paths[i]) {
				_try_move_item(to_move[i], new_paths[i], file_renames, folder_renames);
				is_moved = true;
			}
		}

		if (is_moved) {
			int current_tab = EditorSceneTabs::get_singleton()->get_current_tab();
			_update_resource_paths_after_move(file_renames);
			_update_dependencies_after_move(file_renames, file_owners);
			_update_project_settings_after_move(file_renames, folder_renames);
			_update_favorites_after_move(file_renames, folder_renames);

			EditorSceneTabs::get_singleton()->set_current_tab(current_tab);

			print_verbose("FileSystem: calling rescan.");
			_rescan();

			current_path = p_to_path;
			current_path_line_edit->set_text(current_path);

			// A cut+paste is realized as this move; now that it has landed, drop the clipboard.
			if (clipboard_clear_after_move) {
				file_clipboard.clear();
			}
		}
	}
	clipboard_clear_after_move = false;
}

void FileSystemDock::_before_move(HashSet<String> &r_file_owners) const {
	HashSet<String> renamed_files;
	for (int i = 0; i < to_move.size(); i++) {
		if (to_move[i].is_file) {
			renamed_files.insert(to_move[i].path);
		} else {
			EditorFileSystemDirectory *current_folder = EditorFileSystem::get_singleton()->get_filesystem_path(to_move[i].path);
			ERR_CONTINUE(current_folder == nullptr);
			List<EditorFileSystemDirectory *> folders;
			folders.push_back(current_folder);
			while (folders.front()) {
				current_folder = folders.front()->get();
				for (int j = 0; j < current_folder->get_file_count(); j++) {
					const String file_path = current_folder->get_file_path(j);
					renamed_files.insert(file_path);
				}
				for (int j = 0; j < current_folder->get_subdir_count(); j++) {
					folders.push_back(current_folder->get_subdir(j));
				}
				folders.pop_front();
			}
		}
	}

	// Look for files that use these moved/renamed resource files.
	_find_file_owners(EditorFileSystem::get_singleton()->get_filesystem(), renamed_files, r_file_owners);

	// Open scenes with dependencies on the ones about to be moved will be reloaded,
	// so save them first to prevent losing unsaved changes.
	EditorNode::get_singleton()->save_scene_list(r_file_owners);
}

Vector<String> FileSystemDock::_tree_get_selected(bool remove_self_inclusion, bool p_include_unselected_cursor) const {
	// Build a list of selected items with the active one at the first position.
	Vector<String> selected_strings;

	TreeItem *cursor_item = tree->get_selected();
	if (cursor_item && (p_include_unselected_cursor || cursor_item->is_selected(0)) && cursor_item != favorites_item) {
		selected_strings.push_back(cursor_item->get_metadata(0));
	}

	TreeItem *selected = tree->get_root();
	selected = tree->get_next_selected(selected);
	while (selected) {
		if (selected != cursor_item && selected != favorites_item && selected->is_visible_in_tree()) {
			selected_strings.push_back(selected->get_metadata(0));
		}
		selected = tree->get_next_selected(selected);
	}

	if (remove_self_inclusion) {
		selected_strings = _remove_self_included_paths(selected_strings);
	}
	return selected_strings;
}

Vector<String> FileSystemDock::_file_list_get_selected() const {
	Vector<int> selected_ids = files->get_selected_items();
	Vector<String> selected;
	selected.resize(selected_ids.size());

	String *selected_write = selected.ptrw();
	int i = 0;
	for (const int id : selected_ids) {
		selected_write[i] = files->get_item_metadata(id);
		i++;
	}
	return selected;
}

Vector<String> FileSystemDock::_remove_self_included_paths(Vector<String> selected_strings) {
	// Remove paths or files that are included into another.
	if (selected_strings.size() > 1) {
		selected_strings.sort_custom<FileNoCaseComparator>();
		String last_path = "";
		for (int i = 0; i < selected_strings.size(); i++) {
			if (!last_path.is_empty() && selected_strings[i].begins_with(last_path)) {
				selected_strings.remove_at(i);
				i--;
			}
			if (selected_strings[i].ends_with("/")) {
				last_path = selected_strings[i];
			}
		}
	}
	return selected_strings;
}

void FileSystemDock::_tree_rmb_option(int p_option) {
	if (p_option > FILE_MENU_MAX && p_option < CONVERT_BASE_ID) {
		// Extra options don't need paths.
		_file_option(p_option, {});
		return;
	}

	Vector<String> selected_strings = _tree_get_selected(false);

	// Execute the current option.
	switch (p_option) {
		case FILE_MENU_EXPAND_ALL:
		case FILE_MENU_COLLAPSE_ALL: {
			// Expand or collapse the folder
			if (selected_strings.size() == 1) {
				tree->get_selected()->set_collapsed_recursive(p_option == FILE_MENU_COLLAPSE_ALL);
			}
		} break;
		case FILE_MENU_RENAME: {
			selected_strings = _tree_get_selected(false, true);
			[[fallthrough]];
		}
		default: {
			_file_option(p_option, selected_strings);
		} break;
	}
}

void FileSystemDock::_file_list_rmb_option(int p_option) {
	if (p_option > FILE_MENU_MAX && p_option < CONVERT_BASE_ID) {
		// Extra options don't need paths.
		_file_option(p_option, {});
		return;
	}
	_file_option(p_option, _file_list_get_selected());
}

void FileSystemDock::_generic_rmb_option_selected(int p_option) {
	// Used for submenu commands where we don't know whether we're
	// calling from the file_list_rmb menu or the _tree_rmb option.
	if (files->has_focus()) {
		_file_list_rmb_option(p_option);
	} else {
		_tree_rmb_option(p_option);
	}
}

void FileSystemDock::_file_option(int p_option, const Vector<String> &p_selected) {
	// The first one should be the active item.

	switch (p_option) {
		case FILE_MENU_VIEW_DESCRIPTION: {
			if (p_selected.size() == 1) {
				_show_description(p_selected[0]);
			}
		} break;

		case FILE_MENU_EDIT_DESCRIPTION: {
			if (p_selected.size() == 1) {
				_edit_description(p_selected[0]);
			}
		} break;

		case FILE_MENU_SHOW_IN_EXPLORER: {
			// Show the file/folder in the OS explorer.
			String fpath = current_path;
			if (current_path == "Favorites") {
				if (p_selected.is_empty()) {
					return;
				}
				fpath = p_selected[0];
			}

			String dir = ProjectSettings::get_singleton()->globalize_path(fpath);
			OS::get_singleton()->shell_show_in_file_manager(dir, true);
		} break;

		case FILE_MENU_OPEN_EXTERNAL: {
			for (const String &fpath : p_selected) {
				if (fpath.ends_with("/")) {
					continue;
				}
				const String file = ProjectSettings::get_singleton()->globalize_path(fpath);
				const String extension = file.get_extension();

				const String resource_type = ResourceLoader::get_resource_type(fpath);
				String external_program;

				if (ClassDB::is_parent_class(resource_type, "Script") || extension == "tres" || extension == "tscn") {
					external_program = EDITOR_GET("text_editor/external/exec_path");
				} else if (extension == "res" || extension == "scn") {
					// Binary resources have no meaningful editor outside Godot, so just fallback to something default.
				} else if (resource_type == "CompressedTexture2D" || resource_type == "Image") {
					if (extension == "svg" || extension == "svgz") {
						external_program = EDITOR_GET("filesystem/external_programs/vector_image_editor");
					} else {
						external_program = EDITOR_GET("filesystem/external_programs/raster_image_editor");
					}
				} else if (ClassDB::is_parent_class(resource_type, "AudioStream")) {
					external_program = EDITOR_GET("filesystem/external_programs/audio_editor");
				} else if (resource_type == "PackedScene") {
					external_program = EDITOR_GET("filesystem/external_programs/3d_model_editor");
				}

				if (external_program.is_empty()) {
					OS::get_singleton()->shell_open(file);
				} else {
					List<String> paths;
					paths.push_back(file);
					OS::get_singleton()->open_with_program(external_program, paths);
				}
			}
		} break;

		case FILE_MENU_OPEN_IN_TERMINAL: {
			String fpath = current_path;
			if (current_path == "Favorites") {
				if (p_selected.is_empty()) {
					return;
				}
				fpath = p_selected[0];
			}

			Vector<String> terminal_emulators;
			const String terminal_emulator_setting = EDITOR_GET("filesystem/external_programs/terminal_emulator");
			if (terminal_emulator_setting.is_empty()) {
				// Figure out a default terminal emulator to use.
#if defined(WINDOWS_ENABLED)
				// Default to PowerShell as done by Windows 10 and later.
				terminal_emulators.push_back("powershell");
#elif defined(MACOS_ENABLED)
				// NOTE: To avoid duplicating the Terminal icon on the Dock, we will use the `open` command
				// rather than directly launching the Terminal.app bundle.
				terminal_emulators.push_back("open");
#elif defined(LINUXBSD_ENABLED)
				// Try terminal emulators that ship with common Linux distributions first.
				terminal_emulators.push_back("gnome-terminal");
				terminal_emulators.push_back("konsole");
				terminal_emulators.push_back("xfce4-terminal");
				terminal_emulators.push_back("lxterminal");
				terminal_emulators.push_back("kitty");
				terminal_emulators.push_back("alacritty");
				terminal_emulators.push_back("urxvt");
				terminal_emulators.push_back("xterm");
#endif
			} else {
				// Use the user-specified terminal.
				terminal_emulators.push_back(terminal_emulator_setting);
			}

			String flags = EDITOR_GET("filesystem/external_programs/terminal_emulator_flags");
			String arguments = flags;
			if (arguments.is_empty()) {
				// NOTE: This default value is ignored further below if the terminal executable is `powershell` or `cmd`,
				// due to these terminals requiring nonstandard syntax to start in a specified folder.
				arguments = "{directory}";
			}

#ifdef LINUXBSD_ENABLED
			String chosen_terminal_emulator;
			for (const String &terminal_emulator : terminal_emulators) {
				String pipe;
				List<String> test_args; // Required for `execute()`, as it doesn't accept `Vector<String>`.
				test_args.push_back("-cr");
				test_args.push_back("command -v " + terminal_emulator);
				const Error err = OS::get_singleton()->execute("bash", test_args, &pipe);
				// Check if a path to the terminal executable exists.
				if (err == OK && pipe.contains_char('/')) {
					chosen_terminal_emulator = terminal_emulator;
					break;
				} else if (err == ERR_CANT_FORK) {
					ERR_PRINT_ED(vformat(TTR("Couldn't run external program to check for terminal emulator presence: command -v %s"), terminal_emulator));
				}
			}
#else
			// On Windows and macOS, the first (and only) terminal emulator in the list is always available.
			String chosen_terminal_emulator = terminal_emulators[0];
#endif

			List<String> terminal_emulator_args; // Required for `execute()`, as it doesn't accept `Vector<String>`.
			bool append_default_args = true;

#ifdef LINUXBSD_ENABLED
			// Prepend default arguments based on the terminal emulator name.
			// Use `String.ends_with()` so that installations in non-default paths
			// or `/usr/local/bin` are detected correctly.
			if (flags.is_empty()) {
				if (chosen_terminal_emulator.ends_with("konsole")) {
					terminal_emulator_args.push_back("--workdir");
				} else if (chosen_terminal_emulator.ends_with("gnome-terminal")) {
					terminal_emulator_args.push_back("--working-directory");
				} else if (chosen_terminal_emulator.ends_with("urxvt")) {
					terminal_emulator_args.push_back("-cd");
				} else if (chosen_terminal_emulator.ends_with("xfce4-terminal")) {
					terminal_emulator_args.push_back("--working-directory");
				} else if (chosen_terminal_emulator.ends_with("lxterminal")) {
					terminal_emulator_args.push_back("--working-directory={directory}");
					append_default_args = false;
				} else if (chosen_terminal_emulator.ends_with("kitty")) {
					terminal_emulator_args.push_back("--directory");
				} else if (chosen_terminal_emulator.ends_with("alacritty")) {
					terminal_emulator_args.push_back("--working-directory");
				} else if (chosen_terminal_emulator.ends_with("xterm")) {
					terminal_emulator_args.push_back("-e");
					terminal_emulator_args.push_back("cd '{directory}' && exec $SHELL");
					append_default_args = false;
				}
			}
#endif

#ifdef MACOS_ENABLED
			if (terminal_emulator_setting.is_empty()) {
				terminal_emulator_args.push_back("-b");
				terminal_emulator_args.push_back("com.apple.terminal");
			}
#endif

#ifdef WINDOWS_ENABLED
			// Prepend default arguments based on the terminal emulator name.
			// Use `String.get_basename().to_lower()` to handle Windows' case-insensitive paths
			// with optional file extensions for executables in `PATH`.
			if (chosen_terminal_emulator.get_basename().to_lower() == "powershell") {
				terminal_emulator_args.push_back("-noexit");
				terminal_emulator_args.push_back("-command");
				terminal_emulator_args.push_back("cd '{directory}'");
				append_default_args = false;
			} else if (chosen_terminal_emulator.get_basename().to_lower() == "cmd") {
				terminal_emulator_args.push_back("/K");
				terminal_emulator_args.push_back("cd /d {directory}");
				append_default_args = false;
			}
#endif

			Vector<String> arguments_array = arguments.split(" ");
			for (const String &argument : arguments_array) {
				if (!append_default_args && argument == "{directory}") {
					// Prevent appending a `{directory}` placeholder twice when using powershell or cmd.
					// This allows users to enter the path to cmd or PowerShell in the custom terminal emulator path,
					// and make it work without having to enter custom arguments.
					continue;
				}
				terminal_emulator_args.push_back(argument);
			}

			const bool is_directory = fpath.ends_with("/");
			for (String &terminal_emulator_arg : terminal_emulator_args) {
				if (is_directory) {
					terminal_emulator_arg = terminal_emulator_arg.replace("{directory}", ProjectSettings::get_singleton()->globalize_path(fpath));
				} else {
					terminal_emulator_arg = terminal_emulator_arg.replace("{directory}", ProjectSettings::get_singleton()->globalize_path(fpath).get_base_dir());
				}
			}

			if (OS::get_singleton()->is_stdout_verbose()) {
				// Print full command line to help with troubleshooting.
				String command_string = chosen_terminal_emulator;
				for (const String &arg : terminal_emulator_args) {
					command_string += " " + arg;
				}
				print_line("Opening terminal emulator:", command_string);
			}

			const Error err = OS::get_singleton()->create_process(chosen_terminal_emulator, terminal_emulator_args, nullptr, true);
			if (err != OK) {
				String args_string;
				for (const String &terminal_emulator_arg : terminal_emulator_args) {
					args_string += terminal_emulator_arg;
				}
				ERR_PRINT_ED(vformat(TTR("Couldn't run external terminal program (error code %d): %s %s\nCheck `filesystem/external_programs/terminal_emulator` and `filesystem/external_programs/terminal_emulator_flags` in the Editor Settings."), err, chosen_terminal_emulator, args_string));
			}
		} break;

		case FILE_MENU_OPEN: {
			// Open folders.
			TreeItem *selected = tree->get_root();
			selected = tree->get_next_selected(selected);
			while (selected) {
				if (p_selected.has(selected->get_metadata(0))) {
					selected->set_collapsed(false);
				}
				selected = tree->get_next_selected(selected);
			}
			// Open the file.
			for (int i = 0; i < p_selected.size(); i++) {
				_select_file(p_selected[i]);
			}
		} break;

		case FILE_MENU_OPEN_LEVEL: {
			if (p_selected.size() == 1) {
				open_scene_in_level_editor(p_selected[0]);
			}
		} break;

		case FILE_MENU_INHERIT: {
			// Create a new scene inherited from the selected one.
			if (p_selected.size() == 1) {
				emit_signal(SNAME("inherit"), p_selected[0]);
			}
		} break;

		case FILE_MENU_MAIN_SCENE: {
			// Set as main scene with selected scene file.
			if (p_selected.size() == 1) {
				main_scene_path = ResourceUID::path_to_uid(p_selected[0]);
				ProjectSettings::get_singleton()->set("application/run/main_scene", main_scene_path);
				ProjectSettings::get_singleton()->save();
				_update_tree(get_uncollapsed_paths());
				_update_file_list(true);
			}
		} break;

		case FILE_MENU_INSTANTIATE: {
			// Instantiate all selected scenes.
			Vector<String> paths;
			for (int i = 0; i < p_selected.size(); i++) {
				const String &fpath = p_selected[i];
				if (EditorFileSystem::get_singleton()->get_file_type(fpath) == "PackedScene") {
					paths.push_back(fpath);
				}
			}
			if (!paths.is_empty()) {
				emit_signal(SNAME("instantiate"), paths);
			}
		} break;

		case FILE_MENU_ADD_FAVORITE: {
			// Add the files from favorites.
			Vector<String> favorites_list = EditorSettings::get_singleton()->get_favorites();
			for (int i = 0; i < p_selected.size(); i++) {
				if (!favorites_list.has(p_selected[i])) {
					favorites_list.push_back(p_selected[i]);
				}
			}
			EditorSettings::get_singleton()->set_favorites(favorites_list);
		} break;

		case FILE_MENU_REMOVE_FAVORITE: {
			// Remove the files from favorites.
			Vector<String> favorites_list = EditorSettings::get_singleton()->get_favorites();
			for (int i = 0; i < p_selected.size(); i++) {
				favorites_list.erase(p_selected[i]);
			}
			EditorSettings::get_singleton()->set_favorites(favorites_list);
		} break;

		case FILE_MENU_SHOW_IN_FILESYSTEM: {
			if (!p_selected.is_empty()) {
				navigate_to_path(p_selected[0]);
			}
		} break;

		case FILE_MENU_DEPENDENCIES: {
			// Checkout the file dependencies.
			if (!p_selected.is_empty()) {
				const String &fpath = p_selected[0];
				deps_editor->edit(fpath);
			}
		} break;

		case FILE_MENU_OWNERS: {
			// Checkout the file owners.
			if (!p_selected.is_empty()) {
				const String &fpath = p_selected[0];
				owners_editor->show(fpath);
			}
		} break;

		case FILE_MENU_MOVE: {
			// Move or copy the files to a given location.
			clipboard_clear_after_move = false; // A manual move is not a cut+paste.
			to_move.clear();
			Vector<String> collapsed_paths = _remove_self_included_paths(p_selected);
			for (int i = collapsed_paths.size() - 1; i >= 0; i--) {
				const String &fpath = collapsed_paths[i];
				if (fpath != "res://") {
					to_move.push_back(FileOrFolder(fpath, !fpath.ends_with("/")));
				}
			}
			if (to_move.size() > 0) {
				move_dialog->config(p_selected);
				move_dialog->popup_centered(Vector2i(260 * EDSCALE, DisplayServer::get_singleton()->screen_get_size().y * 0.6));
			}
		} break;

		case FILE_MENU_RENAME: {
			if (!p_selected.is_empty()) {
				// Set to_rename variable for callback execution.
				to_rename.path = p_selected[0];
				to_rename.is_file = !to_rename.path.ends_with("/");
				if (to_rename.path == "res://") {
					break;
				}

				// Rename has same logic as move for resource files.
				to_move.clear();
				to_move.push_back(to_rename);

				if (tree->has_focus()) {
					tree->grab_focus(!tree->has_focus(true));
					// Edit node in Tree.
					tree->edit_selected(true);

					if (to_rename.is_file) {
						String name = to_rename.path.get_file();
						tree->set_editor_selection(0, name.rfind_char('.'));
					} else {
						String name = to_rename.path.left(-1).get_file(); // Removes the "/" suffix for folders.
						tree->set_editor_selection(0, name.length());
					}
				} else if (files->has_focus()) {
					files->edit_selected();
				}
			}
		} break;

		case FILE_MENU_REMOVE: {
			// Remove the selected files.
			Vector<String> remove_files;
			Vector<String> remove_folders;
			Vector<String> collapsed_paths = _remove_self_included_paths(p_selected);

			for (int i = 0; i < collapsed_paths.size(); i++) {
				const String &fpath = collapsed_paths[i];
				if (fpath != "res://") {
					if (fpath.ends_with("/")) {
						remove_folders.push_back(fpath);
					} else {
						remove_files.push_back(fpath);
					}
				}
			}

			if (remove_files.size() + remove_folders.size() > 0) {
				remove_dialog->show(remove_folders, remove_files);
			}
		} break;

		case FILE_MENU_DUPLICATE: {
			if (p_selected.size() != 1 || p_selected[0] == "res://") {
				return;
			}

			to_duplicate.path = p_selected[0];
			to_duplicate.is_file = !to_duplicate.path.ends_with("/");
			if (to_duplicate.is_file) {
				String name = to_duplicate.path.get_file();
				make_dir_dialog->config(to_duplicate.path.get_base_dir(), callable_mp(this, &FileSystemDock::_duplicate_operation_confirm),
						DirectoryCreateDialog::MODE_FILE, TTR("Duplicating file:") + " " + name, name);
			} else {
				String name = to_duplicate.path.trim_suffix("/").get_file();
				make_dir_dialog->config(to_duplicate.path.trim_suffix("/").get_base_dir(), callable_mp(this, &FileSystemDock::_duplicate_operation_confirm),
						DirectoryCreateDialog::MODE_DIRECTORY, TTR("Duplicating folder:") + " " + name, name);
			}
			make_dir_dialog->popup_centered();
		} break;

		case FILE_MENU_COPY:
		case FILE_MENU_CUT: {
			// Stash the selection on the dock's file clipboard for a later Paste. Collapse nested
			// selections (a selected folder already carries its children) as Move/Delete do.
			file_clipboard.clear();
			Vector<String> collapsed_paths = _remove_self_included_paths(p_selected);
			for (const String &fpath : collapsed_paths) {
				if (fpath != "res://") {
					file_clipboard.push_back(FileOrFolder(fpath, !fpath.ends_with("/")));
				}
			}
			file_clipboard_is_cut = (p_option == FILE_MENU_CUT);
		} break;

		case FILE_MENU_PASTE: {
			if (file_clipboard.is_empty()) {
				break;
			}
			// Paste into the current folder (or the folder holding the current file).
			String target_dir = current_path;
			if (!target_dir.ends_with("/")) {
				target_dir = target_dir.get_base_dir();
			}
			to_move.clear();
			for (const FileOrFolder &item : file_clipboard) {
				to_move.push_back(item);
			}
			// A cut clears the clipboard only once the move actually completes.
			clipboard_clear_after_move = file_clipboard_is_cut;
			// Reuse the move/copy pipeline: collision detection, the overwrite/rename dialog,
			// .import/UID handling, dependency fixups, rescan, and reveal all come for free.
			_move_operation_confirm(target_dir, !file_clipboard_is_cut, OVERWRITE_UNDECIDED);
		} break;

		case FILE_MENU_REIMPORT: {
			ImportDock::get_singleton()->reimport_resources(p_selected);
		} break;

		case FILE_MENU_NEW_FOLDER: {
			String directory = current_path;
			if (!directory.ends_with("/")) {
				directory = directory.get_base_dir();
			}
			make_dir_dialog->config(directory, callable_mp(this, &FileSystemDock::create_directory).bind(directory),
					DirectoryCreateDialog::MODE_DIRECTORY, TTR("Create Folder"), "new folder");
			make_dir_dialog->popup_centered();
		} break;

		case FILE_MENU_NEW_SCENE: {
			String directory = current_path;
			if (!directory.ends_with("/")) {
				directory = directory.get_base_dir();
			}
			make_scene_dialog->config(directory);
			make_scene_dialog->popup_centered();
		} break;

		case FILE_MENU_NEW_SCRIPT: {
			String fpath = current_path;
			if (!fpath.ends_with("/")) {
				fpath = fpath.get_base_dir();
			}
			make_script_dialog->config("Node", fpath.path_join("new_script.gd"), false, false);
			make_script_dialog->popup_centered();
		} break;

		case FILE_MENU_COPY_PATH: {
			if (!p_selected.is_empty()) {
				const String &fpath = p_selected[0];
				DisplayServer::get_singleton()->clipboard_set(fpath);
			}
		} break;

		case FILE_MENU_COPY_ABSOLUTE_PATH: {
			if (!p_selected.is_empty()) {
				const String &fpath = p_selected[0];
				const String absolute_path = ProjectSettings::get_singleton()->globalize_path(fpath);
				DisplayServer::get_singleton()->clipboard_set(absolute_path);
			}
		} break;

		case FILE_MENU_COPY_UID: {
			if (!p_selected.is_empty()) {
				ResourceUID::ID uid = ResourceLoader::get_resource_uid(p_selected[0]);
				if (uid != ResourceUID::INVALID_ID) {
					String uid_string = ResourceUID::get_singleton()->id_to_text(uid);
					DisplayServer::get_singleton()->clipboard_set(uid_string);
				}
			}
		} break;

		case FILE_MENU_NEW_RESOURCE: {
			new_resource_dialog->popup_create(true);
		} break;
		case FILE_MENU_NEW_TEXTFILE: {
			String fpath = current_path;
			if (!fpath.ends_with("/")) {
				fpath = fpath.get_base_dir();
			}
			String dir = ProjectSettings::get_singleton()->globalize_path(fpath);
			ScriptEditor::get_singleton()->open_text_file_create_dialog(dir);
		} break;
		case FILE_MENU_RUN_SCRIPT: {
			if (p_selected.size() == 1) {
				const String &fpath = p_selected[0];
				Ref<Script> scr = ResourceLoader::load(fpath);
				ERR_FAIL_COND(scr.is_null());
				EditorNode::get_singleton()->run_editor_script(scr);
			}
		} break;

		case EXTRA_FOCUS_PATH: {
			focus_on_filter();
		} break;
		case EXTRA_FOCUS_FILTER: {
			focus_on_filter();
		} break;

		default: {
			if (p_option >= EditorContextMenuPlugin::BASE_ID) {
				if (!EditorContextMenuPluginManager::get_singleton()->activate_custom_option(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, p_option, p_selected)) {
					// For create new file option, pass the path location of mouse click position instead, to plugin callback.
					String fpath = current_path;
					if (!fpath.ends_with("/")) {
						fpath = fpath.get_base_dir();
					}
					EditorContextMenuPluginManager::get_singleton()->activate_custom_option(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM_CREATE, p_option, { fpath });
				}
			} else if (p_option >= CONVERT_BASE_ID) {
				selected_conversion_id = p_option - CONVERT_BASE_ID;
				ERR_FAIL_INDEX(selected_conversion_id, (int)cached_valid_conversion_targets.size());

				to_convert.clear();
				for (const String &S : p_selected) {
					to_convert.push_back(S);
				}

				int conversion_id = 0;
				for (const String &E : cached_valid_conversion_targets) {
					if (conversion_id == selected_conversion_id) {
						conversion_dialog->set_text(vformat(TTR("Do you wish to convert these files to %s? (This operation cannot be undone!)"), E));
						conversion_dialog->popup_centered();
						break;
					}
					conversion_id++;
				}
			}
			break;
		}
	}
}

int FileSystemDock::_get_menu_option_from_key(const Ref<InputEventKey> &p_key) {
	if (ED_IS_SHORTCUT("filesystem_dock/duplicate", p_key)) {
		return FILE_MENU_DUPLICATE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/copy", p_key)) {
		return FILE_MENU_COPY;
	} else if (ED_IS_SHORTCUT("filesystem_dock/cut", p_key)) {
		return FILE_MENU_CUT;
	} else if (ED_IS_SHORTCUT("filesystem_dock/paste", p_key)) {
		return FILE_MENU_PASTE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/copy_path", p_key)) {
		return FILE_MENU_COPY_PATH;
	} else if (ED_IS_SHORTCUT("filesystem_dock/copy_absolute_path", p_key)) {
		return FILE_MENU_COPY_ABSOLUTE_PATH;
	} else if (ED_IS_SHORTCUT("filesystem_dock/copy_uid", p_key)) {
		return FILE_MENU_COPY_UID;
	} else if (ED_IS_SHORTCUT("filesystem_dock/delete", p_key)) {
		return FILE_MENU_REMOVE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/new_folder", p_key)) {
		return FILE_MENU_NEW_FOLDER;
	} else if (ED_IS_SHORTCUT("filesystem_dock/new_scene", p_key)) {
		return FILE_MENU_NEW_SCENE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/new_script", p_key)) {
		return FILE_MENU_NEW_SCRIPT;
	} else if (ED_IS_SHORTCUT("filesystem_dock/new_resource", p_key)) {
		return FILE_MENU_NEW_RESOURCE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/new_textfile", p_key)) {
		return FILE_MENU_NEW_TEXTFILE;
	} else if (ED_IS_SHORTCUT("filesystem_dock/rename", p_key)) {
		return FILE_MENU_RENAME;
	} else if (ED_IS_SHORTCUT("filesystem_dock/show_in_explorer", p_key)) {
		return FILE_MENU_SHOW_IN_EXPLORER;
	} else if (ED_IS_SHORTCUT("filesystem_dock/open_in_external_program", p_key)) {
		return FILE_MENU_OPEN_EXTERNAL;
	} else if (ED_IS_SHORTCUT("filesystem_dock/open_in_terminal", p_key)) {
		return FILE_MENU_OPEN_IN_TERMINAL;
	} else if (ED_IS_SHORTCUT("filesystem_dock/focus_path", p_key)) {
		return EXTRA_FOCUS_PATH;
	} else if (ED_IS_SHORTCUT("editor/open_search", p_key)) {
		return EXTRA_FOCUS_FILTER;
	}
	return -1;
}

void FileSystemDock::_resource_created() {
	String fpath = current_path;
	if (!fpath.ends_with("/")) {
		fpath = fpath.get_base_dir();
	}

	const String type_name = new_resource_dialog->get_selected_type();
	if (type_name == "Shader") {
		make_shader_dialog->config(fpath.path_join("new_shader"), false, false, type_name);
		make_shader_dialog->popup_centered();
		return;
	} else if (type_name == "ShaderInclude") {
		make_shader_dialog->config(fpath.path_join("new_shader_include"), false, false, type_name);
		make_shader_dialog->popup_centered();
		return;
	} else if (type_name == "VisualShader") {
		make_shader_dialog->config(fpath.path_join("new_shader"), false, false, type_name);
		make_shader_dialog->popup_centered();
		return;
	} else if (ClassDB::is_parent_class(type_name, "Script")) {
		make_script_dialog->config("Node", fpath.path_join("new_script"), false, false);
		make_script_dialog->popup_centered();
		return;
	}

	Variant c = new_resource_dialog->instantiate_selected();

	ERR_FAIL_COND(!c);
	Resource *r = Object::cast_to<Resource>(c);
	ERR_FAIL_NULL(r);

	PackedScene *scene = Object::cast_to<PackedScene>(r);
	if (scene) {
		Node *node = memnew(Node);
		node->set_name("Node");
		scene->pack(node);
		memdelete(node);
	}

	EditorNode::get_singleton()->push_item(r);
	EditorNode::get_singleton()->save_resource_as(Ref<Resource>(r), fpath);
}

void FileSystemDock::_script_or_shader_created(const Ref<Resource> &p_resource) {
	if (Object::cast_to<Script>(p_resource.ptr()) && !EDITOR_GET("docks/filesystem/automatically_open_created_scripts").operator bool()) {
		return;
	}
	EditorNode::get_singleton()->push_item(p_resource.ptr());
}

void FileSystemDock::_search_changed(const String &p_text, const Control *p_from) {
	if (searched_tokens.is_empty()) {
		// Register the uncollapsed paths before they change.
		uncollapsed_paths_before_search = get_uncollapsed_paths();
	}

	const String searched_string = p_text.to_lower();
	if (searched_string.begins_with("uid://")) {
		ResourceUID::ID id = ResourceUID::get_singleton()->text_to_id(searched_string);
		if (id != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(id)) {
			navigate_to_path(ResourceUID::get_singleton()->get_id_path(id));
			return;
		}
	}

	searched_tokens = searched_string.split(" ", false);

	if (p_from == tree_search_box) {
		file_list_search_box->set_text(searched_string);
	} else { // File_list_search_box.
		tree_search_box->set_text(searched_string);
	}

	if (_is_color_collection_active()) {
		_update_tree(get_uncollapsed_paths(), false, false);
	} else {
		_update_filtered_items();
	}
	if (display_mode == DISPLAY_MODE_HSPLIT || display_mode == DISPLAY_MODE_VSPLIT) {
		_update_file_list(false);
	}
	if (searched_tokens.is_empty() && !_is_color_collection_active()) {
		_navigate_to_path(current_path);
	}
}

bool FileSystemDock::_matches_all_search_tokens(const String &p_text) {
	if (searched_tokens.is_empty()) {
		return false;
	}
	const String s = p_text.to_lower();
	for (const String &t : searched_tokens) {
		if (!s.contains(t)) {
			return false;
		}
	}
	return true;
}

void FileSystemDock::_rescan() {
	if (tree->has_focus()) {
		had_focus = tree;
	} else if (files->has_focus()) {
		had_focus = files;
	}

	_set_scanning_mode();
	EditorFileSystem::get_singleton()->scan();
}

void FileSystemDock::_change_split_mode() {
	DisplayMode next_mode = DISPLAY_MODE_TREE_ONLY;
	if (display_mode == DISPLAY_MODE_VSPLIT) {
		next_mode = DISPLAY_MODE_HSPLIT;
	} else if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		next_mode = DISPLAY_MODE_VSPLIT;
	}

	set_display_mode(next_mode);
	emit_signal(SNAME("display_mode_changed"));
}

void FileSystemDock::_split_dragged(int p_offset) {
	if (split_box->is_vertical()) {
		split_box_offset_v = p_offset;
	} else {
		split_box_offset_h = p_offset;
	}
}

void FileSystemDock::_category_split_dragged(int p_offset) {
	if (responsive_layout == RESPONSIVE_LAYOUT_NARROW) {
		category_narrow_split_offset = CLAMP(p_offset, int(80 * EDSCALE), int(160 * EDSCALE));
		category_narrow_split->set_split_offset(category_narrow_split_offset);
	} else {
		category_wide_split_offset = p_offset;
	}
	emit_signal(SNAME("display_mode_changed"));
}

void FileSystemDock::_set_category_layout_narrow(bool p_narrow) {
	if (p_narrow) {
		if (category_rail->get_parent() == category_narrow_split) {
			return;
		}
		category_wide_split_offset = category_wide_split->get_split_offset();
		category_rail->reparent(category_narrow_split);
		split_box->reparent(category_narrow_split);
		category_narrow_split->move_child(category_rail, 0);
		category_narrow_split->move_child(split_box, 1);
		category_wide_split->hide();
		category_narrow_split->show();
		category_narrow_split_offset = CLAMP(category_narrow_split_offset, int(80 * EDSCALE), int(160 * EDSCALE));
		category_narrow_split->set_split_offset(category_narrow_split_offset);
	} else {
		if (category_rail->get_parent() == category_wide_split) {
			return;
		}
		category_narrow_split_offset = category_narrow_split->get_split_offset();
		category_rail->reparent(category_wide_split);
		split_box->reparent(category_wide_split);
		category_wide_split->move_child(category_rail, 0);
		category_wide_split->move_child(split_box, 1);
		category_narrow_split->hide();
		category_wide_split->show();
		category_wide_split->set_split_offset(category_wide_split_offset);
	}
}

void FileSystemDock::_update_responsive_layout() {
	if (updating_responsive_layout || !category_rail || get_size().x <= 0) {
		return;
	}

	const float width = get_size().x / EDSCALE;
	ResponsiveLayout next_layout = RESPONSIVE_LAYOUT_WIDE;
	if (width < 640.0f) {
		next_layout = RESPONSIVE_LAYOUT_NARROW;
	} else if (width < 960.0f) {
		next_layout = RESPONSIVE_LAYOUT_MEDIUM;
	}
	if (next_layout == responsive_layout) {
		return;
	}

	updating_responsive_layout = true;
	const ResponsiveLayout previous_layout = responsive_layout;
	if (next_layout == RESPONSIVE_LAYOUT_NARROW) {
		if (previous_layout == RESPONSIVE_LAYOUT_WIDE) {
			responsive_wide_display_mode = display_mode;
		}
		_set_category_layout_narrow(true);
		category_rail->set_custom_minimum_size(Size2(0, 80) * EDSCALE);
		update_layout(EditorDock::DOCK_LAYOUT_VERTICAL, EditorDock::DOCK_SLOT_LEFT_BR);
	} else {
		_set_category_layout_narrow(false);
		category_rail->set_custom_minimum_size(Size2(next_layout == RESPONSIVE_LAYOUT_WIDE ? 190 : 140, 0) * EDSCALE);
		update_layout(EditorDock::DOCK_LAYOUT_HORIZONTAL, EditorDock::DOCK_SLOT_BOTTOM);
		if (next_layout == RESPONSIVE_LAYOUT_MEDIUM) {
			if (previous_layout == RESPONSIVE_LAYOUT_WIDE || previous_layout == RESPONSIVE_LAYOUT_UNSET) {
				responsive_wide_display_mode = display_mode;
			}
			if (display_mode == DISPLAY_MODE_HSPLIT) {
				set_display_mode(DISPLAY_MODE_VSPLIT);
			}
		} else if (previous_layout != RESPONSIVE_LAYOUT_WIDE && previous_layout != RESPONSIVE_LAYOUT_UNSET) {
			set_display_mode(responsive_wide_display_mode);
		}
		const int maximum_rail_width = int((next_layout == RESPONSIVE_LAYOUT_WIDE ? 280 : 180) * EDSCALE);
		category_wide_split->set_split_offset(MIN(category_wide_split_offset, maximum_rail_width));
	}
	responsive_layout = next_layout;
	if (scanning_vb->is_visible()) {
		category_wide_split->hide();
		category_narrow_split->hide();
	}
	updating_responsive_layout = false;
}

void FileSystemDock::fix_dependencies(const String &p_for_file) {
	deps_editor->edit(p_for_file);
}

void FileSystemDock::update_all() {
	if (tree->is_visible()) {
		_update_tree(get_uncollapsed_paths(), false, false);
	}

	if (file_list_vb->is_visible()) {
		_update_file_list(true);
	}
}

void FileSystemDock::focus_on_path() {
	current_path_line_edit->grab_focus();
	current_path_line_edit->select_all();
}

void FileSystemDock::focus_on_filter() {
	LineEdit *current_search_box = nullptr;
	if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		current_search_box = tree_search_box;
	} else {
		current_search_box = file_list_search_box;
	}

	if (current_search_box) {
		current_search_box->grab_focus();
		current_search_box->select_all();
	}
}

void FileSystemDock::create_directory(const String &p_path, const String &p_base_dir) {
	String trimmed_path = p_path;
	if (!p_base_dir.is_empty()) {
		// Trims off the joining '/' if the base didn't end with one. If the base did have it
		// and there's two slashes, the empty directory is safe to trim off anyways.
		trimmed_path = trimmed_path.trim_prefix(p_base_dir).trim_prefix("/");
	}
	Error err = EditorFileSystem::get_singleton()->make_dir_recursive(trimmed_path, p_base_dir);
	if (err != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("Could not create folder: %s"), TTR(error_names[err])));
	}
}

ScriptCreateDialog *FileSystemDock::get_script_create_dialog() const {
	return make_script_dialog;
}

void FileSystemDock::set_file_list_display_mode(FileListDisplayMode p_mode) {
	if (p_mode == file_list_display_mode) {
		return;
	}

	_toggle_file_display();
}

void FileSystemDock::add_resource_tooltip_plugin(const Ref<EditorResourceTooltipPlugin> &p_plugin) {
	tooltip_plugins.push_back(p_plugin);
}

void FileSystemDock::remove_resource_tooltip_plugin(const Ref<EditorResourceTooltipPlugin> &p_plugin) {
	int index = tooltip_plugins.find(p_plugin);
	ERR_FAIL_COND_MSG(index == -1, "Can't remove plugin that wasn't registered.");
	tooltip_plugins.remove_at(index);
}

String FileSystemDock::get_folder_path_at_mouse_position() const {
	TreeItem *item = tree->get_item_at_position(tree->get_local_mouse_position());
	if (!item) {
		return String();
	}
	String fpath = item->get_metadata(0);
	return fpath.get_base_dir();
}

Control *FileSystemDock::create_tooltip_for_path(const String &p_path) const {
	if (p_path == "Favorites") {
		// No tooltip for the "Favorites" group.
		return nullptr;
	}
	if (DirAccess::exists(p_path)) {
		// No tooltip for directory.
		return nullptr;
	}
	ERR_FAIL_COND_V(!FileAccess::exists(p_path), nullptr);

	const String type = ResourceLoader::get_resource_type(p_path);
	Control *tooltip = EditorResourceTooltipPlugin::make_default_tooltip(p_path);

	for (const Ref<EditorResourceTooltipPlugin> &plugin : tooltip_plugins) {
		if (plugin->handles(type)) {
			tooltip = plugin->make_tooltip_for_path(p_path, EditorResourcePreview::get_singleton()->get_preview_metadata(p_path), tooltip);
		}
	}
	return tooltip;
}

Variant FileSystemDock::get_drag_data_fw(const Point2 &p_point, Control *p_from) {
	bool all_favorites = true;
	bool all_not_favorites = true;

	Vector<String> paths;

	if (p_from == tree) {
		// Check if the first selected is in favorite.
		TreeItem *selected = tree->get_next_selected(tree->get_root());
		while (selected) {
			if (selected == favorites_item) {
				// The "Favorites" item is not draggable.
				return Variant();
			}

			bool is_favorite = selected->get_parent() != nullptr && tree->get_root()->get_first_child() == selected->get_parent();
			all_favorites &= is_favorite;
			all_not_favorites &= !is_favorite;
			selected = tree->get_next_selected(selected);
		}
		if (!all_not_favorites) {
			paths = _tree_get_selected(false);
		} else {
			paths = _tree_get_selected();
		}
	} else if (p_from == files) {
		// Don't allow dragging from empty space in the file list.
		int item = files->get_item_at_position(p_point, true);
		if (item == -1 || !files->is_selected(item)) {
			return Variant();
		}
		for (int i = 0; i < files->get_item_count(); i++) {
			if (files->is_selected(i)) {
				paths.push_back(files->get_item_metadata(i));
			}
		}
		all_favorites = false;
		all_not_favorites = true;
	}

	if (paths.is_empty()) {
		return Variant();
	}

	Dictionary drag_data = EditorNode::get_singleton()->drag_files_and_dirs(paths, p_from);
	if (!all_not_favorites) {
		drag_data["favorite"] = all_favorites ? "all" : "mixed";
	}
	return drag_data;
}

bool FileSystemDock::can_drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) const {
	Dictionary drag_data = p_data;

	if (drag_data.has("favorite")) {
		if (String(drag_data["favorite"]) != "all") {
			return false;
		}

		// Moving favorite around.
		TreeItem *ti = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_selected() : tree->get_item_at_position(p_point);
		if (!ti) {
			return false;
		}

		int drop_section = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_drop_section_at_position(tree->get_item_rect(ti).position) : tree->get_drop_section_at_position(p_point);
		if (ti == favorites_item) {
			return (drop_section == 1); // The parent, first fav.
		}
		if (ti->get_parent() && favorites_item == ti->get_parent()) {
			return true; // A favorite
		}
		if (ti == resources_item) {
			return (drop_section == -1); // The tree, last fav.
		}

		return false;
	}

	if (drag_data.has("type") && String(drag_data["type"]) == "resource") {
		// Move resources.
		String to_dir;
		bool favorite;
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);
		return !favorite;
	}

	if (drag_data.has("type") && (String(drag_data["type"]) == "files" || String(drag_data["type"]) == "files_and_dirs")) {
		// Move files or dir.
		String to_dir;
		bool favorite;
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);

		if (favorite) {
			return true;
		}

		if (to_dir.is_empty()) {
			return false;
		}

		// Attempting to move a folder into itself will fail later,
		// rather than bring up a message don't try to do it in the first place.
		to_dir = to_dir.ends_with("/") ? to_dir : (to_dir + "/");
		Vector<String> fnames = drag_data["files"];
		for (int i = 0; i < fnames.size(); ++i) {
			if (fnames[i].ends_with("/") && to_dir.begins_with(fnames[i])) {
				return false;
			}
		}

		return true;
	}

	if (drag_data.has("type") && String(drag_data["type"]) == "nodes") {
		// Save branch as scene.
		String to_dir;
		bool favorite;
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);
		return !favorite && Array(drag_data["nodes"]).size() == 1;
	}

	return false;
}

void FileSystemDock::drop_data_fw(const Point2 &p_point, const Variant &p_data, Control *p_from) {
	if (!can_drop_data_fw(p_point, p_data, p_from)) {
		return;
	}
	Dictionary drag_data = p_data;

	Vector<String> dirs = EditorSettings::get_singleton()->get_favorites();

	if (drag_data.has("favorite")) {
		if (String(drag_data["favorite"]) != "all") {
			return;
		}
		// Moving favorite around.
		TreeItem *ti = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_selected() : tree->get_item_at_position(p_point);
		if (!ti) {
			return;
		}
		int drop_section = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_drop_section_at_position(tree->get_item_rect(ti).position) : tree->get_drop_section_at_position(p_point);

		int drop_position;
		Vector<String> drag_files = drag_data["files"];
		if (ti == favorites_item) {
			// Drop on the favorite folder.
			drop_position = 0;
		} else if (ti == resources_item) {
			// Drop on the resource item.
			drop_position = dirs.size();
		} else {
			// Drop in the list.
			drop_position = dirs.find(ti->get_metadata(0));
			if (drop_section == 1) {
				drop_position++;
			}
		}

		// Remove dragged favorites.
		Vector<int> to_remove;
		int offset = 0;
		for (int i = 0; i < drag_files.size(); i++) {
			int to_remove_pos = dirs.find(drag_files[i]);
			to_remove.push_back(to_remove_pos);
			if (to_remove_pos < drop_position) {
				offset++;
			}
		}
		drop_position -= offset;
		to_remove.sort();
		for (int i = 0; i < to_remove.size(); i++) {
			dirs.remove_at(to_remove[i] - i);
		}

		// Re-add them at the right position.
		for (int i = 0; i < drag_files.size(); i++) {
			dirs.insert(drop_position, drag_files[i]);
			drop_position++;
		}

		EditorSettings::get_singleton()->set_favorites(dirs);
		_update_tree(get_uncollapsed_paths());

		if (display_mode != DISPLAY_MODE_TREE_ONLY && current_path == "Favorites") {
			_update_file_list(true);
		}
		return;
	}

	if (drag_data.has("type") && String(drag_data["type"]) == "resource") {
		// Moving resource.
		Ref<Resource> res = drag_data["resource"];
		String to_dir;
		bool favorite;
		tree->set_drop_mode_flags(Tree::DROP_MODE_ON_ITEM);
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);
		if (to_dir.is_empty()) {
			to_dir = get_current_directory();
		}

		if (res.is_valid() && !to_dir.is_empty()) {
			EditorNode::get_singleton()->push_item(res.ptr());
			EditorNode::get_singleton()->save_resource_as(res, to_dir);
		}
	}

	if (drag_data.has("type") && (String(drag_data["type"]) == "files" || String(drag_data["type"]) == "files_and_dirs")) {
		// Move files or add to favorites.
		String to_dir;
		bool favorite;
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);
		if (!to_dir.is_empty()) {
			Vector<String> fnames = drag_data["files"];
			to_move.clear();
			String target_dir = to_dir == "res://" ? to_dir : to_dir.trim_suffix("/");

			for (int i = 0; i < fnames.size(); i++) {
				if (fnames[i].trim_suffix("/").get_base_dir() != target_dir) {
					to_move.push_back(FileOrFolder(fnames[i], !fnames[i].ends_with("/")));
				}
			}
			if (!to_move.is_empty()) {
				String move_confirm_text;
				confirm_move_to_dir = to_dir;

				bool ask_before_moving_files = EDITOR_GET("docks/filesystem/ask_before_moving_files") && !Input::get_singleton()->is_key_pressed(Key::SHIFT);
				confirm_to_copy = Input::get_singleton()->is_key_pressed(Key::CMD_OR_CTRL);

				if (!ask_before_moving_files) {
					_move_operation_confirm(to_dir, confirm_to_copy);
				} else {
					if (confirm_to_copy) {
						move_confirm_text = vformat(TTRN("Copy %d selected item to \"%s\"?", "Copy %d selected items to \"%s\"?", to_move.size()), to_move.size(), target_dir);
					} else {
						move_confirm_text = vformat(TTRN("Move %d selected item to \"%s\"?", "Move %d selected items to \"%s\"?", to_move.size()), to_move.size(), target_dir);
					}
					move_confirm_dialog_label->set_text(move_confirm_text);
					move_confirm_dialog->popup_centered();
				}
			}
		} else if (favorite) {
			// Add the files from favorites.
			Vector<String> fnames = drag_data["files"];
			Vector<String> favorites_list = EditorSettings::get_singleton()->get_favorites();
			for (int i = 0; i < fnames.size(); i++) {
				if (!favorites_list.has(fnames[i])) {
					favorites_list.push_back(fnames[i]);
				}
			}
			EditorSettings::get_singleton()->set_favorites(favorites_list);
			_update_tree(get_uncollapsed_paths());
		}
	}

	if (drag_data.has("type") && String(drag_data["type"]) == "nodes") {
		String to_dir;
		bool favorite;
		tree->set_drop_mode_flags(Tree::DROP_MODE_ON_ITEM);
		_get_drag_target_folder(to_dir, favorite, p_point, p_from);
		if (to_dir.is_empty()) {
			to_dir = get_current_directory();
		}
		SceneTreeDock::get_singleton()->save_branch_to_file(to_dir);
	}
}

void FileSystemDock::_get_drag_target_folder(String &target, bool &target_favorites, const Point2 &p_point, Control *p_from) const {
	target = String();
	target_favorites = false;

	// In the file list.
	if (p_from == files) {
		int pos = (p_point == Vector2(Math::INF, Math::INF)) ? -1 : files->get_item_at_position(p_point, true);
		if (pos == -1) {
			target = get_current_directory();
			return;
		}

		String ltarget = files->get_item_metadata(pos);
		target = ltarget.ends_with("/") ? ltarget : current_path.get_base_dir();
		return;
	}

	// In the tree.
	if (p_from == tree) {
		TreeItem *ti = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_selected() : tree->get_item_at_position(p_point);
		if (ti) {
			int section = (p_point == Vector2(Math::INF, Math::INF)) ? tree->get_drop_section_at_position(tree->get_item_rect(ti).position) : tree->get_drop_section_at_position(p_point);

			// Check the favorites first.
			if (ti == tree->get_root()->get_first_child() && section >= 0) {
				target_favorites = true;
				return;
			} else if (ti->get_parent() == tree->get_root()->get_first_child()) {
				target_favorites = true;
				return;
			} else {
				String fpath = ti->get_metadata(0);
				if (section == 0 || section == 2) {
					if (fpath.ends_with("/")) {
						// We drop on a folder.
						target = fpath;
						return;
					} else {
						// We drop on the folder that the target file is in.
						target = fpath.get_base_dir();
						return;
					}
				} else {
					if (ti->get_parent() != tree->get_root()->get_first_child()) {
						// Not in the favorite section.
						if (fpath != "res://") {
							// We drop between two files
							if (fpath.ends_with("/")) {
								fpath = fpath.substr(0, fpath.length() - 1);
							}
							target = fpath.get_base_dir();
							return;
						}
					}
				}
			}
		}
	}
}

void FileSystemDock::_update_folder_colors_setting() {
	if (!ProjectSettings::get_singleton()->has_setting("file_customization/folder_colors")) {
		ProjectSettings::get_singleton()->set_setting("file_customization/folder_colors", assigned_folder_colors);
	} else if (assigned_folder_colors.is_empty()) {
		ProjectSettings::get_singleton()->set_setting("file_customization/folder_colors", Variant());
	}
	ProjectSettings::get_singleton()->save();
}

void FileSystemDock::_folder_color_index_pressed(int p_index, PopupMenu *p_menu) {
	Variant chosen_color_name = p_menu->get_item_metadata(p_index);
	Vector<String> selected;

	// Get all selected folders based on whether the files panel or tree panel is currently focused.
	if (files->has_focus()) {
		Vector<int> files_selected_ids = files->get_selected_items();
		for (int i = 0; i < files_selected_ids.size(); i++) {
			selected.push_back(files->get_item_metadata(files_selected_ids[i]));
		}
	} else {
		TreeItem *tree_selected = tree->get_root();
		tree_selected = tree->get_next_selected(tree_selected);
		while (tree_selected) {
			selected.push_back(tree_selected->get_metadata(0));
			tree_selected = tree->get_next_selected(tree_selected);
		}
	}

	// Update project settings with new folder colors.
	for (int i = 0; i < selected.size(); i++) {
		const String &fpath = selected[i];

		if (chosen_color_name) {
			assigned_folder_colors[fpath] = chosen_color_name;
		} else {
			assigned_folder_colors.erase(fpath);
		}
	}

	_update_folder_colors_setting();
	update_all();

	emit_signal(SNAME("folder_color_changed"));
}

// Explore categories are the existing folder-color collections promoted to a first-class rail. --------

String FileSystemDock::_get_color_label(const String &p_color_key) const {
	HashMap<String, String>::ConstIterator it = color_labels.find(p_color_key);
	if (it && !it->value.strip_edges().is_empty()) {
		return it->value;
	}
	return p_color_key.capitalize();
}

void FileSystemDock::_load_color_labels() {
	color_labels.clear();
	if (ProjectSettings::get_singleton()->has_setting("file_customization/color_labels")) {
		Dictionary d = ProjectSettings::get_singleton()->get_setting("file_customization/color_labels");
		for (const Variant &k : d.get_key_list()) {
			color_labels[k] = d[k];
		}
	}
}

String FileSystemDock::_get_color_icon_id(const String &p_color_key, bool p_use_edited) const {
	const HashMap<String, String> &source = p_use_edited ? edited_color_icons : color_icons;
	HashMap<String, String>::ConstIterator icon = source.find(p_color_key);
	return icon ? get_explore_category_icon(icon->value)->theme_icon : String("Folder");
}

void FileSystemDock::_load_color_icons() {
	color_icons.clear();
	if (!ProjectSettings::get_singleton()->has_setting("file_customization/color_icons")) {
		return;
	}

	Dictionary icons = ProjectSettings::get_singleton()->get_setting("file_customization/color_icons");
	for (const Variant &key : icons.get_key_list()) {
		const String color_key = key;
		const String icon_id = icons[key];
		if (folder_colors.has(color_key) && icon_id == get_explore_category_icon(icon_id)->theme_icon && icon_id != "Folder") {
			color_icons[color_key] = icon_id;
		}
	}
}

void FileSystemDock::_load_color_customization() {
	if (ProjectSettings::get_singleton()->has_setting("file_customization/folder_colors")) {
		assigned_folder_colors = ProjectSettings::get_singleton()->get_setting("file_customization/folder_colors");
	} else {
		assigned_folder_colors = Dictionary();
	}
	_load_color_labels();
	_load_color_icons();
}

void FileSystemDock::_rebuild_category_rail() {
	if (!category_tree) {
		return;
	}
	category_edit_button->set_button_icon(get_editor_theme_icon(SNAME("Edit")));
	category_result_edit_button->set_button_icon(get_editor_theme_icon(SNAME("Edit")));

	HashSet<String> assigned_colors;
	for (const Variant &path : assigned_folder_colors.get_key_list()) {
		const String folder_path = path;
		const String color_key = assigned_folder_colors[path];
		if (folder_colors.has(color_key) && DirAccess::dir_exists_absolute(ProjectSettings::get_singleton()->globalize_path(folder_path))) {
			assigned_colors.insert(color_key);
		}
	}

	HashSet<String> visible_colors;
	for (const String &color_key : folder_color_order) {
		HashMap<String, String>::ConstIterator label = color_labels.find(color_key);
		const bool explicitly_named = label && !label->value.strip_edges().is_empty();
		if (explicitly_named || assigned_colors.has(color_key)) {
			visible_colors.insert(color_key);
		}
	}

	bool selection_changed = false;
	for (const String &color_key : folder_color_order) {
		if (active_color_filter.has(color_key) && !visible_colors.has(color_key)) {
			active_color_filter.erase(color_key);
			selection_changed = true;
		}
	}
	if (selection_changed && active_color_filter.is_empty()) {
		_end_category_filter();
	}

	category_tree->clear();
	TreeItem *root = category_tree->create_item();
	TreeItem *all_assets = category_tree->create_item(root);
	all_assets->set_icon(0, get_editor_theme_icon(SNAME("Folder")));
	all_assets->set_cell_mode(1, TreeItem::CELL_MODE_CHECK);
	all_assets->set_checked(1, active_color_filter.is_empty());
	all_assets->set_text(1, TTR("All Assets"));
	all_assets->set_metadata(0, String());
	all_assets->set_metadata(1, String());
	all_assets->set_selectable(0, true);
	all_assets->set_selectable(1, true);

	for (const String &color_key : folder_color_order) {
		if (!visible_colors.has(color_key)) {
			continue;
		}

		TreeItem *item = category_tree->create_item(root);
		const ExploreCategoryIcon *icon = get_explore_category_icon(_get_color_icon_id(color_key));
		item->set_icon(0, get_editor_theme_icon(icon->theme_icon));
		if (icon->tintable) {
			item->set_icon_modulate(0, editor_is_dark_icon_and_font ? folder_colors[color_key] : folder_colors[color_key] * ITEM_COLOR_SCALE);
		} else {
			item->set_icon(1, get_editor_theme_icon(SNAME("Folder")));
			item->set_icon_modulate(1, editor_is_dark_icon_and_font ? folder_colors[color_key] : folder_colors[color_key] * ITEM_COLOR_SCALE);
		}
		item->set_cell_mode(1, TreeItem::CELL_MODE_CHECK);
		item->set_checked(1, active_color_filter.has(color_key));
		item->set_text(1, _get_color_label(color_key));
		item->set_metadata(0, color_key);
		item->set_metadata(1, color_key);
		item->set_selectable(0, true);
		item->set_selectable(1, true);
	}

	category_rail_empty_state->set_visible(visible_colors.is_empty());
}

void FileSystemDock::_category_rail_item_clicked(const Vector2 &p_pos, MouseButton p_button) {
	if (p_button != MouseButton::LEFT) {
		return;
	}
	TreeItem *item = category_tree->get_item_at_position(p_pos);
	if (!item || item == category_tree->get_root()) {
		return;
	}
	const String color_key = item->get_metadata(1);
	callable_mp(this, &FileSystemDock::_set_category_filter).call_deferred(color_key);
}

void FileSystemDock::_begin_category_filter() {
	if (category_restore_state_valid) {
		return;
	}
	category_restore_path = current_path;
	category_restore_selection = get_selected_paths();
	category_restore_uncollapsed_paths = get_uncollapsed_paths();
	category_scope_path.clear();
	category_restore_state_valid = true;
}

void FileSystemDock::_end_category_filter() {
	category_scope_path.clear();
	if (!category_restore_state_valid) {
		return;
	}
	current_path = category_restore_path;
	category_restore_path.clear();
	category_restore_state_valid = false;
	category_restore_pending = true;
}

void FileSystemDock::_set_category_filter(const String &p_color_key) {
	const bool was_active = _is_color_collection_active();
	if (p_color_key.is_empty()) {
		active_color_filter.clear();
	} else {
		if (!was_active) {
			_begin_category_filter();
		}
		if (active_color_filter.has(p_color_key)) {
			active_color_filter.erase(p_color_key);
		} else {
			active_color_filter.insert(p_color_key);
		}
	}

	if (active_color_filter.is_empty()) {
		_end_category_filter();
	} else if (was_active) {
		// Changing the union returns it to its project-wide scope. The saved All Assets path remains intact.
		category_scope_path.clear();
		if (category_restore_state_valid) {
			current_path = category_restore_path;
		}
	}
	_rebuild_category_rail();
	_update_color_filter_view();
	emit_signal(SNAME("display_mode_changed"));
}

void FileSystemDock::_update_color_filter_view() {
	_update_display_mode(true);
	if (!_is_color_collection_active() && category_restore_pending) {
		_update_tree(category_restore_uncollapsed_paths, false, true, category_restore_selection);
		if (display_mode != DISPLAY_MODE_TREE_ONLY) {
			_update_file_list(true, category_restore_selection);
		}
		category_restore_selection.clear();
		category_restore_uncollapsed_paths.clear();
		category_restore_pending = false;
	}
	_set_current_path_line_edit_text(current_path);
}

void FileSystemDock::_build_category_visible_paths() {
	category_visible_paths.clear();
	if (!_is_color_collection_active()) {
		return;
	}
	_gather_category_tree_paths(EditorFileSystem::get_singleton()->get_filesystem(), "res://", String());
	category_visible_paths.insert("res://");
}

bool FileSystemDock::_gather_category_tree_paths(EditorFileSystemDirectory *p_dir, const String &p_dir_path, const String &p_inherited_color) {
	if (!p_dir) {
		return false;
	}
	const String explicit_color = assigned_folder_colors.get(p_dir_path, String());
	const String effective_color = folder_colors.has(explicit_color) ? explicit_color : p_inherited_color;
	bool keep = active_color_filter.has(effective_color);
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		EditorFileSystemDirectory *subdir = p_dir->get_subdir(i);
		keep = _gather_category_tree_paths(subdir, p_dir_path.path_join(subdir->get_name()) + "/", effective_color) || keep;
	}
	if (keep) {
		category_visible_paths.insert(p_dir_path);
	}
	return keep;
}

void FileSystemDock::_gather_color_collection(EditorFileSystemDirectory *p_dir, const String &p_dir_path, const String &p_inherited_color, List<FileInfo> *r_matches, CategoryCollectionStats *r_stats) {
	if (!p_dir) {
		return;
	}

	const String explicit_color = assigned_folder_colors.get(p_dir_path, String());
	const String effective_color = folder_colors.has(explicit_color) ? explicit_color : p_inherited_color;
	if (r_stats && active_color_filter.has(explicit_color)) {
		r_stats->assigned_folder_count++;
	}

	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		EditorFileSystemDirectory *subdir = p_dir->get_subdir(i);
		_gather_color_collection(subdir, p_dir_path.path_join(subdir->get_name()) + "/", effective_color, r_matches, r_stats);
	}

	if (!active_color_filter.has(effective_color) || (!category_scope_path.is_empty() && !p_dir_path.begins_with(category_scope_path))) {
		return;
	}

	for (int i = 0; i < p_dir->get_file_count(); i++) {
		if (r_stats) {
			r_stats->total_file_count++;
		}
		const StringName file_type = p_dir->get_file_type(i);
		if (_is_file_type_disabled_by_feature_profile(file_type)) {
			continue;
		}
		if (r_stats) {
			r_stats->available_file_count++;
		}
		const String file_name = p_dir->get_file(i);
		if (!searched_tokens.is_empty() && !_matches_all_search_tokens(file_name)) {
			continue;
		}

		if (r_stats) {
			r_stats->matched_file_count++;
		}
		// TREE_ONLY only needs the stats (rows come from _create_tree), so it passes a null match list
		// and we skip building a FileInfo that would be discarded.
		if (r_matches) {
			FileInfo file_info;
			file_info.name = file_name;
			file_info.path = p_dir->get_file_path(i);
			file_info.type = file_type;
			file_info.icon_path = p_dir->get_file_icon_path(i);
			file_info.import_broken = !p_dir->get_file_import_is_valid(i);
			file_info.modified_time = p_dir->get_file_modified_time(i);
			r_matches->push_back(file_info);
		}
	}
}

String FileSystemDock::_get_active_category_display_name() const {
	String category_name;
	int count = 0;
	for (const String &color_key : folder_color_order) {
		if (!active_color_filter.has(color_key)) {
			continue;
		}
		category_name = _get_color_label(color_key);
		count++;
	}
	return count == 1 ? category_name : vformat(TTR("%d selected categories"), count);
}

void FileSystemDock::_update_category_empty_state() {
	if (!category_result_empty_state) {
		return;
	}

	VBoxContainer *target_parent = display_mode == DISPLAY_MODE_TREE_ONLY ? tree_content_vb : files_content_vb;
	if (category_result_empty_state->get_parent() != target_parent) {
		category_result_empty_state->reparent(target_parent);
	}

	bool show_empty_state = false;
	bool show_edit = false;
	bool show_clear_search = false;
	String message;
	String hint;
	if (_is_color_collection_active()) {
		const String category_name = _get_active_category_display_name();
		if (category_collection_stats.assigned_folder_count == 0) {
			show_empty_state = true;
			show_edit = true;
			message = vformat(TTR("No folders use “%s”."), category_name);
			hint = TTR("Right-click a folder and choose Set Folder Color... to assign this category.");
		} else if (category_collection_stats.total_file_count == 0) {
			show_empty_state = true;
			message = vformat(TTR("No assets in “%s”."), category_name);
		} else if (category_collection_stats.available_file_count == 0) {
			show_empty_state = true;
			message = TTR("No available assets match this category under the current feature profile.");
		} else if (!searched_tokens.is_empty() && category_collection_stats.matched_file_count == 0) {
			show_empty_state = true;
			show_clear_search = true;
			message = vformat(TTR("No assets match “%s”."), file_list_search_box->get_text());
		}
	}

	category_result_empty_label->set_text(message);
	category_result_empty_hint->set_text(hint);
	category_result_empty_hint->set_visible(!hint.is_empty());
	category_result_edit_button->set_visible(show_edit);
	category_result_clear_search_button->set_visible(show_clear_search);
	category_result_empty_state->set_visible(show_empty_state);

	if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		tree->show();
		files->show();
	} else {
		files->set_visible(!show_empty_state);
	}
}

void FileSystemDock::_clear_category_search() {
	file_list_search_box->clear();
}

void FileSystemDock::_popup_color_labels_dialog() {
	if (!color_labels_dialog) {
		color_labels_dialog = memnew(ConfirmationDialog);
		color_labels_dialog->set_title(TTR("Edit Categories"));
		color_labels_dialog->set_ok_button_text(TTR("Save"));
		color_labels_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_color_labels_dialog_confirmed));
		add_child(color_labels_dialog);

		VBoxContainer *vb = memnew(VBoxContainer);
		color_labels_dialog->add_child(vb);

		Label *hint = memnew(Label);
		hint->set_text(TTR("Give each folder color a name and icon. Categories are saved in the project."));
		vb->add_child(hint);

		GridContainer *grid = memnew(GridContainer);
		grid->set_columns(2);
		grid->set_v_size_flags(SIZE_EXPAND_FILL);
		vb->add_child(grid);

		for (const String &color_key : folder_color_order) {
			MenuButton *icon_button = memnew(MenuButton);
			icon_button->set_text(String::chr(0x25A0));
			icon_button->set_accessibility_name(vformat(TTR("Icon for %s category"), color_key.capitalize()));
			icon_button->set_custom_minimum_size(Size2(72, 28) * EDSCALE);
			PopupMenu *icon_popup = icon_button->get_popup();
			for (int i = 0; i < (int)std_size(explore_category_icons); i++) {
				const ExploreCategoryIcon &option = explore_category_icons[i];
				icon_popup->add_icon_item(get_editor_theme_icon(option.theme_icon), TTR(option.display_name), i);
			}
			icon_popup->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_category_icon_selected).bind(color_key));
			grid->add_child(icon_button);
			color_icon_buttons[color_key] = icon_button;

			LineEdit *le = memnew(LineEdit);
			le->set_h_size_flags(SIZE_EXPAND_FILL);
			le->set_placeholder(color_key.capitalize());
			grid->add_child(le);
			color_label_edits[color_key] = le;
		}
	}

	edited_color_icons = color_icons;
	// Seed each field from the current label (blank == using the default capitalized name).
	for (const KeyValue<String, LineEdit *> &E : color_label_edits) {
		HashMap<String, String>::ConstIterator it = color_labels.find(E.key);
		E.value->set_text(it ? it->value : String());
		_update_color_icon_button(E.key);
	}
	color_labels_dialog->popup_centered(Size2(500, 0) * EDSCALE);
}

void FileSystemDock::_category_icon_selected(int p_id, const String &p_color_key) {
	ERR_FAIL_INDEX(p_id, (int)std_size(explore_category_icons));
	edited_color_icons[p_color_key] = explore_category_icons[p_id].theme_icon;
	_update_color_icon_button(p_color_key);
}

void FileSystemDock::_update_color_icon_button(const String &p_color_key) {
	MenuButton **button_ptr = color_icon_buttons.getptr(p_color_key);
	if (!button_ptr) {
		return;
	}
	MenuButton *button = *button_ptr;
	const ExploreCategoryIcon *icon = get_explore_category_icon(_get_color_icon_id(p_color_key, true));
	button->set_button_icon(get_editor_theme_icon(icon->theme_icon));
	button->set_tooltip_text(vformat(TTR("Category icon: %s"), TTR(icon->display_name)));
	button->set_theme_type_variation(icon->tintable ? StringName("FlatMenuButton") : StringName("FlatMenuButtonNoIconTint"));
	const Color category_color = editor_is_dark_icon_and_font ? folder_colors[p_color_key] : folder_colors[p_color_key] * ITEM_COLOR_SCALE;
	for (const StringName &state : { SNAME("font_color"), SNAME("font_hover_color"), SNAME("font_pressed_color"), SNAME("font_focus_color") }) {
		button->add_theme_color_override(state, category_color);
	}
	for (const StringName &state : { SNAME("icon_normal_color"), SNAME("icon_hover_color"), SNAME("icon_pressed_color"), SNAME("icon_focus_color") }) {
		if (icon->tintable) {
			button->add_theme_color_override(state, category_color);
		} else {
			button->remove_theme_color_override(state);
		}
	}
}

void FileSystemDock::_color_labels_dialog_confirmed() {
	Dictionary labels_dictionary;
	for (const KeyValue<String, LineEdit *> &E : color_label_edits) {
		const String txt = E.value->get_text().strip_edges();
		if (!txt.is_empty() && txt != E.key.capitalize()) {
			color_labels[E.key] = txt;
			labels_dictionary[E.key] = txt;
		} else {
			color_labels.erase(E.key);
		}
	}

	Dictionary icons_dictionary;
	color_icons.clear();
	for (const String &color_key : folder_color_order) {
		const String icon_id = _get_color_icon_id(color_key, true);
		if (icon_id != "Folder") {
			color_icons[color_key] = icon_id;
			icons_dictionary[color_key] = icon_id;
		}
	}

	// Names and icons are one category edit transaction and share a single project save.
	ProjectSettings::get_singleton()->set_setting("file_customization/color_labels", labels_dictionary.is_empty() ? Variant() : Variant(labels_dictionary));
	ProjectSettings::get_singleton()->set_setting("file_customization/color_icons", icons_dictionary.is_empty() ? Variant() : Variant(icons_dictionary));
	ProjectSettings::get_singleton()->save();

	_rebuild_category_rail();
	_update_color_filter_view();
}

void FileSystemDock::_file_and_folders_fill_popup(PopupMenu *p_popup, const Vector<String> &p_paths, bool p_display_path_dependent_options) {
	Vector<String> filenames;
	Vector<String> foldernames;

	Vector<String> favorites_list = EditorSettings::get_singleton()->get_favorites();

	bool no_paths = p_paths.is_empty();
	bool single_path = !no_paths && p_paths.size() == 1;

	bool all_files = !no_paths;
	bool all_files_scenes = true;
	bool all_folders = !no_paths;
	bool all_favorites = true;
	bool all_not_favorites = true;

	for (const String &fpath : p_paths) {
		if (fpath.ends_with("/")) {
			foldernames.push_back(fpath);
			all_files = false;
		} else {
			filenames.push_back(fpath);
			all_folders = false;
			all_files_scenes &= (EditorFileSystem::get_singleton()->get_file_type(fpath) == "PackedScene");
		}

		// Check if in favorites.
		bool found = false;
		for (const String &fav : favorites_list) {
			if (fav == fpath) {
				found = true;
				break;
			}
		}
		if (found) {
			all_not_favorites = false;
		} else {
			all_favorites = false;
		}
	}

	if (all_files) {
		if (all_files_scenes) {
			if (filenames.size() == 1) {
				p_popup->add_icon_item(get_editor_theme_icon(SNAME("Load")), TTRC("Open Scene"), FILE_MENU_OPEN);
				if (filenames[0].get_extension().to_lower() == "tscn") {
					p_popup->add_icon_item(get_editor_theme_icon(SNAME("Grid")), TTRC("Open in Level Editor"), FILE_MENU_OPEN_LEVEL);
				}
				p_popup->add_icon_item(get_editor_theme_icon(SNAME("CreateNewSceneFrom")), TTRC("New Inherited Scene"), FILE_MENU_INHERIT);
				if (main_scene_path != filenames[0]) {
					p_popup->add_icon_item(get_editor_theme_icon(SNAME("PlayScene")), TTRC("Set as Main Scene"), FILE_MENU_MAIN_SCENE);
				}
			} else {
				p_popup->add_icon_item(get_editor_theme_icon(SNAME("Load")), TTRC("Open Scenes"), FILE_MENU_OPEN);
			}
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("Instance")), TTRC("Instantiate"), FILE_MENU_INSTANTIATE);
			p_popup->add_separator();
		} else if (filenames.size() == 1) {
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("Load")), TTRC("Open"), FILE_MENU_OPEN);

			String type = EditorFileSystem::get_singleton()->get_file_type(filenames[0]);
			if (ClassDB::is_parent_class(type, "Script")) {
				Ref<Script> scr = ResourceLoader::load(filenames[0]);
				if (scr.is_valid()) {
					if (ClassDB::is_parent_class(scr->get_instance_base_type(), "EditorScript")) {
						p_popup->add_icon_item(get_editor_theme_icon(SNAME("MainPlay")), TTRC("Run"), FILE_MENU_RUN_SCRIPT);
					}
				}
			}
			p_popup->add_separator();
		}

		if (filenames.size() == 1) {
			p_popup->add_item(TTRC("Edit Dependencies..."), FILE_MENU_DEPENDENCIES);
			p_popup->add_item(TTRC("View Owners..."), FILE_MENU_OWNERS);
			p_popup->add_separator();
		}
	}

	if (filenames.size() == 1 && EditorAssetDescription::is_supported(filenames[0])) {
		if (_asset_has_description(filenames[0])) {
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("Info")), TTRC("View Description"), FILE_MENU_VIEW_DESCRIPTION);
		}
		p_popup->add_icon_item(get_editor_theme_icon(SNAME("Edit")), TTRC("Edit Description"), FILE_MENU_EDIT_DESCRIPTION);
		p_popup->add_separator();
	}

	if (no_paths) {
		_add_create_options(p_popup, String());
	} else if (single_path && p_display_path_dependent_options) {
		PopupMenu *new_menu = memnew(PopupMenu);
		_add_create_options(new_menu, p_paths[0].get_base_dir());
		new_menu->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_generic_rmb_option_selected));

		p_popup->add_submenu_node_item(TTRC("Create New"), new_menu, FILE_MENU_NEW);
		p_popup->set_item_icon(-1, get_editor_theme_icon(SNAME("Add")));
		p_popup->add_separator();
	}

	// Check if the root path is selected, we must check p_paths[1] because the first string in
	// the list of paths obtained by _tree_get_selected(...) is not always the root path.
	bool root_path_not_selected = !no_paths && p_paths[0] != "res://" && (p_paths.size() <= 1 || p_paths[1] != "res://");

	if (all_folders && foldernames.size() > 0) {
		p_popup->add_icon_item(get_editor_theme_icon(SNAME("Load")), TTRC("Expand Folder"), FILE_MENU_OPEN);

		if (foldernames.size() == 1) {
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("GuiTreeArrowDown")), TTRC("Expand Hierarchy"), FILE_MENU_EXPAND_ALL);
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("GuiTreeArrowRight")), TTRC("Collapse Hierarchy"), FILE_MENU_COLLAPSE_ALL);
		}

		p_popup->add_separator();

		// Only add the 'Set Folder Color...' option if the root path is not selected.
		if (root_path_not_selected) {
			PopupMenu *folder_colors_menu = memnew(PopupMenu);
			folder_colors_menu->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_folder_color_index_pressed).bind(folder_colors_menu));

			p_popup->add_submenu_node_item(TTRC("Set Folder Color..."), folder_colors_menu);
			p_popup->set_item_icon(-1, get_editor_theme_icon(SNAME("Paint")));

			folder_colors_menu->add_icon_item(get_editor_theme_icon(SNAME("Folder")), TTRC("Default (Reset)"));
			folder_colors_menu->set_item_icon_modulate(0, get_theme_color(SNAME("folder_icon_color"), SNAME("FileDialog")));
			folder_colors_menu->add_separator();

			for (const String &color_key : folder_color_order) {
				folder_colors_menu->add_icon_item(get_editor_theme_icon(SNAME("Folder")), _get_color_label(color_key));

				folder_colors_menu->set_item_icon_modulate(-1, editor_is_dark_icon_and_font ? folder_colors[color_key] : folder_colors[color_key] * 2);
				folder_colors_menu->set_item_metadata(-1, color_key);
			}
		}
	}

	// Add the options that are only available when a single item is selected.
	if (single_path) {
		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("ActionCopy")), ED_GET_SHORTCUT("filesystem_dock/copy_path"), FILE_MENU_COPY_PATH);
		p_popup->add_shortcut(ED_GET_SHORTCUT("filesystem_dock/copy_absolute_path"), FILE_MENU_COPY_ABSOLUTE_PATH);
		if (ResourceLoader::get_resource_uid(p_paths[0]) != ResourceUID::INVALID_ID) {
			p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Instance")), ED_GET_SHORTCUT("filesystem_dock/copy_uid"), FILE_MENU_COPY_UID);
		}
		if (root_path_not_selected) {
			p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Rename")), ED_GET_SHORTCUT("filesystem_dock/rename"), FILE_MENU_RENAME);
			p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Duplicate")), ED_GET_SHORTCUT("filesystem_dock/duplicate"), FILE_MENU_DUPLICATE);
		}
	}

	// Add the options that are only available when the root path is not selected.
	if (root_path_not_selected) {
		p_popup->add_icon_item(get_editor_theme_icon(SNAME("MoveUp")), TTRC("Move/Duplicate To..."), FILE_MENU_MOVE);
		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("ActionCopy")), ED_GET_SHORTCUT("filesystem_dock/copy"), FILE_MENU_COPY);
		p_popup->add_shortcut(ED_GET_SHORTCUT("filesystem_dock/cut"), FILE_MENU_CUT);
		if (!file_clipboard.is_empty()) {
			p_popup->add_shortcut(ED_GET_SHORTCUT("filesystem_dock/paste"), FILE_MENU_PASTE);
		}
		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Remove")), ED_GET_SHORTCUT("filesystem_dock/delete"), FILE_MENU_REMOVE);
	}

	// Only add a separator if we have actually placed any options in the menu since the last separator.
	if (single_path || root_path_not_selected) {
		p_popup->add_separator();
	}

	// Add the options that are available when one or more items are selected.
	if (p_paths.size() >= 1) {
		if (!all_favorites) {
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("Favorites")), TTRC("Add to Favorites"), FILE_MENU_ADD_FAVORITE);
		}
		if (!all_not_favorites) {
			p_popup->add_icon_item(get_editor_theme_icon(SNAME("NonFavorite")), TTRC("Remove from Favorites"), FILE_MENU_REMOVE_FAVORITE);
		}

		if (root_path_not_selected) {
			cached_valid_conversion_targets = _get_valid_conversions_for_file_paths(p_paths);

			int relative_id = 0;
			if (!cached_valid_conversion_targets.is_empty()) {
				p_popup->add_separator();

				// If we have more than one type we can convert into, collapse it into a submenu.
				const int CONVERSION_SUBMENU_THRESHOLD = 1;

				PopupMenu *container_menu = p_popup;
				String conversion_string_template = "Convert to %s";

				if (cached_valid_conversion_targets.size() > CONVERSION_SUBMENU_THRESHOLD) {
					container_menu = memnew(PopupMenu);
					container_menu->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_generic_rmb_option_selected));

					p_popup->add_submenu_node_item(TTRC("Convert to..."), container_menu, FILE_MENU_NEW);
					conversion_string_template = "%s";
				}

				for (const String &E : cached_valid_conversion_targets) {
					Ref<Texture2D> icon;
					if (has_theme_icon(E, SNAME("EditorIcons"))) {
						icon = get_editor_theme_icon(E);
					} else {
						icon = get_editor_theme_icon(SNAME("Object"));
					}

					container_menu->add_icon_item(icon, vformat(TTR(conversion_string_template), E), CONVERT_BASE_ID + relative_id);
					relative_id++;
				}
			}
		}

		{
			List<String> resource_extensions;
			ResourceFormatImporter::get_singleton()->get_recognized_extensions_for_type("Resource", &resource_extensions);
			HashSet<String> extension_list;
			for (const String &extension : resource_extensions) {
				extension_list.insert(extension);
			}

			bool resource_valid = true;
			String main_extension;

			for (int i = 0; i != p_paths.size(); ++i) {
				String extension = p_paths[i].get_extension();
				if (extension_list.has(extension)) {
					if (main_extension.is_empty()) {
						main_extension = extension;
					} else if (extension != main_extension) {
						resource_valid = false;
						break;
					}
				} else {
					resource_valid = false;
					break;
				}
			}

			if (resource_valid) {
				p_popup->add_icon_item(get_editor_theme_icon(SNAME("Load")), TTRC("Reimport"), FILE_MENU_REIMPORT);
			}
		}
	}

	if (single_path) {
		const String &fpath = p_paths[0];

		[[maybe_unused]] bool added_separator = false;

		if (favorites_list.has(fpath)) {
			TreeItem *cursor_item = tree->get_selected();
			bool is_item_in_favorites = false;
			while (cursor_item != nullptr) {
				if (cursor_item == favorites_item) {
					is_item_in_favorites = true;
					break;
				}

				cursor_item = cursor_item->get_parent();
			}

			if (is_item_in_favorites) {
				p_popup->add_separator();
				added_separator = true;
				p_popup->add_icon_item(get_editor_theme_icon(SNAME("ShowInFileSystem")), TTRC("Show in Explore"), FILE_MENU_SHOW_IN_FILESYSTEM);
			}
		}

#if !defined(ANDROID_ENABLED) && !defined(WEB_ENABLED)
		if (!added_separator) {
			p_popup->add_separator();
			added_separator = true;
		}

		// Opening the system file manager is not supported on the Android and web editors.
		const bool is_directory = fpath.ends_with("/");

		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Terminal")), ED_GET_SHORTCUT("filesystem_dock/open_in_terminal"), FILE_MENU_OPEN_IN_TERMINAL);
		p_popup->set_item_text(p_popup->get_item_index(FILE_MENU_OPEN_IN_TERMINAL), is_directory ? TTRC("Open in Terminal") : TTRC("Open Folder in Terminal"));

		if (!is_directory) {
			p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("ExternalLink")), ED_GET_SHORTCUT("filesystem_dock/open_in_external_program"), FILE_MENU_OPEN_EXTERNAL);
		}

		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Filesystem")), ED_GET_SHORTCUT("filesystem_dock/show_in_explorer"), FILE_MENU_SHOW_IN_EXPLORER);
		p_popup->set_item_text(p_popup->get_item_index(FILE_MENU_SHOW_IN_EXPLORER), is_directory ? OS::get_singleton()->get_platform_string(OS::PLATFORM_STRING_FILE_MANAGER_OPEN) : OS::get_singleton()->get_platform_string(OS::PLATFORM_STRING_FILE_MANAGER_SHOW));
#endif

		current_path = fpath;
	} else if (no_paths) {
		if (!file_clipboard.is_empty()) {
			p_popup->add_shortcut(ED_GET_SHORTCUT("filesystem_dock/paste"), FILE_MENU_PASTE);
		}
#if !defined(ANDROID_ENABLED) && !defined(WEB_ENABLED)
		tree_popup->add_separator();
		tree_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Terminal")), ED_GET_SHORTCUT("filesystem_dock/open_in_terminal"), FILE_MENU_OPEN_IN_TERMINAL);
		tree_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("Filesystem")), ED_GET_SHORTCUT("filesystem_dock/show_in_explorer"), FILE_MENU_SHOW_IN_EXPLORER);
#endif
	}

#if !defined(ANDROID_ENABLED) && !defined(WEB_ENABLED)
	if (all_files && p_paths.size() > 1) {
		p_popup->add_separator();
		p_popup->add_icon_shortcut(get_editor_theme_icon(SNAME("ExternalLink")), ED_GET_SHORTCUT("filesystem_dock/open_in_external_program"), FILE_MENU_OPEN_EXTERNAL);
	}
#endif
	EditorContextMenuPluginManager::get_singleton()->add_options_from_plugins(p_popup, EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, p_paths);
}

void FileSystemDock::_add_create_options(PopupMenu *p_popup, const String &p_base_folder) {
	bool prefix_new = p_base_folder.is_empty();
	p_popup->add_icon_item(get_editor_theme_icon(SNAME("Folder")), prefix_new ? TTRC("New Folder...") : TTRC("Folder..."), FILE_MENU_NEW_FOLDER);
	p_popup->set_item_shortcut(-1, ED_GET_SHORTCUT("filesystem_dock/new_folder"));
	p_popup->add_icon_item(get_editor_theme_icon(SNAME("PackedScene")), prefix_new ? TTRC("New Scene...") : TTRC("Scene..."), FILE_MENU_NEW_SCENE);
	p_popup->set_item_shortcut(-1, ED_GET_SHORTCUT("filesystem_dock/new_scene"));
	p_popup->add_icon_item(get_editor_theme_icon(SNAME("Script")), prefix_new ? TTRC("New Script...") : TTRC("Script..."), FILE_MENU_NEW_SCRIPT);
	p_popup->set_item_shortcut(-1, ED_GET_SHORTCUT("filesystem_dock/new_script"));
	p_popup->add_icon_item(get_editor_theme_icon(SNAME("Object")), prefix_new ? TTRC("New Resource...") : TTRC("Resource..."), FILE_MENU_NEW_RESOURCE);
	p_popup->set_item_shortcut(-1, ED_GET_SHORTCUT("filesystem_dock/new_resource"));
	p_popup->add_icon_item(get_editor_theme_icon(SNAME("TextFile")), prefix_new ? TTRC("New TextFile...") : TTRC("TextFile..."), FILE_MENU_NEW_TEXTFILE);
	p_popup->set_item_shortcut(-1, ED_GET_SHORTCUT("filesystem_dock/new_textfile"));
	// Options for CONTEXT_SLOT_FILESYSTEM_CREATE are added with an offset, to avoid conflicts in case plugins add options for both FileSystem slots.
	EditorContextMenuPluginManager::get_singleton()->add_options_from_plugins(p_popup, EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM_CREATE, prefix_new ? PackedStringArray() : PackedStringArray{ p_base_folder }, 500);
}

void FileSystemDock::_tree_rmb_select(const Vector2 &p_pos, MouseButton p_button) {
	if (p_button != MouseButton::RIGHT) {
		return;
	}
	tree->grab_focus(true);

	// Right click is pressed in the tree.
	Vector<String> paths = _tree_get_selected(false);

	tree_popup->clear();

	// Popup.
	if (!paths.is_empty()) {
		tree_popup->reset_size();
		_file_and_folders_fill_popup(tree_popup, paths);
		tree_popup->set_position(tree->get_screen_position() + p_pos);
		tree_popup->reset_size();
		tree_popup->popup();
	}
}

void FileSystemDock::_tree_empty_click(const Vector2 &p_pos, MouseButton p_button) {
	if (p_button != MouseButton::RIGHT) {
		return;
	}
	// Right click is pressed in the empty space of the tree.
	current_path = "res://";
	tree_popup->clear();
	_file_and_folders_fill_popup(tree_popup, PackedStringArray());
	tree_popup->set_position(tree->get_screen_position() + p_pos);
	tree_popup->reset_size();
	tree_popup->popup();
}

void FileSystemDock::_tree_empty_selected() {
	tree->deselect_all();
	current_path = "";
	current_path_line_edit->set_text(current_path);
	if (file_list_vb->is_visible()) {
		_update_file_list(false);
	}
	_update_selection_changed();
}

void FileSystemDock::_file_list_item_clicked(int p_item, const Vector2 &p_pos, MouseButton p_mouse_button_index) {
	if (p_mouse_button_index != MouseButton::RIGHT) {
		return;
	}
	files->grab_focus(true);

	// Right click is pressed in the file list.
	Vector<String> paths;
	for (int i = 0; i < files->get_item_count(); i++) {
		if (!files->is_selected(i)) {
			continue;
		}
		if (files->get_item_text(p_item) == "..") {
			files->deselect(i);
			continue;
		}
		paths.push_back(files->get_item_metadata(i));
	}

	// Popup.
	if (!paths.is_empty()) {
		file_list_popup->clear();
		_file_and_folders_fill_popup(file_list_popup, paths, searched_tokens.is_empty());
		file_list_popup->set_position(files->get_screen_position() + p_pos);
		file_list_popup->reset_size();
		file_list_popup->popup();
	}
}

void FileSystemDock::_file_list_empty_clicked(const Vector2 &p_pos, MouseButton p_mouse_button_index) {
	if (p_mouse_button_index != MouseButton::RIGHT) {
		return;
	}

	// Right click on empty space for file list.
	if (!searched_tokens.is_empty()) {
		return;
	}

	current_path = current_path_line_edit->get_text();

	// Favorites isn't a directory so don't show menu.
	if (current_path == "Favorites") {
		return;
	}

	file_list_popup->clear();
	_file_and_folders_fill_popup(file_list_popup, PackedStringArray());
	file_list_popup->set_position(files->get_screen_position() + p_pos);
	file_list_popup->reset_size();
	file_list_popup->popup();
}

void FileSystemDock::select_file(const String &p_file) {
	_navigate_to_path(p_file);
}

void FileSystemDock::_file_multi_selected(int p_index, bool p_selected) {
	// Set the path to the current focused item.
	int current = files->get_current();
	if (current == p_index) {
		String fpath = files->get_item_metadata(current);
		if (!fpath.ends_with("/")) {
			current_path = fpath;
			current_path_line_edit->set_text(fpath);
		}
	}

	// Update the import dock.
	import_dock_needs_update = true;
	callable_mp(this, &FileSystemDock::_update_import_dock).call_deferred();
}

void FileSystemDock::_update_selection_changed() {
	Vector<String> selection;
	selection.append_array(_tree_get_selected());
	selection.append_array(_file_list_get_selected());
	if (prev_selection != selection) {
		prev_selection = selection;
		emit_signal(SNAME("selection_changed"));
	}
}

void FileSystemDock::_tree_mouse_exited() {
	if (holding_branch) {
		_reselect_items_selected_on_drag_begin();
	}
}

void FileSystemDock::_reselect_items_selected_on_drag_begin(bool reset) {
	TreeItem *selected_item = tree->get_next_selected(tree->get_root());
	if (selected_item) {
		selected_item->deselect(0);
	}
	if (!tree_items_selected_on_drag_begin.is_empty()) {
		bool reselected = false;
		for (TreeItem *item : tree_items_selected_on_drag_begin) {
			if (item->get_tree()) {
				item->select(0);
				reselected = true;
			}
		}

		if (reset) {
			tree_items_selected_on_drag_begin.clear();
		}

		if (!reselected) {
			// If couldn't reselect the items selected on drag begin, select the "res://" item.
			tree->get_root()->get_child(1)->select(0);
		}
	}

	files->deselect_all();
	if (!list_items_selected_on_drag_begin.is_empty()) {
		for (const int idx : list_items_selected_on_drag_begin) {
			files->select(idx, false);
		}

		if (reset) {
			list_items_selected_on_drag_begin.clear();
		}
	}
}

void FileSystemDock::_tree_gui_input(Ref<InputEvent> p_event) {
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		TreeItem *item = tree->get_item_at_position(mm->get_position());
		if (item && holding_branch) {
			String fpath = item->get_metadata(0);
			while (!fpath.ends_with("/") && fpath != "res://" && item->get_parent()) { // Find the parent folder tree item.
				item = item->get_parent();
				fpath = item->get_metadata(0);
			}

			TreeItem *deselect_item = tree->get_next_selected(tree->get_root());
			while (deselect_item) {
				deselect_item->deselect(0);
				deselect_item = tree->get_next_selected(deselect_item);
			}
			item->select(0);

			if (display_mode != DisplayMode::DISPLAY_MODE_TREE_ONLY) {
				files->deselect_all();
				// Try to select the corresponding file list item.
				const int files_item_idx = files->find_metadata(fpath);
				if (files_item_idx != -1) {
					files->select(files_item_idx);
				}
			}
		}
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		int option_id = _get_menu_option_from_key(key);
		if (option_id > -1) {
			_tree_rmb_option(option_id);
		} else {
			bool create = false;
			Callable custom_callback = EditorContextMenuPluginManager::get_singleton()->match_custom_shortcut(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, p_event);
			if (!custom_callback.is_valid()) {
				create = true;
				custom_callback = EditorContextMenuPluginManager::get_singleton()->match_custom_shortcut(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM_CREATE, p_event);
			}

			if (custom_callback.is_valid()) {
				Vector<String> selected = _tree_get_selected(false);
				if (create) {
					if (selected.is_empty()) {
						selected.append("res://");
					} else if (selected.size() == 1) {
						selected.write[0] = selected[0].get_base_dir();
					} else {
						return;
					}
				}
				EditorContextMenuPluginManager::get_singleton()->invoke_callback(custom_callback, selected);
			} else {
				return;
			}
		}

		accept_event();
	}
}

void FileSystemDock::_file_list_gui_input(Ref<InputEvent> p_event) {
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && holding_branch) {
		const int item_idx = files->get_item_at_position(mm->get_position(), true);
		files->deselect_all();
		String fpath;
		if (item_idx != -1) {
			fpath = files->get_item_metadata(item_idx);
			if (fpath.ends_with("/") || fpath == "res://") {
				files->select(item_idx);
			}
		} else {
			fpath = get_current_directory();
		}

		TreeItem *deselect_item = tree->get_next_selected(tree->get_root());
		while (deselect_item) {
			deselect_item->deselect(0);
			deselect_item = tree->get_next_selected(deselect_item);
		}

		// Try to select the corresponding tree item.
		TreeItem *tree_item = (item_idx != -1) ? tree->get_item_with_text(files->get_item_text(item_idx)) : nullptr;

		if (tree_item) {
			tree_item->select(0);
		} else {
			// Find parent folder.
			fpath = fpath.substr(0, fpath.rfind_char('/') + 1);
			if (fpath.size() > String("res://").size()) {
				fpath = fpath.left(fpath.size() - 2); // Remove last '/'.
				const int slash_idx = fpath.rfind_char('/');
				fpath = fpath.substr(slash_idx + 1);
			}

			tree_item = tree->get_item_with_text(fpath);
			if (tree_item) {
				tree_item->select(0);
			}
		}
	}

	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		int option_id = _get_menu_option_from_key(key);
		if (option_id > -1) {
			_file_list_rmb_option(option_id);
		} else {
			Callable custom_callback = EditorContextMenuPluginManager::get_singleton()->match_custom_shortcut(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, p_event);
			if (!custom_callback.is_valid()) {
				custom_callback = EditorContextMenuPluginManager::get_singleton()->match_custom_shortcut(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM_CREATE, p_event);
			}

			if (custom_callback.is_valid()) {
				EditorContextMenuPluginManager::get_singleton()->invoke_callback(custom_callback, _file_list_get_selected());
			} else {
				return;
			}
		}

		accept_event();
	}
}

bool FileSystemDock::_get_imported_files(const String &p_path, String &r_extension, Vector<String> &r_files) const {
	if (!p_path.ends_with("/")) {
		if (FileAccess::exists(p_path + ".import")) {
			if (r_extension.is_empty()) {
				r_extension = p_path.get_extension();
			} else if (r_extension != p_path.get_extension()) {
				r_files.clear();
				return false; // File type mismatch, stop search.
			}

			r_files.push_back(p_path);
		}
		return true;
	}

	Ref<DirAccess> da = DirAccess::open(p_path);
	ERR_FAIL_COND_V(da.is_null(), false);

	da->list_dir_begin();
	String n = da->get_next();
	while (!n.is_empty()) {
		if (n != "." && n != ".." && !n.ends_with(".import")) {
			String npath = p_path + n + (da->current_is_dir() ? "/" : "");
			if (!_get_imported_files(npath, r_extension, r_files)) {
				return false;
			}
		}
		n = da->get_next();
	}
	da->list_dir_end();
	return true;
}

void FileSystemDock::_update_import_dock() {
	if (!import_dock_needs_update) {
		return;
	}

	_update_selection_changed();

	// ItemList and Tree change selection on mouse-down, which would replace the adjacent Resource
	// inspector before the selected file can be dragged into one of its fields. Mouse selection only
	// commits to the detail panels after release; an actual FileSystem drag discards this pending update.
	if (filesystem_drag_preserves_details) {
		return;
	}
	if (Input::get_singleton()->is_mouse_button_pressed(MouseButton::LEFT)) {
		details_update_waiting_for_mouse_release = true;
		set_process_internal(true);
		return;
	}
	details_update_waiting_for_mouse_release = false;
	set_process_internal(false);

	// List selected.
	Vector<String> selected;
	if (display_mode == DISPLAY_MODE_TREE_ONLY) {
		// Use the tree
		selected = _tree_get_selected();

	} else {
		// Use the file list.
		for (int i = 0; i < files->get_item_count(); i++) {
			if (!files->is_selected(i)) {
				continue;
			}

			selected.push_back(files->get_item_metadata(i));
		}
	}

	// Resource properties follow the same effective FileSystem selection as Import: the tree in
	// tree-only mode, and the file list in split mode (where the tree still selects the containing folder).
	if (ResourceInspectorDock::get_singleton()) {
		if (selected.size() == 1) {
			ResourceInspectorDock::get_singleton()->set_edit_path(selected[0]);
		} else {
			ResourceInspectorDock::get_singleton()->clear();
		}
	}

	if (!selected.is_empty() && selected[0] == "res://") {
		// Scanning res:// is costly and unlikely to yield any useful results.
		return;
	}

	// Expand directory selection.
	Vector<String> efiles;
	String extension;
	for (const String &fpath : selected) {
		_get_imported_files(fpath, extension, efiles);
	}

	// Check import.
	Vector<String> imports;
	String import_type;
	for (int i = 0; i < efiles.size(); i++) {
		const String &fpath = efiles[i];
		Ref<ConfigFile> cf;
		cf.instantiate();
		Error err = cf->load(fpath + ".import");
		if (err != OK) {
			imports.clear();
			break;
		}

		String type;
		if (cf->has_section_key("remap", "type")) {
			type = cf->get_value("remap", "type");
		}
		if (import_type.is_empty()) {
			import_type = type;
		} else if (import_type != type) {
			// All should be the same type.
			imports.clear();
			break;
		}
		imports.push_back(fpath);
	}

	if (imports.is_empty()) {
		ImportDock::get_singleton()->clear();
	} else if (imports.size() == 1) {
		ImportDock::get_singleton()->set_edit_path(imports[0]);
	} else {
		ImportDock::get_singleton()->set_edit_multiple_paths(imports);
	}

	import_dock_needs_update = false;
}

void FileSystemDock::_feature_profile_changed() {
	_update_display_mode(true);
}

void FileSystemDock::_project_settings_changed() {
	_load_color_customization();
	_rebuild_category_rail();
	_update_color_filter_view();

	const String &current_main_scene_path = ResourceUID::ensure_path(GLOBAL_GET("application/run/main_scene"));
	if (main_scene_path != current_main_scene_path) {
		main_scene_path = current_main_scene_path;
		update_all();
	}
}

void FileSystemDock::set_file_sort(FileSortOption p_file_sort) {
	for (int i = 0; i != (int)FileSortOption::FILE_SORT_MAX; i++) {
		tree_button_sort->get_popup()->set_item_checked(i, (i == (int)p_file_sort));
		file_list_button_sort->get_popup()->set_item_checked(i, (i == (int)p_file_sort));
	}
	file_sort = p_file_sort;

	// Update everything needed.
	update_all();
}

void FileSystemDock::_file_sort_popup(int p_id) {
	set_file_sort((FileSortOption)p_id);
}

// TODO: Could use a unit test.
Color FileSystemDock::get_dir_icon_color(const String &p_dir_path, const Color &p_default) {
	if (!singleton) { // This method can be called from the project manager.
		return p_default;
	}
	Color folder_icon_color = p_default;

	// Check for a folder color to inherit (if one is assigned).
	String parent_dir = ProjectSettings::get_singleton()->localize_path(p_dir_path);
	while (!parent_dir.is_empty() && parent_dir != "res://") {
		if (!parent_dir.ends_with("/")) {
			parent_dir += "/";
		}

		const String color_name = singleton->assigned_folder_colors.get(parent_dir, String());
		if (!color_name.is_empty()) {
			folder_icon_color = singleton->folder_colors[color_name];
			break;
		}
		parent_dir = parent_dir.trim_suffix("/").get_base_dir();
	}
	return folder_icon_color;
}

const HashMap<String, Color> &FileSystemDock::get_folder_colors() const {
	return folder_colors;
}

Dictionary FileSystemDock::get_assigned_folder_colors() const {
	return assigned_folder_colors;
}

MenuButton *FileSystemDock::_create_file_menu_button() {
	MenuButton *button = memnew(MenuButton);
	button->set_flat(false);
	button->set_theme_type_variation("FlatMenuButton");
	button->set_tooltip_text(TTRC("Sort Files"));
	button->set_accessibility_name(TTRC("Sort Files"));

	PopupMenu *p = button->get_popup();
	p->connect(SceneStringName(id_pressed), callable_mp(this, &FileSystemDock::_file_sort_popup));
	p->add_radio_check_item(TTRC("Sort by Name (Ascending)"), (int)FileSortOption::FILE_SORT_NAME);
	p->add_radio_check_item(TTRC("Sort by Name (Descending)"), (int)FileSortOption::FILE_SORT_NAME_REVERSE);
	p->add_radio_check_item(TTRC("Sort by Type (Ascending)"), (int)FileSortOption::FILE_SORT_TYPE);
	p->add_radio_check_item(TTRC("Sort by Type (Descending)"), (int)FileSortOption::FILE_SORT_TYPE_REVERSE);
	p->add_radio_check_item(TTRC("Sort by Last Modified"), (int)FileSortOption::FILE_SORT_MODIFIED_TIME);
	p->add_radio_check_item(TTRC("Sort by First Modified"), (int)FileSortOption::FILE_SORT_MODIFIED_TIME_REVERSE);
	p->set_item_checked((int)file_sort, true);
	return button;
}

void FileSystemDock::update_layout(EditorDock::DockLayout p_layout, int p_slot) {
	bool new_horizontal = (p_layout == EditorDock::DOCK_LAYOUT_HORIZONTAL);
	bool new_touches_bottom = (p_slot != EditorDock::DOCK_SLOT_BOTTOM);
	if (horizontal == new_horizontal && touches_bottom == new_touches_bottom) {
		return;
	}
	horizontal = new_horizontal;
	touches_bottom = new_touches_bottom;

	if (horizontal) {
		path_hb->reparent(toolbar_hbc);
		toolbar_hbc->move_child(path_hb, 2);
		set_meta("_dock_display_mode", get_display_mode());
		set_meta("_dock_file_display_mode", get_file_list_display_mode());

		FileSystemDock::DisplayMode new_display_mode = FileSystemDock::DisplayMode(int(get_meta("_bottom_display_mode", int(FileSystemDock::DISPLAY_MODE_HSPLIT))));
		FileSystemDock::FileListDisplayMode new_file_display_mode = FileSystemDock::FileListDisplayMode(int(get_meta("_bottom_file_display_mode", int(FileSystemDock::FILE_LIST_DISPLAY_THUMBNAILS))));

		set_display_mode(new_display_mode);
		set_file_list_display_mode(new_file_display_mode);
		set_custom_minimum_size(Size2(0, 200) * EDSCALE);
	} else {
		path_hb->reparent(file_list_vb);
		file_list_vb->move_child(path_hb, 0);
		set_meta("_bottom_display_mode", get_display_mode());
		set_meta("_bottom_file_display_mode", get_file_list_display_mode());

		FileSystemDock::DisplayMode new_display_mode = FileSystemDock::DISPLAY_MODE_TREE_ONLY;
		FileSystemDock::FileListDisplayMode new_file_display_mode = FileSystemDock::FILE_LIST_DISPLAY_LIST;

		new_display_mode = FileSystemDock::DisplayMode(int(get_meta("_dock_display_mode", new_display_mode)));
		new_file_display_mode = FileSystemDock::FileListDisplayMode(int(get_meta("_dock_file_display_mode", new_file_display_mode)));

		set_display_mode(new_display_mode);
		set_file_list_display_mode(new_file_display_mode);
		set_custom_minimum_size(Size2(0, 0));
	}
}

void FileSystemDock::save_layout_to_config(Ref<ConfigFile> &p_layout, const String &p_section) const {
	p_layout->set_value(p_section, "h_split_offset", get_h_split_offset());
	p_layout->set_value(p_section, "v_split_offset", get_v_split_offset());
	p_layout->set_value(p_section, "category_wide_split_offset", category_wide_split_offset);
	p_layout->set_value(p_section, "category_narrow_split_offset", category_narrow_split_offset);
	p_layout->set_value(p_section, "display_mode", get_display_mode());
	p_layout->set_value(p_section, "file_sort", (int)get_file_sort());
	p_layout->set_value(p_section, "file_list_display_mode", get_file_list_display_mode());
	const Vector<String> selected_paths = _is_color_collection_active() && category_restore_state_valid ? category_restore_selection : get_selected_paths();
	const Vector<String> uncollapsed_paths = _is_color_collection_active() && category_restore_state_valid ? category_restore_uncollapsed_paths : (searched_tokens.is_empty() ? get_uncollapsed_paths() : uncollapsed_paths_before_search);
	p_layout->set_value(p_section, "selected_paths", selected_paths);
	p_layout->set_value(p_section, "uncollapsed_paths", uncollapsed_paths);

	PackedStringArray active_categories;
	for (const String &color_key : folder_color_order) {
		if (active_color_filter.has(color_key)) {
			active_categories.push_back(color_key);
		}
	}
	p_layout->set_value(p_section, "active_categories", active_categories);
	p_layout->set_value(p_section, "category_restore_path", category_restore_state_valid ? category_restore_path : current_path);
	p_layout->set_value(p_section, "category_scope_path", category_scope_path);
}

void FileSystemDock::load_layout_from_config(const Ref<ConfigFile> &p_layout, const String &p_section) {
	if (p_layout->has_section_key(p_section, "h_split_offset")) {
		int fs_h_split_ofs = p_layout->get_value(p_section, "h_split_offset");
		set_h_split_offset(fs_h_split_ofs);
	}

	if (p_layout->has_section_key(p_section, "v_split_offset")) {
		int fs_v_split_ofs = p_layout->get_value(p_section, "v_split_offset");
		set_v_split_offset(fs_v_split_ofs);
	}

	category_wide_split_offset = p_layout->get_value(p_section, "category_wide_split_offset", category_wide_split_offset);
	category_narrow_split_offset = p_layout->get_value(p_section, "category_narrow_split_offset", category_narrow_split_offset);
	category_wide_split->set_split_offset(category_wide_split_offset);
	category_narrow_split->set_split_offset(CLAMP(category_narrow_split_offset, int(80 * EDSCALE), int(160 * EDSCALE)));

	if (p_layout->has_section_key(p_section, "display_mode")) {
		DisplayMode dock_filesystem_display_mode = DisplayMode(int(p_layout->get_value(p_section, "display_mode")));
		set_display_mode(dock_filesystem_display_mode);
	}

	if (p_layout->has_section_key(p_section, "file_sort")) {
		FileSortOption dock_filesystem_file_sort = FileSortOption(int(p_layout->get_value(p_section, "file_sort")));
		set_file_sort(dock_filesystem_file_sort);
	}

	if (p_layout->has_section_key(p_section, "file_list_display_mode")) {
		FileListDisplayMode dock_filesystem_file_list_display_mode = FileListDisplayMode(int(p_layout->get_value(p_section, "file_list_display_mode")));
		set_file_list_display_mode(dock_filesystem_file_list_display_mode);
	}

	// Restore uncollapsed state.
	{
		PackedStringArray uncollapsed_tis;
		if (p_layout->has_section_key(p_section, "uncollapsed_paths")) {
			uncollapsed_tis = p_layout->get_value(p_section, "uncollapsed_paths");
		} else {
			uncollapsed_tis = { "res://" };
		}

		TreeItem *item = tree->get_item_with_metadata("res://", 0);
		item->set_collapsed_recursive(true);
		LocalVector<TreeItem *> ti_visit;
		ti_visit.push_back(item);

		// BFS to uncollapse items (skipping those in favorites).
		while (!ti_visit.is_empty()) {
			TreeItem *curr_ti = ti_visit[0];
			const String &path = curr_ti->get_metadata(0);

			if (uncollapsed_tis.has(path)) {
				curr_ti->set_collapsed(false);

				uncollapsed_tis.erase(path);
				if (uncollapsed_tis.is_empty()) {
					break;
				}
			}

			for (TreeItem *child_ti = curr_ti->get_first_child(); child_ti; child_ti = child_ti->get_next()) {
				ti_visit.push_back(child_ti);
			}
			ti_visit.remove_at(0);
		}
	}

	tree->deselect_all();
	files->deselect_all();
	current_path = "";

	if (p_layout->has_section_key(p_section, "selected_paths")) {
		PackedStringArray dock_filesystem_selected_paths = p_layout->get_value(p_section, "selected_paths");

		if (dock_filesystem_selected_paths.size() > 1) {
			Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
			Vector<String> files_to_select;
			Vector<String> dirs_to_select;

			// Properly allocate the selections between the views.
			for (const String &path : dock_filesystem_selected_paths) {
				if (da->file_exists(path)) {
					if (display_mode == DISPLAY_MODE_TREE_ONLY) {
						dirs_to_select.append(path);
					} else {
						files_to_select.append(path);
					}
				} else if (da->dir_exists(path)) {
					dirs_to_select.append(path);
				}
			}

			if (files_to_select.is_empty() && dirs_to_select.is_empty()) {
				select_file("res://"); // No valid file to select, default to root folder.
			} else {
				TreeItem *item = tree->get_item_with_metadata("res://", 0);
				LocalVector<TreeItem *> ti_visit = { item };
				bool first_selection = true;

				// BFS to select items (skipping those in favorites).
				while (!ti_visit.is_empty()) {
					TreeItem *curr_ti = ti_visit[0];
					const String &path = curr_ti->get_metadata(0);

					if (dirs_to_select.has(path)) {
						curr_ti->select(0, first_selection);
						if (first_selection) {
							first_selection = false;
							current_path = curr_ti->get_metadata(0);
							tree->ensure_cursor_is_visible();
						}

						dirs_to_select.erase(path);
						if (dirs_to_select.is_empty()) {
							break;
						}
					}

					if (!curr_ti->is_collapsed()) {
						for (TreeItem *child_ti = curr_ti->get_first_child(); child_ti; child_ti = child_ti->get_next()) {
							ti_visit.push_back(child_ti);
						}
					}
					ti_visit.remove_at(0);
				}

				if (display_mode != DISPLAY_MODE_TREE_ONLY) {
					// The folders not found could be from the selected folder.
					files_to_select.append_array(dirs_to_select);

					_update_file_list(!files_to_select.is_empty(), files_to_select);

					for (const int idx : files->get_selected_items()) {
						const String &path = files->get_item_metadata(idx);
						// Subfolders shouldn't be set as the current path.
						if (!path.ends_with("/")) {
							current_path = path;
							break;
						}
					}
				}

				current_path_line_edit->set_text(current_path);
			}
		} else if (dock_filesystem_selected_paths.size() == 1) {
			Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
			const String &path = dock_filesystem_selected_paths[0];

			if (da->file_exists(path) || da->dir_exists(path)) {
				select_file(path);
			} else {
				select_file("res://"); // For single-selection, default to root folder.
			}
		}
	}

	active_color_filter.clear();
	const PackedStringArray active_categories = p_layout->get_value(p_section, "active_categories", PackedStringArray());
	for (const String &color_key : active_categories) {
		if (folder_colors.has(color_key)) {
			active_color_filter.insert(color_key);
		}
	}
	if (_is_color_collection_active()) {
		category_restore_path = p_layout->get_value(p_section, "category_restore_path", current_path);
		category_restore_selection = p_layout->get_value(p_section, "selected_paths", PackedStringArray());
		PackedStringArray restore_uncollapsed = p_layout->get_value(p_section, "uncollapsed_paths", PackedStringArray());
		if (restore_uncollapsed.is_empty()) {
			restore_uncollapsed.push_back("res://");
		}
		category_restore_uncollapsed_paths = restore_uncollapsed;
		category_restore_state_valid = true;
		category_scope_path = p_layout->get_value(p_section, "category_scope_path", String());
		if (!category_scope_path.is_empty() && DirAccess::dir_exists_absolute(ProjectSettings::get_singleton()->globalize_path(category_scope_path))) {
			current_path = category_scope_path;
		} else {
			category_scope_path.clear();
			current_path = category_restore_path;
		}
	}
	_rebuild_category_rail();
	_update_color_filter_view();
}

void FileSystemDock::_on_open_editor_settings_file_exts() {
	unrecognized_ext_dialog->hide();

	// The FileSystem settings are under "advanced settings", so we have to ensure
	// that setting is enabled before we attempt to open the menu to them.
	EditorSettingsDialog *ed_settings = EditorNode::get_singleton()->editor_settings_dialog;
	ed_settings->set_advanced_mode_enabled(true);
	ed_settings->popup_edit_settings();
	ed_settings->set_current_section("docks/filesystem");
}

void FileSystemDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("navigate_to_path", "path"), &FileSystemDock::navigate_to_path);
	ClassDB::bind_method(D_METHOD("open_scene_in_level_editor", "path"), &FileSystemDock::open_scene_in_level_editor);

	ClassDB::bind_method(D_METHOD("add_resource_tooltip_plugin", "plugin"), &FileSystemDock::add_resource_tooltip_plugin);
	ClassDB::bind_method(D_METHOD("remove_resource_tooltip_plugin", "plugin"), &FileSystemDock::remove_resource_tooltip_plugin);

	ADD_SIGNAL(MethodInfo("inherit", PropertyInfo(Variant::STRING, "file")));
	ADD_SIGNAL(MethodInfo("instantiate", PropertyInfo(Variant::PACKED_STRING_ARRAY, "files")));

	ADD_SIGNAL(MethodInfo("resource_removed", PropertyInfo(Variant::OBJECT, "resource", PROPERTY_HINT_RESOURCE_TYPE, Resource::get_class_static())));
	ADD_SIGNAL(MethodInfo("file_removed", PropertyInfo(Variant::STRING, "file")));
	ADD_SIGNAL(MethodInfo("folder_removed", PropertyInfo(Variant::STRING, "folder")));
	ADD_SIGNAL(MethodInfo("files_moved", PropertyInfo(Variant::STRING, "old_file"), PropertyInfo(Variant::STRING, "new_file")));
	ADD_SIGNAL(MethodInfo("folder_moved", PropertyInfo(Variant::STRING, "old_folder"), PropertyInfo(Variant::STRING, "new_folder")));
	ADD_SIGNAL(MethodInfo("folder_color_changed"));
	ADD_SIGNAL(MethodInfo("selection_changed"));

	ADD_SIGNAL(MethodInfo("display_mode_changed"));
}

FileSystemDock::FileSystemDock() {
	singleton = this;
	set_name(TTRC("Explore"));
	set_icon_name("Folder");
	// Registered by EditorNode before this unmanaged drawer-hosted dock is constructed.
	set_dock_shortcut(ED_GET_SHORTCUT("docks/open_filesystem"));
	set_default_slot(EditorDock::DOCK_SLOT_LEFT_BR);
	set_available_layouts(DOCK_LAYOUT_ALL);

	ProjectSettings::get_singleton()->add_hidden_prefix("file_customization/");

	// `KeyModifierMask::CMD_OR_CTRL | Key::C` conflicts with other editor shortcuts.
	ED_SHORTCUT("filesystem_dock/copy_path", TTRC("Copy Path"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::SHIFT | Key::C);
	ED_SHORTCUT("filesystem_dock/copy_absolute_path", TTRC("Copy Absolute Path"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::ALT | Key::C);
	ED_SHORTCUT("filesystem_dock/copy_uid", TTRC("Copy UID"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::ALT | KeyModifierMask::SHIFT | Key::C);
	ED_SHORTCUT("filesystem_dock/duplicate", TTRC("Duplicate..."), KeyModifierMask::CMD_OR_CTRL | Key::D);
	// Plain Ctrl+C/X/V copy/cut/paste files. These only fire while the FileSystem dock's tree/list
	// is focused (routed through _get_menu_option_from_key in gui_input), and SceneTreeDock now
	// stands down its own context-less Ctrl+C/X/V while this dock has focus, so there's no clash.
	ED_SHORTCUT("filesystem_dock/copy", TTRC("Copy"), KeyModifierMask::CMD_OR_CTRL | Key::C);
	ED_SHORTCUT("filesystem_dock/cut", TTRC("Cut"), KeyModifierMask::CMD_OR_CTRL | Key::X);
	ED_SHORTCUT("filesystem_dock/paste", TTRC("Paste"), KeyModifierMask::CMD_OR_CTRL | Key::V);
	ED_SHORTCUT("filesystem_dock/delete", TTRC("Delete"), Key::KEY_DELETE);
	ED_SHORTCUT("filesystem_dock/new_folder", TTRC("New Folder..."), Key::NONE);
	ED_SHORTCUT("filesystem_dock/new_scene", TTRC("New Scene..."), Key::NONE);
	ED_SHORTCUT("filesystem_dock/new_script", TTRC("New Script..."), Key::NONE);
	ED_SHORTCUT("filesystem_dock/new_resource", TTRC("New Resource..."), Key::NONE);
	ED_SHORTCUT("filesystem_dock/new_textfile", TTRC("New TextFile..."), Key::NONE);
	ED_SHORTCUT("filesystem_dock/rename", TTRC("Rename..."), Key::F2);
	ED_SHORTCUT_OVERRIDE("filesystem_dock/rename", "macos", Key::ENTER);
#if !defined(ANDROID_ENABLED) && !defined(WEB_ENABLED)
	// Opening the system file manager or opening in an external program is not supported on the Android and web editors.
	ED_SHORTCUT("filesystem_dock/show_in_explorer", TTRC("Open in File Manager"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::ALT | Key::R);
	ED_SHORTCUT("filesystem_dock/open_in_external_program", TTRC("Open in External Program"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::ALT | Key::E);
	ED_SHORTCUT("filesystem_dock/open_in_terminal", TTRC("Open in Terminal"), KeyModifierMask::CMD_OR_CTRL | KeyModifierMask::ALT | Key::T);
#endif

	ED_SHORTCUT("filesystem_dock/focus_path", TTRC("Focus Path"), KeyModifierMask::CMD_OR_CTRL | Key::L);
	// Allow both Cmd + L and Cmd + Shift + G to match Safari's and Finder's shortcuts respectively.
	ED_SHORTCUT_OVERRIDE_ARRAY("filesystem_dock/focus_path", "macos",
			{ int32_t(KeyModifierMask::META | Key::L), int32_t(KeyModifierMask::META | KeyModifierMask::SHIFT | Key::G) });

	// Properly translating color names would require a separate HashMap, so for simplicity they are provided as comments.
	folder_color_order.push_back("red");
	folder_color_order.push_back("orange");
	folder_color_order.push_back("yellow");
	folder_color_order.push_back("green");
	folder_color_order.push_back("teal");
	folder_color_order.push_back("blue");
	folder_color_order.push_back("purple");
	folder_color_order.push_back("pink");
	folder_color_order.push_back("gray");
	folder_colors["red"] = Color(1.0, 0.271, 0.271); // TTR("Red")
	folder_colors["orange"] = Color(1.0, 0.561, 0.271); // TTR("Orange")
	folder_colors["yellow"] = Color(1.0, 0.890, 0.271); // TTR("Yellow")
	folder_colors["green"] = Color(0.502, 1.0, 0.271); // TTR("Green")
	folder_colors["teal"] = Color(0.271, 1.0, 0.635); // TTR("Teal")
	folder_colors["blue"] = Color(0.271, 0.843, 1.0); // TTR("Blue")
	folder_colors["purple"] = Color(0.502, 0.271, 1.0); // TTR("Purple")
	folder_colors["pink"] = Color(1.0, 0.271, 0.588); // TTR("Pink")
	folder_colors["gray"] = Color(0.616, 0.616, 0.616); // TTR("Gray")

	_load_color_customization();

	editor_is_dark_icon_and_font = EditorThemeManager::is_dark_icon_and_font();

	VBoxContainer *main_vb = memnew(VBoxContainer);
	add_child(main_vb);

	VBoxContainer *top_vbc = memnew(VBoxContainer);
	main_vb->add_child(top_vbc);

	toolbar_hbc = memnew(HBoxContainer);
	top_vbc->add_child(toolbar_hbc);

	HBoxContainer *nav_hbc = memnew(HBoxContainer);
	nav_hbc->add_theme_constant_override("separation", 0);
	toolbar_hbc->add_child(nav_hbc);

	button_hist_prev = memnew(Button);
	button_hist_prev->set_theme_type_variation(SceneStringName(FlatButton));
	button_hist_prev->set_disabled(true);
	button_hist_prev->set_focus_mode(FOCUS_ACCESSIBILITY);
	button_hist_prev->set_tooltip_text(TTRC("Go to previous selected folder/file."));
	nav_hbc->add_child(button_hist_prev);

	button_hist_next = memnew(Button);
	button_hist_next->set_theme_type_variation(SceneStringName(FlatButton));
	button_hist_next->set_disabled(true);
	button_hist_next->set_focus_mode(FOCUS_ACCESSIBILITY);
	button_hist_next->set_tooltip_text(TTRC("Go to next selected folder/file."));
	nav_hbc->add_child(button_hist_next);

	current_path_line_edit = memnew(LineEdit);
	current_path_line_edit->set_structured_text_bidi_override(TextServer::STRUCTURED_TEXT_FILE);
	current_path_line_edit->set_accessibility_name(TTRC("Path"));
	current_path_line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	_set_current_path_line_edit_text(current_path);
	toolbar_hbc->add_child(current_path_line_edit);

	button_toggle_display_mode = memnew(Button);
	button_toggle_display_mode->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_change_split_mode));
	button_toggle_display_mode->set_focus_mode(FOCUS_ACCESSIBILITY);
	button_toggle_display_mode->set_tooltip_text(TTRC("Change Split Mode"));
	button_toggle_display_mode->set_theme_type_variation("FlatMenuButton");
	toolbar_hbc->add_child(button_toggle_display_mode);

	toolbar2_hbc = memnew(HBoxContainer);
	top_vbc->add_child(toolbar2_hbc);

	tree_search_box = memnew(LineEdit);
	tree_search_box->set_h_size_flags(SIZE_EXPAND_FILL);
	tree_search_box->set_placeholder(TTRC("Filter Assets"));
	tree_search_box->set_clear_button_enabled(true);
	tree_search_box->connect(SceneStringName(text_changed), callable_mp(this, &FileSystemDock::_search_changed).bind(tree_search_box));
	toolbar2_hbc->add_child(tree_search_box);

	tree_button_sort = _create_file_menu_button();
	toolbar2_hbc->add_child(tree_button_sort);

	file_list_popup = memnew(PopupMenu);

	add_child(file_list_popup);

	tree_popup = memnew(PopupMenu);

	add_child(tree_popup);

	category_wide_split = memnew(HSplitContainer);
	category_wide_split->set_v_size_flags(SIZE_EXPAND_FILL);
	category_wide_split->connect("dragged", callable_mp(this, &FileSystemDock::_category_split_dragged));
	category_wide_split_offset = 210 * EDSCALE;
	category_wide_split->set_split_offset(category_wide_split_offset);
	main_vb->add_child(category_wide_split);

	category_narrow_split = memnew(VSplitContainer);
	category_narrow_split->set_v_size_flags(SIZE_EXPAND_FILL);
	category_narrow_split->connect("dragged", callable_mp(this, &FileSystemDock::_category_split_dragged));
	category_narrow_split_offset = 120 * EDSCALE;
	category_narrow_split->set_split_offset(category_narrow_split_offset);
	category_narrow_split->hide();
	main_vb->add_child(category_narrow_split);

	category_rail = memnew(VBoxContainer);
	category_rail->set_v_size_flags(SIZE_EXPAND_FILL);
	category_rail->set_custom_minimum_size(Size2(190, 0) * EDSCALE);
	category_wide_split->add_child(category_rail);

	Label *category_title = memnew(Label);
	category_title->set_text(TTRC("Categories"));
	category_rail->add_child(category_title);

	category_tree = memnew(Tree);
	category_tree->set_accessibility_name(TTRC("Asset Categories"));
	category_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	category_tree->set_hide_root(true);
	category_tree->set_columns(2);
	category_tree->set_column_expand(0, false);
	category_tree->set_column_custom_minimum_width(0, 24 * EDSCALE);
	category_tree->set_column_expand(1, true);
	category_tree->set_column_clip_content(1, true);
	category_tree->set_allow_reselect(true);
	category_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	category_tree->connect("item_mouse_selected", callable_mp(this, &FileSystemDock::_category_rail_item_clicked));
	category_rail->add_child(category_tree);

	category_rail_empty_state = memnew(VBoxContainer);
	category_rail->add_child(category_rail_empty_state);
	Label *no_categories_label = memnew(Label);
	no_categories_label->set_text(TTRC("No categories yet"));
	category_rail_empty_state->add_child(no_categories_label);
	Label *no_categories_hint = memnew(Label);
	no_categories_hint->set_text(TTRC("Right-click a folder to set its color, or edit category names below."));
	no_categories_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	category_rail_empty_state->add_child(no_categories_hint);

	category_edit_button = memnew(Button);
	category_edit_button->set_text(TTRC("Edit Categories..."));
	category_edit_button->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_popup_color_labels_dialog));
	category_rail->add_child(category_edit_button);

	split_box = memnew(SplitContainer);
	split_box->set_v_size_flags(SIZE_EXPAND_FILL);
	split_box->set_h_size_flags(SIZE_EXPAND_FILL);
	split_box->connect("dragged", callable_mp(this, &FileSystemDock::_split_dragged));
	split_box_offset_h = 240 * EDSCALE;
	category_wide_split->add_child(split_box);

	tree_mc = memnew(MarginContainer);
	split_box->add_child(tree_mc);
	tree_mc->set_theme_type_variation("NoBorderHorizontalBottom");
	tree_mc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tree_content_vb = memnew(VBoxContainer);
	tree_content_vb->set_v_size_flags(SIZE_EXPAND_FILL);
	tree_mc->add_child(tree_content_vb);

	tree = memnew(FileSystemTree);
	tree->set_accessibility_name(TTRC("Directories"));
	tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	tree->set_hide_root(true);
	tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_TOP);
	SET_DRAG_FORWARDING_GCD(tree, FileSystemDock);
	tree->set_allow_reselect(true);
	tree->set_allow_rmb_select(true);
	tree->set_select_mode(Tree::SELECT_MULTI);
	tree->set_custom_minimum_size(Size2(40 * EDSCALE, 15 * EDSCALE));
	tree->set_column_clip_content(0, true);
	tree_content_vb->add_child(tree);

	tree->connect("item_activated", callable_mp(this, &FileSystemDock::_tree_activate_file));
	tree->connect("button_clicked", callable_mp(this, &FileSystemDock::_tree_description_button_clicked));
	tree->connect("multi_selected", callable_mp(this, &FileSystemDock::_tree_multi_selected));
	tree->connect("item_mouse_selected", callable_mp(this, &FileSystemDock::_tree_rmb_select));
	tree->connect("empty_clicked", callable_mp(this, &FileSystemDock::_tree_empty_click));
	tree->connect("nothing_selected", callable_mp(this, &FileSystemDock::_tree_empty_selected));
	tree->connect(SceneStringName(gui_input), callable_mp(this, &FileSystemDock::_tree_gui_input));
	tree->connect(SceneStringName(mouse_exited), callable_mp(this, &FileSystemDock::_tree_mouse_exited));
	tree->connect("item_edited", callable_mp(this, &FileSystemDock::_rename_operation_confirm));

	file_list_vb = memnew(VBoxContainer);
	file_list_vb->set_v_size_flags(SIZE_EXPAND_FILL);
	split_box->add_child(file_list_vb);

	path_hb = memnew(HBoxContainer);
	path_hb->set_h_size_flags(SIZE_EXPAND_FILL);
	file_list_vb->add_child(path_hb);

	file_list_search_box = memnew(LineEdit);
	file_list_search_box->set_h_size_flags(SIZE_EXPAND_FILL);
	file_list_search_box->set_placeholder(TTRC("Filter Assets"));
	file_list_search_box->set_accessibility_name(TTRC("Filter Assets"));
	file_list_search_box->set_clear_button_enabled(true);
	file_list_search_box->connect(SceneStringName(text_changed), callable_mp(this, &FileSystemDock::_search_changed).bind(file_list_search_box));
	path_hb->add_child(file_list_search_box);

	file_list_button_sort = _create_file_menu_button();
	path_hb->add_child(file_list_button_sort);

	button_file_list_display_mode = memnew(Button);
	button_file_list_display_mode->set_accessibility_name(TTRC("Display Mode"));
	button_file_list_display_mode->set_theme_type_variation("FlatMenuButton");
	path_hb->add_child(button_file_list_display_mode);

	files_mc = memnew(MarginContainer);
	file_list_vb->add_child(files_mc);
	files_mc->set_theme_type_variation("NoBorderHorizontalBottom");
	files_mc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	files_content_vb = memnew(VBoxContainer);
	files_content_vb->set_v_size_flags(SIZE_EXPAND_FILL);
	files_mc->add_child(files_content_vb);

	files = memnew(FileSystemList);
	files->set_accessibility_name(TTRC("Files"));
	files->set_select_mode(ItemList::SELECT_MULTI);
	files->set_scroll_hint_mode(ItemList::SCROLL_HINT_MODE_TOP);
	SET_DRAG_FORWARDING_GCD(files, FileSystemDock);
	files->connect("item_clicked", callable_mp(this, &FileSystemDock::_file_list_item_clicked));
	files->connect("item_action_clicked", callable_mp(this, &FileSystemDock::_file_list_description_action_clicked));
	files->connect(SceneStringName(gui_input), callable_mp(this, &FileSystemDock::_file_list_gui_input));
	files->connect("multi_selected", callable_mp(this, &FileSystemDock::_file_multi_selected));
	files->connect("empty_clicked", callable_mp(this, &FileSystemDock::_file_list_empty_clicked));
	files->connect("item_edited", callable_mp(this, &FileSystemDock::_rename_operation_confirm));
	files->connect(SceneStringName(resized), callable_mp(this, &FileSystemDock::_queue_visible_scene_previews_update));
	files->get_v_scroll_bar()->connect(SceneStringName(value_changed), callable_mp(this, &FileSystemDock::_file_list_scroll_changed));
	files->get_h_scroll_bar()->connect(SceneStringName(value_changed), callable_mp(this, &FileSystemDock::_file_list_scroll_changed));
	files->set_custom_minimum_size(Size2(0, 15 * EDSCALE));
	files->set_allow_rmb_select(true);
	files_content_vb->add_child(files);

	category_result_empty_state = memnew(VBoxContainer);
	category_result_empty_state->set_h_size_flags(SIZE_EXPAND_FILL);
	category_result_empty_state->set_v_size_flags(SIZE_SHRINK_CENTER);
	category_result_empty_state->hide();
	files_content_vb->add_child(category_result_empty_state);

	category_result_empty_label = memnew(Label);
	category_result_empty_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	category_result_empty_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	category_result_empty_state->add_child(category_result_empty_label);

	category_result_empty_hint = memnew(Label);
	category_result_empty_hint->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	category_result_empty_hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	category_result_empty_state->add_child(category_result_empty_hint);

	category_result_edit_button = memnew(Button);
	category_result_edit_button->set_text(TTRC("Edit Categories..."));
	category_result_edit_button->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_popup_color_labels_dialog));
	category_result_empty_state->add_child(category_result_edit_button);

	category_result_clear_search_button = memnew(Button);
	category_result_clear_search_button->set_text(TTRC("Clear Search"));
	category_result_clear_search_button->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_clear_category_search));
	category_result_empty_state->add_child(category_result_clear_search_button);

	scanning_vb = memnew(VBoxContainer);
	scanning_vb->hide();
	main_vb->add_child(scanning_vb);

	Label *slabel = memnew(Label);
	slabel->set_text(TTRC("Scanning Files,\nPlease Wait..."));
	slabel->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	scanning_vb->add_child(slabel);

	scanning_progress = memnew(ProgressBar);
	scanning_progress->set_accessibility_name(TTRC("Filesystem Scan"));
	scanning_vb->add_child(scanning_progress);

	deps_editor = memnew(DependencyEditor);
	add_child(deps_editor);

	owners_editor = memnew(DependencyEditorOwners());
	add_child(owners_editor);

	remove_dialog = memnew(DependencyRemoveDialog);
	remove_dialog->connect("resource_removed", callable_mp(this, &FileSystemDock::_resource_removed));
	remove_dialog->connect("file_removed", callable_mp(this, &FileSystemDock::_file_removed));
	remove_dialog->connect("folder_removed", callable_mp(this, &FileSystemDock::_folder_removed));
	add_child(remove_dialog);

	move_dialog = memnew(EditorDirDialog);
	add_child(move_dialog);
	move_dialog->connect("move_pressed", callable_mp(this, &FileSystemDock::_move_operation_confirm).bind(false, OVERWRITE_UNDECIDED));
	move_dialog->connect("copy_pressed", callable_mp(this, &FileSystemDock::_move_operation_confirm).bind(true, OVERWRITE_UNDECIDED));

	overwrite_dialog = memnew(ConfirmationDialog);
	add_child(overwrite_dialog);
	overwrite_dialog->set_ok_button_text(TTRC("Overwrite"));
	overwrite_dialog->add_button(TTRC("Keep Both"), true)->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_overwrite_dialog_action).bind(false));
	overwrite_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_overwrite_dialog_action).bind(true));

	VBoxContainer *overwrite_dialog_vb = memnew(VBoxContainer);
	overwrite_dialog->add_child(overwrite_dialog_vb);

	overwrite_dialog_header = memnew(Label);
	overwrite_dialog_vb->add_child(overwrite_dialog_header);

	overwrite_dialog_scroll = memnew(ScrollContainer);
	overwrite_dialog_vb->add_child(overwrite_dialog_scroll);
	overwrite_dialog_scroll->set_custom_minimum_size(Vector2(50, 50) * EDSCALE);
	overwrite_dialog_scroll->set_v_size_flags(SIZE_EXPAND_FILL);

	overwrite_dialog_file_list = memnew(Label);
	overwrite_dialog_scroll->add_child(overwrite_dialog_file_list);

	overwrite_dialog_footer = memnew(Label);
	overwrite_dialog_vb->add_child(overwrite_dialog_footer);

	make_dir_dialog = memnew(DirectoryCreateDialog);
	add_child(make_dir_dialog);

	make_scene_dialog = memnew(SceneCreateDialog);
	add_child(make_scene_dialog);
	make_scene_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_make_scene_confirm));

	make_script_dialog = memnew(ScriptCreateDialog);
	make_script_dialog->set_title(TTRC("Create Script"));
	add_child(make_script_dialog);
	make_script_dialog->connect("script_created", callable_mp(this, &FileSystemDock::_script_or_shader_created));

	make_shader_dialog = memnew(ShaderCreateDialog);
	add_child(make_shader_dialog);
	make_shader_dialog->connect("shader_created", callable_mp(this, &FileSystemDock::_script_or_shader_created));
	make_shader_dialog->connect("shader_include_created", callable_mp(this, &FileSystemDock::_script_or_shader_created));

	new_resource_dialog = memnew(CreateDialog);
	add_child(new_resource_dialog);
	new_resource_dialog->set_base_type("Resource");
	new_resource_dialog->connect("create", callable_mp(this, &FileSystemDock::_resource_created));

	conversion_dialog = memnew(ConfirmationDialog);
	conversion_dialog->set_flag(Window::FLAG_RESIZE_DISABLED, true);
	add_child(conversion_dialog);
	conversion_dialog->set_ok_button_text(TTRC("Convert"));
	conversion_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_convert_dialog_action));

	move_confirm_dialog = memnew(ConfirmationDialog);
	add_child(move_confirm_dialog);
	move_confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_move_confirm));

	VBoxContainer *vb = memnew(VBoxContainer);
	move_confirm_dialog->add_child(vb);
	move_confirm_dialog_label = memnew(Label);
	move_confirm_dialog_label->set_focus_mode(Control::FOCUS_ACCESSIBILITY);
	vb->add_child(move_confirm_dialog_label);
	confirm_before_move_checkbox = memnew(CheckBox(TTRC("Don't Ask Again")));
	confirm_before_move_checkbox->set_tooltip_text(TTRC("This dialog can be skipped by holding shift or enabled/disabled in the Editor Settings: Docks > Explore > Ask Before Moving Files."));
	vb->add_child(confirm_before_move_checkbox);

	description_viewer = memnew(AcceptDialog);
	description_viewer->set_ok_button_text(TTRC("Close"));
	description_viewer->set_hide_on_ok(true);
	add_child(description_viewer);
	Button *description_edit_button = description_viewer->add_button(TTRC("Edit"), true);
	description_edit_button->connect(SceneStringName(pressed), callable_mp(this, &FileSystemDock::_edit_viewed_description));
	description_viewer_text = memnew(RichTextLabel);
	description_viewer_text->set_use_bbcode(false);
	description_viewer_text->set_selection_enabled(true);
	description_viewer_text->set_context_menu_enabled(true);
	description_viewer_text->set_focus_mode(Control::FOCUS_ALL);
	description_viewer_text->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	description_viewer_text->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	description_viewer_text->set_custom_minimum_size(Size2(620, 400) * EDSCALE);
	description_viewer_text->connect("meta_clicked", callable_mp(this, &FileSystemDock::_description_meta_clicked));
	description_viewer->add_child(description_viewer_text);

	description_editor = memnew(ConfirmationDialog);
	description_editor->set_ok_button_text(TTRC("Save"));
	description_editor->set_hide_on_ok(false);
	description_editor->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_save_description));
	add_child(description_editor);
	VBoxContainer *description_editor_vb = memnew(VBoxContainer);
	description_editor_vb->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	description_editor->add_child(description_editor_vb);
	description_path_type_label = memnew(Label);
	description_path_type_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	description_editor_vb->add_child(description_path_type_label);
	Label *description_syntax_hint = memnew(Label);
	description_syntax_hint->set_text(TTRC("Markdown: # headings, - lists, **bold**, *italic*, `code`, fenced code, and http/https links."));
	description_syntax_hint->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	description_editor_vb->add_child(description_syntax_hint);
	description_text_edit = memnew(TextEdit);
	description_text_edit->set_accessibility_name(TTRC("Asset description Markdown"));
	description_text_edit->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	description_text_edit->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	description_text_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	description_text_edit->set_custom_minimum_size(Size2(640, 360) * EDSCALE);
	description_editor_vb->add_child(description_text_edit);

	description_error_dialog = memnew(AcceptDialog);
	description_error_dialog->set_title(TTRC("Asset Description"));
	add_child(description_error_dialog);

	description_link_dialog = memnew(ConfirmationDialog);
	description_link_dialog->set_title(TTRC("Open External Link"));
	description_link_dialog->set_ok_button_text(TTRC("Open"));
	description_link_dialog->connect(SceneStringName(confirmed), callable_mp(this, &FileSystemDock::_open_description_link));
	add_child(description_link_dialog);

	unrecognized_ext_dialog = memnew(AcceptDialog);
	unrecognized_ext_dialog->set_flag(Window::FLAG_RESIZE_DISABLED, true);
	add_child(unrecognized_ext_dialog);
	unrecognized_ext_dialog->set_text(TTRC("This file extension is not recognized by the editor.\nIf you want to rename it anyway, use your operating system's file manager.\nAfter renaming to an unknown extension, the file won't be shown in the editor anymore.\nTo make the editor recognize this file extension, add it to one of the lists of extensions in Editor Settings > Docks > Explore."));
	Button *settings_button = unrecognized_ext_dialog->add_button(TTRC("Open Editor Settings"), false, "open_editor_settings_docks_filesystem");
	settings_button->connect("pressed", callable_mp(this, &FileSystemDock::_on_open_editor_settings_file_exts));

	uncollapsed_paths_before_search = Vector<String>();

	tree_update_id = 0;

	history_pos = 0;
	history_max_size = 20;
	history.push_back("res://");

	display_mode = DISPLAY_MODE_TREE_ONLY;
	old_display_mode = DISPLAY_MODE_TREE_ONLY;
	file_list_display_mode = FILE_LIST_DISPLAY_THUMBNAILS;

	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &FileSystemDock::_project_settings_changed));
	EditorSettings::get_singleton()->connect("_favorites_changed", callable_mp(this, &FileSystemDock::update_all));
	main_scene_path = ResourceUID::ensure_path(GLOBAL_GET("application/run/main_scene"));

	add_resource_tooltip_plugin(memnew(EditorTextureTooltipPlugin));
	add_resource_tooltip_plugin(memnew(EditorAudioStreamTooltipPlugin));
}

FileSystemDock::~FileSystemDock() {
	_cancel_visible_scene_previews();
	singleton = nullptr;
}
