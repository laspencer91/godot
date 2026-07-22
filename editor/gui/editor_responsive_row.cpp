/**************************************************************************/
/*  editor_responsive_row.cpp                                             */
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

#include "editor_responsive_row.h"

#include "core/object/class_db.h"

void EditorResponsiveRow::_notification(int p_what) {
	if (p_what != NOTIFICATION_SORT_CHILDREN) {
		return;
	}

	const int line_count = get_line_count();
	wrapped = line_count > 1;
	if (line_count == last_line_count) {
		return;
	}

	last_line_count = line_count;
	emit_signal(SNAME("layout_changed"), line_count);
}

void EditorResponsiveRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_wrapped"), &EditorResponsiveRow::is_wrapped);

	ADD_SIGNAL(MethodInfo("layout_changed", PropertyInfo(Variant::INT, "line_count")));
}

EditorResponsiveRow::EditorResponsiveRow() {
	set_h_size_flags(SIZE_EXPAND_FILL);
}
