/**************************************************************************/
/*  test_editor_responsive_layout.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_responsive_layout)

#ifdef TOOLS_ENABLED

#include "core/object/callable_mp.h"
#include "editor/docks/editor_dock.h"
#include "editor/gui/editor_responsive_row.h"
#include "scene/gui/control.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestEditorResponsiveLayout {

class TestDock : public EditorDock {
public:
	EditorDock::DockLayout callback_layout = (EditorDock::DockLayout)0;
	int callback_slot = EditorDock::DOCK_SLOT_NONE;
	int signal_layout = 0;
	int signal_slot = EditorDock::DOCK_SLOT_NONE;
	int signal_count = 0;

	void apply_layout(EditorDock::DockLayout p_layout, EditorDock::DockSlot p_slot) {
		_set_layout_context(p_layout, p_slot);
	}

	virtual void update_layout(EditorDock::DockLayout p_layout, int p_slot) override {
		callback_layout = p_layout;
		callback_slot = p_slot;
		CHECK(get_current_layout() == p_layout);
		CHECK(get_current_slot() == (p_layout == EditorDock::DOCK_LAYOUT_FLOATING ? EditorDock::DOCK_SLOT_NONE : p_slot));
	}

	void record_layout_signal(int p_layout, int p_slot) {
		signal_layout = p_layout;
		signal_slot = p_slot;
		signal_count++;
	}
};

TEST_CASE("[Editor][ResponsiveLayout] Responsive rows wrap complete child groups") {
	EditorResponsiveRow *row = memnew(EditorResponsiveRow);
	Window *root = SceneTree::get_singleton()->get_root();
	root->add_child(row);
	row->add_theme_constant_override("h_separation", 0);
	row->add_theme_constant_override("v_separation", 0);
	row->set_size(Size2(90, 100));

	Control *first_group = memnew(Control);
	Control *second_group = memnew(Control);
	Control *third_group = memnew(Control);
	first_group->set_custom_minimum_size(Size2(40, 10));
	second_group->set_custom_minimum_size(Size2(40, 10));
	third_group->set_custom_minimum_size(Size2(40, 10));
	row->add_child(first_group);
	row->add_child(second_group);
	row->add_child(third_group);
	SceneTree::get_singleton()->process(0);

	CHECK(row->get_line_count() == 2);
	CHECK(row->is_wrapped());
	CHECK(row->get_combined_minimum_size().x == 40);
	CHECK(first_group->get_position() == Point2(0, 0));
	CHECK(second_group->get_position() == Point2(40, 0));
	CHECK(third_group->get_position() == Point2(0, 10));

	row->set_size(Size2(120, 100));
	SceneTree::get_singleton()->process(0);
	CHECK(row->get_line_count() == 1);
	CHECK_FALSE(row->is_wrapped());

	memdelete(row);
}

TEST_CASE("[Editor][ResponsiveLayout] Dock context is current during callbacks and signals") {
	TestDock *dock = memnew(TestDock);
	dock->connect(SNAME("layout_changed"), callable_mp(dock, &TestDock::record_layout_signal));

	dock->apply_layout(EditorDock::DOCK_LAYOUT_VERTICAL, EditorDock::DOCK_SLOT_RIGHT_UL);
	CHECK(dock->callback_layout == EditorDock::DOCK_LAYOUT_VERTICAL);
	CHECK(dock->callback_slot == EditorDock::DOCK_SLOT_RIGHT_UL);
	CHECK(dock->get_current_layout() == EditorDock::DOCK_LAYOUT_VERTICAL);
	CHECK(dock->get_current_slot() == EditorDock::DOCK_SLOT_RIGHT_UL);
	CHECK(dock->signal_layout == EditorDock::DOCK_LAYOUT_VERTICAL);
	CHECK(dock->signal_slot == EditorDock::DOCK_SLOT_RIGHT_UL);
	CHECK(dock->signal_count == 1);

	dock->apply_layout(EditorDock::DOCK_LAYOUT_FLOATING, EditorDock::DOCK_SLOT_NONE);
	CHECK(dock->callback_layout == EditorDock::DOCK_LAYOUT_FLOATING);
	CHECK(dock->callback_slot == EditorDock::DOCK_SLOT_NONE);
	CHECK(dock->get_current_layout() == EditorDock::DOCK_LAYOUT_FLOATING);
	CHECK(dock->get_current_slot() == EditorDock::DOCK_SLOT_NONE);
	CHECK(dock->signal_layout == EditorDock::DOCK_LAYOUT_FLOATING);
	CHECK(dock->signal_slot == EditorDock::DOCK_SLOT_NONE);
	CHECK(dock->signal_count == 2);

	memdelete(dock);
}

} // namespace TestEditorResponsiveLayout

#endif // TOOLS_ENABLED
