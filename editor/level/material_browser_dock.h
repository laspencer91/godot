/**************************************************************************/
/*  material_browser_dock.h                                               */
/**************************************************************************/
/*  G-Level LE2: virtualized, flat-thumbnail material browser dock.       */
/**************************************************************************/

#pragma once

#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"
#include "editor/docks/editor_dock.h"

class Button;
class CheckButton;
class Control;
class HSlider;
class LevelDocument;
class LevelEditor;
class LineEdit;
class MaterialBrowserPreviewQueue;
class MaterialBrowserTile;
class MaterialIndex;
class OptionButton;
class PopupMenu;
class ScrollContainer;
class TexelDensityScanner;

class MaterialBrowserDock : public EditorDock {
	GDCLASS(MaterialBrowserDock, EditorDock);

public:
	enum SourceFilter {
		SOURCE_PROJECT,
		SOURCE_IN_LEVEL,
		SOURCE_HIDDEN,
	};

	// Kept as an alias for the original smoke/script API during the drawer
	// migration. The shelf now virtualizes columns, not rows.
	static constexpr int OVERSCAN_COLUMNS = 1;
	static constexpr int OVERSCAN_ROWS = OVERSCAN_COLUMNS;

private:
	static constexpr int DEFAULT_CELL_SIZE = 108;
	static constexpr int MIN_CELL_SIZE = 80;
	static constexpr int MAX_CELL_SIZE = 160;
	static constexpr int CELL_SIZE_STEP = 8;

	LevelEditor *level_editor = nullptr;
	LevelDocument *bound_document = nullptr; // Not owned; the document view releases this browser first.
	Ref<MaterialIndex> material_index;
	Ref<TexelDensityScanner> scanner;
	Ref<MaterialBrowserPreviewQueue> preview_queue;
	bool owns_preview_queue = false;
	OptionButton *source_filter = nullptr;
	LineEdit *search_field = nullptr;
	CheckButton *convention_only = nullptr;
	Button *zoom_out = nullptr;
	HSlider *zoom_slider = nullptr;
	Button *zoom_in = nullptr;
	ScrollContainer *scroll = nullptr;
	Control *grid_content = nullptr;
	PopupMenu *context_menu = nullptr;
	Vector<String> filtered_paths;
	HashSet<String> in_level_paths;
	Vector<MaterialBrowserTile *> tile_pool;
	String context_path;
	String selected_path;
	SourceFilter current_source = SOURCE_PROJECT;
	String search_text;
	bool m_only = false;
	int logical_cell_size = DEFAULT_CELL_SIZE;
	int cell_size = DEFAULT_CELL_SIZE;
	int row_count = 1;
	int total_columns = 0;
	int visible_first_index = -1;
	int visible_last_index = -1;
	ObjectID pressed_tile_id;
	String pressed_path;
	bool pressed_tile_dragging = false;
	bool pressed_tile_double_click = false;
	bool reveal_selected_on_open = false;

	bool _path_precedes(const String &p_a, const String &p_b) const;
	bool _entry_matches(const String &p_path) const;
	void _insert_filtered_path(const String &p_path);
	void _remove_filtered_path(const String &p_path);
	void _rebuild_filtered_model();
	void _rebuild_and_reset_scroll();
	void _invalidate_material_caches(const String &p_path);
	void _refresh_after_model_change();
	void _material_added(const String &p_path);
	void _material_removed(const String &p_path);
	void _material_changed(const String &p_path);

	void _source_filter_selected(int p_index);
	void _search_text_changed(const String &p_text);
	void _convention_only_toggled(bool p_pressed);
	void _zoom_slider_changed(double p_value);
	void _zoom_out_pressed();
	void _zoom_in_pressed();
	void _gallery_gui_input(const Ref<InputEvent> &p_event);
	void _scroll_changed(double p_value);
	void _grid_resized();
	void _update_grid_metrics();
	void _update_visible_range(bool p_force = false);
	void _deferred_set_horizontal_scroll(double p_value);
	void _resize_pool(int p_size);
	void _bind_tile(MaterialBrowserTile *p_tile, int p_model_index);
	void _thumbnail_ready(const String &p_path, const Ref<Texture2D> &p_texture);
	void _tile_gui_input(const Ref<InputEvent> &p_event, ObjectID p_tile_id);
	Variant _tile_get_drag_data(const Point2 &p_point, Control *p_from);
	void _show_context_menu(const String &p_path, const Vector2 &p_screen_position);
	void _context_option_selected(int p_option);
	void _open_path_in_inspector(const String &p_path);
	void _set_path_active(const String &p_path);
	void _active_material_changed(const Ref<Material> &p_material, const String &p_path);
	void _active_material_changed_for_document(int64_t p_history_id, const Ref<Material> &p_material, const String &p_path);
	void _update_zoom_controls();

	static void _add_variant_material_paths(const Variant &p_value, HashSet<String> &r_paths);
	static void _collect_block_node_materials(Node *p_node, HashSet<String> &r_paths);
	static void _collect_level_materials(Node *p_node, HashSet<String> &r_paths);
	void _refresh_in_level_paths();
	void _reload_in_level_source();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void active_document_changed();
	void set_bound_document(LevelDocument *p_document);
	LevelDocument *get_bound_document() const { return bound_document; }
	void request_preview(const String &p_path, const Callable &p_callback);
	void scroll_to_material(const String &p_path);
	void set_filters(int p_source, const String &p_search, bool p_m_only);
	void set_zoom(int p_cell_size);
	int get_zoom() const { return logical_cell_size; }
	double get_horizontal_scroll() const;
	void set_horizontal_scroll(double p_value);
	Dictionary get_presentation_state() const;
	void set_presentation_state(const Dictionary &p_state);
	void focus_search();
	void drawer_opened(bool p_focus_search = true);
	int get_filtered_count() const { return filtered_paths.size(); }
	PackedStringArray get_filtered_paths() const;
	int get_virtualized_pool_size() const { return tile_pool.size(); }
	int get_overscan_rows() const { return OVERSCAN_ROWS; }
	int get_overscan_columns() const { return OVERSCAN_COLUMNS; }
	int get_row_count() const { return row_count; }
	String get_selected_path() const { return selected_path; }
	Ref<MaterialBrowserPreviewQueue> get_preview_queue() const;
	Ref<TexelDensityScanner> get_scanner() const;

	MaterialBrowserDock(LevelEditor *p_level_editor, const Ref<MaterialIndex> &p_material_index, const Ref<TexelDensityScanner> &p_scanner,
			LevelDocument *p_document = nullptr, const Ref<MaterialBrowserPreviewQueue> &p_preview_queue = Ref<MaterialBrowserPreviewQueue>());
	~MaterialBrowserDock();
};

VARIANT_ENUM_CAST(MaterialBrowserDock::SourceFilter);
