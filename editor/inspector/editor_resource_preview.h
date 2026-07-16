/**************************************************************************/
/*  editor_resource_preview.h                                             */
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

#pragma once

#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "scene/main/node.h"

class Image;
class ImageTexture;
class PackedScene;
class SceneState;
class SubViewport;
class Texture2D;

class EditorResourcePreviewGenerator : public RefCounted {
	GDCLASS(EditorResourcePreviewGenerator, RefCounted);

protected:
	static void _bind_methods();

	GDVIRTUAL1RC_REQUIRED(bool, _handles, String)
	GDVIRTUAL3RC_REQUIRED(Ref<Texture2D>, _generate, Ref<Resource>, Vector2i, Dictionary)
	GDVIRTUAL3RC(Ref<Texture2D>, _generate_from_path, String, Vector2i, Dictionary)
	GDVIRTUAL0RC(bool, _generate_small_preview_automatically)
	GDVIRTUAL0RC(bool, _can_generate_small_preview)

	class DrawRequester : public Object {
		Semaphore semaphore;

		void _post_semaphore();
		void _prepare_draw(RID p_viewport);

	public:
		void request_and_wait(RID p_viewport);
		void abort();
	};

public:
	virtual bool handles(const String &p_type) const;
	virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const;
	virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const;
	virtual void abort() {}

	virtual bool generate_small_preview_automatically() const;
	virtual bool can_generate_small_preview() const;
	void request_draw_and_wait(RID viewport) const;
};

class EditorResourcePreview : public Node {
	GDCLASS(EditorResourcePreview, Node);

public:
	enum PreviewPriority {
		PREVIEW_PRIORITY_LOW,
		PREVIEW_PRIORITY_NORMAL,
		PREVIEW_PRIORITY_HIGH,
	};

private:
	static constexpr int CURRENT_METADATA_VERSION = 2; // Increment this number to invalidate all previews.
	static constexpr int MAX_SCENE_PREVIEW_NODES = 4096;
	static constexpr int MAX_SCENE_PREVIEW_QUEUE_SIZE = 256;
	static constexpr int MAX_SCENE_PREVIEW_RESOURCE_DEPTH = 64;
	inline static EditorResourcePreview *singleton = nullptr;

	friend struct EditorResourcePreviewTestAccess;

	struct QueueItem {
		Ref<Resource> resource;
		String path;
		Callable callback;
	};

	struct SceneQueueItem {
		String path;
		Callable callback;
		PreviewPriority priority = PREVIEW_PRIORITY_NORMAL;
	};

	enum ScenePreviewPhase {
		SCENE_PREVIEW_PHASE_SETUP,
		SCENE_PREVIEW_PHASE_RENDER,
	};

	struct ActiveScenePreview {
		SceneQueueItem item;
		SubViewport *viewport = nullptr;
		ScenePreviewPhase phase = SCENE_PREVIEW_PHASE_SETUP;
		uint64_t source_modified_time = 0;
		String source_hash;
	};

	List<QueueItem> queue;
	List<SceneQueueItem> scene_queue;
	ActiveScenePreview active_scene_preview;

	Mutex preview_mutex;
	Semaphore preview_sem;
	Thread thread;
	SafeFlag exiting;
	SafeFlag exited;

	struct Item {
		Ref<Texture2D> preview;
		Ref<Texture2D> small_preview;
		Dictionary preview_metadata;
		uint32_t last_hash = 0;
		uint64_t modified_time = 0;
		// Distinguishes a scene-pipeline result (including a negative one) from a
		// null entry cached by the generic worker, which has no scene generator.
		bool scene_preview_attempted = false;
	};

	HashMap<String, Item> cache;

	void _preview_ready(const String &p_path, int p_hash, const Ref<Texture2D> &p_texture, const Ref<Texture2D> &p_small_texture, const Callable &p_callback, const Dictionary &p_metadata);
	void _generate_preview(Ref<ImageTexture> &r_texture, Ref<ImageTexture> &r_small_texture, const QueueItem &p_item, Dictionary &p_metadata);
	bool _load_cached_preview(const String &p_path, Ref<ImageTexture> &r_texture, Ref<ImageTexture> &r_small_texture, Dictionary &r_metadata);
	static bool _cached_preview_suppresses_scene_generation(const Item &p_item);
	static bool _is_preview_cache_valid(int p_cached_size, int p_expected_size, bool p_outdated, uint64_t p_cached_modified_time, uint64_t p_modified_time, const String &p_cached_hash, const String &p_current_hash);
	static uint64_t _get_preview_modified_time(const String &p_path);

	int small_thumbnail_size = -1;

	static void _thread_func(void *ud);
	void _thread(); // For rendering drivers supporting async texture creation.
	static void _idle_callback(); // For other rendering drivers (i.e., OpenGL).
	void _iterate();
	void _process_scene_preview();
	void _queue_scene_preview(const SceneQueueItem &p_item);
	bool _pop_scene_preview(SceneQueueItem &r_item);
	void _begin_scene_preview(const SceneQueueItem &p_item);
	void _setup_scene_preview_frame();
	void _capture_scene_preview();
	void _finish_scene_preview(const Ref<Image> &p_image, const String &p_failure_reason = String());
	void _scene_preview_ready(const String &p_path, const Ref<Texture2D> &p_texture, const Ref<Texture2D> &p_small_texture, const Callable &p_callback);
	void _clear_active_scene_preview();
	static bool _is_scene_node_count_within_limit(const Ref<SceneState> &p_state, int p_limit = MAX_SCENE_PREVIEW_NODES);
	static bool _count_scene_nodes(const Ref<SceneState> &p_state, int p_limit, int &r_count, HashSet<ObjectID> &r_stack);
	static bool _sanitize_packed_scene(const Ref<PackedScene> &p_scene, HashSet<ObjectID> &r_visited, String &r_failure_reason, int p_depth = 0);
	static bool _sanitize_scene_variant(Variant &r_variant, HashSet<ObjectID> &r_visited, String &r_failure_reason, int p_depth);
	static bool _sanitize_proxy_node(Node *p_node, int &r_node_count, String &r_failure_reason);
	static void _find_scene_bounds(Node *p_node, AABB &r_aabb, bool &r_has_3d, Rect2 &r_rect, bool &r_has_2d);
	static void _hide_canvas_items(Node *p_node);

	void _write_preview_cache(Ref<FileAccess> p_file, int p_thumbnail_size, bool p_has_small_texture, uint64_t p_modified_time, const String &p_hash, const Dictionary &p_metadata);
	void _read_preview_cache(Ref<FileAccess> p_file, int *r_thumbnail_size, bool *r_has_small_texture, uint64_t *r_modified_time, String *r_hash, Dictionary *r_metadata, bool *r_outdated);

	Vector<Ref<EditorResourcePreviewGenerator>> preview_generators;

	void _update_thumbnail_sizes();

	// TODO: These should be deprecated and the new methods exposed instead.
	void _queue_resource_preview(const String &p_path, Object *p_receiver, const StringName &p_receiver_func, const Variant &p_userdata);
	void _queue_edited_resource_preview(const Ref<Resource> &p_res, Object *p_receiver, const StringName &p_receiver_func, const Variant &p_userdata);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static EditorResourcePreview *get_singleton();

	struct PreviewItem {
		Ref<Texture2D> preview;
		Ref<Texture2D> small_preview;
	};

	void queue_resource_preview(const String &p_path, const Callable &p_callback);
	void queue_edited_resource_preview(const Ref<Resource> &p_res, const Callable &p_callback);
	void queue_scene_preview(const String &p_path, const Callable &p_callback, PreviewPriority p_priority = PREVIEW_PRIORITY_NORMAL);
	void cancel_scene_preview(const String &p_path, const Callable &p_callback);
	const Dictionary get_preview_metadata(const String &p_path) const;

	PreviewItem get_resource_preview_if_available(const String &p_path);
	// Publishes the image files and their fingerprint metadata as one cache commit.
	Error save_preview_cache(const String &p_path, const Ref<Image> &p_preview, const Ref<Image> &p_small_preview = Ref<Image>(), const Dictionary &p_metadata = Dictionary());

	void add_preview_generator(const Ref<EditorResourcePreviewGenerator> &p_generator);
	void remove_preview_generator(const Ref<EditorResourcePreviewGenerator> &p_generator);
	void check_for_invalidation(const String &p_path);

	void start();
	void stop();
	bool is_threaded() const;

	EditorResourcePreview();
	~EditorResourcePreview();
};
