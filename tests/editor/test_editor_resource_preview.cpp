/**************************************************************************/
/*  test_editor_resource_preview.cpp                                      */
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

TEST_FORCE_LINK(test_editor_resource_preview)

#ifdef TOOLS_ENABLED
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "editor/file_system/editor_paths.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/texture.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/resources/packed_scene.h"
#include "tests/test_utils.h"

struct EditorResourcePreviewTestAccess {
	static void cache_negative_preview(EditorResourcePreview &p_preview, const String &p_path, uint64_t p_modified_time = 0) {
		EditorResourcePreview::Item item;
		item.modified_time = p_modified_time;
		item.scene_preview_attempted = true;
		p_preview.cache[p_path] = item;
	}

	static bool cached_preview_suppresses_scene_generation(bool p_has_preview, bool p_scene_preview_attempted) {
		EditorResourcePreview::Item item;
		if (p_has_preview) {
			item.preview = ImageTexture::create_from_image(Image::create_empty(4, 4, false, Image::FORMAT_RGBA8));
		}
		item.scene_preview_attempted = p_scene_preview_attempted;
		return EditorResourcePreview::_cached_preview_suppresses_scene_generation(item);
	}

	static bool has_cached_preview(EditorResourcePreview &p_preview, const String &p_path) {
		return p_preview.cache.has(p_path);
	}

	static void write_cache_metadata(EditorResourcePreview &p_preview, const Ref<FileAccess> &p_file, int p_thumbnail_size, uint64_t p_modified_time, const String &p_hash) {
		p_preview._write_preview_cache(p_file, p_thumbnail_size, false, p_modified_time, p_hash, Dictionary());
	}

	static void queue_and_iterate(EditorResourcePreview &p_preview, const String &p_path, const Callable &p_callback) {
		EditorResourcePreview::QueueItem item;
		item.path = p_path;
		item.callback = p_callback;
		p_preview.queue.push_back(item);
		p_preview._iterate();
	}

	static void clear_singleton() {
		EditorResourcePreview::singleton = nullptr;
	}

	static void enqueue_scene_preview(EditorResourcePreview &p_preview, const String &p_path, EditorResourcePreview::PreviewPriority p_priority) {
		EditorResourcePreview::SceneQueueItem item;
		item.path = p_path;
		item.priority = p_priority;
		p_preview._queue_scene_preview(item);
	}

	static String pop_scene_preview(EditorResourcePreview &p_preview) {
		EditorResourcePreview::SceneQueueItem item;
		return p_preview._pop_scene_preview(item) ? item.path : String();
	}

	static bool is_scene_queue_empty(const EditorResourcePreview &p_preview) {
		return p_preview.scene_queue.is_empty();
	}

	static bool is_preview_cache_valid(int p_cached_size, int p_expected_size, bool p_outdated, uint64_t p_cached_modified_time, uint64_t p_modified_time, const String &p_cached_hash, const String &p_current_hash) {
		return EditorResourcePreview::_is_preview_cache_valid(p_cached_size, p_expected_size, p_outdated, p_cached_modified_time, p_modified_time, p_cached_hash, p_current_hash);
	}

	static bool is_scene_node_count_within_limit(const Ref<SceneState> &p_state, int p_limit) {
		return EditorResourcePreview::_is_scene_node_count_within_limit(p_state, p_limit);
	}
};

namespace TestEditorResourcePreview {

static void _preview_ready(const String &, const Ref<Texture2D> &, const Ref<Texture2D> &) {
}

struct PreviewSingletonReset {
	~PreviewSingletonReset() {
		EditorResourcePreviewTestAccess::clear_singleton();
	}
};

TEST_CASE("[Editor][EditorResourcePreview] Scene requests are stable and priority ordered") {
	PreviewSingletonReset singleton_reset;
	EditorResourcePreview preview;

	EditorResourcePreviewTestAccess::enqueue_scene_preview(preview, "low_a", EditorResourcePreview::PREVIEW_PRIORITY_LOW);
	EditorResourcePreviewTestAccess::enqueue_scene_preview(preview, "normal", EditorResourcePreview::PREVIEW_PRIORITY_NORMAL);
	EditorResourcePreviewTestAccess::enqueue_scene_preview(preview, "high_a", EditorResourcePreview::PREVIEW_PRIORITY_HIGH);
	EditorResourcePreviewTestAccess::enqueue_scene_preview(preview, "low_b", EditorResourcePreview::PREVIEW_PRIORITY_LOW);
	EditorResourcePreviewTestAccess::enqueue_scene_preview(preview, "high_b", EditorResourcePreview::PREVIEW_PRIORITY_HIGH);

	CHECK(EditorResourcePreviewTestAccess::pop_scene_preview(preview) == "high_a");
	CHECK(EditorResourcePreviewTestAccess::pop_scene_preview(preview) == "high_b");
	CHECK(EditorResourcePreviewTestAccess::pop_scene_preview(preview) == "normal");
	CHECK(EditorResourcePreviewTestAccess::pop_scene_preview(preview) == "low_a");
	CHECK(EditorResourcePreviewTestAccess::pop_scene_preview(preview) == "low_b");
}

TEST_CASE("[Editor][EditorResourcePreview] Failed scene previews stay negatively cached") {
	PreviewSingletonReset singleton_reset;
	EditorResourcePreview preview;
	const String path = "res://missing_scene_preview.tscn";

	EditorResourcePreviewTestAccess::cache_negative_preview(preview, path);
	preview.queue_scene_preview(path, callable_mp_static(_preview_ready), EditorResourcePreview::PREVIEW_PRIORITY_HIGH);
	preview.queue_scene_preview(path, callable_mp_static(_preview_ready), EditorResourcePreview::PREVIEW_PRIORITY_HIGH);

	CHECK(EditorResourcePreviewTestAccess::is_scene_queue_empty(preview));
	CHECK(EditorResourcePreviewTestAccess::has_cached_preview(preview, path));
}

TEST_CASE("[Editor][EditorResourcePreview] Worker-cached null previews do not suppress scene generation") {
	// The generic worker has no scene generator, so its null entry means "never
	// attempted", while a scene-pipeline negative result means "do not retry".
	CHECK_FALSE(EditorResourcePreviewTestAccess::cached_preview_suppresses_scene_generation(false, false));
	CHECK(EditorResourcePreviewTestAccess::cached_preview_suppresses_scene_generation(false, true));
	CHECK(EditorResourcePreviewTestAccess::cached_preview_suppresses_scene_generation(true, false));
	CHECK(EditorResourcePreviewTestAccess::cached_preview_suppresses_scene_generation(true, true));
}

TEST_CASE("[Editor][EditorResourcePreview] Cache invalidation uses size, version, mtime, and hash") {
	CHECK(EditorResourcePreviewTestAccess::is_preview_cache_valid(64, 64, false, 10, 10, "old", String()));
	CHECK(EditorResourcePreviewTestAccess::is_preview_cache_valid(64, 64, false, 10, 11, "same", "same"));
	CHECK_FALSE(EditorResourcePreviewTestAccess::is_preview_cache_valid(32, 64, false, 10, 10, "same", String()));
	CHECK_FALSE(EditorResourcePreviewTestAccess::is_preview_cache_valid(64, 64, true, 10, 10, "same", String()));
	CHECK_FALSE(EditorResourcePreviewTestAccess::is_preview_cache_valid(64, 64, false, 10, 11, "old", "new"));
}

TEST_CASE("[Editor][EditorResourcePreview] Scene node limit is checked before instantiation") {
	Node *root = memnew(Node);
	root->set_name("Root");
	Node *child = memnew(Node);
	child->set_name("Child");
	root->add_child(child);
	child->set_owner(root);

	Ref<PackedScene> scene;
	scene.instantiate();
	REQUIRE(scene->pack(root) == OK);
	CHECK(EditorResourcePreviewTestAccess::is_scene_node_count_within_limit(scene->get_state(), 2));
	CHECK_FALSE(EditorResourcePreviewTestAccess::is_scene_node_count_within_limit(scene->get_state(), 1));
	memdelete(root);
}

TEST_CASE("[Editor][EditorResourcePreview] Cache survives an mtime-only source change") {
	PreviewSingletonReset singleton_reset;
	EditorResourcePreview preview;

	const String source_path = TestUtils::get_temp_path("editor_resource_preview_mtime.tres");
	Ref<FileAccess> source_file = FileAccess::open(source_path, FileAccess::WRITE);
	REQUIRE(source_file.is_valid());
	source_file->store_string("[gd_resource type=\"Resource\" format=3]\n\n[resource]\n");
	source_file.unref();

	Ref<Image> image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.25, 0.5, 0.75));

	EditorResourcePreviewTestAccess::cache_negative_preview(preview, source_path);
	CHECK(EditorResourcePreviewTestAccess::has_cached_preview(preview, source_path));
	REQUIRE(preview.save_preview_cache(source_path, image) == OK);
	CHECK_FALSE(EditorResourcePreviewTestAccess::has_cached_preview(preview, source_path));

	String cache_base = ProjectSettings::get_singleton()->globalize_path(source_path).md5_text();
	cache_base = EditorPaths::get_singleton()->get_cache_dir().path_join("resthumb-" + cache_base);
	const String metadata_path = cache_base + ".txt";

	const uint64_t modified_time = FileAccess::get_modified_time(source_path);
	const uint64_t previous_modified_time = modified_time == 0 ? 1 : modified_time - 1;
	int thumbnail_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	thumbnail_size *= EDSCALE;
	Ref<FileAccess> metadata_file = FileAccess::open(metadata_path, FileAccess::WRITE);
	REQUIRE(metadata_file.is_valid());
	EditorResourcePreviewTestAccess::write_cache_metadata(preview, metadata_file, thumbnail_size, previous_modified_time, FileAccess::get_md5(source_path));
	metadata_file.unref();

	EditorResourcePreviewTestAccess::queue_and_iterate(preview, source_path, callable_mp_static(_preview_ready));
	const EditorResourcePreview::PreviewItem cached_preview = preview.get_resource_preview_if_available(source_path);
	CHECK(cached_preview.preview.is_valid());

	if (FileAccess::exists(source_path)) {
		DirAccess::remove_absolute(source_path);
	}
	if (FileAccess::exists(cache_base + ".png")) {
		DirAccess::remove_absolute(cache_base + ".png");
	}
	if (FileAccess::exists(cache_base + "_small.png")) {
		DirAccess::remove_absolute(cache_base + "_small.png");
	}
	if (FileAccess::exists(metadata_path)) {
		DirAccess::remove_absolute(metadata_path);
	}
}

} // namespace TestEditorResourcePreview
#endif // TOOLS_ENABLED
