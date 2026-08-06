/**************************************************************************/
/*  editor_component_gallery_plugin.cpp                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_component_gallery_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/gui/components/editor_action.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_card.h"
#include "editor/gui/components/editor_empty_state.h"
#include "editor/gui/components/editor_form.h"
#include "editor/gui/components/editor_form_row.h"
#include "editor/gui/components/editor_form_section.h"
#include "editor/gui/components/editor_pane_header.h"
#include "editor/gui/components/editor_search_bar.h"
#include "editor/gui/components/editor_status_panel.h"
#include "editor/gui/components/editor_toolbar.h"
#include "editor/gui/editor_responsive_row.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/material.h"

namespace {

constexpr int INSPECTOR_CHROME_AXIS_CHIP_WIDTH = 14;
constexpr int INSPECTOR_CHROME_AXIS_FIELD_HEIGHT = 21;

class InspectorChromeHeaderPreview : public Control {
public:
	enum Kind {
		KIND_CATEGORY,
		KIND_DIVIDER,
		KIND_SECTION,
		KIND_SUBSECTION,
	};

private:
	Kind kind = KIND_SECTION;
	String title;
	String badge;
	int palette_index = 0;
	int indentation_depth = 0;
	bool expanded = true;
	bool hovered = false;
	Control *collapse_target = nullptr;

	void _theme_updated() {
		update_minimum_size();
		queue_redraw();
	}

	void _draw_header() {
		const Size2 size = get_size();
		const bool rtl = is_layout_rtl();
		const StringName theme_type = SNAME("InspectorChromeHeaderPreview");
		StringName start_name = SNAME("section_start");
		StringName end_name = SNAME("section_end");
		if (kind == KIND_CATEGORY) {
			start_name = StringName("class_" + itos(CLAMP(palette_index, 0, 2)) + "_start");
			end_name = StringName("class_" + itos(CLAMP(palette_index, 0, 2)) + "_end");
		} else if (kind == KIND_DIVIDER) {
			start_name = SNAME("divider_start");
			end_name = SNAME("divider_end");
		} else if (kind == KIND_SUBSECTION) {
			start_name = SNAME("subsection_start");
			end_name = SNAME("subsection_end");
		}
		Color gradient_start = get_theme_color(start_name, theme_type);
		Color gradient_end = get_theme_color(end_name, theme_type);
		if (rtl && kind == KIND_SECTION) {
			SWAP(gradient_start, gradient_end);
		}

		Vector<Point2> points = {
			Point2(0, 0),
			Point2(size.x, 0),
			Point2(size.x, size.y),
			Point2(0, size.y),
		};
		Vector<Color> colors;
		if (kind == KIND_CATEGORY) {
			colors = { gradient_start, gradient_start, gradient_end, gradient_end };
		} else {
			colors = { gradient_start, gradient_end, gradient_end, gradient_start };
		}
		draw_polygon(points, colors);
		if (hovered) {
			draw_rect(Rect2(Point2(), size), get_theme_color(SNAME("hover_overlay_color"), theme_type));
		}
		const float border_width = MAX(1.0f, EDSCALE);
		if (kind == KIND_CATEGORY) {
			Color top_border = gradient_start.lerp(Color(1, 1, 1, gradient_start.a), 0.26f);
			Color bottom_border = gradient_end.lerp(Color(0, 0, 0, gradient_end.a), 0.34f);
			draw_line(Point2(0, border_width * 0.5f), Point2(size.x, border_width * 0.5f), top_border, border_width);
			draw_line(Point2(0, size.y - border_width * 0.5f), Point2(size.x, size.y - border_width * 0.5f), bottom_border, border_width);
		}

		const StringName padding_name = kind == KIND_CATEGORY ? SNAME("category_horizontal_padding") : SNAME("horizontal_padding");
		const int padding = get_theme_constant(padding_name, theme_type) + indentation_depth * 12 * EDSCALE;
		const Ref<Font> font = get_theme_font(SNAME("bold"), EditorStringName(EditorFonts));
		int font_size = get_theme_font_size(SNAME("bold_size"), EditorStringName(EditorFonts));
		if (kind == KIND_DIVIDER || kind == KIND_SUBSECTION) {
			font_size = MAX(8, font_size - Math::round(EDSCALE));
		}
		const Ref<Font> secondary_font = get_theme_font(SNAME("main"), EditorStringName(EditorFonts));
		const int secondary_size = MAX(8, get_theme_font_size(SNAME("main_size"), EditorStringName(EditorFonts)) - 1);
		const StringName font_color_name = kind == KIND_DIVIDER ? SNAME("divider_font_color") : (kind == KIND_SUBSECTION ? SNAME("subsection_font_color") : SceneStringName(font_color));
		const Color font_color = get_theme_color(font_color_name, theme_type);
		const Color secondary_color = get_theme_color(SNAME("secondary_font_color"), theme_type);
		const float baseline = (size.y - font->get_height(font_size)) * 0.5f + font->get_ascent(font_size);

		float leading = padding;
		float trailing = padding;
		if (kind != KIND_CATEGORY) {
			StringName arrow_name = expanded ? SNAME("arrow") : (rtl ? SNAME("arrow_collapsed_mirrored") : SNAME("arrow_collapsed"));
			const Ref<Texture2D> arrow = get_theme_icon(arrow_name, SNAME("Tree"));
			if (arrow.is_valid()) {
				const float arrow_x = rtl ? size.x - leading - arrow->get_width() : leading;
				draw_texture(arrow, Point2(arrow_x, (size.y - arrow->get_height()) * 0.5f), Color(1, 1, 1, 0.82));
				leading += arrow->get_width() + 5 * EDSCALE;
			}
		}

		if (!badge.is_empty()) {
			const float badge_text_width = secondary_font->get_string_size(badge, HORIZONTAL_ALIGNMENT_LEFT, -1, secondary_size).x;
			if (kind == KIND_SECTION) {
				const float radius = MAX(8 * EDSCALE, badge_text_width * 0.5f + 4 * EDSCALE);
				const Point2 center = rtl ? Point2(padding + radius, size.y * 0.5f) : Point2(size.x - padding - radius, size.y * 0.5f);
				const Color badge_fill_color = get_theme_color(SNAME("badge_fill_color"), theme_type);
				draw_circle(center, radius, badge_fill_color);
				const float badge_baseline = center.y - secondary_font->get_height(secondary_size) * 0.5f + secondary_font->get_ascent(secondary_size);
				draw_string(secondary_font, Point2(center.x - badge_text_width * 0.5f, badge_baseline), badge, HORIZONTAL_ALIGNMENT_LEFT, -1, secondary_size, get_theme_color(SNAME("badge_text_color"), theme_type));
				trailing += radius * 2 + 5 * EDSCALE;
			} else {
				const float badge_baseline = (size.y - secondary_font->get_height(secondary_size)) * 0.5f + secondary_font->get_ascent(secondary_size);
				const float badge_x = rtl ? padding : size.x - padding - badge_text_width;
				draw_string(secondary_font, Point2(badge_x, badge_baseline), badge, HORIZONTAL_ALIGNMENT_LEFT, -1, secondary_size, secondary_color);
				trailing += badge_text_width + 8 * EDSCALE;
			}
		}

		const float text_x = rtl ? trailing : leading;
		const float text_width = MAX(0.0f, size.x - leading - trailing);
		draw_string(font, Point2(text_x, baseline), title, rtl ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT, text_width, font_size, font_color, TextServer::JUSTIFICATION_CONSTRAIN_ELLIPSIS);
	}

	void _set_expanded(bool p_expanded) {
		expanded = p_expanded;
		if (collapse_target) {
			collapse_target->set_visible(expanded);
		}
		queue_redraw();
	}

	void _set_hovered(bool p_hovered) {
		hovered = p_hovered;
		queue_redraw();
	}

	void _gui_input(const Ref<InputEvent> &p_event) {
		Ref<InputEventMouseButton> mouse_button = p_event;
		if (mouse_button.is_valid() && mouse_button->get_button_index() == MouseButton::LEFT && mouse_button->is_pressed()) {
			_set_expanded(!expanded);
			accept_event();
			return;
		}

		Ref<InputEventKey> key = p_event;
		if (key.is_valid() && key->is_pressed() && !key->is_echo() && (key->get_keycode() == Key::ENTER || key->get_keycode() == Key::SPACE)) {
			_set_expanded(!expanded);
			accept_event();
		}
	}

public:
	Size2 get_minimum_size() const override {
		const StringName theme_type = SNAME("InspectorChromeHeaderPreview");
		StringName height_name = SNAME("section_height");
		if (kind == KIND_CATEGORY) {
			height_name = SNAME("category_height");
		} else if (kind == KIND_DIVIDER) {
			height_name = SNAME("divider_height");
		} else if (kind == KIND_SUBSECTION) {
			height_name = SNAME("subsection_height");
		}
		return Size2(0, get_theme_constant(height_name, theme_type));
	}

	void set_collapse_target(Control *p_target) {
		collapse_target = p_target;
		if (collapse_target) {
			collapse_target->set_visible(expanded);
		}
	}

	InspectorChromeHeaderPreview(Kind p_kind, const String &p_title, const String &p_badge = String(), int p_palette_index = 0, bool p_expanded = true, int p_indentation_depth = 0) {
		kind = p_kind;
		title = p_title;
		badge = p_badge;
		palette_index = p_palette_index;
		indentation_depth = p_indentation_depth;
		expanded = p_expanded;
		set_mouse_filter(MOUSE_FILTER_STOP);
		set_focus_mode(FOCUS_ALL);
		set_default_cursor_shape(CURSOR_POINTING_HAND);
		set_accessibility_name(title);
		connect(SceneStringName(draw), callable_mp(this, &InspectorChromeHeaderPreview::_draw_header));
		connect(SceneStringName(gui_input), callable_mp(this, &InspectorChromeHeaderPreview::_gui_input));
		connect(SceneStringName(mouse_entered), callable_mp(this, &InspectorChromeHeaderPreview::_set_hovered).bind(true));
		connect(SceneStringName(mouse_exited), callable_mp(this, &InspectorChromeHeaderPreview::_set_hovered).bind(false));
		connect(SceneStringName(theme_changed), callable_mp(this, &InspectorChromeHeaderPreview::_theme_updated));
	}
};

class InspectorChromeSpinSlider : public EditorSpinSlider {
public:
	Size2 get_minimum_size() const override {
		Size2 minimum_size = EditorSpinSlider::get_minimum_size();
		minimum_size.width = 34 * EDSCALE;
		minimum_size.height = INSPECTOR_CHROME_AXIS_FIELD_HEIGHT * EDSCALE;
		return minimum_size;
	}

	InspectorChromeSpinSlider() {
		set_custom_value_text_margin(Math::round(EDSCALE));
		set_custom_value_font_size(MAX(8, Math::round(10 * EDSCALE)));
		set_value_text_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	}
};

class InspectorChromeBodyPreview : public VBoxContainer {
	int depth = 0;

	void _theme_updated() {
		queue_redraw();
	}

	void _draw_guide() {
		const float guide_x = (13 + depth * 12) * EDSCALE;
		draw_line(Point2(guide_x, 0), Point2(guide_x, get_size().y), get_theme_color(SNAME("guide_color"), SNAME("InspectorChromeBodyPreview")), MAX(1.0f, EDSCALE));
	}

public:
	InspectorChromeBodyPreview(int p_depth = 0) {
		depth = p_depth;
		connect(SceneStringName(draw), callable_mp(this, &InspectorChromeBodyPreview::_draw_guide));
		connect(SceneStringName(theme_changed), callable_mp(this, &InspectorChromeBodyPreview::_theme_updated));
	}
};

class InspectorChromeAxisChipPreview : public Control {
	String axis;
	StringName theme_type;

	void _theme_updated() {
		update_minimum_size();
		queue_redraw();
	}

	void _draw_chip() {
		draw_style_box(get_theme_stylebox(SceneStringName(panel), theme_type), Rect2(Point2(), get_size()));
		const Ref<Font> font = get_theme_font(SNAME("main"), EditorStringName(EditorFonts));
		const int font_size = get_theme_font_size(SceneStringName(font_size), SNAME("InspectorChromeAxisLabel"));
		const float text_width = font->get_string_size(axis, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x;
		const float baseline = (get_size().y - font->get_height(font_size)) * 0.5f + font->get_ascent(font_size);
		draw_string(font, Point2((get_size().x - text_width) * 0.5f, baseline), axis, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, get_theme_color(SceneStringName(font_color), SNAME("InspectorChromeAxisLabel")));
	}

public:
	Size2 get_minimum_size() const override {
		return Size2(INSPECTOR_CHROME_AXIS_CHIP_WIDTH, INSPECTOR_CHROME_AXIS_FIELD_HEIGHT) * EDSCALE;
	}

	InspectorChromeAxisChipPreview(const String &p_axis, int p_axis_index) {
		const StringName axis_types[] = { SNAME("InspectorChromeAxisX"), SNAME("InspectorChromeAxisY"), SNAME("InspectorChromeAxisZ") };
		axis = p_axis;
		theme_type = axis_types[CLAMP(p_axis_index, 0, 2)];
		set_mouse_filter(MOUSE_FILTER_IGNORE);
		connect(SceneStringName(draw), callable_mp(this, &InspectorChromeAxisChipPreview::_draw_chip));
		connect(SceneStringName(theme_changed), callable_mp(this, &InspectorChromeAxisChipPreview::_theme_updated));
	}
};

class InspectorChromeResetButton : public Button {
	void _update_icon() {
		if (is_inside_tree()) {
			set_button_icon(get_editor_theme_icon(SNAME("ReloadSmall")));
		}
	}

public:
	InspectorChromeResetButton(bool p_modified) {
		set_theme_type_variation(SNAME("InspectorChromeResetButton"));
		set_custom_minimum_size(Size2(18, 18) * EDSCALE);
		set_tooltip_text(TTR("Revert property"));
		set_disabled(!p_modified);
		set_modulate(Color(1, 1, 1, p_modified ? 1.0f : 0.0f));
		set_mouse_filter(p_modified ? MOUSE_FILTER_STOP : MOUSE_FILTER_IGNORE);
		connect(SceneStringName(tree_entered), callable_mp(this, &InspectorChromeResetButton::_update_icon));
		connect(SceneStringName(theme_changed), callable_mp(this, &InspectorChromeResetButton::_update_icon));
	}
};

class InspectorChromeMaterialThumbnail : public TextureRect {
	Ref<StandardMaterial3D> material;
	bool has_preview = false;

	void _update_fallback() {
		if (!has_preview && is_inside_tree()) {
			set_texture(get_editor_theme_icon(SNAME("StandardMaterial3D")));
		}
	}

	void _queue_preview() {
		_update_fallback();
		if (EditorResourcePreview::get_singleton()) {
			EditorResourcePreview::get_singleton()->queue_edited_resource_preview(material, callable_mp(this, &InspectorChromeMaterialThumbnail::_preview_ready));
		}
	}

	void _preview_ready(const String &p_path, const Ref<Texture2D> &p_preview, const Ref<Texture2D> &p_small_preview) {
		Ref<Texture2D> preview = p_preview.is_valid() ? p_preview : p_small_preview;
		if (preview.is_valid()) {
			has_preview = true;
			set_texture(preview);
		}
	}

public:
	InspectorChromeMaterialThumbnail(const String &p_name, const Color &p_color, float p_metallic, float p_roughness) {
		material.instantiate();
		material->set_name(p_name);
		material->set_albedo(p_color);
		material->set_metallic(p_metallic);
		material->set_roughness(p_roughness);
		set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		set_custom_minimum_size(Size2(20, 20) * EDSCALE);
		set_mouse_filter(MOUSE_FILTER_IGNORE);
		connect(SceneStringName(tree_entered), callable_mp(this, &InspectorChromeMaterialThumbnail::_queue_preview));
		connect(SceneStringName(theme_changed), callable_mp(this, &InspectorChromeMaterialThumbnail::_update_fallback));
	}
};

static MarginContainer *_chrome_inset(Control *p_control, int p_left = 10, int p_vertical = 5, int p_right = -1) {
	MarginContainer *margin = memnew(MarginContainer);
	margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p_control->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	margin->add_theme_constant_override(SNAME("margin_left"), p_left * EDSCALE);
	margin->add_theme_constant_override(SNAME("margin_right"), (p_right < 0 ? p_left : p_right) * EDSCALE);
	margin->add_theme_constant_override(SNAME("margin_top"), p_vertical * EDSCALE);
	margin->add_theme_constant_override(SNAME("margin_bottom"), p_vertical * EDSCALE);
	margin->add_child(p_control);
	return margin;
}

static LineEdit *_chrome_field(const String &p_text, int p_width = 165, bool p_modified = false) {
	LineEdit *field = memnew(LineEdit);
	field->set_text(p_text);
	field->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	field->set_custom_minimum_size(Size2(p_width * EDSCALE, 0));
	field->set_theme_type_variation(p_modified ? SNAME("InspectorChromeModifiedField") : SNAME("InspectorChromeField"));
	return field;
}

static PanelContainer *_chrome_axis_field(const String &p_axis, const String &p_value, int p_axis_index, bool p_modified) {
	PanelContainer *field = memnew(PanelContainer);
	field->set_theme_type_variation(p_modified ? SNAME("InspectorChromeAxisFieldModified") : SNAME("InspectorChromeAxisField"));
	field->set_custom_minimum_size(Size2(48, INSPECTOR_CHROME_AXIS_FIELD_HEIGHT) * EDSCALE);
	field->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	field->set_v_size_flags(Control::SIZE_SHRINK_CENTER);

	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override(SNAME("separation"), 0);
	field->add_child(row);

	row->add_child(memnew(InspectorChromeAxisChipPreview(p_axis, p_axis_index)));

	EditorSpinSlider *value = memnew(InspectorChromeSpinSlider);
	value->set_value(p_value.to_float());
	value->set_step(0.1);
	value->set_allow_greater(true);
	value->set_allow_lesser(true);
	value->set_control_state(EditorSpinSlider::CONTROL_STATE_HIDE);
	value->set_flat(true);
	value->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	value->set_accessibility_name(vformat(TTR("%s component"), p_axis));
	row->add_child(value);
	return field;
}

static HBoxContainer *_chrome_vector(const String &p_x, const String &p_y, const String &p_z, bool p_modified = false) {
	HBoxContainer *vector = memnew(HBoxContainer);
	vector->set_custom_minimum_size(Size2(165 * EDSCALE, 0));
	vector->set_h_size_flags(Control::SIZE_SHRINK_END);
	vector->add_theme_constant_override(SNAME("separation"), 5 * EDSCALE);
	vector->add_child(_chrome_axis_field("X", p_x, 0, p_modified));
	vector->add_child(_chrome_axis_field("Y", p_y, 1, p_modified));
	vector->add_child(_chrome_axis_field("Z", p_z, 2, p_modified));
	return vector;
}

static PanelContainer *_chrome_resource_field(const String &p_name, const String &p_badge, const Color &p_color, bool p_modified = false, int p_width = 165) {
	PanelContainer *field = memnew(PanelContainer);
	field->set_theme_type_variation(p_modified ? SNAME("InspectorChromeResourceFieldModified") : SNAME("InspectorChromeResourceField"));
	field->set_custom_minimum_size(Size2(p_width * EDSCALE, 0));

	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override(SNAME("separation"), 4 * EDSCALE);
	field->add_child(row);

	const bool metal = p_name.containsn("panel");
	row->add_child(memnew(InspectorChromeMaterialThumbnail(p_name, p_color, metal ? 0.55f : 0.0f, metal ? 0.35f : 0.82f)));

	Label *name = memnew(Label);
	name->set_theme_type_variation(SNAME("InspectorChromeResourceName"));
	name->set_text(p_name);
	name->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	name->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_child(name);

	Label *badge = memnew(Label);
	badge->set_theme_type_variation(SNAME("InspectorChromeResourceBadge"));
	badge->set_text(p_badge);
	row->add_child(badge);
	return field;
}

static MarginContainer *_chrome_property_row(const String &p_label, Control *p_editor, bool p_stacked = false, bool p_modified = false, int p_depth = 0) {
	if (p_stacked) {
		VBoxContainer *column = memnew(VBoxContainer);
		column->add_theme_constant_override(SNAME("separation"), 4 * EDSCALE);
		Label *label = memnew(Label);
		label->set_theme_type_variation(SNAME("InspectorChromePropertyLabel"));
		label->set_text(p_label);
		label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		column->add_child(label);
		MarginContainer *editor_inset = memnew(MarginContainer);
		editor_inset->add_theme_constant_override(SNAME("margin_left"), 5 * EDSCALE);
		p_editor->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		editor_inset->add_child(p_editor);
		column->add_child(editor_inset);
		return _chrome_inset(column, 22 + p_depth * 12, 4, 11);
	}

	HBoxContainer *row = memnew(HBoxContainer);
	row->add_theme_constant_override(SNAME("separation"), 8 * EDSCALE);
	Label *label = memnew(Label);
	label->set_theme_type_variation(SNAME("InspectorChromePropertyLabel"));
	label->set_text(p_label);
	label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	label->set_custom_minimum_size(Size2(96 * EDSCALE, 0));
	row->add_child(label);
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_child(spacer);
	HBoxContainer *editor_cluster = memnew(HBoxContainer);
	editor_cluster->set_h_size_flags(Control::SIZE_SHRINK_END);
	editor_cluster->add_theme_constant_override(SNAME("separation"), 4 * EDSCALE);
	editor_cluster->add_child(memnew(InspectorChromeResetButton(p_modified)));
	p_editor->set_h_size_flags(Control::SIZE_SHRINK_END);
	editor_cluster->add_child(p_editor);
	row->add_child(editor_cluster);
	return _chrome_inset(row, 38 + p_depth * 12, 3, 11);
}

static VBoxContainer *_chrome_body(bool p_draw_guide = false, int p_depth = 0) {
	VBoxContainer *body = p_draw_guide ? memnew(InspectorChromeBodyPreview(p_depth)) : memnew(VBoxContainer);
	body->add_theme_constant_override(SNAME("separation"), 1 * EDSCALE);
	return body;
}

static VBoxContainer *_chrome_collapsible(InspectorChromeHeaderPreview::Kind p_kind, const String &p_title, const String &p_badge, Control *p_body, int p_palette_index = 0, bool p_expanded = true, int p_indentation_depth = 0) {
	VBoxContainer *group = memnew(VBoxContainer);
	group->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	group->add_theme_constant_override(SNAME("separation"), 0);
	InspectorChromeHeaderPreview *header = memnew(InspectorChromeHeaderPreview(p_kind, p_title, p_badge, p_palette_index, p_expanded, p_indentation_depth));
	group->add_child(header);
	p_body->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	group->add_child(p_body);
	header->set_collapse_target(p_body);
	return group;
}

} // namespace

Ref<EditorAction> EditorComponentGalleryPlugin::_make_action(const String &p_text, const StringName &p_icon, bool p_enabled) {
	Ref<EditorAction> action;
	action.instantiate();
	action->set_text(p_text);
	action->set_icon_name(p_icon);
	action->set_tooltip(vformat(TTR("Example action: %s"), p_text));
	action->set_enabled(p_enabled);
	return action;
}

void EditorComponentGalleryPlugin::_open_gallery() {
	if (!gallery) {
		_build_gallery();
	}
	gallery->popup_centered_clamped(Size2(900, 760) * EDSCALE, 0.9);
}

Control *EditorComponentGalleryPlugin::_build_inspector_chrome_panel(bool p_narrow) {
	PanelContainer *frame = memnew(PanelContainer);
	frame->set_theme_type_variation(SNAME("InspectorChromePreviewFrame"));
	frame->set_custom_minimum_size(Size2(p_narrow ? 300 : 400, 0) * EDSCALE);

	VBoxContainer *root = memnew(VBoxContainer);
	root->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	root->add_theme_constant_override(SNAME("separation"), 0);
	frame->add_child(root);

	EditorPaneHeader *header = memnew(EditorPaneHeader);
	header->set_theme_type_variation(SNAME("InspectorChromePaneHeader"));
	header->set_title(TTR("TimberFront"));
	header->set_subtitle(p_narrow ? TTR("Compact Inspector - 300 px") : TTR("DestructibleWall3D > Node3D > Node"));
	header->set_icon_name(SNAME("Node3D"));
	Ref<EditorAction> options = _make_action(String::chr(0x2026));
	options->set_tooltip(TTR("Inspector options"));
	EditorActionButton *overflow_button = header->add_action(options, SNAME("secondary"));
	overflow_button->set_theme_type_variation(SNAME("InspectorChromeOverflowButton"));
	root->add_child(header);

	EditorSearchBar *search = memnew(EditorSearchBar);
	search->set_placeholder(TTR("Search properties"));
	search->set_result_count(p_narrow ? 3 : 6);
	root->add_child(_chrome_inset(search, 0, 7));

	if (!p_narrow) {
		VBoxContainer *wall = _chrome_body();
		wall->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Identity"), "1", _chrome_body(), 0, false));

		VBoxContainer *structure = _chrome_body();
		VBoxContainer *dimensions = _chrome_body(true, 1);
		dimensions->add_child(_chrome_property_row(TTR("Length"), _chrome_field("4.00 m"), false, false, 1));
		dimensions->add_child(_chrome_property_row(TTR("Height"), _chrome_field("3.00 m"), false, false, 1));
		dimensions->add_child(_chrome_property_row(TTR("Thickness"), _chrome_field("0.125 m", 165, true), false, true, 1));
		dimensions->add_child(_chrome_property_row(TTR("Studs"), _chrome_field("7"), false, false, 1));
		structure->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Dimensions"), "4", dimensions, 0, true, 1));

		VBoxContainer *structural_health = _chrome_body();
		VBoxContainer *health = _chrome_body(true, 2);
		health->add_child(_chrome_property_row(TTR("Panel Face"), _chrome_field("84.0", 165, true), false, true, 2));
		structural_health->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SUBSECTION, TTR("Health"), String(), health, 0, true, 2));
		structural_health->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SUBSECTION, TTR("Armor"), "1", _chrome_body(), 0, false, 2));
		structure->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Structural Health"), "2", structural_health, 0, true, 1));
		wall->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_DIVIDER, TTR("STRUCTURE"), TTR("2 changed"), structure));

		VBoxContainer *materials = _chrome_body(true);
		materials->add_child(_chrome_property_row(TTR("Panel Face"), _chrome_resource_field(TTR("panel_face_painted"), TTR("unique"), Color(0.78, 0.22, 0.75), true), false, true));
		materials->add_child(_chrome_property_row(TTR("Stud"), _chrome_resource_field(TTR("timber_stud_oak"), TTR("shared"), Color(0.68, 0.48, 0.33), true), false, true));
		wall->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Visual Materials"), "2", materials));
		root->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_CATEGORY, TTR("DestructibleWall3D"), TTR("5 changed"), wall, 0));

		VBoxContainer *node_3d = _chrome_body();
		VBoxContainer *transform = _chrome_body(true);
		transform->add_child(_chrome_property_row(TTR("Location"), _chrome_vector("-3.0", "0.0", "-3.0", true), false, true));
		transform->add_child(_chrome_property_row(TTR("Rotation"), _chrome_vector("0.0", "0.0", "0.0")));
		transform->add_child(_chrome_property_row(TTR("Scale"), _chrome_vector("1.0", "1.0", "1.0")));
		node_3d->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Transform"), "3", transform));
		node_3d->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Visibility"), String(), _chrome_body(), 0, false));
		root->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_CATEGORY, TTR("Node3D"), TTR("3 changed"), node_3d, 1));

		VBoxContainer *node = _chrome_body();
		node->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Process"), String(), _chrome_body(), 0, false));
		node->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Editor Description"), String(), _chrome_body(), 0, false));
		root->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_CATEGORY, TTR("Node"), TTR("clean"), node, 2));
	} else {
		VBoxContainer *node_3d = _chrome_body();
		VBoxContainer *transform = _chrome_body(true);
		transform->add_child(_chrome_property_row(TTR("Location"), _chrome_vector("-3.0", "0.0", "-3.0", true), true, true));
		transform->add_child(_chrome_property_row(TTR("Rotation"), _chrome_vector("0.0", "0.0", "0.0"), true));
		transform->add_child(_chrome_property_row(TTR("Scale"), _chrome_vector("1.0", "1.0", "1.0"), true));
		node_3d->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Transform"), "3", transform));
		VBoxContainer *materials = _chrome_body(true);
		VBoxContainer *surface = _chrome_body(true, 1);
		surface->add_child(_chrome_property_row(TTR("Panel Face"), _chrome_resource_field(TTR("panel_face_painted"), TTR("unique"), Color(0.78, 0.22, 0.75), true, 250), true, true, 1));
		materials->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SUBSECTION, TTR("Surface"), "1", surface, 0, true, 1));
		node_3d->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Visual Materials"), "1", materials));
		root->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_CATEGORY, TTR("Node3D"), TTR("3 changed"), node_3d, 1));

		VBoxContainer *node = _chrome_body();
		node->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_SECTION, TTR("Process"), String(), _chrome_body(), 0, false));
		root->add_child(_chrome_collapsible(InspectorChromeHeaderPreview::KIND_CATEGORY, TTR("Node"), TTR("clean"), node, 2));
	}

	return frame;
}

void EditorComponentGalleryPlugin::_build_gallery() {
	gallery = memnew(AcceptDialog);
	gallery->set_title(TTR("Editor UI Component Gallery"));
	gallery->set_ok_button_text(TTR("Close"));
	add_child(gallery);

	ScrollContainer *scroll = memnew(ScrollContainer);
	scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	gallery->add_child(scroll);

	MarginContainer *margin = memnew(MarginContainer);
	margin->set_theme_type_variation(SNAME("MarginContainer4px"));
	margin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	scroll->add_child(margin);

	VBoxContainer *content = memnew(VBoxContainer);
	content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	content->add_theme_constant_override(SNAME("separation"), 20 * EDSCALE);
	margin->add_child(content);

	EditorCard *inspector_chrome = memnew(EditorCard);
	inspector_chrome->set_title(TTR("Inspector Chrome Direction"));
	inspector_chrome->set_description(TTR("Gallery-only prototype: full-bleed type, group, and subgroup hierarchy with depth-aware guide rails, standardized fields, compact vectors, and resource rows. The live Inspector is unchanged."));
	inspector_chrome->set_badge(TTR("Prototype"));
	content->add_child(inspector_chrome);

	EditorResponsiveRow *inspector_previews = memnew(EditorResponsiveRow);
	inspector_previews->add_theme_constant_override(SNAME("h_separation"), 16 * EDSCALE);
	inspector_previews->add_theme_constant_override(SNAME("v_separation"), 16 * EDSCALE);
	inspector_chrome->add_body_control(inspector_previews);

	VBoxContainer *wide_preview = memnew(VBoxContainer);
	Label *wide_caption = memnew(Label);
	wide_caption->set_theme_type_variation(SNAME("InspectorChromePreviewCaption"));
	wide_caption->set_text(TTR("Standard pane (400 px)"));
	wide_preview->add_child(wide_caption);
	wide_preview->add_child(_build_inspector_chrome_panel(false));
	inspector_previews->add_child(wide_preview);

	VBoxContainer *narrow_preview = memnew(VBoxContainer);
	Label *narrow_caption = memnew(Label);
	narrow_caption->set_theme_type_variation(SNAME("InspectorChromePreviewCaption"));
	narrow_caption->set_text(TTR("Narrow responsive pane (300 px)"));
	narrow_preview->add_child(narrow_caption);
	narrow_preview->add_child(_build_inspector_chrome_panel(true));
	inspector_previews->add_child(narrow_preview);

	EditorPaneHeader *pane_header = memnew(EditorPaneHeader);
	pane_header->set_title(TTR("Brushed Metal"));
	pane_header->set_subtitle(TTR("Material • res://materials/brushed_metal.tres"));
	pane_header->set_icon_name(SNAME("StandardMaterial3D"));
	pane_header->set_dirty(true);
	pane_header->add_action(_make_action(TTR("Save"), SNAME("Save")));
	pane_header->add_action(_make_action(TTR("Options"), SNAME("Tools")), SNAME("secondary"));
	content->add_child(pane_header);

	EditorSearchBar *search_bar = memnew(EditorSearchBar);
	search_bar->set_placeholder(TTR("Search assets..."));
	search_bar->set_result_count(24);
	search_bar->add_filter_action(_make_action(TTR("Filters"), SNAME("FilenameFilter")));
	content->add_child(search_bar);

	EditorCard *actions_card = memnew(EditorCard);
	actions_card->set_title(TTR("Actions and Toolbars"));
	actions_card->set_description(TTR("Actions share their state with every visual presentation."));
	actions_card->set_badge(TTR("Responsive"));
	content->add_child(actions_card);

	EditorToolbar *toolbar = memnew(EditorToolbar);
	toolbar->add_action(_make_action(TTR("Create"), SNAME("Add")));
	toolbar->add_action(_make_action(TTR("Save"), SNAME("Save")));
	toolbar->add_separator();
	Ref<EditorAction> toggle_action = _make_action(TTR("Preview"), SNAME("GuiVisibilityVisible"));
	toggle_action->set_checkable(true);
	toggle_action->set_checked(true);
	toolbar->add_action(toggle_action);
	toolbar->add_action(_make_action(TTR("Unavailable"), SNAME("Lock"), false), SNAME("secondary"));
	actions_card->add_body_control(toolbar);

	EditorForm *form = memnew(EditorForm);
	EditorFormSection *identity = form->add_section(TTR("Identity"));
	identity->set_description(TTR("Form rows align at comfortable widths and stack in narrow panes."));
	identity->set_collapsible(true);

	LineEdit *name_edit = memnew(LineEdit);
	name_edit->set_text(TTR("Brushed Metal"));
	EditorFormRow *name_row = identity->add_row(TTR("Display Name"), name_edit);
	name_row->set_required(true);
	name_row->set_description(TTR("Shown in asset browsers and selection fields."));

	OptionButton *category = memnew(OptionButton);
	category->add_item(TTR("Material"));
	category->add_item(TTR("Texture"));
	category->add_item(TTR("Model"));
	identity->add_row(TTR("Asset Type"), category);

	CheckBox *managed = memnew(CheckBox(TTR("Store source in the managed asset vault")));
	EditorFormRow *managed_row = identity->add_row(TTR("Source Management"), managed);
	managed_row->set_status(EditorFormRow::STATUS_WARNING, TTR("Changing this setting will reimport the asset."));

	EditorFormSection *advanced = form->add_section(TTR("Advanced"));
	advanced->set_collapsible(true);
	advanced->set_collapsed(true);
	advanced->add_row(TTR("Stable UID"), memnew(LineEdit));
	content->add_child(form);

	EditorCard *status_card = memnew(EditorCard);
	status_card->set_title(TTR("Persistent Status"));
	status_card->set_badge(TTR("4 states"));
	status_card->set_collapsible(true);
	status_card->set_collapsed(true);
	content->add_child(status_card);

	const EditorStatusPanel::Severity severities[] = {
		EditorStatusPanel::SEVERITY_INFO,
		EditorStatusPanel::SEVERITY_SUCCESS,
		EditorStatusPanel::SEVERITY_WARNING,
		EditorStatusPanel::SEVERITY_ERROR,
	};
	const String titles[] = { TTR("Information"), TTR("Import complete"), TTR("Source changed"), TTR("Import failed") };
	const String messages[] = {
		TTR("This message remains visible until its state changes."),
		TTR("The asset and its generated previews are up to date."),
		TTR("The source file differs from the last imported revision."),
		TTR("The source file could not be decoded."),
	};
	for (int i = 0; i < 4; i++) {
		EditorStatusPanel *status = memnew(EditorStatusPanel);
		status->set_severity(severities[i]);
		status->set_title(titles[i]);
		status->set_message(messages[i]);
		status->set_dismissible(i == 0);
		if (i >= 2) {
			status->set_action(_make_action(TTR("Details")));
		}
		status_card->add_body_control(status);
	}

	EditorCard *empty_card = memnew(EditorCard);
	empty_card->set_title(TTR("Empty State"));
	empty_card->set_collapsible(true);
	empty_card->set_collapsed(true);
	content->add_child(empty_card);

	EditorEmptyState *empty_state = memnew(EditorEmptyState);
	empty_state->set_custom_minimum_size(Size2(0, 220) * EDSCALE);
	empty_state->set_icon_name(SNAME("Folder"));
	empty_state->set_title(TTR("No managed assets yet"));
	empty_state->set_description(TTR("Import a source file or create an asset to begin."));
	empty_state->set_primary_action(_make_action(TTR("Import Asset"), SNAME("Load")));
	empty_state->set_secondary_action(_make_action(TTR("Create Asset"), SNAME("Add")));
	empty_card->add_body_control(empty_state);
}

EditorComponentGalleryPlugin::EditorComponentGalleryPlugin() {
	menu_name = TTR("UI Component Gallery...");
	add_tool_menu_item(menu_name, callable_mp(this, &EditorComponentGalleryPlugin::_open_gallery));
}

EditorComponentGalleryPlugin::~EditorComponentGalleryPlugin() {
	remove_tool_menu_item(menu_name);
}
