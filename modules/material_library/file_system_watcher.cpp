/**************************************************************************/
/*  file_system_watcher.cpp                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "file_system_watcher.h"

#include "core/os/os.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

#ifdef WINDOWS_ENABLED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

FileSystemWatcher::FileSystemWatcher() {
	ignore_patterns.push_back("*.tmp");
	ignore_patterns.push_back("~*");
	ignore_patterns.push_back("Photoshop Temp*");
	ignore_patterns.push_back(".library/**");
	ignore_patterns.push_back(".git/**");
}

FileSystemWatcher::~FileSystemWatcher() {
	stop();
}

String FileSystemWatcher::_normalize(const String &p_path) {
	String p = p_path.replace("\\", "/").simplify_path();
	while (p.length() > 1 && p.ends_with("/")) {
		p = p.substr(0, p.length() - 1);
	}
	return p;
}

bool FileSystemWatcher::_is_ignored(const String &p_relative) const {
	if (p_relative.is_empty()) {
		return false;
	}
	const Vector<String> segments = p_relative.split("/", false);
	for (int i = 0; i < ignore_patterns.size(); i++) {
		const String pattern = ignore_patterns[i];
		if (pattern.is_empty()) {
			continue;
		}
		if (pattern.ends_with("/**")) {
			// Directory subtree pattern: matches the directory itself at any depth.
			const String dir = pattern.substr(0, pattern.length() - 3);
			for (int s = 0; s < segments.size(); s++) {
				if (segments[s] == dir) {
					return true;
				}
			}
			continue;
		}
		if (pattern.contains("/")) {
			if (p_relative.match(pattern)) {
				return true;
			}
			continue;
		}
		// Plain glob: applies to any path segment (file name or intermediate dir).
		for (int s = 0; s < segments.size(); s++) {
			if (segments[s].match(pattern)) {
				return true;
			}
		}
	}
	return false;
}

void FileSystemWatcher::_merge_locked(const String &p_abs, ChangeType p_type, const String &p_old_abs) {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	PendingEvent *existing = pending.getptr(p_abs);
	if (!existing) {
		PendingEvent e;
		e.type = p_type;
		e.old_path = p_old_abs;
		e.first_ms = now;
		e.last_ms = now;
		pending.insert(p_abs, e);
		return;
	}

	existing->last_ms = now;

	switch (p_type) {
		case CREATED: {
			// Delete-then-recreate (the Photoshop save dance) is a modification.
			if (existing->type == DELETED) {
				existing->type = MODIFIED;
				existing->old_path = String();
			}
			// CREATED/MODIFIED/RENAMED already describe a live file; keep them.
		} break;
		case MODIFIED: {
			if (existing->type == DELETED) {
				existing->type = MODIFIED;
				existing->old_path = String();
			}
			// A CREATED or RENAMED that is then written is still CREATED/RENAMED.
		} break;
		case DELETED: {
			if (existing->type == CREATED) {
				// Created and removed inside one window: a net no-op.
				pending.erase(p_abs);
				return;
			}
			existing->type = DELETED;
			existing->old_path = String();
		} break;
		case RENAMED: {
			existing->type = RENAMED;
			existing->old_path = p_old_abs;
		} break;
	}
}

void FileSystemWatcher::_push_event(const String &p_relative, ChangeType p_type) {
	MutexLock lock(mutex);
	if (_is_ignored(p_relative)) {
		return;
	}
	_merge_locked(root + "/" + p_relative, p_type, String());
}

void FileSystemWatcher::_push_rename(const String &p_relative_old, const String &p_relative_new) {
	MutexLock lock(mutex);

	const bool old_ignored = _is_ignored(p_relative_old);
	const bool new_ignored = _is_ignored(p_relative_new);

	if (old_ignored && new_ignored) {
		return;
	}
	if (old_ignored) {
		// temp -> real: this is the tail of a save, i.e. a creation/modification.
		_merge_locked(root + "/" + p_relative_new, CREATED, String());
		return;
	}
	if (new_ignored) {
		// real -> temp: from the outside the real path went away.
		_merge_locked(root + "/" + p_relative_old, DELETED, String());
		return;
	}

	const String old_abs = root + "/" + p_relative_old;
	const String new_abs = root + "/" + p_relative_new;

	bool was_new_file = false;
	if (PendingEvent *old_entry = pending.getptr(old_abs)) {
		// The source path was itself pending; collapse it into the destination.
		was_new_file = (old_entry->type == CREATED);
		pending.erase(old_abs);
	}
	if (was_new_file) {
		// Nobody was ever told about the old path, so this is simply a new file.
		_merge_locked(new_abs, CREATED, String());
	} else {
		_merge_locked(new_abs, RENAMED, old_abs);
	}
}

/*** Public API ***/

Error FileSystemWatcher::watch(const String &p_root) {
#ifdef WINDOWS_ENABLED
	ERR_FAIL_COND_V_MSG(watching, ERR_ALREADY_IN_USE, "FileSystemWatcher is already watching a directory; call stop() first.");

	const String normalized = _normalize(p_root);
	ERR_FAIL_COND_V_MSG(normalized.is_empty(), ERR_INVALID_PARAMETER, "FileSystemWatcher.watch(): empty root path.");

	const Char16String wroot = normalized.utf16();
	HANDLE dir = CreateFileW((LPCWSTR)wroot.get_data(),
			FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			nullptr);
	if (dir == INVALID_HANDLE_VALUE) {
		ERR_FAIL_V_MSG(ERR_CANT_OPEN, vformat("FileSystemWatcher.watch(): cannot open '%s' for watching (error %d).", normalized, (int)GetLastError()));
	}

	HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!stop) {
		CloseHandle(dir);
		ERR_FAIL_V_MSG(FAILED, "FileSystemWatcher.watch(): could not create the stop event.");
	}

	root = normalized;
	dir_handle = dir;
	stop_event = stop;
	{
		MutexLock lock(mutex);
		pending.clear();
		overflowed = false;
	}
	watching = true;
	thread.start(&FileSystemWatcher::_thread_func, this);
	return OK;
#else
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "FileSystemWatcher is only implemented on Windows.");
#endif
}

void FileSystemWatcher::stop() {
#ifdef WINDOWS_ENABLED
	if (!watching) {
		return;
	}
	watching = false;
	SetEvent((HANDLE)stop_event);
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	if (dir_handle) {
		CloseHandle((HANDLE)dir_handle);
		dir_handle = nullptr;
	}
	if (stop_event) {
		CloseHandle((HANDLE)stop_event);
		stop_event = nullptr;
	}
	{
		MutexLock lock(mutex);
		pending.clear();
		overflowed = false;
	}
	root = String();
#endif
}

bool FileSystemWatcher::is_watching() const {
	return watching;
}

int64_t FileSystemWatcher::begin_suppress(const PackedStringArray &p_paths) {
	MutexLock lock(mutex);
	const uint64_t token = next_token++;
	const uint64_t expires = OS::get_singleton()->get_ticks_msec() + (uint64_t)suppress_timeout_ms;
	for (int i = 0; i < p_paths.size(); i++) {
		SuppressEntry e;
		e.token = token;
		e.expires_ms = expires;
		suppress[_normalize(p_paths[i])] = e;
	}
	return (int64_t)token;
}

void FileSystemWatcher::end_suppress(int64_t p_token) {
	if (p_token <= 0) {
		return;
	}
	MutexLock lock(mutex);
	// The op is done on disk, but its notifications are still in flight and will
	// not mature for up to (delete_hold_ms + debounce_ms). Entries therefore are
	// not dropped here; their expiry is only shortened to that settle window so a
	// genuinely external later edit is not swallowed for the full timeout.
	const uint64_t settle = OS::get_singleton()->get_ticks_msec() + (uint64_t)delete_hold_ms + (uint64_t)debounce_ms + 200;
	for (KeyValue<String, SuppressEntry> &E : suppress) {
		if (E.value.token == (uint64_t)p_token && E.value.expires_ms > settle) {
			E.value.expires_ms = settle;
		}
	}
}

void FileSystemWatcher::poll() {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();

	LocalVector<MaturedEvent> ready;
	bool resync = false;

	{
		MutexLock lock(mutex);

		// Expire stale suppression registrations (crash safety).
		if (!suppress.is_empty()) {
			LocalVector<String> expired;
			for (const KeyValue<String, SuppressEntry> &E : suppress) {
				if (now >= E.value.expires_ms) {
					expired.push_back(E.key);
				}
			}
			for (const String &s : expired) {
				suppress.erase(s);
			}
		}

		if (overflowed) {
			overflowed = false;
			pending.clear();
			resync = true;
		} else {
			LocalVector<String> done;
			for (const KeyValue<String, PendingEvent> &E : pending) {
				const uint64_t hold = (E.value.type == DELETED) ? (uint64_t)delete_hold_ms : (uint64_t)debounce_ms;
				if (now - E.value.last_ms < hold) {
					continue;
				}
				done.push_back(E.key);
				if (suppress.has(E.key)) {
					// Consume-once: our own write. The next event on this path emits.
					suppress.erase(E.key);
					continue;
				}
				MaturedEvent m;
				m.path = E.key;
				m.type = E.value.type;
				m.old_path = E.value.old_path;
				m.first_ms = E.value.first_ms;
				ready.push_back(m);
			}
			for (const String &s : done) {
				pending.erase(s);
			}
		}
	}

	if (resync) {
		emit_signal(SNAME("resync_required"));
		return;
	}
	if (ready.is_empty()) {
		return;
	}

	// Phase 2 (unlocked): re-stat held deletes and run the openability gate.
	Array changes;
	LocalVector<MaturedEvent> repend;

	for (const MaturedEvent &m : ready) {
		MaturedEvent ev = m;
#ifdef WINDOWS_ENABLED
		const Char16String wpath = ev.path.utf16();
		DWORD attrs = GetFileAttributesW((LPCWSTR)wpath.get_data());
		const bool exists = (attrs != INVALID_FILE_ATTRIBUTES);

		if (ev.type == DELETED) {
			if (exists) {
				// The file came back within the hold window: it was a save, not a delete.
				ev.type = MODIFIED;
				ev.old_path = String();
			} else {
				Dictionary d;
				d["path"] = ev.path;
				d["type"] = (int)DELETED;
				d["old_path"] = String();
				changes.push_back(d);
				continue;
			}
		} else if (!exists) {
			if (ev.type == CREATED) {
				continue; // Appeared and vanished; nothing to report.
			}
			// Re-arm as a delete and let delete_hold_ms confirm it.
			ev.type = DELETED;
			ev.old_path = String();
			repend.push_back(ev);
			continue;
		}

		if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			// Openability gate: an exclusive read open fails while a writer holds the file.
			HANDLE probe = CreateFileW((LPCWSTR)wpath.get_data(), GENERIC_READ, 0, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (probe == INVALID_HANDLE_VALUE) {
				const DWORD err = GetLastError();
				if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
					ev.type = DELETED;
					ev.old_path = String();
					repend.push_back(ev);
					continue;
				}
				if (now - ev.first_ms < GATE_GIVEUP_MS) {
					repend.push_back(ev); // Still write-locked; try again next poll.
					continue;
				}
				// Locked for too long (or unreadable for another reason): report anyway.
			} else {
				CloseHandle(probe);
			}
		}
#endif
		Dictionary d;
		d["path"] = ev.path;
		d["type"] = (int)ev.type;
		d["old_path"] = (ev.type == RENAMED) ? ev.old_path : String();
		changes.push_back(d);
	}

	if (!repend.is_empty()) {
		MutexLock lock(mutex);
		for (const MaturedEvent &ev : repend) {
			if (pending.has(ev.path)) {
				continue; // The worker already queued something newer; it wins.
			}
			PendingEvent p;
			p.type = ev.type;
			p.old_path = ev.old_path;
			p.first_ms = ev.first_ms;
			// Deletes restart their hold; gated files stay matured and retry next poll.
			p.last_ms = (ev.type == DELETED) ? now : (now - (uint64_t)debounce_ms);
			pending.insert(ev.path, p);
		}
	}

	if (!changes.is_empty()) {
		emit_signal(SNAME("changes_detected"), changes);
	}
}

/*** Windows worker thread ***/

#ifdef WINDOWS_ENABLED
void FileSystemWatcher::_thread_func(void *p_userdata) {
	static_cast<FileSystemWatcher *>(p_userdata)->_watch_loop();
}

void FileSystemWatcher::_watch_loop() {
	constexpr DWORD BUFFER_BYTES = 64 * 1024;
	constexpr DWORD FILTER = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;

	// FILE_NOTIFY_INFORMATION requires DWORD alignment.
	DWORD *buffer = memnew_arr(DWORD, BUFFER_BYTES / sizeof(DWORD));

	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) {
		memdelete_arr(buffer);
		ERR_FAIL_MSG("FileSystemWatcher: could not create the completion event.");
	}

	String pending_rename_old; // FILE_ACTION_RENAMED_OLD_NAME awaiting its NEW_NAME.

	while (true) {
		ResetEvent(ov.hEvent);
		if (!ReadDirectoryChangesW((HANDLE)dir_handle, buffer, BUFFER_BYTES, TRUE, FILTER, nullptr, &ov, nullptr)) {
			break;
		}

		HANDLE waits[2] = { (HANDLE)stop_event, ov.hEvent };
		const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
		if (w != WAIT_OBJECT_0 + 1) {
			CancelIo((HANDLE)dir_handle);
			// Let the cancelled request drain so the buffer is not written after free.
			DWORD ignored = 0;
			GetOverlappedResult((HANDLE)dir_handle, &ov, &ignored, TRUE);
			break;
		}

		DWORD bytes = 0;
		if (!GetOverlappedResult((HANDLE)dir_handle, &ov, &bytes, FALSE)) {
			break;
		}
		if (bytes == 0) {
			// The kernel buffer overflowed: notifications were lost.
			MutexLock lock(mutex);
			overflowed = true;
			pending.clear();
			continue;
		}

		const uint8_t *cursor = (const uint8_t *)buffer;
		while (true) {
			const FILE_NOTIFY_INFORMATION *info = (const FILE_NOTIFY_INFORMATION *)cursor;
			const String rel = String::utf16((const char16_t *)info->FileName, info->FileNameLength / sizeof(WCHAR)).replace("\\", "/");

			switch (info->Action) {
				case FILE_ACTION_ADDED:
					_push_event(rel, CREATED);
					break;
				case FILE_ACTION_REMOVED:
					_push_event(rel, DELETED);
					break;
				case FILE_ACTION_MODIFIED:
					_push_event(rel, MODIFIED);
					break;
				case FILE_ACTION_RENAMED_OLD_NAME:
					pending_rename_old = rel;
					break;
				case FILE_ACTION_RENAMED_NEW_NAME:
					if (pending_rename_old.is_empty()) {
						_push_event(rel, CREATED);
					} else {
						_push_rename(pending_rename_old, rel);
						pending_rename_old = String();
					}
					break;
				default:
					break;
			}

			if (info->NextEntryOffset == 0) {
				break;
			}
			cursor += info->NextEntryOffset;
		}
	}

	CloseHandle(ov.hEvent);
	memdelete_arr(buffer);
}
#endif

/*** Properties ***/

void FileSystemWatcher::set_debounce_ms(int p_ms) {
	debounce_ms = MAX(0, p_ms);
}

int FileSystemWatcher::get_debounce_ms() const {
	return debounce_ms;
}

void FileSystemWatcher::set_delete_hold_ms(int p_ms) {
	delete_hold_ms = MAX(0, p_ms);
}

int FileSystemWatcher::get_delete_hold_ms() const {
	return delete_hold_ms;
}

void FileSystemWatcher::set_suppress_timeout_ms(int p_ms) {
	suppress_timeout_ms = MAX(0, p_ms);
}

int FileSystemWatcher::get_suppress_timeout_ms() const {
	return suppress_timeout_ms;
}

void FileSystemWatcher::set_ignore_patterns(const PackedStringArray &p_patterns) {
	MutexLock lock(mutex);
	ignore_patterns = p_patterns;
}

PackedStringArray FileSystemWatcher::get_ignore_patterns() const {
	MutexLock lock(mutex);
	return ignore_patterns;
}

void FileSystemWatcher::_bind_methods() {
	ClassDB::bind_method(D_METHOD("watch", "root"), &FileSystemWatcher::watch);
	ClassDB::bind_method(D_METHOD("stop"), &FileSystemWatcher::stop);
	ClassDB::bind_method(D_METHOD("is_watching"), &FileSystemWatcher::is_watching);
	ClassDB::bind_method(D_METHOD("poll"), &FileSystemWatcher::poll);

	ClassDB::bind_method(D_METHOD("begin_suppress", "paths"), &FileSystemWatcher::begin_suppress);
	ClassDB::bind_method(D_METHOD("end_suppress", "token"), &FileSystemWatcher::end_suppress);

	ClassDB::bind_method(D_METHOD("set_debounce_ms", "ms"), &FileSystemWatcher::set_debounce_ms);
	ClassDB::bind_method(D_METHOD("get_debounce_ms"), &FileSystemWatcher::get_debounce_ms);
	ClassDB::bind_method(D_METHOD("set_delete_hold_ms", "ms"), &FileSystemWatcher::set_delete_hold_ms);
	ClassDB::bind_method(D_METHOD("get_delete_hold_ms"), &FileSystemWatcher::get_delete_hold_ms);
	ClassDB::bind_method(D_METHOD("set_suppress_timeout_ms", "ms"), &FileSystemWatcher::set_suppress_timeout_ms);
	ClassDB::bind_method(D_METHOD("get_suppress_timeout_ms"), &FileSystemWatcher::get_suppress_timeout_ms);
	ClassDB::bind_method(D_METHOD("set_ignore_patterns", "patterns"), &FileSystemWatcher::set_ignore_patterns);
	ClassDB::bind_method(D_METHOD("get_ignore_patterns"), &FileSystemWatcher::get_ignore_patterns);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "debounce_ms", PROPERTY_HINT_RANGE, "0,10000,1,suffix:ms"), "set_debounce_ms", "get_debounce_ms");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "delete_hold_ms", PROPERTY_HINT_RANGE, "0,10000,1,suffix:ms"), "set_delete_hold_ms", "get_delete_hold_ms");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "suppress_timeout_ms", PROPERTY_HINT_RANGE, "0,60000,1,suffix:ms"), "set_suppress_timeout_ms", "get_suppress_timeout_ms");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "ignore_patterns"), "set_ignore_patterns", "get_ignore_patterns");

	ADD_SIGNAL(MethodInfo("changes_detected", PropertyInfo(Variant::ARRAY, "changes")));
	ADD_SIGNAL(MethodInfo("resync_required"));

	BIND_ENUM_CONSTANT(CREATED);
	BIND_ENUM_CONSTANT(DELETED);
	BIND_ENUM_CONSTANT(MODIFIED);
	BIND_ENUM_CONSTANT(RENAMED);
}
