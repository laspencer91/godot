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

	Ref<StyleBoxFlat> pane_header = _component_style(p_config.base_style, p_config.surface_low_color, resting_border, stroke, 14 * EDSCALE, 12 * EDSCALE, radius);
	p_theme->set_stylebox("panel", "EditorPaneHeader", pane_header);
	p_theme->set_constant("icon_size", "EditorPaneHeader", 20 * EDSCALE);
	p_theme->set_type_variation("EditorPaneTitle", "HeaderSmall");
	p_theme->set_type_variation("EditorPaneSubtitle", "Label");
	p_theme->set_color("font_color", "EditorPaneSubtitle", p_config.font_secondary_color);
	p_theme->set_type_variation("EditorPaneDirtyIndicator", "Label");
	p_theme->set_color("font_color", "EditorPaneDirtyIndicator", p_config.font_secondary_color.lerp(p_config.warning_color, 0.55));
	p_theme->set_font_size(SceneStringName(font_size), "EditorPaneDirtyIndicator", MAX(8, p_theme->get_default_font_size() - Math::round(EDSCALE)));
	Ref<StyleBoxFlat> dirty_chip = _component_style(p_config.base_style, p_config.surface_lower_color.lerp(p_config.warning_color, 0.03), resting_border.lerp(p_config.warning_color, 0.28), stroke, 5 * EDSCALE, 1 * EDSCALE, MAX(2, radius - Math::round(EDSCALE)));
	p_theme->set_type_variation("EditorPaneDirtyChip", "PanelContainer");
	p_theme->set_stylebox("panel", "EditorPaneDirtyChip", dirty_chip);

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
