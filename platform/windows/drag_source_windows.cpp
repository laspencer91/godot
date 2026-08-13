/**************************************************************************/
/*  drag_source_windows.cpp                                               */
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

#include "drag_source_windows.h"

#include "core/input/input.h"
#include "core/os/thread.h"
#include "core/variant/variant.h"
#include "main/main.h"
#include "servers/display/display_server_enums.h"

#include <objidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

GODOT_GCC_WARNING_PUSH_AND_IGNORE("-Wnon-virtual-dtor") // Silence warning due to a COM API weirdness.

#define DRAG_OUT_MAX_FILES 512
// Default PER-SLOT provider deadline. 2000 was a spike-era guess; a 4K
// normal-map renormalize computed on demand blows through it and shell targets
// then abort the WHOLE transfer, so the floor was raised. Callers can override
// per drag (clamped below).
#define DRAG_OUT_PROVIDER_TIMEOUT_MS 10000
#define DRAG_OUT_PROVIDER_TIMEOUT_MIN_MS 500
#define DRAG_OUT_PROVIDER_TIMEOUT_MAX_MS 60000
#define DRAG_OUT_TARGET_DEBOUNCE_MS 200
#define DRAG_OUT_TARGET_PROP L"GodotMaterialDropTarget"
#define DRAG_OUT_EDITOR_APPID_PREFIX L"Godot.GodotEditor."
#define DRAG_OUT_MATERIAL_FORMAT "application/x-godot-material-drop-v1"

// ---------------------------------------------------------------------------
// Snapshot state.
//
// Everything the delayed-rendering path reads lives here as plain data,
// marshalled once at drag start. GetData() and IStream::Read() touch this
// struct and Win32 only.
// ---------------------------------------------------------------------------

enum SlotState {
	SLOT_IDLE,
	SLOT_REQUESTED,
	SLOT_READY,
	SLOT_FAILED,
};

struct DragOutSlot {
	wchar_t rel_path[MAX_PATH]; // Backslash-separated, as the shell wants it.
	volatile LONG state;
	unsigned char *data;
	uint64_t size;
};

struct DragOutState {
	DragOutSlot slots[DRAG_OUT_MAX_FILES];
	int32_t file_count = 0;
	// Per-slot provider deadline, snapshotted at drag start exactly like
	// file_count — _require_slot must never do a live read of a Godot type.
	int32_t provider_timeout_ms = DRAG_OUT_PROVIDER_TIMEOUT_MS;

	unsigned char *manifest = nullptr;
	uint64_t manifest_size = 0;

	// Target classification, written by GiveFeedback on the main thread and
	// read by the (also main-thread, but modal) data object.
	volatile LONG target_kind = DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
	volatile LONG target_classified = 0;
	volatile LONG abort_requested = 0;
	HWND last_root = nullptr;
	int last_root_kind = DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
	int reported_kind = -1;
	int pending_kind = -1;
	uint64_t pending_since_ms = 0;
	bool left_self = false;
	DWORD self_pid = 0;

	// CF_HDROP fallback staging (UNKNOWN targets only).
	wchar_t staging_dir[MAX_PATH] = { 0 };
	volatile LONG staged = 0;

	volatile LONG engine_free_depth = 0;
	volatile LONG drag_active = 0;
	// Cleared before the snapshot is freed. A target that hangs on to one of
	// our streams past the end of the drag gets an error, not freed memory.
	volatile LONG snapshot_valid = 0;
};

static DragOutState g_drag;

static HANDLE g_request_event = nullptr;
static HANDLE g_done_event = nullptr;
static HANDLE g_quit_event = nullptr;
static Thread g_provider_thread;
static Callable g_provider;
static Callable g_target_changed_callback;

static UINT g_cf_filedescriptor = 0;
static UINT g_cf_filecontents = 0;
static UINT g_cf_preferred_drop_effect = 0;
static UINT g_cf_material_drop = 0;

// Scoped marker for code that must not call into the engine.
struct DragOutEngineFreeZone {
	DragOutEngineFreeZone() { InterlockedIncrement(&g_drag.engine_free_depth); }
	~DragOutEngineFreeZone() { InterlockedDecrement(&g_drag.engine_free_depth); }
};

bool DragSourceWindows::is_engine_free_zone() {
	return InterlockedCompareExchange(&g_drag.engine_free_depth, 0, 0) != 0;
}

bool DragSourceWindows::is_dragging() {
	return InterlockedCompareExchange(&g_drag.drag_active, 0, 0) != 0;
}

void DragSourceWindows::pump_engine() {
	if (is_engine_free_zone()) {
		return;
	}
	// Mirrors the WM_ENTERSIZEMOVE / WM_TIMER pump in display_server_windows.cpp.
	if (Main::is_iterating()) {
		return;
	}
	Main::iteration();
}

// ---------------------------------------------------------------------------
// Byte provider worker.
//
// The COM side may not call the engine, so the bytes are produced here, on a
// thread of our own, and handed over as a plain malloc'd buffer.
// ---------------------------------------------------------------------------

static void _provider_thread_func(void *) {
	while (true) {
		HANDLE handles[2] = { g_quit_event, g_request_event };
		DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		if (wait == WAIT_OBJECT_0) {
			return;
		}
		if (wait != WAIT_OBJECT_0 + 1) {
			return;
		}

		for (int i = 0; i < g_drag.file_count; i++) {
			DragOutSlot &slot = g_drag.slots[i];
			if (InterlockedCompareExchange(&slot.state, SLOT_REQUESTED, SLOT_REQUESTED) != SLOT_REQUESTED) {
				continue;
			}

			bool ok = false;
			if (g_provider.is_valid()) {
				String rel = String::utf16((const char16_t *)slot.rel_path).replace("\\", "/");
				Variant v_index = i;
				Variant v_path = rel;
				const Variant *args[2] = { &v_index, &v_path };
				Variant ret;
				Callable::CallError ce;
				g_provider.callp(args, 2, ret, ce);
				if (ce.error != Callable::CallError::CALL_OK) {
					ERR_PRINT(vformat("Drag-out: the file content provider failed for '%s'.", rel));
				} else if (ret.get_type() != Variant::PACKED_BYTE_ARRAY) {
					ERR_PRINT(vformat("Drag-out: the file content provider must return a PackedByteArray for '%s'.", rel));
				} else {
					PackedByteArray bytes = ret;
					uint64_t size = (uint64_t)bytes.size();
					unsigned char *buffer = (unsigned char *)memalloc(size ? size : 1);
					if (buffer) {
						if (size) {
							memcpy(buffer, bytes.ptr(), size);
						}
						slot.data = buffer;
						slot.size = size;
						ok = true;
					}
				}
			}

			InterlockedExchange(&slot.state, ok ? SLOT_READY : SLOT_FAILED);
			SetEvent(g_done_event);
		}
	}
}

// Engine-free: asks the worker for a slot's bytes and waits for them with a
// COM-only filtered pump, so cross-apartment calls keep flowing while we block.
static bool _require_slot(int p_index) {
	if (InterlockedCompareExchange(&g_drag.snapshot_valid, 0, 0) == 0) {
		return false;
	}
	if (p_index < 0 || p_index >= g_drag.file_count) {
		return false;
	}
	DragOutSlot &slot = g_drag.slots[p_index];

	LONG state = InterlockedCompareExchange(&slot.state, SLOT_REQUESTED, SLOT_IDLE);
	if (state == SLOT_READY) {
		return true;
	}
	if (state == SLOT_FAILED) {
		return false;
	}
	if (state == SLOT_IDLE) {
		SetEvent(g_request_event);
	}

	const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)g_drag.provider_timeout_ms;
	while (true) {
		LONG current = InterlockedCompareExchange(&slot.state, 0, 0);
		if (current == SLOT_READY) {
			return true;
		}
		if (current == SLOT_FAILED) {
			return false;
		}

		ULONGLONG now = GetTickCount64();
		if (now >= deadline) {
			return false;
		}
		DWORD remaining = (DWORD)(deadline - now);

		DWORD wait = MsgWaitForMultipleObjectsEx(1, &g_done_event, remaining, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
		if (wait == WAIT_OBJECT_0 + 1) {
			// A cross-apartment COM call is waiting. Peeking dispatches it
			// without pulling any posted input message off the queue.
			MSG msg;
			PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		} else if (wait == WAIT_TIMEOUT) {
			return false;
		}
	}
}

// ---------------------------------------------------------------------------
// IStream over lazily generated content.
//
// Spike S1 correction 1: Explorer pulls the FILECONTENTS *medium* during hover
// (16x in one drag) but reads no bytes until the drop, so generation belongs
// here and not in GetData.
// ---------------------------------------------------------------------------

class DragOutStream : public IStream {
	LONG ref_count = 1;
	int index = 0;
	ULONGLONG pos = 0;

public:
	explicit DragOutStream(int p_index) :
			index(p_index) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
			*ppvObject = static_cast<IStream *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&ref_count);
		if (count == 0) {
			delete this;
		}
		return count;
	}

	HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override {
		DragOutEngineFreeZone zone;
		if (pcbRead) {
			*pcbRead = 0;
		}
		if (!_require_slot(index)) {
			return E_FAIL;
		}
		const DragOutSlot &slot = g_drag.slots[index];
		ULONGLONG remaining = (pos < slot.size) ? (slot.size - pos) : 0;
		ULONG count = (ULONG)((remaining < (ULONGLONG)cb) ? remaining : (ULONGLONG)cb);
		if (count) {
			memcpy(pv, slot.data + pos, count);
			pos += count;
		}
		if (pcbRead) {
			*pcbRead = count;
		}
		return (count == cb) ? S_OK : S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override {
		DragOutEngineFreeZone zone;
		LONGLONG base = 0;
		switch (dwOrigin) {
			case STREAM_SEEK_SET:
				base = 0;
				break;
			case STREAM_SEEK_CUR:
				base = (LONGLONG)pos;
				break;
			case STREAM_SEEK_END: {
				// Seeking to the end is a size query in disguise; it has to
				// force generation like Stat does.
				if (!_require_slot(index)) {
					return E_FAIL;
				}
				base = (LONGLONG)g_drag.slots[index].size;
			} break;
			default:
				return STG_E_INVALIDFUNCTION;
		}
		LONGLONG target = base + dlibMove.QuadPart;
		if (target < 0) {
			return STG_E_INVALIDFUNCTION;
		}
		pos = (ULONGLONG)target;
		if (plibNewPosition) {
			plibNewPosition->QuadPart = pos;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Stat(STATSTG *pstatstg, DWORD grfStatFlag) override {
		DragOutEngineFreeZone zone;
		if (!pstatstg) {
			return STG_E_INVALIDPOINTER;
		}
		if (!_require_slot(index)) {
			return E_FAIL;
		}
		ZeroMemory(pstatstg, sizeof(STATSTG));
		pstatstg->type = STGTY_STREAM;
		pstatstg->cbSize.QuadPart = g_drag.slots[index].size;
		if (!(grfStatFlag & STATFLAG_NONAME)) {
			size_t bytes = (lstrlenW(g_drag.slots[index].rel_path) + 1) * sizeof(wchar_t);
			pstatstg->pwcsName = (LPOLESTR)CoTaskMemAlloc(bytes);
			if (!pstatstg->pwcsName) {
				return STG_E_INSUFFICIENTMEMORY;
			}
			CopyMemory(pstatstg->pwcsName, g_drag.slots[index].rel_path, bytes);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) override {
		DragOutEngineFreeZone zone;
		if (!ppstm) {
			return STG_E_INVALIDPOINTER;
		}
		DragOutStream *clone = new DragOutStream(index);
		clone->pos = pos;
		*ppstm = clone;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override { return STG_E_ACCESSDENIED; }
	HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
	HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
};

// ---------------------------------------------------------------------------
// IEnumFORMATETC
// ---------------------------------------------------------------------------

#define DRAG_OUT_MAX_FORMATS 8

class DragOutEnumFormatEtc : public IEnumFORMATETC {
	LONG ref_count = 1;
	FORMATETC formats[DRAG_OUT_MAX_FORMATS];
	ULONG format_count = 0;
	ULONG index = 0;

public:
	DragOutEnumFormatEtc(const FORMATETC *p_formats, ULONG p_count, ULONG p_index) :
			format_count(p_count), index(p_index) {
		for (ULONG i = 0; i < p_count && i < DRAG_OUT_MAX_FORMATS; i++) {
			formats[i] = p_formats[i];
		}
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
			*ppvObject = static_cast<IEnumFORMATETC *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&ref_count);
		if (count == 0) {
			delete this;
		}
		return count;
	}

	HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) override {
		ULONG fetched = 0;
		while (fetched < celt && index < format_count) {
			rgelt[fetched] = formats[index];
			index++;
			fetched++;
		}
		if (pceltFetched) {
			*pceltFetched = fetched;
		}
		return (fetched == celt) ? S_OK : S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override {
		index += celt;
		return (index <= format_count) ? S_OK : S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE Reset() override {
		index = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **ppenum) override {
		if (!ppenum) {
			return E_POINTER;
		}
		*ppenum = new DragOutEnumFormatEtc(formats, format_count, index);
		return S_OK;
	}
};

// ---------------------------------------------------------------------------
// CF_HDROP fallback staging (UNKNOWN targets only).
//
// Spike S1 correction 4: CF_HDROP flattens nested paths, so a material with a
// textures/ subfolder cannot be offered as a file list. The staging tier writes
// a REAL directory tree and hands over its root instead, which is the only way
// the structure survives.
// ---------------------------------------------------------------------------

static void _remove_dir_recursive_w(const wchar_t *p_dir) {
	// SHFileOperation needs a double-null-terminated path.
	wchar_t from[MAX_PATH + 2] = { 0 };
	lstrcpynW(from, p_dir, MAX_PATH);
	SHFILEOPSTRUCTW op;
	ZeroMemory(&op, sizeof(op));
	op.wFunc = FO_DELETE;
	op.pFrom = from;
	op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
	SHFileOperationW(&op);
}

void DragSourceWindows::sweep_staging() {
	if (g_drag.staging_dir[0]) {
		_remove_dir_recursive_w(g_drag.staging_dir);
		g_drag.staging_dir[0] = 0;
	}
	InterlockedExchange(&g_drag.staged, 0);
}

// Engine-free: writes the whole tree, pulling bytes through the same worker
// hand-off the lazy streams use.
static bool _stage_files() {
	if (InterlockedCompareExchange(&g_drag.staged, 0, 0) != 0) {
		return g_drag.staging_dir[0] != 0;
	}

	wchar_t temp_root[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, temp_root)) {
		return false;
	}
	wchar_t dir[MAX_PATH];
	wsprintfW(dir, L"%sgodot_dragout_%u_%u", temp_root, GetCurrentProcessId(), GetTickCount());
	if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return false;
	}
	lstrcpynW(g_drag.staging_dir, dir, MAX_PATH);

	for (int i = 0; i < g_drag.file_count; i++) {
		if (!_require_slot(i)) {
			_remove_dir_recursive_w(g_drag.staging_dir);
			g_drag.staging_dir[0] = 0;
			return false;
		}

		wchar_t path[MAX_PATH * 2];
		lstrcpynW(path, dir, MAX_PATH);
		lstrcatW(path, L"\\");
		lstrcatW(path, g_drag.slots[i].rel_path);

		wchar_t parent[MAX_PATH * 2];
		lstrcpynW(parent, path, MAX_PATH * 2);
		wchar_t *slash = wcsrchr(parent, L'\\');
		if (slash) {
			*slash = 0;
			SHCreateDirectoryExW(nullptr, parent, nullptr);
		}

		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			_remove_dir_recursive_w(g_drag.staging_dir);
			g_drag.staging_dir[0] = 0;
			return false;
		}
		uint64_t written = 0;
		const DragOutSlot &slot = g_drag.slots[i];
		while (written < slot.size) {
			DWORD chunk = (DWORD)MIN((uint64_t)(64 * 1024), slot.size - written);
			DWORD out = 0;
			if (!WriteFile(file, slot.data + written, chunk, &out, nullptr) || out == 0) {
				break;
			}
			written += out;
		}
		CloseHandle(file);
	}

	InterlockedExchange(&g_drag.staged, 1);
	return true;
}

// ---------------------------------------------------------------------------
// IDataObject
// ---------------------------------------------------------------------------

class DragOutDataObject : public IDataObject {
	LONG ref_count = 1;
	FORMATETC formats[DRAG_OUT_MAX_FORMATS];
	ULONG format_count = 0;

	// The target is only known once the drag is under way, but the format list
	// has to exist before it starts. So the list is fixed and the *answers* are
	// gated: CF_HDROP is refused unless the target classified as UNKNOWN, and
	// the descriptor pair is refused when it did. The two are never both live,
	// which is what the spike proved they must never be (CF_HDROP is probed
	// first and would short-circuit the descriptor path).
	bool _format_live(UINT p_cf) const {
		if (p_cf == CF_HDROP) {
			return InterlockedCompareExchange(&g_drag.target_classified, 0, 0) != 0 &&
					InterlockedCompareExchange(&g_drag.target_kind, 0, 0) == DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
		}
		if (p_cf == g_cf_filedescriptor || p_cf == g_cf_filecontents) {
			return !(InterlockedCompareExchange(&g_drag.target_classified, 0, 0) != 0 &&
					InterlockedCompareExchange(&g_drag.target_kind, 0, 0) == DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN);
		}
		return true;
	}

	bool _matches(const FORMATETC *p_fmt, const FORMATETC *p_offered) const {
		if (p_fmt->cfFormat != p_offered->cfFormat) {
			return false;
		}
		if (!(p_fmt->tymed & p_offered->tymed)) {
			return false;
		}
		if (p_fmt->dwAspect != p_offered->dwAspect) {
			return false;
		}
		if (!_format_live(p_offered->cfFormat)) {
			return false;
		}
		if (p_offered->lindex == -1) {
			return p_fmt->lindex == -1;
		}
		// FILECONTENTS is indexed per file.
		return p_fmt->lindex >= 0 && p_fmt->lindex < g_drag.file_count;
	}

	HGLOBAL _build_descriptor() const {
		int count = g_drag.file_count;
		SIZE_T bytes = sizeof(FILEGROUPDESCRIPTORW) + (SIZE_T)(count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
		HGLOBAL mem = GlobalAlloc(GHND, bytes);
		if (!mem) {
			return nullptr;
		}
		FILEGROUPDESCRIPTORW *group = (FILEGROUPDESCRIPTORW *)GlobalLock(mem);
		group->cItems = (UINT)count;
		for (int i = 0; i < count; i++) {
			FILEDESCRIPTORW *fd = &group->fgd[i];
			// FD_FILESIZE is deliberately omitted (DESIGN.md section 4): the
			// size is not known until the content has been generated, and
			// declaring a wrong one truncates the drop.
			fd->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
			fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
			lstrcpynW(fd->cFileName, g_drag.slots[i].rel_path, MAX_PATH);
		}
		GlobalUnlock(mem);
		return mem;
	}

	HGLOBAL _build_hdrop() const {
		if (!_stage_files()) {
			return nullptr;
		}

		// A flat list of every file would lose the directory structure, so what
		// is offered is the staging root's immediate children: nested files
		// travel inside their own folder, top-level files stay individual.
		SIZE_T chars = 1;
		for (int pass = 0; pass < 2; pass++) {
			if (pass == 1) {
				chars += 1; // Room for the extra terminator.
			}
			wchar_t pattern[MAX_PATH * 2];
			lstrcpynW(pattern, g_drag.staging_dir, MAX_PATH);
			lstrcatW(pattern, L"\\*");

			WIN32_FIND_DATAW found;
			HANDLE search = FindFirstFileW(pattern, &found);
			if (search == INVALID_HANDLE_VALUE) {
				return nullptr;
			}
			HGLOBAL mem = nullptr;
			DROPFILES *df = nullptr;
			wchar_t *cursor = nullptr;
			if (pass == 1) {
				mem = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
				if (!mem) {
					FindClose(search);
					return nullptr;
				}
				df = (DROPFILES *)GlobalLock(mem);
				df->pFiles = sizeof(DROPFILES);
				df->fWide = TRUE;
				cursor = (wchar_t *)((unsigned char *)df + sizeof(DROPFILES));
			}
			do {
				if (lstrcmpW(found.cFileName, L".") == 0 || lstrcmpW(found.cFileName, L"..") == 0) {
					continue;
				}
				wchar_t path[MAX_PATH * 2];
				lstrcpynW(path, g_drag.staging_dir, MAX_PATH);
				lstrcatW(path, L"\\");
				lstrcatW(path, found.cFileName);
				int len = lstrlenW(path);
				if (pass == 0) {
					chars += len + 1;
				} else {
					CopyMemory(cursor, path, (len + 1) * sizeof(wchar_t));
					cursor += len + 1;
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);

			if (pass == 1) {
				*cursor = 0;
				GlobalUnlock(mem);
				return mem;
			}
		}
		return nullptr;
	}

	HGLOBAL _build_manifest() const {
		if (!g_drag.manifest || !g_drag.manifest_size) {
			return nullptr;
		}
		HGLOBAL mem = GlobalAlloc(GHND, (SIZE_T)g_drag.manifest_size + 1);
		if (!mem) {
			return nullptr;
		}
		unsigned char *dst = (unsigned char *)GlobalLock(mem);
		CopyMemory(dst, g_drag.manifest, (SIZE_T)g_drag.manifest_size);
		dst[g_drag.manifest_size] = 0;
		GlobalUnlock(mem);
		return mem;
	}

	HGLOBAL _build_preferred_effect() const {
		HGLOBAL mem = GlobalAlloc(GHND, sizeof(DWORD));
		if (!mem) {
			return nullptr;
		}
		DWORD *effect = (DWORD *)GlobalLock(mem);
		*effect = DROPEFFECT_COPY;
		GlobalUnlock(mem);
		return mem;
	}

public:
	DragOutDataObject() {
		// Order matters: targets take the first format they understand. Our own
		// editor looks for the material manifest above everything else; the
		// descriptor pair serves every other virtual-file target; CF_HDROP is
		// last and only ever answered for UNKNOWN targets.
		if (g_drag.manifest && g_drag.manifest_size) {
			formats[format_count++] = { (CLIPFORMAT)g_cf_material_drop, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		}
		formats[format_count++] = { (CLIPFORMAT)g_cf_filedescriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		formats[format_count++] = { (CLIPFORMAT)g_cf_filecontents, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM | TYMED_HGLOBAL };
		formats[format_count++] = { (CLIPFORMAT)CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		formats[format_count++] = { (CLIPFORMAT)g_cf_preferred_drop_effect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == IID_IUnknown || riid == IID_IDataObject) {
			*ppvObject = static_cast<IDataObject *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&ref_count);
		if (count == 0) {
			delete this;
		}
		return count;
	}

	// Engine-free zone. Everything below reads the snapshot and Win32 only.
	HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetcIn, STGMEDIUM *pmedium) override {
		DragOutEngineFreeZone zone;
		if (!pformatetcIn || !pmedium) {
			return E_INVALIDARG;
		}
		if (InterlockedCompareExchange(&g_drag.snapshot_valid, 0, 0) == 0) {
			return E_FAIL;
		}
		ZeroMemory(pmedium, sizeof(STGMEDIUM));

		for (ULONG i = 0; i < format_count; i++) {
			if (!_matches(pformatetcIn, &formats[i])) {
				continue;
			}
			UINT cf = formats[i].cfFormat;
			if (cf == g_cf_filecontents) {
				if (pformatetcIn->tymed & TYMED_ISTREAM) {
					// Lazy: not one byte is produced here.
					pmedium->tymed = TYMED_ISTREAM;
					pmedium->pstm = new DragOutStream(pformatetcIn->lindex);
					pmedium->pUnkForRelease = nullptr;
					return S_OK;
				}
				// HGLOBAL contents have no lazy form; render now.
				if (!_require_slot(pformatetcIn->lindex)) {
					return E_FAIL;
				}
				const DragOutSlot &slot = g_drag.slots[pformatetcIn->lindex];
				HGLOBAL mem = GlobalAlloc(GHND, (SIZE_T)(slot.size ? slot.size : 1));
				if (!mem) {
					return STG_E_MEDIUMFULL;
				}
				unsigned char *dst = (unsigned char *)GlobalLock(mem);
				if (slot.size) {
					CopyMemory(dst, slot.data, (SIZE_T)slot.size);
				}
				GlobalUnlock(mem);
				pmedium->tymed = TYMED_HGLOBAL;
				pmedium->hGlobal = mem;
				return S_OK;
			}

			HGLOBAL mem = nullptr;
			if (cf == g_cf_filedescriptor) {
				mem = _build_descriptor();
			} else if (cf == CF_HDROP) {
				mem = _build_hdrop();
			} else if (cf == g_cf_material_drop) {
				mem = _build_manifest();
			} else if (cf == g_cf_preferred_drop_effect) {
				mem = _build_preferred_effect();
			}
			if (!mem) {
				return STG_E_MEDIUMFULL;
			}
			pmedium->tymed = TYMED_HGLOBAL;
			pmedium->hGlobal = mem;
			return S_OK;
		}
		return DV_E_FORMATETC;
	}

	HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override {
		DragOutEngineFreeZone zone;
		if (!pformatetc) {
			return E_INVALIDARG;
		}
		for (ULONG i = 0; i < format_count; i++) {
			if (_matches(pformatetc, &formats[i])) {
				return S_OK;
			}
		}
		return DV_E_FORMATETC;
	}

	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pformatetcOut) override {
		if (pformatetcOut) {
			ZeroMemory(pformatetcOut, sizeof(FORMATETC));
			pformatetcOut->ptd = nullptr;
		}
		return E_NOTIMPL;
	}

	// Targets push shell bookkeeping formats ("Performed DropEffect", ...).
	// Accept and discard: returning E_NOTIMPL is a known way to lose drops.
	HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *pmedium, BOOL fRelease) override {
		DragOutEngineFreeZone zone;
		if (fRelease && pmedium) {
			ReleaseStgMedium(pmedium);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override {
		DragOutEngineFreeZone zone;
		if (!ppenumFormatEtc) {
			return E_POINTER;
		}
		if (dwDirection != DATADIR_GET) {
			return E_NOTIMPL;
		}
		*ppenumFormatEtc = new DragOutEnumFormatEtc(formats, format_count, 0);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// ---------------------------------------------------------------------------
// Target classification (stage B).
// ---------------------------------------------------------------------------

static bool _window_app_id(HWND p_hwnd, wchar_t *r_app_id, int p_len) {
	r_app_id[0] = 0;
	IPropertyStore *prop_store = nullptr;
	if (SHGetPropertyStoreForWindow(p_hwnd, IID_IPropertyStore, (void **)&prop_store) != S_OK || !prop_store) {
		return false;
	}
	PROPVARIANT val;
	PropVariantInit(&val);
	bool ok = false;
	if (prop_store->GetValue(PKEY_AppUserModel_ID, &val) == S_OK && val.vt == VT_LPWSTR && val.pwszVal) {
		lstrcpynW(r_app_id, val.pwszVal, p_len);
		ok = true;
	}
	PropVariantClear(&val);
	prop_store->Release();
	return ok;
}

static int _classify_window(HWND p_root, String *r_name) {
	DWORD pid = 0;
	GetWindowThreadProcessId(p_root, &pid);
	if (pid == g_drag.self_pid) {
		return DisplayServerEnums::FILE_DRAG_TARGET_SELF;
	}

	wchar_t title[256] = { 0 };
	GetWindowTextW(p_root, title, 256);
	if (r_name) {
		*r_name = String::utf16((const char16_t *)title);
	}

	// This fork's editor advertises itself with a window property set beside
	// RegisterDragDrop, so we can tell it from a stock editor and hand it the
	// richer material manifest.
	if (GetPropW(p_root, DRAG_OUT_TARGET_PROP) != nullptr) {
		return DisplayServerEnums::FILE_DRAG_TARGET_EDITOR_FORK;
	}

	wchar_t cls[128] = { 0 };
	GetClassNameW(p_root, cls, 128);

	if (lstrcmpW(cls, L"Engine") == 0) {
		wchar_t app_id[256] = { 0 };
		if (_window_app_id(p_root, app_id, 256)) {
			const int prefix_len = lstrlenW(DRAG_OUT_EDITOR_APPID_PREFIX);
			if (wcsncmp(app_id, DRAG_OUT_EDITOR_APPID_PREFIX, prefix_len) == 0) {
				if (r_name) {
					String version = String::utf16((const char16_t *)(app_id + prefix_len));
					if (!version.is_empty()) {
						*r_name = vformat("%s (%s)", *r_name, version);
					}
				}
				return DisplayServerEnums::FILE_DRAG_TARGET_EDITOR;
			}
		}
		return DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
	}

	if (lstrcmpW(cls, L"CabinetWClass") == 0 || lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0) {
		return DisplayServerEnums::FILE_DRAG_TARGET_EXPLORER;
	}

	return DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
}

// Runs on the main thread from GiveFeedback. Not an engine-free zone: this is
// where the target-changed callback is dispatched from.
static void _update_target() {
	POINT pt;
	if (!GetCursorPos(&pt)) {
		return;
	}
	HWND hwnd = WindowFromPoint(pt);
	if (!hwnd) {
		return;
	}
	hwnd = GetAncestor(hwnd, GA_ROOT);

	String name;
	int kind;
	if (hwnd == g_drag.last_root) {
		kind = g_drag.last_root_kind;
	} else {
		kind = _classify_window(hwnd, &name);
		g_drag.last_root = hwnd;
		g_drag.last_root_kind = kind;
	}

	if (kind != DisplayServerEnums::FILE_DRAG_TARGET_SELF) {
		g_drag.left_self = true;
	} else if (g_drag.left_self) {
		// Dragged back over ourselves: abort the OS drag so the app's internal
		// drag and drop can take over again.
		InterlockedExchange(&g_drag.abort_requested, 1);
	}

	uint64_t now = GetTickCount64();
	if (kind != g_drag.pending_kind) {
		g_drag.pending_kind = kind;
		g_drag.pending_since_ms = now;
		return;
	}
	if (now - g_drag.pending_since_ms < DRAG_OUT_TARGET_DEBOUNCE_MS) {
		return;
	}

	// Stabilized.
	InterlockedExchange(&g_drag.target_kind, kind);
	InterlockedExchange(&g_drag.target_classified, 1);
	if (kind != g_drag.reported_kind) {
		g_drag.reported_kind = kind;
		if (g_target_changed_callback.is_valid()) {
			if (name.is_empty()) {
				wchar_t title[256] = { 0 };
				GetWindowTextW(hwnd, title, 256);
				name = String::utf16((const char16_t *)title);
			}
			g_target_changed_callback.call_deferred(kind, name);
		}
	}
}

// ---------------------------------------------------------------------------
// IDropSource
// ---------------------------------------------------------------------------

class DragOutDropSource : public IDropSource {
	LONG ref_count = 1;

public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
		if (riid == IID_IUnknown || riid == IID_IDropSource) {
			*ppvObject = static_cast<IDropSource *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_count); }
	ULONG STDMETHODCALLTYPE Release() override {
		ULONG count = InterlockedDecrement(&ref_count);
		if (count == 0) {
			delete this;
		}
		return count;
	}

	HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
		if (fEscapePressed || InterlockedCompareExchange(&g_drag.abort_requested, 0, 0) != 0) {
			return DRAGDROP_S_CANCEL;
		}
		if (!(grfKeyState & MK_LBUTTON)) {
			return DRAGDROP_S_DROP;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD dwEffect) override {
		(void)dwEffect;
		_update_target();
		// GiveFeedback only fires on input, so this is the top-up half of the
		// pump; the WM_TIMER half is what keeps a still mouse rendering.
		DragSourceWindows::pump_engine();
		return DRAGDROP_S_USEDEFAULTCURSORS;
	}
};

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------

static void _register_formats() {
	if (g_cf_filedescriptor) {
		return;
	}
	// UNICODE is not defined for the engine build, so the CFSTR_* macros are
	// narrow strings; use the TCHAR-resolved entry point like
	// drop_target_windows.cpp does.
	g_cf_filedescriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
	g_cf_filecontents = RegisterClipboardFormat(CFSTR_FILECONTENTS);
	g_cf_preferred_drop_effect = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
	g_cf_material_drop = RegisterClipboardFormat(DRAG_OUT_MATERIAL_FORMAT);
}

static void _release_snapshot() {
	InterlockedExchange(&g_drag.snapshot_valid, 0);
	for (int i = 0; i < g_drag.file_count; i++) {
		if (g_drag.slots[i].data) {
			memfree(g_drag.slots[i].data);
			g_drag.slots[i].data = nullptr;
		}
		g_drag.slots[i].size = 0;
		g_drag.slots[i].state = SLOT_IDLE;
	}
	if (g_drag.manifest) {
		memfree(g_drag.manifest);
		g_drag.manifest = nullptr;
	}
	g_drag.manifest_size = 0;
	g_drag.file_count = 0;
}

Error DragSourceWindows::start_drag(HWND p_owner, const Vector<FileEntry> &p_files, const Callable &p_provider, const Callable &p_finished_callback, const Callable &p_target_changed_callback, const String &p_manifest, int p_provider_timeout_ms) {
	ERR_FAIL_COND_V_MSG(is_dragging(), ERR_BUSY, "A file drag is already in progress.");
	ERR_FAIL_COND_V_MSG(p_files.is_empty(), ERR_INVALID_PARAMETER, "No files to drag.");
	ERR_FAIL_COND_V_MSG(p_files.size() > DRAG_OUT_MAX_FILES, ERR_INVALID_PARAMETER, vformat("Too many files in one drag (max %d).", DRAG_OUT_MAX_FILES));
	ERR_FAIL_COND_V_MSG(!p_provider.is_valid(), ERR_INVALID_PARAMETER, "A valid file content provider is required.");

	_register_formats();

	// Marshal the snapshot. Names are final from here on: the shell pulls the
	// descriptor over and over during hover and will not ask again later.
	ZeroMemory(g_drag.slots, sizeof(g_drag.slots));
	for (int i = 0; i < p_files.size(); i++) {
		String name = p_files[i].name.strip_edges();
		ERR_FAIL_COND_V_MSG(name.is_empty(), ERR_INVALID_PARAMETER, "Dragged file names cannot be empty.");
		ERR_FAIL_COND_V_MSG(name.contains("/") || name.contains("\\"), ERR_INVALID_PARAMETER, vformat("Dragged file name '%s' cannot contain a path separator; use the 'dir' entry instead.", name));

		String dir = p_files[i].dir.replace("\\", "/").trim_prefix("/").trim_suffix("/");
		ERR_FAIL_COND_V_MSG(dir.begins_with("..") || dir.contains("../"), ERR_INVALID_PARAMETER, vformat("Dragged file directory '%s' cannot escape the drop root.", dir));
		String rel = dir.is_empty() ? name : dir.path_join(name);

		Char16String rel16 = rel.replace("/", "\\").utf16();
		lstrcpynW(g_drag.slots[i].rel_path, (LPCWSTR)rel16.ptr(), MAX_PATH);
		g_drag.slots[i].state = SLOT_IDLE;
	}
	g_drag.file_count = p_files.size();
	g_drag.provider_timeout_ms = CLAMP(p_provider_timeout_ms, DRAG_OUT_PROVIDER_TIMEOUT_MIN_MS, DRAG_OUT_PROVIDER_TIMEOUT_MAX_MS);

	if (!p_manifest.is_empty()) {
		CharString utf8 = p_manifest.utf8();
		g_drag.manifest_size = (uint64_t)utf8.length();
		g_drag.manifest = (unsigned char *)memalloc(g_drag.manifest_size + 1);
		if (g_drag.manifest) {
			memcpy(g_drag.manifest, utf8.get_data(), g_drag.manifest_size + 1);
		} else {
			g_drag.manifest_size = 0;
		}
	}

	g_drag.self_pid = GetCurrentProcessId();
	g_drag.target_kind = DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
	g_drag.target_classified = 0;
	g_drag.abort_requested = 0;
	g_drag.last_root = nullptr;
	g_drag.last_root_kind = DisplayServerEnums::FILE_DRAG_TARGET_UNKNOWN;
	g_drag.reported_kind = -1;
	g_drag.pending_kind = -1;
	g_drag.pending_since_ms = 0;
	g_drag.left_self = false;
	g_drag.staging_dir[0] = 0;
	g_drag.staged = 0;
	g_drag.engine_free_depth = 0;
	g_drag.snapshot_valid = 1;

	g_provider = p_provider;
	g_target_changed_callback = p_target_changed_callback;

	g_request_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!g_request_event || !g_done_event || !g_quit_event) {
		_release_snapshot();
		ERR_FAIL_V_MSG(FAILED, "Failed to create the drag-out synchronization events.");
	}
	g_provider_thread.start(_provider_thread_func, nullptr);

	// The OS owns the mouse from here; the engine must not think a button is
	// still held when control comes back.
	Input::get_singleton()->release_pressed_events();
	ReleaseCapture();

	InterlockedExchange(&g_drag.drag_active, 1);

	DragOutDataObject *data_object = new DragOutDataObject();
	DragOutDropSource *drop_source = new DragOutDropSource();

	DWORD effect = DROPEFFECT_NONE;
	// MOVE is never offered: it would let a target delete whatever it decides
	// our source was.
	HRESULT hr = SHDoDragDrop(p_owner, data_object, drop_source, DROPEFFECT_COPY, &effect);

	InterlockedExchange(&g_drag.drag_active, 0);

	data_object->Release();
	drop_source->Release();

	SetEvent(g_quit_event);
	g_provider_thread.wait_to_finish();
	CloseHandle(g_request_event);
	CloseHandle(g_done_event);
	CloseHandle(g_quit_event);
	g_request_event = nullptr;
	g_done_event = nullptr;
	g_quit_event = nullptr;
	g_provider = Callable();

	int result = DisplayServerEnums::FILE_DRAG_RESULT_FAILED;
	if (hr == DRAGDROP_S_CANCEL) {
		result = DisplayServerEnums::FILE_DRAG_RESULT_CANCELLED;
	} else if (hr == DRAGDROP_S_DROP) {
		// Advisory only: targets are known to misreport the effect they applied.
		result = (effect == DROPEFFECT_NONE) ? DisplayServerEnums::FILE_DRAG_RESULT_CANCELLED : DisplayServerEnums::FILE_DRAG_RESULT_DROPPED;
	}
	int kind = (int)InterlockedCompareExchange(&g_drag.target_kind, 0, 0);

	sweep_staging();
	_release_snapshot();

	if (p_finished_callback.is_valid()) {
		// Deferred through the MessageQueue so the app resumes on a clean stack.
		p_finished_callback.call_deferred(result, kind);
	}
	g_target_changed_callback = Callable();

	return OK;
}

GODOT_GCC_WARNING_POP
