/**************************************************************************/
/*  test_dir_access.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_dir_access)

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

namespace TestDirAccess {

// Covers the DirAccess::supports_entry_metadata()/get_current_modified_time() virtuals added
// so EditorFileSystem can reuse the modified time that directory enumeration already returns
// on some platforms (currently Windows), instead of doing a separate stat per file. The safety
// invariant is: whenever the platform reports it supports entry metadata and returns a nonzero
// value, that value must be identical to what FileAccess::get_modified_time() would report for
// the same entry; any other combination must be treated as "unknown" by callers.
TEST_CASE("[DirAccess] Enumeration-provided modified time matches FileAccess::get_modified_time()") {
	ProjectSettings::get_singleton()->set_setting("application/config/name", "godot_tests");

	const String base = OS::get_singleton()->get_user_data_dir().path_join("dir_access_metadata_test");
	// Clean up any leftovers from a previously interrupted run before creating a fresh tree.
	DirAccess::remove_absolute(base.path_join("subdir").path_join("c.txt"));
	DirAccess::remove_absolute(base.path_join("subdir"));
	DirAccess::remove_absolute(base.path_join("a.txt"));
	DirAccess::remove_absolute(base.path_join("b.txt"));
	DirAccess::remove_absolute(base);

	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(base.path_join("subdir")), OK);

	{
		Ref<FileAccess> f = FileAccess::open(base.path_join("a.txt"), FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("hello");
	}
	{
		Ref<FileAccess> f = FileAccess::open(base.path_join("b.txt"), FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("world");
	}
	{
		Ref<FileAccess> f = FileAccess::open(base.path_join("subdir").path_join("c.txt"), FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("!");
	}

	Ref<DirAccess> da = DirAccess::open(base);
	REQUIRE(da.is_valid());

	int compared = 0;
	da->list_dir_begin();
	for (String entry = da->get_next(); !entry.is_empty(); entry = da->get_next()) {
		if (entry == "." || entry == "..") {
			continue;
		}

		if (da->supports_entry_metadata()) {
			uint64_t cached_mt = da->get_current_modified_time();
			// A zero value means "unknown" and callers must fall back to a real stat, so it
			// carries no parity requirement; only nonzero values are required to match exactly.
			if (cached_mt != 0) {
				uint64_t stat_mt = FileAccess::get_modified_time(base.path_join(entry));
				CHECK_EQ(cached_mt, stat_mt);
				compared++;
			}
		}
	}
	da->list_dir_end();

	if (da->supports_entry_metadata()) {
		CHECK_MESSAGE(compared > 0, "Expected at least one entry with a comparable cached modified time on a platform that supports entry metadata.");
	}

	DirAccess::remove_absolute(base.path_join("subdir").path_join("c.txt"));
	DirAccess::remove_absolute(base.path_join("subdir"));
	DirAccess::remove_absolute(base.path_join("a.txt"));
	DirAccess::remove_absolute(base.path_join("b.txt"));
	DirAccess::remove_absolute(base);
}

// Covers the EditorFileSystem::_reimport_file() optimization that hashes the .import file's
// content directly from the in-memory String it just wrote (String::md5_text(), which hashes
// the String's UTF-8 bytes) instead of reopening and rereading it from disk
// (FileAccess::get_md5(), which hashes the file's raw bytes). Since the file is written via
// FileAccess::store_string() with no newline translation, both must produce the identical
// lowercase hex MD5 for the same content.
TEST_CASE("[FileAccess] In-memory md5 of a String matches get_md5() of the file it was written to") {
	ProjectSettings::get_singleton()->set_setting("application/config/name", "godot_tests");

	const String content = "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\nuid=\"uid://deadbeef\"\n\n[deps]\n\nsource_file=\"res://icon.png\"\n\n[params]\n\ncompress/mode=0\n";
	const String path = OS::get_singleton()->get_user_data_dir().path_join("md5_parity_test.import");

	DirAccess::remove_absolute(path);
	REQUIRE_EQ(DirAccess::make_dir_recursive_absolute(OS::get_singleton()->get_user_data_dir()), OK);

	{
		Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string(content);
	}

	CHECK_EQ(FileAccess::get_md5(path), content.md5_text());

	DirAccess::remove_absolute(path);
}

} // namespace TestDirAccess
