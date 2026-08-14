/**************************************************************************/
/*  test_derived_data_dialog.cpp                                          */
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

#include "editor/derived_data/derived_data_dialog.h"
#include "editor/derived_data/editor_derived_data.h"

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_derived_data_dialog)

struct TestDerivedDataDialogAccess {
	static bool referenced_orphan_is_deletable() {
		DerivedDataDialog::Entry entry;
		entry.state = DerivedDataDialog::STATE_ORPHAN;
		entry.referrers.push_back("res://still_uses_it.tscn");
		return DerivedDataDialog::_entry_deletable(entry);
	}

	static bool referenced_legacy_is_deletable() {
		DerivedDataDialog::Entry entry;
		entry.state = DerivedDataDialog::STATE_LEGACY;
		entry.referrers.push_back("res://still_uses_it.tres");
		return DerivedDataDialog::_entry_deletable(entry);
	}

	static bool invalid_manifest_is_deletable() {
		DerivedDataDialog::Entry entry;
		entry.state = DerivedDataDialog::STATE_UNKNOWN;
		return DerivedDataDialog::_entry_deletable(entry);
	}

	static bool unreferenced_orphan_is_deletable() {
		DerivedDataDialog::Entry entry;
		entry.state = DerivedDataDialog::STATE_ORPHAN;
		return DerivedDataDialog::_entry_deletable(entry);
	}
};

namespace TestDerivedDataDialog {

TEST_CASE("[Editor][DerivedData] Producers register slots without project configuration") {
	EditorDerivedData *derived_data = EditorDerivedData::get_singleton();
	REQUIRE(derived_data != nullptr);

	PackedStringArray extensions;
	extensions.push_back("bin");
	const StringName slot = SNAME("test.zero_configuration");
	CHECK(derived_data->register_slot(slot, SNAME("test_producer"), extensions) == OK);
	CHECK(derived_data->has_slot(slot));
	CHECK(derived_data->register_slot(slot, SNAME("test_producer"), extensions) == OK);
	CHECK(derived_data->register_slot(slot, SNAME("other_producer"), extensions) == ERR_ALREADY_EXISTS);

	const PackedStringArray roots = derived_data->get_roots();
	CHECK(roots.has("res://__derived/"));
	CHECK(roots.has("res://.godot/derived/"));

	derived_data->unregister_slot(slot);
	CHECK_FALSE(derived_data->has_slot(slot));
}

TEST_CASE("[Editor][DerivedData] References always prevent automatic deletion") {
	CHECK_FALSE(TestDerivedDataDialogAccess::referenced_orphan_is_deletable());
	CHECK_FALSE(TestDerivedDataDialogAccess::referenced_legacy_is_deletable());
}

TEST_CASE("[Editor][DerivedData] Invalid manifests fail closed") {
	CHECK_FALSE(TestDerivedDataDialogAccess::invalid_manifest_is_deletable());
}

TEST_CASE("[Editor][DerivedData] An unreferenced orphan remains reclaimable") {
	CHECK(TestDerivedDataDialogAccess::unreferenced_orphan_is_deletable());
}

} // namespace TestDerivedDataDialog
