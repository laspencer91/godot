/**************************************************************************/
/*  editor_component_gallery_plugin.cpp                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "editor_component_gallery_plugin.h"

#include "core/object/callable_mp.h"
#include "editor/gui/components/editor_action.h"
#include "editor/gui/components/editor_card.h"
#include "editor/gui/components/editor_empty_state.h"
#include "editor/gui/components/editor_form.h"
#include "editor/gui/components/editor_form_row.h"
#include "editor/gui/components/editor_form_section.h"
#include "editor/gui/components/editor_pane_header.h"
#include "editor/gui/components/editor_search_bar.h"
#include "editor/gui/components/editor_status_panel.h"
#include "editor/gui/components/editor_toolbar.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"

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
