/**************************************************************************/
/*  test_editor_components.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_components)

#ifdef TOOLS_ENABLED

#include "core/object/callable_mp.h"
#include "editor/gui/components/editor_action.h"
#include "editor/gui/components/editor_action_button.h"
#include "editor/gui/components/editor_card.h"
#include "editor/gui/components/editor_form_row.h"
#include "editor/gui/components/editor_form_section.h"
#include "editor/gui/components/editor_search_bar.h"
#include "editor/gui/components/editor_section_header.h"
#include "editor/gui/components/editor_status_panel.h"
#include "editor/gui/components/editor_toolbar.h"
#include "scene/gui/line_edit.h"

namespace TestEditorComponents {

class ActionReceiver : public Object {
public:
	int trigger_count = 0;

	void action_triggered() {
		trigger_count++;
	}
};

TEST_CASE("[Editor][Components] Actions share state with their buttons") {
	ActionReceiver receiver;
	Ref<EditorAction> action;
	action.instantiate();
	action->set_text("Save");
	action->set_callback(callable_mp(&receiver, &ActionReceiver::action_triggered));

	EditorToolbar *toolbar = memnew(EditorToolbar);
	EditorActionButton *button = toolbar->add_action(action);
	CHECK(button->get_text() == "Save");
	CHECK_FALSE(button->is_disabled());
	CHECK(button->get_default_cursor_shape() == Control::CURSOR_POINTING_HAND);

	action->set_enabled(false);
	CHECK(button->is_disabled());
	action->trigger();
	CHECK(receiver.trigger_count == 0);

	action->set_enabled(true);
	action->trigger();
	CHECK(receiver.trigger_count == 1);

	memdelete(toolbar);
}

TEST_CASE("[Editor][Components] Form sections own responsive rows and collapse state") {
	EditorFormSection *section = memnew(EditorFormSection);
	section->set_title("Identity");
	section->set_collapsible(true);
	CHECK(section->get_header()->get_default_cursor_shape() == Control::CURSOR_POINTING_HAND);

	LineEdit *editor = memnew(LineEdit);
	EditorFormRow *row = section->add_row("Name", editor);
	row->set_required(true);
	row->set_status(EditorFormRow::STATUS_WARNING, "Name already exists.");

	CHECK(row->get_editor() == editor);
	CHECK(editor->get_theme_type_variation() == SNAME("EditorFieldLineEdit"));
	CHECK(row->is_required());
	CHECK(row->get_status() == EditorFormRow::STATUS_WARNING);

	section->set_collapsed(true);
	CHECK(section->is_collapsed());
	section->set_collapsed(false);
	CHECK_FALSE(section->is_collapsed());

	memdelete(section);
}

TEST_CASE("[Editor][Components] Cards own body, footer, and collapse state") {
	EditorCard *card = memnew(EditorCard);
	card->set_title("Identity");
	card->set_description("Asset identity fields.");
	card->set_collapsible(true);

	LineEdit *body_control = memnew(LineEdit);
	card->add_body_control(body_control);
	CHECK(body_control->get_parent() == card->get_body());

	Ref<EditorAction> save_action;
	save_action.instantiate();
	save_action->set_text("Save");
	EditorActionButton *footer_action = card->add_footer_action(save_action, true);
	CHECK(footer_action->get_parent() == card->get_footer());

	card->set_collapsed(true);
	CHECK(card->is_collapsed());
	card->set_collapsed(false);
	CHECK_FALSE(card->is_collapsed());

	memdelete(card);
}

TEST_CASE("[Editor][Components] Search and status controls retain view state") {
	EditorSearchBar *search = memnew(EditorSearchBar);
	search->set_query("metal");
	search->set_result_count(12);
	search->set_debounce_msec(0);
	CHECK(search->get_query() == "metal");
	CHECK(search->get_result_count() == 12);
	CHECK(search->get_debounce_msec() == 0);
	memdelete(search);

	EditorStatusPanel *status = memnew(EditorStatusPanel);
	status->set_title("Import failed");
	status->set_message("The source could not be decoded.");
	status->set_severity(EditorStatusPanel::SEVERITY_ERROR);
	status->set_dismissible(true);
	CHECK(status->get_severity() == EditorStatusPanel::SEVERITY_ERROR);
	CHECK(status->is_dismissible());
	memdelete(status);
}

} // namespace TestEditorComponents

#endif // TOOLS_ENABLED
