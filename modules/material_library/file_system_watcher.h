/**************************************************************************/
/*  file_system_watcher.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Recursive filesystem watcher for the material library.
//
// Windows: one ReadDirectoryChangesW subtree watch on a dedicated thread
// (asynchronous, 64 KiB buffer, manual-reset stop event). The worker thread only
// normalizes raw notifications into `pending` under `mutex`; every signal is
// emitted from poll() on the caller's thread.
//
// Other platforms: watch() returns ERR_UNAVAILABLE (stub).
class FileSystemWatcher : public Object {
	GDCLASS(FileSystemWatcher, Object);

public:
	enum ChangeType {
		CREATED,
		DELETED,
		MODIFIED,
		RENAMED,
	};

private:
	struct PendingEvent {
		ChangeType type = CREATED;
		String old_path; // RENAMED only.
		uint64_t last_ms = 0; // Last raw notification; debounce window restarts here.
		uint64_t first_ms = 0; // First raw notification; used for the openability give-up.
	};

	struct SuppressEntry {
		uint64_t token = 0;
		uint64_t expires_ms = 0;
	};

	struct MaturedEvent {
		String path;
		ChangeType type = CREATED;
		String old_path;
		uint64_t first_ms = 0;
	};

	// Tuning.
	int debounce_ms = 400;
	int delete_hold_ms = 1200;
	int suppress_timeout_ms = 3000;
	PackedStringArray ignore_patterns;

	// Shared worker <-> caller state (guarded by `mutex`).
	mutable Mutex mutex;
	HashMap<String, PendingEvent> pending;
	HashMap<String, SuppressEntry> suppress;
	bool overflowed = false;

	// Caller-thread only.
	Thread thread;
	String root;
	bool watching = false;
	uint64_t next_token = 1;

	// A file that stays write-locked forever must not pend forever.
	static constexpr uint64_t GATE_GIVEUP_MS = 30000;

	static String _normalize(const String &p_path);
	bool _is_ignored(const String &p_relative) const; // Caller must hold `mutex`.

	// Worker-thread entry points.
	void _push_event(const String &p_relative, ChangeType p_type);
	void _push_rename(const String &p_relative_old, const String &p_relative_new);
	void _merge_locked(const String &p_abs, ChangeType p_type, const String &p_old_abs);

#ifdef WINDOWS_ENABLED
	void *dir_handle = nullptr; // HANDLE
	void *stop_event = nullptr; // HANDLE
	static void _thread_func(void *p_userdata);
	void _watch_loop();
#endif

protected:
	static void _bind_methods();

public:
	Error watch(const String &p_root);
	void stop();
	bool is_watching() const;

	void poll();

	int64_t begin_suppress(const PackedStringArray &p_paths);
	void end_suppress(int64_t p_token);

	void set_debounce_ms(int p_ms);
	int get_debounce_ms() const;
	void set_delete_hold_ms(int p_ms);
	int get_delete_hold_ms() const;
	void set_suppress_timeout_ms(int p_ms);
	int get_suppress_timeout_ms() const;
	void set_ignore_patterns(const PackedStringArray &p_patterns);
	PackedStringArray get_ignore_patterns() const;

	FileSystemWatcher();
	~FileSystemWatcher();
};

VARIANT_ENUM_CAST(FileSystemWatcher::ChangeType);
