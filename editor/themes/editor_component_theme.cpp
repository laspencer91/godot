/**************************************************************************/
/*  editor_component_theme.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_component_theme.h"

#include "editor/themes/editor_scale.h"
#include "scene/resources/style_box.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/style_box_line.h"
#include "scene/scene_string_names.h"

static Ref<StyleBoxFlat> _component_style(const Ref<StyleBoxFlat> &p_base, const Color &p_background, const Color &p_border, int p_border_width, float p_horizontal_padding, float p_vertical_padding, int p_corner_radius) {
	Ref<StyleBoxFlat> style = p_base->duplicate();
	style->set_bg_color(p_background);
	style->set_border_width_all(p_border_width);
	style->set_border_color(p_border);
	style->set_content_margin_individual(p_horizontal_padding, p_vertical_padding, p_horizontal_padding, p_vertical_padding);
	style->set_corner_radius_all(p_corner_radius);
	return style;
}

void EditorComponentTheme::populate(const Ref<EditorTheme> &p_theme, const EditorThemeManager::ThemeConfiguration &p_config) {
	const int stroke = MAX(1, Math::round(EDSCALE));
	const int radius = MAX(3, Math::round(p_config.corner_radius * EDSCALE));
	const float control_padding_h = 10 * EDSCALE;
	const float control_padding_v = 6 * EDSCALE;
	const Color resting_border = p_config.separator_color.lerp(p_config.font_secondary_color, 0.28);
	const Color hover_border = resting_border.lerp(p_config.font_color, 0.32);
	const Color focus_border = resting_border.lerp(p_config.accent_color, 0.68);
	const Color card_border = p_config.separator_color.lerp(p_config.font_secondary_color, 0.5);

	// Action presentation.
	p_theme->set_type_variation("EditorActionButtonFlat", "Button");
	p_theme->set_type_variation("EditorActionButtonPrimary", "Button");
	Ref<StyleBoxFlat> action_normal = _component_style(p_config.button_style, p_config.button_normal_color, resting_border, stroke, control_padding_h, control_padding_v, radius);
	Ref<StyleBoxFlat> action_hover = _component_style(p_config.button_style_hover, p_config.button_hover_color, hover_border, stroke, control_padding_h, control_padding_v, radius);
	Ref<StyleBoxFlat> action_pressed = _component_style(p_config.button_style_pressed, p_config.button_pressed_color, p_config.accent_color, stroke, control_padding_h, control_padding_v, radius);
	Ref<StyleBoxFlat> action_disabled = _component_style(p_config.button_style_disabled, p_config.button_disabled_color, p_config.disabled_border_color, stroke, control_padding_h, control_padding_v, radius);
	// One focus overlay for every control in the set: a stronger border drawn on
	// top, so focus never changes a control's measured size.
	Ref<StyleBoxFlat> focus_overlay = p_config.focus_style->duplicate();
	focus_overlay->set_border_width_all(stroke);
	focus_overlay->set_border_color(focus_border);
	focus_overlay->set_corner_radius_all(radius);
	for (const StringName &type : { StringName("EditorActionButtonFlat"), StringName("EditorActionButtonPrimary") }) {
		p_theme->set_stylebox(CoreStringName(normal), type, action_normal);
		p_theme->set_stylebox(SceneStringName(hover), type, action_hover);
		p_theme->set_stylebox(SceneStringName(pressed), type, action_pressed);
		p_theme->set_stylebox("hover_pressed", type, action_pressed);
		p_theme->set_stylebox("disabled", type, action_disabled);
		p_theme->set_stylebox("focus", type, focus_overlay);
		p_theme->set_constant("h_separation", type, 8 * EDSCALE);
		p_theme->set_constant("minimum_height", type, 32 * EDSCALE);
	}

	// Responsive toolbars and action groups.
	p_theme->set_constant("h_separation", "EditorToolbar", 8 * EDSCALE);
	p_theme->set_constant("v_separation", "EditorToolbar", 8 * EDSCALE);
	p_theme->set_type_variation("EditorToolbarGroup", "HBoxContainer");
	p_theme->set_constant("separation", "EditorToolbarGroup", 8 * EDSCALE);

	// Standard fields. Resting borders define the hit target; focus uses a
	// stronger overlay without changing the control's measured size.
	Ref<StyleBoxFlat> field_normal = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, stroke, control_padding_h, control_padding_v, radius);
	Ref<StyleBoxFlat> field_read_only = _component_style(p_config.base_style, p_config.disabled_bg_color, p_config.disabled_border_color, stroke, control_padding_h, control_padding_v, radius);
	p_theme->set_type_variation("EditorFieldLineEdit", "LineEdit");
	p_theme->set_stylebox(CoreStringName(normal), "EditorFieldLineEdit", field_normal);
	p_theme->set_stylebox("focus", "EditorFieldLineEdit", focus_overlay);
	p_theme->set_stylebox("read_only", "EditorFieldLineEdit", field_read_only);

	p_theme->set_type_variation("EditorFieldOptionButton", "OptionButton");
	p_theme->set_stylebox(CoreStringName(normal), "EditorFieldOptionButton", field_normal);
	p_theme->set_stylebox(SceneStringName(hover), "EditorFieldOptionButton", action_hover);
	p_theme->set_stylebox(SceneStringName(pressed), "EditorFieldOptionButton", action_pressed);
	p_theme->set_stylebox("disabled", "EditorFieldOptionButton", field_read_only);
	p_theme->set_stylebox("focus", "EditorFieldOptionButton", focus_overlay);
	p_theme->set_constant("h_separation", "EditorFieldOptionButton", 8 * EDSCALE);

	// Search.
	p_theme->set_constant("separation", "EditorSearchBar", 10 * EDSCALE);
	Ref<StyleBoxFlat> search_normal = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, stroke, control_padding_h, 0, radius);
	Ref<StyleBoxFlat> search_focused = _component_style(p_config.base_style, p_config.surface_lower_color, focus_border, stroke, control_padding_h, 0, radius);
	p_theme->set_type_variation("EditorSearchField", "PanelContainer");
	p_theme->set_type_variation("EditorSearchFieldFocused", "EditorSearchField");
	p_theme->set_stylebox("panel", "EditorSearchField", search_normal);
	p_theme->set_stylebox("panel", "EditorSearchFieldFocused", search_focused);
	p_theme->set_type_variation("EditorSearchFieldContent", "HBoxContainer");
	p_theme->set_constant("separation", "EditorSearchFieldContent", 8 * EDSCALE);
	p_theme->set_constant("icon_size", "EditorSearchBar", 16 * EDSCALE);
	Ref<StyleBoxEmpty> search_line = p_config.base_empty_style->duplicate();
	search_line->set_content_margin_individual(0, control_padding_v, 0, control_padding_v);
	p_theme->set_type_variation("EditorSearchLineEdit", "LineEdit");
	p_theme->set_stylebox(CoreStringName(normal), "EditorSearchLineEdit", search_line);
	p_theme->set_stylebox("focus", "EditorSearchLineEdit", p_config.base_empty_style);
	p_theme->set_stylebox("read_only", "EditorSearchLineEdit", search_line);
	p_theme->set_type_variation("EditorSearchCount", "Label");
	p_theme->set_color("font_color", "EditorSearchCount", p_config.font_secondary_color);

	// Cards provide hierarchy without requiring every child control to paint a
	// surface. Their internal regions share the same edge and spacing rhythm.
	// Keep child surfaces inside the outline. With zero content margins the
	// header panel paints over the card's top and side borders.
	Ref<StyleBoxFlat> card = _component_style(p_config.base_style, p_config.surface_low_color, card_border, stroke, stroke, stroke, radius);
	p_theme->set_stylebox("panel", "EditorCard", card);
	p_theme->set_type_variation("EditorCardLayout", "VBoxContainer");
	p_theme->set_constant("separation", "EditorCardLayout", 0);
	Ref<StyleBoxFlat> card_header = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, 0, 12 * EDSCALE, 9 * EDSCALE, radius);
	card_header->set_corner_radius(CORNER_BOTTOM_LEFT, 0);
	card_header->set_corner_radius(CORNER_BOTTOM_RIGHT, 0);
	p_theme->set_type_variation("EditorCardHeader", "PanelContainer");
	p_theme->set_stylebox("panel", "EditorCardHeader", card_header);
	p_theme->set_type_variation("EditorCardDivider", "HSeparator");
	p_theme->set_stylebox("separator", "EditorCardDivider", EditorThemeManager::make_line_stylebox(card_border, stroke, 0, 0));
	p_theme->set_constant("separation", "EditorCardDivider", 0);
	p_theme->set_type_variation("EditorCardBody", "MarginContainer");
	for (const String &margin : { "margin_left", "margin_top", "margin_right", "margin_bottom" }) {
		p_theme->set_constant(margin, "EditorCardBody", 12 * EDSCALE);
	}
	p_theme->set_type_variation("EditorCardBodyContent", "VBoxContainer");
	p_theme->set_constant("separation", "EditorCardBodyContent", 10 * EDSCALE);
	Ref<StyleBoxFlat> card_footer = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, 0, 12 * EDSCALE, 8 * EDSCALE, radius);
	card_footer->set_border_width(SIDE_TOP, stroke);
	card_footer->set_corner_radius(CORNER_TOP_LEFT, 0);
	card_footer->set_corner_radius(CORNER_TOP_RIGHT, 0);
	p_theme->set_type_variation("EditorCardFooter", "PanelContainer");
	p_theme->set_stylebox("panel", "EditorCardFooter", card_footer);
	p_theme->set_type_variation("EditorCardFooterContent", "HBoxContainer");
	p_theme->set_constant("separation", "EditorCardFooterContent", 8 * EDSCALE);

	// Section and pane headers.
	p_theme->set_constant("separation", "EditorSectionHeader", 6 * EDSCALE);
	p_theme->set_type_variation("EditorSectionHeaderRow", "HBoxContainer");
	p_theme->set_constant("separation", "EditorSectionHeaderRow", 8 * EDSCALE);
	p_theme->set_type_variation("EditorSectionTitle", "HeaderSmall");
	p_theme->set_type_variation("EditorSectionDescription", "Label");
	p_theme->set_color("font_color", "EditorSectionDescription", p_config.font_secondary_color);
	p_theme->set_type_variation("EditorSectionBadge", "Label");
	p_theme->set_color("font_color", "EditorSectionBadge", p_config.accent_color);

	Ref<StyleBoxFlat> pane_header = _component_style(p_config.base_style, p_config.surface_low_color, resting_border, stroke, 14 * EDSCALE, 8 * EDSCALE, radius);
	p_theme->set_stylebox("panel", "EditorPaneHeader", pane_header);
	p_theme->set_constant("icon_size", "EditorPaneHeader", 20 * EDSCALE);
	p_theme->set_type_variation("EditorPaneHeaderLayout", "VBoxContainer");
	p_theme->set_constant("separation", "EditorPaneHeaderLayout", 0);
	p_theme->set_type_variation("EditorPaneTitle", "HeaderSmall");
	p_theme->set_type_variation("EditorPaneSubtitle", "Label");
	p_theme->set_color("font_color", "EditorPaneSubtitle", p_config.font_secondary_color);
	p_theme->set_font_size(SceneStringName(font_size), "EditorPaneSubtitle", MAX(8, p_theme->get_default_font_size() - 2 * Math::round(EDSCALE)));
	p_theme->set_type_variation("EditorPaneDirtyIndicator", "Label");
	p_theme->set_color("font_color", "EditorPaneDirtyIndicator", p_config.font_secondary_color.lerp(p_config.warning_color, 0.48));
	p_theme->set_font_size(SceneStringName(font_size), "EditorPaneDirtyIndicator", MAX(8, p_theme->get_default_font_size() - 2 * Math::round(EDSCALE)));
	Ref<StyleBoxEmpty> dirty_chip = p_config.base_empty_style->duplicate();
	dirty_chip->set_content_margin_individual(3 * EDSCALE, 0, 3 * EDSCALE, 0);
	p_theme->set_type_variation("EditorPaneDirtyChip", "PanelContainer");
	p_theme->set_stylebox("panel", "EditorPaneDirtyChip", dirty_chip);

	// Inspector chrome direction study. These tokens are intentionally consumed
	// only by the component gallery for now. Keeping the prototype theme-driven
	// lets it exercise light/dark themes and editor scaling before any of the
	// live Inspector controls adopt the visual language.
	// Reserve the outer stroke as content margin so full-bleed descendants
	// cannot paint over the single outline that owns the whole Inspector pane.
	Ref<StyleBoxFlat> inspector_preview_frame = _component_style(p_config.base_style, p_config.surface_base_color, card_border, stroke, stroke, stroke, radius);
	p_theme->set_type_variation("InspectorChromePreviewFrame", "PanelContainer");
	p_theme->set_stylebox("panel", "InspectorChromePreviewFrame", inspector_preview_frame);
	Ref<StyleBoxFlat> inspector_pane_header = _component_style(p_config.base_style, p_config.surface_low_color, card_border, 0, 14 * EDSCALE, 8 * EDSCALE, MAX(0, radius - stroke));
	inspector_pane_header->set_border_width(SIDE_BOTTOM, stroke);
	inspector_pane_header->set_border_color(card_border);
	inspector_pane_header->set_corner_radius(CORNER_BOTTOM_LEFT, 0);
	inspector_pane_header->set_corner_radius(CORNER_BOTTOM_RIGHT, 0);
	p_theme->set_type_variation("InspectorChromePaneHeader", "EditorPaneHeader");
	p_theme->set_stylebox("panel", "InspectorChromePaneHeader", inspector_pane_header);
	p_theme->set_type_variation("InspectorChromePreviewCaption", "Label");
	p_theme->set_color("font_color", "InspectorChromePreviewCaption", p_config.font_secondary_color);
	p_theme->set_type_variation("InspectorChromeOverflowButton", "Button");
	p_theme->set_stylebox(CoreStringName(normal), "InspectorChromeOverflowButton", p_config.base_empty_style);
	p_theme->set_stylebox(SceneStringName(hover), "InspectorChromeOverflowButton", p_config.flat_button_hover);
	p_theme->set_stylebox(SceneStringName(pressed), "InspectorChromeOverflowButton", p_config.flat_button_pressed);
	p_theme->set_stylebox("hover_pressed", "InspectorChromeOverflowButton", p_config.flat_button_hover_pressed);
	p_theme->set_stylebox("focus", "InspectorChromeOverflowButton", focus_overlay);
	p_theme->set_constant("minimum_width", "InspectorChromeOverflowButton", 24 * EDSCALE);
	p_theme->set_constant("minimum_height", "InspectorChromeOverflowButton", 24 * EDSCALE);

	// A saturated amber reads as an intentional modified-state accent without
	// inheriting the editor warning color's muddy/tan cast on dark surfaces.
	const Color inspector_modified_color = p_config.dark_theme ? Color(0.96, 0.58, 0.14) : Color(0.72, 0.34, 0.025);
	const Color class_warm = p_config.surface_high_color.lerp(inspector_modified_color, p_config.dark_theme ? 0.20 : 0.12);
	const Color class_accent = p_config.surface_high_color.lerp(p_config.accent_color, p_config.dark_theme ? 0.22 : 0.13);
	Color shifted_accent = p_config.accent_color;
	shifted_accent.set_hsv(Math::fmod(shifted_accent.get_h() + 0.14f, 1.0f), shifted_accent.get_s() * 0.72f, shifted_accent.get_v());
	const Color class_shifted = p_config.surface_high_color.lerp(shifted_accent, p_config.dark_theme ? 0.20 : 0.12);
	const Color gradient_fade = p_config.surface_base_color;
	p_theme->set_color("class_0_start", "InspectorChromeHeaderPreview", class_warm);
	p_theme->set_color("class_0_end", "InspectorChromeHeaderPreview", gradient_fade.lerp(class_warm, 0.30));
	p_theme->set_color("class_1_start", "InspectorChromeHeaderPreview", class_accent);
	p_theme->set_color("class_1_end", "InspectorChromeHeaderPreview", gradient_fade.lerp(class_accent, 0.30));
	p_theme->set_color("class_2_start", "InspectorChromeHeaderPreview", class_shifted);
	p_theme->set_color("class_2_end", "InspectorChromeHeaderPreview", gradient_fade.lerp(class_shifted, 0.30));
	const Color section_fill = p_config.surface_base_color.lerp(p_config.surface_high_color, p_config.dark_theme ? 0.14 : 0.10);
	p_theme->set_color("section_start", "InspectorChromeHeaderPreview", section_fill);
	p_theme->set_color("section_end", "InspectorChromeHeaderPreview", section_fill);
	p_theme->set_color("hover_overlay_color", "InspectorChromeHeaderPreview", Color(p_config.mono_color, p_config.dark_theme ? 0.055 : 0.035));
	p_theme->set_color("font_color", "InspectorChromeHeaderPreview", p_config.font_color);
	p_theme->set_color("secondary_font_color", "InspectorChromeHeaderPreview", p_config.font_secondary_color);
	Color section_badge_fill = p_config.font_secondary_color.lerp(p_config.accent_color, p_config.dark_theme ? 0.16 : 0.10);
	section_badge_fill.a = p_config.dark_theme ? 0.18 : 0.12;
	p_theme->set_color("badge_fill_color", "InspectorChromeHeaderPreview", section_badge_fill);
	p_theme->set_color("badge_text_color", "InspectorChromeHeaderPreview", p_config.font_secondary_color.lerp(p_config.font_color, 0.20));
	p_theme->set_constant("category_height", "InspectorChromeHeaderPreview", 24 * EDSCALE);
	p_theme->set_constant("section_height", "InspectorChromeHeaderPreview", 29 * EDSCALE);
	p_theme->set_constant("category_horizontal_padding", "InspectorChromeHeaderPreview", 14 * EDSCALE);
	p_theme->set_constant("horizontal_padding", "InspectorChromeHeaderPreview", 9 * EDSCALE);
	p_theme->set_color("guide_color", "InspectorChromeBodyPreview", p_config.separator_color.lerp(p_config.font_secondary_color, 0.24));
	p_theme->set_type_variation("InspectorChromePropertyLabel", "Label");
	p_theme->set_color("font_color", "InspectorChromePropertyLabel", p_config.font_color.lerp(p_config.accent_color, p_config.dark_theme ? 0.10 : 0.06));
	p_theme->set_font_size(SceneStringName(font_size), "InspectorChromePropertyLabel", MAX(8, p_theme->get_default_font_size() - 2 * Math::round(EDSCALE)));
	p_theme->set_type_variation("InspectorChromeResetButton", "Button");
	p_theme->set_stylebox(CoreStringName(normal), "InspectorChromeResetButton", p_config.base_empty_style);
	p_theme->set_stylebox(SceneStringName(hover), "InspectorChromeResetButton", p_config.flat_button_hover);
	p_theme->set_stylebox(SceneStringName(pressed), "InspectorChromeResetButton", p_config.flat_button_pressed);
	p_theme->set_stylebox("focus", "InspectorChromeResetButton", focus_overlay);
	p_theme->set_color("icon_normal_color", "InspectorChromeResetButton", inspector_modified_color);
	p_theme->set_color("icon_hover_color", "InspectorChromeResetButton", inspector_modified_color.lerp(p_config.mono_color, 0.18));

	p_theme->set_type_variation("InspectorChromeField", "EditorFieldLineEdit");
	p_theme->set_font_size(SceneStringName(font_size), "InspectorChromeField", MAX(8, p_theme->get_default_font_size() - 2 * Math::round(EDSCALE)));
	Ref<StyleBoxFlat> inspector_modified_field = field_normal->duplicate();
	inspector_modified_field->set_bg_color(p_config.surface_lower_color.lerp(inspector_modified_color, 0.025));
	inspector_modified_field->set_border_color(resting_border.lerp(inspector_modified_color, 0.68));
	p_theme->set_type_variation("InspectorChromeModifiedField", "InspectorChromeField");
	p_theme->set_stylebox(CoreStringName(normal), "InspectorChromeModifiedField", inspector_modified_field);
	p_theme->set_stylebox("focus", "InspectorChromeModifiedField", focus_overlay);
	p_theme->set_stylebox("read_only", "InspectorChromeModifiedField", field_read_only);

	Ref<StyleBoxFlat> axis_field = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, stroke, 0, 0, MAX(2, radius - stroke));
	p_theme->set_type_variation("InspectorChromeAxisField", "PanelContainer");
	p_theme->set_stylebox("panel", "InspectorChromeAxisField", axis_field);
	Ref<StyleBoxFlat> axis_field_modified = axis_field->duplicate();
	axis_field_modified->set_border_color(resting_border.lerp(inspector_modified_color, 0.68));
	p_theme->set_type_variation("InspectorChromeAxisFieldModified", "InspectorChromeAxisField");
	p_theme->set_stylebox("panel", "InspectorChromeAxisFieldModified", axis_field_modified);
	Ref<StyleBoxEmpty> axis_value = p_config.base_empty_style->duplicate();
	axis_value->set_content_margin_individual(5 * EDSCALE, 3 * EDSCALE, 5 * EDSCALE, 3 * EDSCALE);
	p_theme->set_type_variation("InspectorChromeAxisValue", "LineEdit");
	p_theme->set_stylebox(CoreStringName(normal), "InspectorChromeAxisValue", axis_value);
	p_theme->set_stylebox("focus", "InspectorChromeAxisValue", p_config.base_empty_style);
	p_theme->set_stylebox("read_only", "InspectorChromeAxisValue", axis_value);
	p_theme->set_type_variation("InspectorChromeAxisChip", "PanelContainer");
	const Color axis_colors[] = { Color(0.72, 0.12, 0.16), Color(0.08, 0.55, 0.22), Color(0.06, 0.34, 0.72) };
	const StringName axis_types[] = { SNAME("InspectorChromeAxisX"), SNAME("InspectorChromeAxisY"), SNAME("InspectorChromeAxisZ") };
	for (int i = 0; i < 3; i++) {
		p_theme->set_type_variation(axis_types[i], "InspectorChromeAxisChip");
		Ref<StyleBoxFlat> axis_chip = _component_style(p_config.base_style, axis_colors[i], Color(0, 0, 0, 0), 0, 0, 0, 0);
		axis_chip->set_border_width(SIDE_LEFT, stroke);
		axis_chip->set_border_width(SIDE_TOP, stroke);
		axis_chip->set_border_width(SIDE_BOTTOM, stroke);
		axis_chip->set_border_color(resting_border);
		axis_chip->set_corner_radius(CORNER_TOP_LEFT, MAX(2, radius - stroke));
		axis_chip->set_corner_radius(CORNER_BOTTOM_LEFT, MAX(2, radius - stroke));
		p_theme->set_stylebox("panel", axis_types[i], axis_chip);
	}
	p_theme->set_type_variation("InspectorChromeAxisLabel", "Label");
	p_theme->set_color("font_color", "InspectorChromeAxisLabel", Color(1, 1, 1));
	p_theme->set_font_size(SceneStringName(font_size), "InspectorChromeAxisLabel", MAX(8, p_theme->get_default_font_size() - 2 * Math::round(EDSCALE)));

	Ref<StyleBoxFlat> inspector_resource_field = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, stroke, 5 * EDSCALE, 2 * EDSCALE, MAX(2, radius - stroke));
	p_theme->set_type_variation("InspectorChromeResourceField", "PanelContainer");
	p_theme->set_stylebox("panel", "InspectorChromeResourceField", inspector_resource_field);
	Ref<StyleBoxFlat> inspector_resource_field_modified = inspector_resource_field->duplicate();
	inspector_resource_field_modified->set_border_color(resting_border.lerp(inspector_modified_color, 0.68));
	p_theme->set_type_variation("InspectorChromeResourceFieldModified", "InspectorChromeResourceField");
	p_theme->set_stylebox("panel", "InspectorChromeResourceFieldModified", inspector_resource_field_modified);
	p_theme->set_type_variation("InspectorChromeResourceBadge", "Label");
	p_theme->set_color("font_color", "InspectorChromeResourceBadge", p_config.highlight_color);
	p_theme->set_font_size(SceneStringName(font_size), "InspectorChromeResourceBadge", MAX(8, p_theme->get_default_font_size() - Math::round(EDSCALE)));
	p_theme->set_type_variation("InspectorChromeResourceName", "Label");
	p_theme->set_font_size(SceneStringName(font_size), "InspectorChromeResourceName", MAX(8, p_theme->get_default_font_size() - Math::round(EDSCALE)));

	// Forms.
	p_theme->set_constant("separation", "EditorForm", 12 * EDSCALE);
	p_theme->set_constant("separation", "EditorFormSection", 0);
	p_theme->set_type_variation("EditorFormSectionBody", "VBoxContainer");
	p_theme->set_constant("separation", "EditorFormSectionBody", 10 * EDSCALE);
	p_theme->set_constant("separation", "EditorFormRow", 5 * EDSCALE);
	p_theme->set_constant("label_minimum_width", "EditorFormRow", 140 * EDSCALE);
	p_theme->set_color("info_color", "EditorFormRow", p_config.highlight_color);
	p_theme->set_color("success_color", "EditorFormRow", p_config.success_color);
	p_theme->set_color("warning_color", "EditorFormRow", p_config.warning_color);
	p_theme->set_color("error_color", "EditorFormRow", p_config.error_color);
	p_theme->set_type_variation("EditorFormMainRow", "HFlowContainer");
	p_theme->set_constant("h_separation", "EditorFormMainRow", 12 * EDSCALE);
	p_theme->set_constant("v_separation", "EditorFormMainRow", 8 * EDSCALE);
	p_theme->set_type_variation("EditorFormEditorSlot", "VBoxContainer");
	p_theme->set_type_variation("EditorFormLabel", "Label");
	p_theme->set_type_variation("EditorFormLabelContainer", "HBoxContainer");
	p_theme->set_constant("separation", "EditorFormLabelContainer", 3 * EDSCALE);
	p_theme->set_type_variation("EditorFormRequired", "Label");
	p_theme->set_color("font_color", "EditorFormRequired", p_config.warning_color);
	p_theme->set_type_variation("EditorFormDescription", "Label");
	p_theme->set_color("font_color", "EditorFormDescription", p_config.font_secondary_color);
	p_theme->set_type_variation("EditorFormStatus", "Label");

	// Empty states.
	p_theme->set_constant("icon_size", "EditorEmptyState", 48 * EDSCALE);
	p_theme->set_constant("content_minimum_width", "EditorEmptyState", 240 * EDSCALE);
	p_theme->set_type_variation("EditorEmptyStateContent", "VBoxContainer");
	p_theme->set_constant("separation", "EditorEmptyStateContent", p_config.increased_margin * EDSCALE);
	p_theme->set_type_variation("EditorEmptyStateTitle", "HeaderMedium");
	p_theme->set_type_variation("EditorEmptyStateDescription", "Label");
	p_theme->set_color("font_color", "EditorEmptyStateDescription", p_config.font_secondary_color);
	p_theme->set_type_variation("EditorEmptyStateActions", "HBoxContainer");
	p_theme->set_constant("separation", "EditorEmptyStateActions", p_config.increased_margin * EDSCALE);

	// Persistent status panels. Status panel backgrounds use the standard panel
	// style; severity is communicated through its icon and title color.
	Ref<StyleBoxFlat> status_panel = _component_style(p_config.base_style, p_config.surface_lower_color, resting_border, stroke, 10 * EDSCALE, 8 * EDSCALE, radius);
	p_theme->set_stylebox("panel", "EditorStatusPanel", status_panel);
	p_theme->set_color("info_color", "EditorStatusPanel", p_config.highlight_color);
	p_theme->set_color("success_color", "EditorStatusPanel", p_config.success_color);
	p_theme->set_color("warning_color", "EditorStatusPanel", p_config.warning_color);
	p_theme->set_color("error_color", "EditorStatusPanel", p_config.error_color);
	p_theme->set_constant("icon_size", "EditorStatusPanel", 16 * EDSCALE);
	p_theme->set_type_variation("EditorStatusPanelRow", "HBoxContainer");
	p_theme->set_constant("separation", "EditorStatusPanelRow", 10 * EDSCALE);
	p_theme->set_type_variation("EditorStatusText", "VBoxContainer");
	p_theme->set_constant("separation", "EditorStatusText", 4 * EDSCALE);
	p_theme->set_type_variation("EditorStatusTitleRow", "HBoxContainer");
	p_theme->set_constant("separation", "EditorStatusTitleRow", 8 * EDSCALE);
	p_theme->set_type_variation("EditorStatusMessageRow", "HBoxContainer");
	p_theme->set_constant("separation", "EditorStatusMessageRow", 8 * EDSCALE);
	p_theme->set_type_variation("EditorStatusTitle", "HeaderSmall");
	p_theme->set_type_variation("EditorStatusMessage", "Label");
	p_theme->set_color("font_color", "EditorStatusMessage", p_config.font_secondary_color);
}
