/**************************************************************************/
/*  material_browser_dock.cpp                                             */
/**************************************************************************/
/*  G-Level LE2: virtualized, flat-thumbnail material browser dock.       */
/**************************************************************************/

#include "material_browser_dock.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/level/level_editor.h"
#include "editor/level/material_index.h"
#include "editor/level/material_preview_generator.h"
#include "editor/level/texel_density_scanner.h"
#include "editor/themes/editor_scale.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/color_rect.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/slider.h"
#include "scene/gui/texture_rect.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh_data.h"

enum MaterialBrowserMenu {
	MENU_SET_ACTIVE,
	MENU_TOGGLE_HIDDEN,
	MENU_OPEN_INSPECTOR,
	MENU_REVEAL_FILESYSTEM,
};

class MaterialBrowserTile : public Control {
	TextureRect *thumbnail = nullptr;
	ColorRect *scrim = nullptr;
	Label *name_label = nullptr;
	Label *status_label = nullptr;
	String path;
	int model_index = -1;
	bool selected = false;

protected:
	void _notification(int p_what) {
		if (p_what != NOTIFICATION_DRAW) {
			return;
		}
		draw_rect(Rect2(Vector2(), get_size()), Color(0.075, 0.075, 0.08), true);
		const Color separator = selected ? Color(0.30, 0.66, 1.0) : Color(0.025, 0.025, 0.03);
		draw_rect(Rect2(Vector2(0.5f, 0.5f), get_size() - Vector2(1, 1)), separator, false, selected ? 2.0f : 1.0f);
	}

public:
	String get_path() const { return path; }
	int get_model_index() const { return model_index; }

	void bind(int p_model_index, const String &p_path, const String &p_name, const String &p_status, const String &p_tooltip, bool p_selected) {
		model_index = p_model_index;
		path = p_path;
		name_label->set_text(p_name);
		status_label->set_text(p_status);
		set_tooltip_text(p_tooltip);
		thumbnail->set_texture(Ref<Texture2D>());
		set_selected(p_selected);
		show();
	}

	void set_thumbnail(const Ref<Texture2D> &p_texture) {
		thumbnail->set_texture(p_texture);
	}

	void set_selected(bool p_selected) {
		if (selected == p_selected) {
			return;
		}
		selected = p_selected;
		queue_redraw();
	}

	MaterialBrowserTile() {
		set_mouse_filter(MOUSE_FILTER_STOP);
		set_clip_contents(true);
		thumbnail = memnew(TextureRect);
		thumbnail->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
		thumbnail->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		thumbnail->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		thumbnail->set_mouse_filter(MOUSE_FILTER_IGNORE);
		add_child(thumbnail);

		scrim = memnew(ColorRect);
		scrim->set_color(Color(0.025, 0.025, 0.03, 0.86f));
		scrim->set_anchors_preset(PRESET_BOTTOM_WIDE);
		scrim->set_offset(SIDE_TOP, -31 * EDSCALE);
		scrim->set_mouse_filter(MOUSE_FILTER_IGNORE);
		add_child(scrim);

		name_label = memnew(Label);
		name_label->set_anchors_preset(PRESET_BOTTOM_WIDE);
		name_label->set_offset(SIDE_LEFT, 4 * EDSCALE);
		name_label->set_offset(SIDE_TOP, -30 * EDSCALE);
		name_label->set_offset(SIDE_RIGHT, -4 * EDSCALE);
		name_label->set_offset(SIDE_BOTTOM, -15 * EDSCALE);
		name_label->set_clip_text(true);
		name_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
		name_label->add_theme_font_size_override(SceneStringName(font_size), 10 * EDSCALE);
		name_label->add_theme_color_override(SceneStringName(font_color), Color(0.96, 0.96, 0.97));
		add_child(name_label);

		status_label = memnew(Label);
		status_label->set_anchors_preset(PRESET_BOTTOM_WIDE);
		status_label->set_offset(SIDE_LEFT, 4 * EDSCALE);
		status_label->set_offset(SIDE_TOP, -17 * EDSCALE);
		status_label->set_offset(SIDE_RIGHT, -4 * EDSCALE);
		status_label->set_offset(SIDE_BOTTOM, -2 * EDSCALE);
		status_label->set_clip_text(true);
		status_label->set_mouse_filter(MOUSE_FILTER_IGNORE);
		status_label->add_theme_font_size_override(SceneStringName(font_size), 8 * EDSCALE);
		status_label->add_theme_color_override(SceneStringName(font_color), Color(0.68, 0.68, 0.71));
		add_child(status_label);
	}
};

bool MaterialBrowserDock::_path_precedes(const String &p_a, const String &p_b) const {
	const MaterialIndexEntry *a = material_index->get_entry(p_a);
	const MaterialIndexEntry *b = material_index->get_entry(p_b);
	if (!a || !b) {
		return p_a < p_b;
	}
	const int name_order = a->display_name.naturalnocasecmp_to(b->display_name);
	return name_order == 0 ? p_a < p_b : name_order < 0;
}

bool MaterialBrowserDock::_entry_matches(const String &p_path) const {
	const MaterialIndexEntry *entry = material_index->get_entry(p_path);
	if (!entry) {
		return false;
	}
	const bool hidden = material_index->is_hidden(p_path);
	if (current_source == SOURCE_HIDDEN) {
		if (!hidden) {
			return false;
		}
	} else {
		if (hidden || (current_source == SOURCE_IN_LEVEL && !in_level_paths.has(p_path))) {
			return false;
		}
	}
	if (m_only && !entry->is_convention_named) {
		return false;
	}
	return search_text.is_empty() || entry->display_name.findn(search_text) >= 0;
}

void MaterialBrowserDock::_insert_filtered_path(const String &p_path) {
	if (!_entry_matches(p_path) || filtered_paths.has(p_path)) {
		return;
	}
	int insertion = 0;
	while (insertion < filtered_paths.size() && !_path_precedes(p_path, filtered_paths[insertion])) {
		insertion++;
	}
	filtered_paths.insert(insertion, p_path);
}

void MaterialBrowserDock::_remove_filtered_path(const String &p_path) {
	const int index = filtered_paths.find(p_path);
	if (index >= 0) {
		filtered_paths.remove_at(index);
	}
}

void MaterialBrowserDock::_rebuild_filtered_model() {
	filtered_paths.clear();
	if (current_source == SOURCE_IN_LEVEL) {
		_refresh_in_level_paths();
	}
	for (const KeyValue<String, MaterialIndexEntry> &entry : material_index->get_entries()) {
		_insert_filtered_path(entry.key);
	}
	visible_first_index = -1;
	visible_last_index = -1;
	_update_grid_metrics();
	_update_visible_range(true);
}

void MaterialBrowserDock::_rebuild_and_reset_scroll() {
	_rebuild_filtered_model();
	callable_mp(this, &MaterialBrowserDock::_deferred_set_horizontal_scroll).call_deferred(0.0);
}

void MaterialBrowserDock::_invalidate_material_caches(const String &p_path) {
	if (owns_preview_queue) {
		preview_queue->invalidate(p_path);
		scanner->invalidate(p_path);
	}
}

void MaterialBrowserDock::_refresh_after_model_change() {
	visible_first_index = -1;
	_update_grid_metrics();
	_update_visible_range(true);
}

void MaterialBrowserDock::_material_added(const String &p_path) {
	_insert_filtered_path(p_path);
	_refresh_after_model_change();
}

void MaterialBrowserDock::_material_removed(const String &p_path) {
	_remove_filtered_path(p_path);
	_invalidate_material_caches(p_path);
	_refresh_after_model_change();
}

void MaterialBrowserDock::_material_changed(const String &p_path) {
	_invalidate_material_caches(p_path);
	const bool present = filtered_paths.has(p_path);
	const bool matches = _entry_matches(p_path);
	if (present && !matches) {
		_remove_filtered_path(p_path);
	} else if (!present && matches) {
		_insert_filtered_path(p_path);
	} else if (present) {
		_remove_filtered_path(p_path);
		_insert_filtered_path(p_path);
	}
	_refresh_after_model_change();
}

void MaterialBrowserDock::_source_filter_selected(int p_index) {
	ERR_FAIL_INDEX(p_index, 3);
	current_source = SourceFilter(p_index);
	_rebuild_and_reset_scroll();
}

void MaterialBrowserDock::_search_text_changed(const String &p_text) {
	search_text = p_text.strip_edges();
	_rebuild_and_reset_scroll();
}

void MaterialBrowserDock::_convention_only_toggled(bool p_pressed) {
	m_only = p_pressed;
	_rebuild_and_reset_scroll();
}

void MaterialBrowserDock::_zoom_slider_changed(double p_value) {
	set_zoom(int(Math::round(p_value)));
}

void MaterialBrowserDock::_zoom_out_pressed() {
	set_zoom(logical_cell_size - CELL_SIZE_STEP);
}

void MaterialBrowserDock::_zoom_in_pressed() {
	set_zoom(logical_cell_size + CELL_SIZE_STEP);
}

void MaterialBrowserDock::_gallery_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_null() || !button->is_pressed()) {
		return;
	}

	const MouseButton button_index = button->get_button_index();
	const bool vertical_wheel = button_index == MouseButton::WHEEL_UP || button_index == MouseButton::WHEEL_DOWN;
	const bool horizontal_wheel = button_index == MouseButton::WHEEL_LEFT || button_index == MouseButton::WHEEL_RIGHT;
	if (!vertical_wheel && !horizontal_wheel) {
		return;
	}

	if (vertical_wheel && button->is_command_or_control_pressed()) {
		set_zoom(logical_cell_size + (button_index == MouseButton::WHEEL_UP ? CELL_SIZE_STEP : -CELL_SIZE_STEP));
		scroll->accept_event();
		return;
	}

	const bool scroll_back = button_index == MouseButton::WHEEL_UP || button_index == MouseButton::WHEEL_LEFT;
	const double wheel_factor = MAX(1.0, double(button->get_factor()));
	const double delta = MAX(32.0 * EDSCALE, cell_size * 0.65) * wheel_factor;
	set_horizontal_scroll(get_horizontal_scroll() + (scroll_back ? -delta : delta));
	scroll->accept_event();
}

void MaterialBrowserDock::_scroll_changed(double p_value) {
	(void)p_value;
	_update_visible_range();
}

void MaterialBrowserDock::_grid_resized() {
	_update_grid_metrics();
	_update_visible_range(true);
}

void MaterialBrowserDock::_update_grid_metrics() {
	if (!scroll || !grid_content) {
		return;
	}
	cell_size = MAX(1, int(Math::round(logical_cell_size * EDSCALE)));
	const int scrollbar_height = int(scroll->get_h_scroll_bar()->get_combined_minimum_size().y);
	const int available_height = MAX(cell_size, int(scroll->get_size().y) - scrollbar_height);
	const int next_row_count = MAX(1, available_height / cell_size);
	if (row_count != next_row_count) {
		row_count = next_row_count;
		visible_first_index = -1;
	}
	total_columns = filtered_paths.is_empty() ? 0 : (filtered_paths.size() + row_count - 1) / row_count;
	grid_content->set_custom_minimum_size(Size2(MAX(cell_size, total_columns * cell_size), row_count * cell_size));
}

void MaterialBrowserDock::_update_visible_range(bool p_force) {
	if (!scroll || !grid_content || !is_visible_in_tree()) {
		return;
	}
	_update_grid_metrics();
	const int first_visible_column = cell_size > 0 ? int(scroll->get_h_scroll_bar()->get_value()) / cell_size : 0;
	const int visible_columns = MAX(1, int(Math::ceil(scroll->get_size().x / double(cell_size))) + 1);
	const int first_column = MAX(0, first_visible_column - OVERSCAN_COLUMNS);
	const int last_column = MIN(total_columns, first_visible_column + visible_columns + OVERSCAN_COLUMNS);
	const int first_index = MIN(filtered_paths.size(), first_column * row_count);
	const int last_index = MIN(filtered_paths.size(), last_column * row_count);
	if (!p_force && first_index == visible_first_index && last_index == visible_last_index) {
		return;
	}
	visible_first_index = first_index;
	visible_last_index = last_index;
	_resize_pool(MAX(0, last_index - first_index));
	for (int pool_index = 0; pool_index < tile_pool.size(); pool_index++) {
		_bind_tile(tile_pool[pool_index], first_index + pool_index);
	}
}

void MaterialBrowserDock::_deferred_set_horizontal_scroll(double p_value) {
	if (!scroll) {
		return;
	}
	scroll->get_h_scroll_bar()->set_value(p_value);
	_update_visible_range(true);
}

void MaterialBrowserDock::_resize_pool(int p_size) {
	while (tile_pool.size() < p_size) {
		MaterialBrowserTile *tile = memnew(MaterialBrowserTile);
		tile->connect(SceneStringName(gui_input), callable_mp(this, &MaterialBrowserDock::_tile_gui_input).bind(tile->get_instance_id()));
		tile->set_drag_forwarding(callable_mp(this, &MaterialBrowserDock::_tile_get_drag_data).bind(tile), Callable(), Callable());
		grid_content->add_child(tile);
		tile_pool.push_back(tile);
	}
	while (tile_pool.size() > p_size) {
		MaterialBrowserTile *tile = tile_pool[tile_pool.size() - 1];
		tile_pool.remove_at(tile_pool.size() - 1);
		tile->hide();
		tile->queue_free();
	}
}

void MaterialBrowserDock::_bind_tile(MaterialBrowserTile *p_tile, int p_model_index) {
	ERR_FAIL_NULL(p_tile);
	ERR_FAIL_INDEX(p_model_index, filtered_paths.size());
	const String &path = filtered_paths[p_model_index];
	const MaterialIndexEntry *entry = material_index->get_entry(path);
	ERR_FAIL_NULL(entry);
	Ref<Material> loaded_material = ResourceLoader::load(path);
	String dimensions = String::utf8("\xE2\x80\x94");
	if (loaded_material.is_valid()) {
		const std::optional<TexelDensityResult> result = scanner->scan(loaded_material, path);
		if (result.has_value()) {
			dimensions = vformat("%dx%d", result->dimensions.x, result->dimensions.y);
		}
	}
	const int column = p_model_index / row_count;
	const int row = p_model_index % row_count;
	const int gap = MAX(2, int(Math::round(4 * EDSCALE)));
	const String tooltip = entry->display_name + "\n" + dimensions + " | " + String(entry->class_name) + "\n" + path +
			"\n" + TTRC("Double-click to apply to the current selection; opens in Inspector when nothing is actionable.");
	p_tile->set_position(Vector2(column * cell_size, row * cell_size));
	p_tile->set_size(Size2(cell_size - gap, cell_size - gap));
	p_tile->bind(p_model_index, path, entry->display_name, dimensions, tooltip, path == selected_path);
	const Ref<Texture2D> cached = preview_queue->get_cached_preview(path);
	if (cached.is_valid()) {
		p_tile->set_thumbnail(cached);
	}
	preview_queue->request_preview(path, callable_mp(this, &MaterialBrowserDock::_thumbnail_ready));
}

void MaterialBrowserDock::_thumbnail_ready(const String &p_path, const Ref<Texture2D> &p_texture) {
	for (MaterialBrowserTile *tile : tile_pool) {
		if (tile && tile->get_path() == p_path) {
			tile->set_thumbnail(p_texture);
		}
	}
}

void MaterialBrowserDock::_tile_gui_input(const Ref<InputEvent> &p_event, ObjectID p_tile_id) {
	MaterialBrowserTile *tile = static_cast<MaterialBrowserTile *>(ObjectDB::get_instance(p_tile_id));
	if (!tile) {
		return;
	}
	Ref<InputEventMouseButton> button = p_event;
	if (button.is_null()) {
		return;
	}
	const MouseButton button_index = button->get_button_index();
	if (button->is_pressed() && (button_index == MouseButton::WHEEL_UP || button_index == MouseButton::WHEEL_DOWN ||
			button_index == MouseButton::WHEEL_LEFT || button_index == MouseButton::WHEEL_RIGHT)) {
		_gallery_gui_input(p_event);
		tile->accept_event();
		return;
	}
	if (button_index == MouseButton::LEFT) {
		if (button->is_pressed()) {
			pressed_tile_id = p_tile_id;
			pressed_path = tile->get_path();
			pressed_tile_dragging = false;
			pressed_tile_double_click = button->is_double_click();
		} else {
			const bool same_tile = pressed_tile_id == p_tile_id && pressed_path == tile->get_path();
			const bool was_dragging = pressed_tile_dragging;
			const bool was_double_click = pressed_tile_double_click;
			pressed_tile_id = ObjectID();
			pressed_path.clear();
			pressed_tile_dragging = false;
			pressed_tile_double_click = false;
			if (same_tile && !was_dragging) {
				_set_path_active(tile->get_path());
				if (was_double_click) {
					const bool applied = level_editor && bound_document &&
							level_editor->apply_active_material_to_selection(bound_document);
					if (!applied) {
						_open_path_in_inspector(tile->get_path());
					}
				}
			}
		}
		tile->accept_event();
	} else if (button_index == MouseButton::RIGHT && button->is_pressed()) {
		_show_context_menu(tile->get_path(), tile->get_screen_position() + button->get_position());
		tile->accept_event();
	}
}

Variant MaterialBrowserDock::_tile_get_drag_data(const Point2 &p_point, Control *p_from) {
	(void)p_point;
	MaterialBrowserTile *tile = static_cast<MaterialBrowserTile *>(p_from);
	if (!tile || pressed_tile_id != tile->get_instance_id() || pressed_path != tile->get_path()) {
		return Variant();
	}
	Ref<Resource> resource = ResourceLoader::load(tile->get_path());
	Ref<Material> dragged_material = resource;
	if (dragged_material.is_null() || !EditorNode::get_singleton()) {
		return Variant();
	}
	pressed_tile_dragging = true;
	return EditorNode::get_singleton()->drag_resource(resource, p_from);
}

void MaterialBrowserDock::_show_context_menu(const String &p_path, const Vector2 &p_screen_position) {
	context_path = p_path;
	context_menu->clear();
	context_menu->add_item(TTRC("Set Active"), MENU_SET_ACTIVE);
	context_menu->add_check_item(material_index->is_hidden(p_path) ? TTRC("Unhide") : TTRC("Hide"), MENU_TOGGLE_HIDDEN);
	context_menu->add_separator();
	context_menu->add_item(TTRC("Open in Inspector"), MENU_OPEN_INSPECTOR);
	context_menu->add_item(TTRC("Reveal in FileSystem"), MENU_REVEAL_FILESYSTEM);
	context_menu->set_position(p_screen_position);
	context_menu->popup();
}

void MaterialBrowserDock::_context_option_selected(int p_option) {
	if (context_path.is_empty()) {
		return;
	}
	switch (p_option) {
		case MENU_SET_ACTIVE: {
			_set_path_active(context_path);
		} break;
		case MENU_TOGGLE_HIDDEN: {
			material_index->set_hidden(context_path, !material_index->is_hidden(context_path));
		} break;
		case MENU_OPEN_INSPECTOR: {
			_open_path_in_inspector(context_path);
		} break;
		case MENU_REVEAL_FILESYSTEM: {
			if (FileSystemDock::get_singleton()) {
				FileSystemDock::get_singleton()->select_file(context_path);
			}
		} break;
	}
}

void MaterialBrowserDock::_open_path_in_inspector(const String &p_path) {
	Ref<Resource> resource = ResourceLoader::load(p_path);
	if (resource.is_valid() && EditorNode::get_singleton()) {
		EditorNode::get_singleton()->edit_resource(resource);
	}
}

void MaterialBrowserDock::_set_path_active(const String &p_path) {
	Ref<Material> loaded_material = ResourceLoader::load(p_path);
	if (loaded_material.is_valid() && level_editor) {
		if (bound_document) {
			level_editor->set_active_material(bound_document, loaded_material, p_path);
		} else {
			level_editor->set_active_material(loaded_material, p_path);
		}
	}
}

void MaterialBrowserDock::_active_material_changed(const Ref<Material> &p_material, const String &p_path) {
	(void)p_material;
	selected_path = p_path;
	for (MaterialBrowserTile *tile : tile_pool) {
		if (tile) {
			tile->set_selected(tile->get_path() == selected_path);
		}
	}
	if (is_visible_in_tree()) {
		scroll_to_material(p_path);
		reveal_selected_on_open = false;
	} else {
		reveal_selected_on_open = true;
	}
}

void MaterialBrowserDock::_active_material_changed_for_document(int64_t p_history_id, const Ref<Material> &p_material, const String &p_path) {
	if (bound_document && bound_document->get_history_id() != p_history_id) {
		return;
	}
	_active_material_changed(p_material, p_path);
}

void MaterialBrowserDock::_add_variant_material_paths(const Variant &p_value, HashSet<String> &r_paths) {
	if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		const String path = p_value;
		if (!path.is_empty()) {
			r_paths.insert(path);
		}
		return;
	}
	if (p_value.get_type() == Variant::PACKED_STRING_ARRAY) {
		for (const String &path : PackedStringArray(p_value)) {
			if (!path.is_empty()) {
				r_paths.insert(path);
			}
		}
		return;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		for (const Variant &value : Array(p_value)) {
			_add_variant_material_paths(value, r_paths);
		}
		return;
	}
	if (p_value.get_type() == Variant::OBJECT) {
		Material *material = Object::cast_to<Material>(p_value.operator Object *());
		if (material && !material->get_path().is_empty()) {
			r_paths.insert(material->get_path());
		}
	}
}

void MaterialBrowserDock::_collect_block_node_materials(Node *p_node, HashSet<String> &r_paths) {
	if (!p_node) {
		return;
	}
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		const int surface_count = mesh_instance->get_surface_override_material_count();
		for (int surface = 0; surface < surface_count; surface++) {
			const Ref<Material> material = mesh_instance->get_active_material(surface);
			if (material.is_valid() && !material->get_path().is_empty()) {
				r_paths.insert(material->get_path());
			}
		}
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_block_node_materials(p_node->get_child(child), r_paths);
	}
}

void MaterialBrowserDock::_collect_level_materials(Node *p_node, HashSet<String> &r_paths) {
	if (!p_node) {
		return;
	}
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		static const StringName metadata_keys[] = { "material_paths", "_level_material_paths", "materials" };
		for (const StringName &key : metadata_keys) {
			if (block->has_meta(key)) {
				_add_variant_material_paths(block->get_meta(key), r_paths);
			}
		}
		const Ref<LevelMeshData> data = block->get_data();
		if (data.is_valid()) {
			_add_variant_material_paths(data->get_material_paths(), r_paths);
			for (const StringName &key : metadata_keys) {
				if (data->has_meta(key)) {
					_add_variant_material_paths(data->get_meta(key), r_paths);
				}
			}
		}
		_collect_block_node_materials(block, r_paths);
		return;
	}
	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_level_materials(p_node->get_child(child), r_paths);
	}
}

void MaterialBrowserDock::_refresh_in_level_paths() {
	in_level_paths.clear();
	EditorDocument *document = bound_document;
	if (!document) {
		EditorNode *editor_node = EditorNode::get_singleton();
		document = editor_node ? editor_node->get_editor_data().get_active_document() : nullptr;
	}
	if (!document || document->get_type() != EditorDocument::TYPE_LEVEL) {
		return;
	}
	_collect_level_materials(document->get_root(), in_level_paths);
}

void MaterialBrowserDock::_update_zoom_controls() {
	if (zoom_out) {
		zoom_out->set_disabled(logical_cell_size <= MIN_CELL_SIZE);
	}
	if (zoom_in) {
		zoom_in->set_disabled(logical_cell_size >= MAX_CELL_SIZE);
	}
	if (zoom_slider) {
		zoom_slider->set_value_no_signal(logical_cell_size);
		zoom_slider->set_tooltip_text(vformat(TTRC("Material thumbnail size: %d px\nCtrl+wheel over the gallery to zoom."), logical_cell_size));
	}
}

void MaterialBrowserDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_filters", "source", "search", "m_only"), &MaterialBrowserDock::set_filters);
	ClassDB::bind_method(D_METHOD("set_zoom", "cell_size"), &MaterialBrowserDock::set_zoom);
	ClassDB::bind_method(D_METHOD("get_zoom"), &MaterialBrowserDock::get_zoom);
	ClassDB::bind_method(D_METHOD("get_horizontal_scroll"), &MaterialBrowserDock::get_horizontal_scroll);
	ClassDB::bind_method(D_METHOD("set_horizontal_scroll", "position"), &MaterialBrowserDock::set_horizontal_scroll);
	ClassDB::bind_method(D_METHOD("get_presentation_state"), &MaterialBrowserDock::get_presentation_state);
	ClassDB::bind_method(D_METHOD("set_presentation_state", "state"), &MaterialBrowserDock::set_presentation_state);
	ClassDB::bind_method(D_METHOD("focus_search"), &MaterialBrowserDock::focus_search);
	ClassDB::bind_method(D_METHOD("drawer_opened", "focus_search"), &MaterialBrowserDock::drawer_opened, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_filtered_count"), &MaterialBrowserDock::get_filtered_count);
	ClassDB::bind_method(D_METHOD("get_filtered_paths"), &MaterialBrowserDock::get_filtered_paths);
	ClassDB::bind_method(D_METHOD("get_virtualized_pool_size"), &MaterialBrowserDock::get_virtualized_pool_size);
	ClassDB::bind_method(D_METHOD("get_overscan_rows"), &MaterialBrowserDock::get_overscan_rows);
	ClassDB::bind_method(D_METHOD("get_overscan_columns"), &MaterialBrowserDock::get_overscan_columns);
	ClassDB::bind_method(D_METHOD("get_row_count"), &MaterialBrowserDock::get_row_count);
	ClassDB::bind_method(D_METHOD("get_selected_path"), &MaterialBrowserDock::get_selected_path);
	ClassDB::bind_method(D_METHOD("get_preview_queue"), &MaterialBrowserDock::get_preview_queue);
	ClassDB::bind_method(D_METHOD("get_scanner"), &MaterialBrowserDock::get_scanner);
	ClassDB::bind_method(D_METHOD("scroll_to_material", "path"), &MaterialBrowserDock::scroll_to_material);

	BIND_ENUM_CONSTANT(SOURCE_PROJECT);
	BIND_ENUM_CONSTANT(SOURCE_IN_LEVEL);
	BIND_ENUM_CONSTANT(SOURCE_HIDDEN);
}

void MaterialBrowserDock::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		if (zoom_out) {
			zoom_out->set_button_icon(get_editor_theme_icon(SNAME("ZoomLess")));
		}
		if (zoom_in) {
			zoom_in->set_button_icon(get_editor_theme_icon(SNAME("ZoomMore")));
		}
		_update_zoom_controls();
		_update_grid_metrics();
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED && is_visible_in_tree()) {
		drawer_opened(false);
	}
}

void MaterialBrowserDock::_reload_in_level_source() {
	_refresh_in_level_paths();
	if (current_source == SOURCE_IN_LEVEL) {
		_rebuild_filtered_model();
	} else {
		_update_visible_range(true);
	}
}

void MaterialBrowserDock::active_document_changed() {
	if (!bound_document && level_editor) {
		selected_path = level_editor->get_active_material_path();
	}
	_reload_in_level_source();
}

void MaterialBrowserDock::set_bound_document(LevelDocument *p_document) {
	if (bound_document == p_document) {
		return;
	}
	bound_document = p_document;
	selected_path = level_editor ? (bound_document ? level_editor->get_active_material_path(bound_document) : level_editor->get_active_material_path()) : String();
	_reload_in_level_source();
}

void MaterialBrowserDock::request_preview(const String &p_path, const Callable &p_callback) {
	preview_queue->request_preview(p_path, p_callback);
}

void MaterialBrowserDock::scroll_to_material(const String &p_path) {
	const int index = filtered_paths.find(p_path);
	if (index < 0 || !scroll) {
		return;
	}
	_update_grid_metrics();
	const int column = index / row_count;
	const double tile_start = column * cell_size;
	const double tile_end = tile_start + cell_size;
	const double viewport_start = get_horizontal_scroll();
	const double viewport_end = viewport_start + scroll->get_size().x;
	double target = viewport_start;
	if (tile_start < viewport_start) {
		target = tile_start;
	} else if (tile_end > viewport_end) {
		target = tile_end - scroll->get_size().x;
	}
	callable_mp(this, &MaterialBrowserDock::_deferred_set_horizontal_scroll).call_deferred(MAX(0.0, target));
}

void MaterialBrowserDock::set_filters(int p_source, const String &p_search, bool p_m_only) {
	ERR_FAIL_INDEX(p_source, 3);
	current_source = SourceFilter(p_source);
	search_text = p_search.strip_edges();
	m_only = p_m_only;
	source_filter->select(p_source);
	search_field->set_text(p_search);
	convention_only->set_pressed_no_signal(p_m_only);
	_rebuild_filtered_model();
}

void MaterialBrowserDock::set_zoom(int p_cell_size) {
	int next_size;
	if (p_cell_size <= MIN_CELL_SIZE) {
		next_size = MIN_CELL_SIZE;
	} else if (p_cell_size >= MAX_CELL_SIZE) {
		next_size = MAX_CELL_SIZE;
	} else {
		next_size = DEFAULT_CELL_SIZE + int(Math::round((p_cell_size - DEFAULT_CELL_SIZE) / double(CELL_SIZE_STEP))) * CELL_SIZE_STEP;
		next_size = CLAMP(next_size, MIN_CELL_SIZE, MAX_CELL_SIZE);
	}
	if (logical_cell_size == next_size) {
		_update_zoom_controls();
		return;
	}
	const double left_column = cell_size > 0 ? get_horizontal_scroll() / cell_size : 0.0;
	logical_cell_size = next_size;
	visible_first_index = -1;
	visible_last_index = -1;
	_update_zoom_controls();
	_update_grid_metrics();
	_update_visible_range(true);
	callable_mp(this, &MaterialBrowserDock::_deferred_set_horizontal_scroll).call_deferred(left_column * cell_size);
}

double MaterialBrowserDock::get_horizontal_scroll() const {
	return scroll ? scroll->get_h_scroll_bar()->get_value() : 0.0;
}

void MaterialBrowserDock::set_horizontal_scroll(double p_value) {
	if (!scroll) {
		return;
	}
	scroll->get_h_scroll_bar()->set_value(MAX(0.0, p_value));
	_update_visible_range();
}

Dictionary MaterialBrowserDock::get_presentation_state() const {
	Dictionary state;
	state["source"] = int(current_source);
	state["search"] = search_text;
	state["m_only"] = m_only;
	state["zoom"] = logical_cell_size;
	state["horizontal_scroll"] = get_horizontal_scroll();
	return state;
}

void MaterialBrowserDock::set_presentation_state(const Dictionary &p_state) {
	const int source = CLAMP(int(p_state.get("source", int(current_source))), int(SOURCE_PROJECT), int(SOURCE_HIDDEN));
	const String search = p_state.get("search", search_text);
	const bool convention = p_state.get("m_only", m_only);
	const int zoom = p_state.get("zoom", logical_cell_size);
	const double horizontal_scroll = p_state.get("horizontal_scroll", 0.0);
	set_filters(source, search, convention);
	set_zoom(zoom);
	callable_mp(this, &MaterialBrowserDock::_deferred_set_horizontal_scroll).call_deferred(MAX(0.0, horizontal_scroll));
}

void MaterialBrowserDock::focus_search() {
	if (!search_field) {
		return;
	}
	search_field->grab_focus();
	search_field->select_all();
}

void MaterialBrowserDock::drawer_opened(bool p_focus_search) {
	_update_grid_metrics();
	_update_visible_range(true);
	if (reveal_selected_on_open && !selected_path.is_empty()) {
		scroll_to_material(selected_path);
		reveal_selected_on_open = false;
	}
	if (p_focus_search) {
		focus_search();
	}
}

PackedStringArray MaterialBrowserDock::get_filtered_paths() const {
	PackedStringArray paths;
	for (const String &path : filtered_paths) {
		paths.push_back(path);
	}
	return paths;
}

Ref<MaterialBrowserPreviewQueue> MaterialBrowserDock::get_preview_queue() const {
	return preview_queue;
}

Ref<TexelDensityScanner> MaterialBrowserDock::get_scanner() const {
	return scanner;
}

MaterialBrowserDock::MaterialBrowserDock(LevelEditor *p_level_editor, const Ref<MaterialIndex> &p_material_index, const Ref<TexelDensityScanner> &p_scanner,
		LevelDocument *p_document, const Ref<MaterialBrowserPreviewQueue> &p_preview_queue) :
		level_editor(p_level_editor), bound_document(p_document), material_index(p_material_index), scanner(p_scanner), preview_queue(p_preview_queue) {
	ERR_FAIL_NULL(level_editor);
	ERR_FAIL_COND(material_index.is_null());
	ERR_FAIL_COND(scanner.is_null());
	owns_preview_queue = preview_queue.is_null();
	set_name("MaterialBrowserDock");
	set_title(TTRC("Materials"));
	set_layout_key("MaterialBrowser");
	set_icon_name(SNAME("StandardMaterial3D"));
	set_default_slot(DOCK_SLOT_RIGHT_BL);
	set_closable(true);
	// Header + scrollbar + two default 108 px rows.
	set_custom_minimum_size(Size2(260 * EDSCALE, 264 * EDSCALE));

	if (owns_preview_queue) {
		preview_queue.instantiate();
		preview_queue->initialize(scanner);
	}

	VBoxContainer *layout = memnew(VBoxContainer);
	layout->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	layout->add_theme_constant_override("separation", 2 * EDSCALE);
	add_child(layout);

	HBoxContainer *filters = memnew(HBoxContainer);
	filters->add_theme_constant_override("separation", 3 * EDSCALE);
	layout->add_child(filters);
	source_filter = memnew(OptionButton);
	source_filter->add_item(TTRC("Project"), SOURCE_PROJECT);
	source_filter->add_item(TTRC("In Level"), SOURCE_IN_LEVEL);
	source_filter->add_item(TTRC("Hidden"), SOURCE_HIDDEN);
	source_filter->set_tooltip_text(TTRC("Material source"));
	source_filter->connect(SceneStringName(item_selected), callable_mp(this, &MaterialBrowserDock::_source_filter_selected));
	filters->add_child(source_filter);

	search_field = memnew(LineEdit);
	search_field->set_placeholder(TTRC("Search materials"));
	search_field->set_h_size_flags(SIZE_EXPAND_FILL);
	search_field->set_clear_button_enabled(true);
	search_field->connect(SceneStringName(text_changed), callable_mp(this, &MaterialBrowserDock::_search_text_changed));
	filters->add_child(search_field);

	convention_only = memnew(CheckButton(TTRC("M_*")));
	convention_only->set_tooltip_text(TTRC("Show convention-named materials only"));
	convention_only->connect(SceneStringName(toggled), callable_mp(this, &MaterialBrowserDock::_convention_only_toggled));
	filters->add_child(convention_only);

	zoom_out = memnew(Button);
	zoom_out->set_name("MaterialZoomOut");
	zoom_out->set_flat(true);
	zoom_out->set_focus_mode(Control::FOCUS_NONE);
	zoom_out->set_tooltip_text(TTRC("Smaller material thumbnails"));
	zoom_out->connect(SceneStringName(pressed), callable_mp(this, &MaterialBrowserDock::_zoom_out_pressed));
	filters->add_child(zoom_out);

	zoom_slider = memnew(HSlider);
	zoom_slider->set_name("MaterialZoom");
	zoom_slider->set_min(MIN_CELL_SIZE);
	zoom_slider->set_max(MAX_CELL_SIZE);
	// Range's native step is anchored to its minimum, while this shelf has an
	// intentional 108 px default. Quantization to 8 px happens in set_zoom().
	zoom_slider->set_step(1);
	zoom_slider->set_value(DEFAULT_CELL_SIZE);
	zoom_slider->set_custom_minimum_size(Size2(72 * EDSCALE, 0));
	zoom_slider->set_v_size_flags(SIZE_SHRINK_CENTER);
	zoom_slider->set_accessibility_name(TTRC("Material thumbnail size"));
	zoom_slider->connect(SceneStringName(value_changed), callable_mp(this, &MaterialBrowserDock::_zoom_slider_changed));
	filters->add_child(zoom_slider);

	zoom_in = memnew(Button);
	zoom_in->set_name("MaterialZoomIn");
	zoom_in->set_flat(true);
	zoom_in->set_focus_mode(Control::FOCUS_NONE);
	zoom_in->set_tooltip_text(TTRC("Larger material thumbnails"));
	zoom_in->connect(SceneStringName(pressed), callable_mp(this, &MaterialBrowserDock::_zoom_in_pressed));
	filters->add_child(zoom_in);

	scroll = memnew(ScrollContainer);
	scroll->set_name("MaterialBrowserScroll");
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
	scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	scroll->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll->set_v_size_flags(SIZE_EXPAND_FILL);
	scroll->connect(SceneStringName(gui_input), callable_mp(this, &MaterialBrowserDock::_gallery_gui_input));
	scroll->connect(SceneStringName(resized), callable_mp(this, &MaterialBrowserDock::_grid_resized));
	scroll->get_h_scroll_bar()->connect(SceneStringName(value_changed), callable_mp(this, &MaterialBrowserDock::_scroll_changed));
	layout->add_child(scroll);

	grid_content = memnew(Control);
	grid_content->set_name("MaterialBrowserGrid");
	grid_content->set_h_size_flags(SIZE_EXPAND_FILL);
	grid_content->set_v_size_flags(SIZE_SHRINK_BEGIN);
	scroll->add_child(grid_content);

	context_menu = memnew(PopupMenu);
	context_menu->connect(SceneStringName(id_pressed), callable_mp(this, &MaterialBrowserDock::_context_option_selected));
	add_child(context_menu);

	material_index->connect(SNAME("material_added"), callable_mp(this, &MaterialBrowserDock::_material_added));
	material_index->connect(SNAME("material_removed"), callable_mp(this, &MaterialBrowserDock::_material_removed));
	material_index->connect(SNAME("material_changed"), callable_mp(this, &MaterialBrowserDock::_material_changed));
	level_editor->connect(SNAME("active_material_changed_for_document"), callable_mp(this, &MaterialBrowserDock::_active_material_changed_for_document));
	selected_path = bound_document ? level_editor->get_active_material_path(bound_document) : level_editor->get_active_material_path();
	_update_zoom_controls();
	_rebuild_filtered_model();
}

MaterialBrowserDock::~MaterialBrowserDock() {
	preview_queue.unref();
}
