/**************************************************************************/
/*  material_preview_generator.h                                          */
/**************************************************************************/
/*  G-Level LE2: private flat-material preview queue and cache.           */
/**************************************************************************/

#pragma once

#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/lru.h"
#include "core/object/ref_counted.h"

class MaterialFlatPreviewGenerator;
class TexelDensityScanner;
class Texture2D;

class MaterialBrowserPreviewQueue : public RefCounted {
	GDCLASS(MaterialBrowserPreviewQueue, RefCounted);

public:
	static constexpr int THUMBNAIL_SIZE = 192;

private:

	struct PreviewRequest {
		String path;
		uint64_t revision = 0;
	};

	Ref<TexelDensityScanner> scanner;
	Ref<MaterialFlatPreviewGenerator> generator;
	LRUCache<String, Ref<Texture2D>> memory_cache = LRUCache<String, Ref<Texture2D>>(128);
	List<PreviewRequest> requests;
	HashSet<String> pending_paths;
	HashSet<String> completed_paths;
	HashMap<String, Vector<Callable>> callbacks;
	HashMap<String, uint64_t> revisions;
	mutable Mutex mutex;
	Semaphore semaphore;
	Thread thread;
	bool initialized = false;
	bool stopping = false;
	bool use_background_thread = false;
	bool deferred_iteration_queued = false;

	static void _thread_func(void *p_userdata);
	void _thread_loop();
	void _deferred_iterate();
	void _schedule_iteration();
	bool _pop_request(PreviewRequest &r_request);
	Ref<Texture2D> _process_request(const PreviewRequest &p_request);
	void _finish_request(const PreviewRequest &p_request, const Ref<Texture2D> &p_texture);

	static String _cache_base(const String &p_path);
	static uint64_t _modified_time(const String &p_path);
	static Ref<Texture2D> _load_disk_cache(const String &p_path);
	static void _store_disk_cache(const String &p_path, const Ref<Texture2D> &p_texture);
	static void _remove_disk_cache(const String &p_path);

protected:
	static void _bind_methods();

public:
	void initialize(const Ref<TexelDensityScanner> &p_scanner);
	void request_preview(const String &p_path, const Callable &p_callback = Callable());
	void request_for_test(const String &p_path) { request_preview(p_path); }
	void invalidate(const String &p_path);
	Ref<Texture2D> get_cached_preview(const String &p_path);
	bool has_completed(const String &p_path) const;
	int get_pending_count() const;
	bool is_rendering_available() const;
	void stop();

	~MaterialBrowserPreviewQueue();
};
