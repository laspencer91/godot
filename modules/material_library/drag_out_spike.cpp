/**************************************************************************/
/*  drag_out_spike.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "drag_out_spike.h"

#ifdef WINDOWS_ENABLED

#include "core/input/input.h"
#include "core/string/print_string.h"
#include "main/main.h"
#include "servers/display/display_server.h"

#include <windows.h>

#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>

// ---------------------------------------------------------------------------
// POD trace / snapshot state.
//
// Everything the delayed-rendering path reads lives here as plain data,
// marshalled once at drag start. GetData() and IStream::Read() may touch this
// struct and Win32 only -- no Godot types, no allocator, no locks.
// ---------------------------------------------------------------------------

#define SPIKE_MAX_FILES 64
#define SPIKE_MAX_EVENTS 2048
#define SPIKE_TIMER_ID 0xD4A6
#define SPIKE_WM_START_DRAG (WM_APP + 0x41)

enum SpikePumpSource {
	PUMP_SOURCE_TIMER,
	PUMP_SOURCE_GIVE_FEEDBACK,
};

struct SpikeFile {
	wchar_t name[MAX_PATH];
	uint64_t size;
	bool is_dir;
	uint32_t seed;
};

struct SpikeEvent {
	uint32_t cf;
	int32_t lindex;
	uint32_t tymed;
	int32_t hr;
	uint64_t usec;
	uint64_t bytes;
};

struct SpikeTrace {
	// Snapshot (written at drag start, read-only afterwards).
	SpikeFile files[SPIKE_MAX_FILES];
	int32_t file_count;
	bool hdrop_mode;
	wchar_t staging_dir[MAX_PATH];

	// What the target asked about vs. what it actually pulled.
	SpikeEvent queries[SPIKE_MAX_EVENTS];
	volatile LONG query_count;
	SpikeEvent gets[SPIKE_MAX_EVENTS];
	volatile LONG get_count;
	SpikeEvent sets[32];
	volatile LONG set_count;
	uint32_t enumerated[32];
	volatile LONG enumerated_count;
	volatile LONG enum_calls;

	volatile LONG stream_reads;
	volatile LONG64 stream_bytes;
	volatile LONG streams_created;
	volatile LONG stat_calls;

	// Pump bookkeeping.
	volatile LONG pump_iterations_timer;
	volatile LONG pump_iterations_feedback;
	volatile LONG pump_skips_reentrant;
	volatile LONG give_feedback_calls;
	volatile LONG query_continue_calls;
	volatile LONG timer_ticks;

	// Engine-free zone enforcement.
	volatile LONG engine_free_depth;
	volatile LONG engine_free_violations;

	// Target classification samples (from GiveFeedback).
	wchar_t last_target_class[128];
	DWORD last_target_pid;
	DWORD self_pid;
	volatile LONG target_changes;
	DWORD last_effect;

	// Outcome.
	int32_t dodragdrop_hr;
	DWORD final_effect;
	uint64_t drag_usec;
	uint64_t max_getdata_usec;
	uint64_t descriptor_usec;
	bool drag_active;
	bool drag_completed;

	// When, relative to the start of the drag, the target pulled content and
	// names. This is what tells hover-time probing apart from drop-time
	// rendering -- i.e. whether delayed rendering actually defers.
	uint64_t drag_start_usec;
	volatile LONG contents_gets;
	uint64_t first_contents_usec;
	uint64_t last_contents_usec;
	volatile LONG descriptor_gets;
	uint64_t first_descriptor_usec;
	uint64_t last_descriptor_usec;
	uint64_t first_stream_read_usec;
	uint64_t last_stream_read_usec;
};

static SpikeTrace g_trace;
static DragOutSpike *g_spike = nullptr;
static HWND g_msg_window = nullptr;
static UINT g_cf_filedescriptor = 0;
static UINT g_cf_filecontents = 0;
static UINT g_cf_preferred_drop_effect = 0;

static uint64_t _spike_now_usec() {
	LARGE_INTEGER freq, ctr;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&ctr);
	return (uint64_t)((ctr.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}

// Deterministic synthetic content. Verifiable byte-for-byte by the self-test
// and cheap enough that the timing we measure is the plumbing, not the payload.
static inline unsigned char _spike_byte(uint64_t p_offset, uint32_t p_seed) {
	return (unsigned char)(((p_offset * 131u) + p_seed) & 0xFF);
}

static void _spike_fill(unsigned char *p_dst, uint64_t p_offset, uint64_t p_count, uint32_t p_seed) {
	for (uint64_t i = 0; i < p_count; i++) {
		p_dst[i] = _spike_byte(p_offset + i, p_seed);
	}
}

// Scoped marker for code that must not call into the engine.
struct SpikeEngineFreeZone {
	SpikeEngineFreeZone() { InterlockedIncrement(&g_trace.engine_free_depth); }
	~SpikeEngineFreeZone() { InterlockedDecrement(&g_trace.engine_free_depth); }
};

static void _spike_record(SpikeEvent *p_buf, volatile LONG *p_count, int p_cap, uint32_t p_cf, int32_t p_lindex, uint32_t p_tymed, int32_t p_hr, uint64_t p_usec, uint64_t p_bytes) {
	LONG slot = InterlockedIncrement(p_count) - 1;
	if (slot >= p_cap) {
		return;
	}
	p_buf[slot].cf = p_cf;
	p_buf[slot].lindex = p_lindex;
	p_buf[slot].tymed = p_tymed;
	p_buf[slot].hr = p_hr;
	p_buf[slot].usec = p_usec;
	p_buf[slot].bytes = p_bytes;
}

// The only place the spike calls back into the engine during a drag.
static void _spike_pump(SpikePumpSource p_source) {
	if (InterlockedCompareExchange(&g_trace.engine_free_depth, 0, 0) != 0) {
		// Would have re-entered the engine from a content-rendering path.
		InterlockedIncrement(&g_trace.engine_free_violations);
		return;
	}
	// Mirrors the WM_ENTERSIZEMOVE / WM_TIMER pump in display_server_windows.cpp.
	if (Main::is_iterating()) {
		InterlockedIncrement(&g_trace.pump_skips_reentrant);
		return;
	}
	if (p_source == PUMP_SOURCE_TIMER) {
		InterlockedIncrement(&g_trace.pump_iterations_timer);
	} else {
		InterlockedIncrement(&g_trace.pump_iterations_feedback);
	}
	Main::iteration();
}

// ---------------------------------------------------------------------------
// IStream over synthesized content. Bytes exist only once the target reads.
// ---------------------------------------------------------------------------

class SpikeStream : public IStream {
	LONG ref_count = 1;
	ULONGLONG size = 0;
	ULONGLONG pos = 0;
	uint32_t seed = 0;
	wchar_t name[MAX_PATH];

public:
	SpikeStream(ULONGLONG p_size, uint32_t p_seed, const wchar_t *p_name) :
			size(p_size), seed(p_seed) {
		lstrcpynW(name, p_name, MAX_PATH);
		InterlockedIncrement(&g_trace.streams_created);
	}

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
		SpikeEngineFreeZone zone;
		ULONGLONG remaining = (pos < size) ? (size - pos) : 0;
		ULONG count = (ULONG)((remaining < (ULONGLONG)cb) ? remaining : (ULONGLONG)cb);
		_spike_fill((unsigned char *)pv, pos, count, seed);
		pos += count;
		if (pcbRead) {
			*pcbRead = count;
		}
		InterlockedIncrement(&g_trace.stream_reads);
		InterlockedExchangeAdd64(&g_trace.stream_bytes, (LONG64)count);
		if (g_trace.drag_start_usec != 0) {
			uint64_t since_start = _spike_now_usec() - g_trace.drag_start_usec;
			if (g_trace.first_stream_read_usec == 0) {
				g_trace.first_stream_read_usec = since_start;
			}
			g_trace.last_stream_read_usec = since_start;
		}
		return (count == cb) ? S_OK : S_FALSE;
	}

	HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override {
		SpikeEngineFreeZone zone;
		LONGLONG base = 0;
		switch (dwOrigin) {
			case STREAM_SEEK_SET:
				base = 0;
				break;
			case STREAM_SEEK_CUR:
				base = (LONGLONG)pos;
				break;
			case STREAM_SEEK_END:
				base = (LONGLONG)size;
				break;
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
		SpikeEngineFreeZone zone;
		if (!pstatstg) {
			return STG_E_INVALIDPOINTER;
		}
		InterlockedIncrement(&g_trace.stat_calls);
		ZeroMemory(pstatstg, sizeof(STATSTG));
		pstatstg->type = STGTY_STREAM;
		pstatstg->cbSize.QuadPart = size;
		if (!(grfStatFlag & STATFLAG_NONAME)) {
			size_t bytes = (lstrlenW(name) + 1) * sizeof(wchar_t);
			pstatstg->pwcsName = (LPOLESTR)CoTaskMemAlloc(bytes);
			if (!pstatstg->pwcsName) {
				return STG_E_INSUFFICIENTMEMORY;
			}
			CopyMemory(pstatstg->pwcsName, name, bytes);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) override {
		SpikeEngineFreeZone zone;
		if (!ppstm) {
			return STG_E_INVALIDPOINTER;
		}
		SpikeStream *clone = new SpikeStream(size, seed, name);
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

class SpikeEnumFormatEtc : public IEnumFORMATETC {
	LONG ref_count = 1;
	FORMATETC formats[8];
	ULONG format_count = 0;
	ULONG index = 0;

public:
	SpikeEnumFormatEtc(const FORMATETC *p_formats, ULONG p_count, ULONG p_index) :
			format_count(p_count), index(p_index) {
		for (ULONG i = 0; i < p_count && i < 8; i++) {
			formats[i] = p_formats[i];
		}
		InterlockedIncrement(&g_trace.enum_calls);
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
			// Record which formats the target actually walked.
			LONG slot = InterlockedIncrement(&g_trace.enumerated_count) - 1;
			if (slot < 32) {
				g_trace.enumerated[slot] = formats[index].cfFormat;
			}
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
		*ppenum = new SpikeEnumFormatEtc(formats, format_count, index);
		return S_OK;
	}
};

// ---------------------------------------------------------------------------
// IDataObject
// ---------------------------------------------------------------------------

class SpikeDataObject : public IDataObject {
	LONG ref_count = 1;
	FORMATETC formats[8];
	ULONG format_count = 0;

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
		if (p_offered->lindex == -1) {
			return p_fmt->lindex == -1;
		}
		// FILECONTENTS is indexed per file.
		return p_fmt->lindex >= 0 && p_fmt->lindex < g_trace.file_count;
	}

	HGLOBAL _build_descriptor() const {
		int count = g_trace.file_count;
		SIZE_T bytes = sizeof(FILEGROUPDESCRIPTORW) + (SIZE_T)(count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
		HGLOBAL mem = GlobalAlloc(GHND, bytes);
		if (!mem) {
			return nullptr;
		}
		FILEGROUPDESCRIPTORW *group = (FILEGROUPDESCRIPTORW *)GlobalLock(mem);
		group->cItems = (UINT)count;
		for (int i = 0; i < count; i++) {
			FILEDESCRIPTORW *fd = &group->fgd[i];
			// FD_FILESIZE deliberately omitted (DESIGN.md section 4): sizes are
			// not known until the content is generated. FD_PROGRESSUI asks the
			// shell to show progress UI while it pulls the streams.
			fd->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
			fd->dwFileAttributes = g_trace.files[i].is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			lstrcpynW(fd->cFileName, g_trace.files[i].name, MAX_PATH);
		}
		GlobalUnlock(mem);
		return mem;
	}

	HGLOBAL _build_hdrop() const {
		// Sized for "<staging>\<name>\0" per file plus the terminating null.
		SIZE_T chars = 1;
		for (int i = 0; i < g_trace.file_count; i++) {
			chars += lstrlenW(g_trace.staging_dir) + 1 + lstrlenW(g_trace.files[i].name) + 1;
		}
		HGLOBAL mem = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
		if (!mem) {
			return nullptr;
		}
		DROPFILES *df = (DROPFILES *)GlobalLock(mem);
		df->pFiles = sizeof(DROPFILES);
		df->fWide = TRUE;
		wchar_t *cursor = (wchar_t *)((unsigned char *)df + sizeof(DROPFILES));
		for (int i = 0; i < g_trace.file_count; i++) {
			wchar_t path[MAX_PATH * 2];
			lstrcpynW(path, g_trace.staging_dir, MAX_PATH);
			lstrcatW(path, L"\\");
			lstrcatW(path, g_trace.files[i].name);
			int len = lstrlenW(path);
			CopyMemory(cursor, path, (len + 1) * sizeof(wchar_t));
			cursor += len + 1;
		}
		*cursor = 0;
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

	HGLOBAL _build_contents_hglobal(int p_index) const {
		const SpikeFile &file = g_trace.files[p_index];
		HGLOBAL mem = GlobalAlloc(GHND, (SIZE_T)file.size ? (SIZE_T)file.size : 1);
		if (!mem) {
			return nullptr;
		}
		unsigned char *dst = (unsigned char *)GlobalLock(mem);
		_spike_fill(dst, 0, file.size, file.seed);
		GlobalUnlock(mem);
		return mem;
	}

public:
	SpikeDataObject() {
		// Format order matters: targets take the first they understand, and
		// CF_HDROP short-circuits the descriptor path wherever both appear.
		// This spike offers exactly one of the two, never both.
		if (g_trace.hdrop_mode) {
			formats[format_count++] = { (CLIPFORMAT)CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		} else {
			formats[format_count++] = { (CLIPFORMAT)g_cf_filedescriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
			formats[format_count++] = { (CLIPFORMAT)g_cf_filecontents, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM | TYMED_HGLOBAL };
		}
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

	// Engine-free zone. Everything below reads the POD snapshot only.
	HRESULT STDMETHODCALLTYPE GetData(FORMATETC *pformatetcIn, STGMEDIUM *pmedium) override {
		SpikeEngineFreeZone zone;
		uint64_t start = _spike_now_usec();
		HRESULT res = DV_E_FORMATETC;
		uint64_t bytes = 0;

		if (!pformatetcIn || !pmedium) {
			return E_INVALIDARG;
		}
		ZeroMemory(pmedium, sizeof(STGMEDIUM));

		for (ULONG i = 0; i < format_count; i++) {
			if (!_matches(pformatetcIn, &formats[i])) {
				continue;
			}
			UINT cf = formats[i].cfFormat;
			if (cf == g_cf_filecontents) {
				const SpikeFile &file = g_trace.files[pformatetcIn->lindex];
				if (pformatetcIn->tymed & TYMED_ISTREAM) {
					pmedium->tymed = TYMED_ISTREAM;
					pmedium->pstm = new SpikeStream(file.size, file.seed, file.name);
					pmedium->pUnkForRelease = nullptr;
					bytes = file.size;
					res = S_OK;
				} else {
					HGLOBAL mem = _build_contents_hglobal(pformatetcIn->lindex);
					if (mem) {
						pmedium->tymed = TYMED_HGLOBAL;
						pmedium->hGlobal = mem;
						bytes = file.size;
						res = S_OK;
					} else {
						res = STG_E_MEDIUMFULL;
					}
				}
			} else {
				HGLOBAL mem = nullptr;
				if (cf == g_cf_filedescriptor) {
					mem = _build_descriptor();
				} else if (cf == CF_HDROP) {
					mem = _build_hdrop();
				} else if (cf == g_cf_preferred_drop_effect) {
					mem = _build_preferred_effect();
				}
				if (mem) {
					pmedium->tymed = TYMED_HGLOBAL;
					pmedium->hGlobal = mem;
					bytes = (uint64_t)GlobalSize(mem);
					res = S_OK;
				} else {
					res = STG_E_MEDIUMFULL;
				}
			}
			break;
		}

		uint64_t elapsed = _spike_now_usec() - start;
		_spike_record(g_trace.gets, &g_trace.get_count, SPIKE_MAX_EVENTS,
				pformatetcIn->cfFormat, pformatetcIn->lindex, pformatetcIn->tymed, (int32_t)res, elapsed, bytes);
		if (elapsed > g_trace.max_getdata_usec) {
			g_trace.max_getdata_usec = elapsed;
		}
		if (pformatetcIn->cfFormat == g_cf_filedescriptor) {
			g_trace.descriptor_usec = elapsed;
		}
		if (res == S_OK && g_trace.drag_start_usec != 0) {
			uint64_t since_start = _spike_now_usec() - g_trace.drag_start_usec;
			if (pformatetcIn->cfFormat == g_cf_filecontents) {
				if (InterlockedIncrement(&g_trace.contents_gets) == 1) {
					g_trace.first_contents_usec = since_start;
				}
				g_trace.last_contents_usec = since_start;
			} else if (pformatetcIn->cfFormat == g_cf_filedescriptor) {
				if (InterlockedIncrement(&g_trace.descriptor_gets) == 1) {
					g_trace.first_descriptor_usec = since_start;
				}
				g_trace.last_descriptor_usec = since_start;
			}
		}
		return res;
	}

	HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }

	HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *pformatetc) override {
		SpikeEngineFreeZone zone;
		HRESULT res = DV_E_FORMATETC;
		if (pformatetc) {
			for (ULONG i = 0; i < format_count; i++) {
				if (_matches(pformatetc, &formats[i])) {
					res = S_OK;
					break;
				}
			}
			_spike_record(g_trace.queries, &g_trace.query_count, SPIKE_MAX_EVENTS,
					pformatetc->cfFormat, pformatetc->lindex, pformatetc->tymed, (int32_t)res, 0, 0);
		}
		return res;
	}

	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pformatetcOut) override {
		if (pformatetcOut) {
			ZeroMemory(pformatetcOut, sizeof(FORMATETC));
			pformatetcOut->ptd = nullptr;
		}
		return E_NOTIMPL;
	}

	// Targets push "Performed DropEffect", "Paste Succeeded", etc. Accept and
	// discard -- refusing makes some shell targets abandon the drop.
	HRESULT STDMETHODCALLTYPE SetData(FORMATETC *pformatetc, STGMEDIUM *pmedium, BOOL fRelease) override {
		SpikeEngineFreeZone zone;
		if (pformatetc) {
			_spike_record(g_trace.sets, &g_trace.set_count, 32,
					pformatetc->cfFormat, pformatetc->lindex, pformatetc->tymed, S_OK, 0, 0);
		}
		if (fRelease && pmedium) {
			ReleaseStgMedium(pmedium);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppenumFormatEtc) override {
		SpikeEngineFreeZone zone;
		if (!ppenumFormatEtc) {
			return E_POINTER;
		}
		if (dwDirection != DATADIR_GET) {
			return E_NOTIMPL;
		}
		*ppenumFormatEtc = new SpikeEnumFormatEtc(formats, format_count, 0);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// ---------------------------------------------------------------------------
// IDropSource
// ---------------------------------------------------------------------------

static void _spike_classify_target() {
	POINT pt;
	if (!GetCursorPos(&pt)) {
		return;
	}
	HWND hwnd = WindowFromPoint(pt);
	if (!hwnd) {
		return;
	}
	hwnd = GetAncestor(hwnd, GA_ROOT);
	wchar_t cls[128] = { 0 };
	GetClassNameW(hwnd, cls, 128);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (lstrcmpW(cls, g_trace.last_target_class) != 0 || pid != g_trace.last_target_pid) {
		lstrcpynW(g_trace.last_target_class, cls, 128);
		g_trace.last_target_pid = pid;
		InterlockedIncrement(&g_trace.target_changes);
	}
}

class SpikeDropSource : public IDropSource {
	LONG ref_count = 1;
	bool pump_feedback = true;

public:
	explicit SpikeDropSource(bool p_pump_feedback) :
			pump_feedback(p_pump_feedback) {}

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
		InterlockedIncrement(&g_trace.query_continue_calls);
		if (fEscapePressed) {
			return DRAGDROP_S_CANCEL;
		}
		if (!(grfKeyState & MK_LBUTTON)) {
			return DRAGDROP_S_DROP;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD dwEffect) override {
		InterlockedIncrement(&g_trace.give_feedback_calls);
		g_trace.last_effect = dwEffect;
		_spike_classify_target();
		if (pump_feedback) {
			_spike_pump(PUMP_SOURCE_GIVE_FEEDBACK);
		}
		return DRAGDROP_S_USEDEFAULTCURSORS;
	}
};

// ---------------------------------------------------------------------------
// Message-only window: start point for the posted drag + the WM_TIMER pump.
// ---------------------------------------------------------------------------

static void _spike_run_drag();

static LRESULT CALLBACK _spike_wnd_proc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
		case SPIKE_WM_START_DRAG: {
			// Dispatched from DisplayServerWindows::process_events(), which
			// os_windows.cpp calls OUTSIDE Main::iteration() -- so the pump's
			// !Main::is_iterating() guard passes for the whole drag.
			_spike_run_drag();
			return 0;
		}
		case WM_TIMER: {
			if (wParam == SPIKE_TIMER_ID) {
				InterlockedIncrement(&g_trace.timer_ticks);
				_spike_pump(PUMP_SOURCE_TIMER);
				return 0;
			}
		} break;
	}
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static bool _spike_ensure_window() {
	if (g_msg_window) {
		return true;
	}
	static const wchar_t *CLASS_NAME = L"GodotMaterialLibraryDragOutSpike";
	static bool registered = false;
	HINSTANCE instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW wc;
		ZeroMemory(&wc, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = _spike_wnd_proc;
		wc.hInstance = instance;
		wc.lpszClassName = CLASS_NAME;
		if (!RegisterClassExW(&wc)) {
			return false;
		}
		registered = true;
	}
	g_msg_window = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
	return g_msg_window != nullptr;
}

static void _spike_run_drag() {
	if (!g_spike) {
		return;
	}

	// Per DESIGN.md: drop the engine's notion of held buttons and release
	// capture before OLE takes over the mouse.
	Input::get_singleton()->release_pressed_events();
	ReleaseCapture();

	HWND owner = nullptr;
	DisplayServer *ds = DisplayServer::get_singleton();
	if (ds) {
		owner = (HWND)ds->window_get_native_handle(DisplayServerEnums::WINDOW_HANDLE, DisplayServerEnums::MAIN_WINDOW_ID);
	}

	bool timer_pump = g_spike->is_pumping_from_timer();
	if (timer_pump && g_msg_window) {
		// GiveFeedback only fires on input; without this the app stops
		// rendering the moment the mouse holds still. Same trick as
		// WM_ENTERSIZEMOVE in display_server_windows.cpp.
		SetTimer(g_msg_window, SPIKE_TIMER_ID, USER_TIMER_MINIMUM, nullptr);
	}

	SpikeDataObject *data_object = new SpikeDataObject();
	SpikeDropSource *drop_source = new SpikeDropSource(g_spike->is_pumping_from_give_feedback());

	DWORD effect = DROPEFFECT_NONE;
	g_trace.drag_active = true;
	uint64_t start = _spike_now_usec();
	g_trace.drag_start_usec = start;
	// MOVE is deliberately not offered: it would let Explorer delete our source.
	HRESULT hr = SHDoDragDrop(owner, data_object, drop_source, DROPEFFECT_COPY, &effect);
	g_trace.drag_usec = _spike_now_usec() - start;
	g_trace.drag_active = false;
	g_trace.drag_completed = true;
	g_trace.dodragdrop_hr = (int32_t)hr;
	g_trace.final_effect = effect;

	if (timer_pump && g_msg_window) {
		KillTimer(g_msg_window, SPIKE_TIMER_ID);
	}

	data_object->Release();
	drop_source->Release();

	g_spike->call_deferred("emit_signal", "drag_finished", (int)hr, (int)effect);
}

// ---------------------------------------------------------------------------
// Snapshot + CF_HDROP staging
// ---------------------------------------------------------------------------

static void _spike_reset_trace() {
	SpikeFile files[SPIKE_MAX_FILES];
	int32_t file_count = g_trace.file_count;
	bool hdrop = g_trace.hdrop_mode;
	CopyMemory(files, g_trace.files, sizeof(files));
	wchar_t staging[MAX_PATH];
	lstrcpynW(staging, g_trace.staging_dir, MAX_PATH);

	ZeroMemory(&g_trace, sizeof(g_trace));

	CopyMemory(g_trace.files, files, sizeof(files));
	g_trace.file_count = file_count;
	g_trace.hdrop_mode = hdrop;
	lstrcpynW(g_trace.staging_dir, staging, MAX_PATH);
	g_trace.self_pid = GetCurrentProcessId();
}

static Error _spike_snapshot(const PackedStringArray &p_names, const PackedInt64Array &p_sizes) {
	ERR_FAIL_COND_V_MSG(p_names.is_empty(), ERR_INVALID_PARAMETER, "DragOutSpike: no file names given.");
	ERR_FAIL_COND_V_MSG(p_names.size() != p_sizes.size(), ERR_INVALID_PARAMETER, "DragOutSpike: names and sizes must be the same length.");
	ERR_FAIL_COND_V_MSG(p_names.size() > SPIKE_MAX_FILES, ERR_INVALID_PARAMETER, "DragOutSpike: too many files for the spike buffer.");

	ZeroMemory(g_trace.files, sizeof(g_trace.files));
	g_trace.file_count = p_names.size();
	for (int i = 0; i < p_names.size(); i++) {
		// The shell expects backslash-separated relative paths in the descriptor.
		String name = p_names[i].replace("/", "\\");
		Char16String name16 = name.utf16();
		lstrcpynW(g_trace.files[i].name, (LPCWSTR)name16.ptr(), MAX_PATH);
		int64_t size = p_sizes[i];
		g_trace.files[i].is_dir = size < 0;
		g_trace.files[i].size = size < 0 ? 0 : (uint64_t)size;
		g_trace.files[i].seed = (uint32_t)(i * 17 + 3);
	}
	return OK;
}

// CF_HDROP mode has no delayed rendering: real bytes must exist on disk before
// the drag begins. This is the fallback tier the spike measures against.
static Error _spike_stage_files() {
	wchar_t temp_root[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, temp_root)) {
		return FAILED;
	}
	wchar_t dir[MAX_PATH];
	wsprintfW(dir, L"%sgodot_dragout_spike_%u", temp_root, GetTickCount());
	if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return FAILED;
	}
	lstrcpynW(g_trace.staging_dir, dir, MAX_PATH);

	for (int i = 0; i < g_trace.file_count; i++) {
		wchar_t path[MAX_PATH * 2];
		lstrcpynW(path, dir, MAX_PATH);
		lstrcatW(path, L"\\");
		lstrcatW(path, g_trace.files[i].name);

		// Reconstruct any nested directories the name implies.
		wchar_t parent[MAX_PATH * 2];
		lstrcpynW(parent, path, MAX_PATH * 2);
		wchar_t *slash = wcsrchr(parent, L'\\');
		if (slash) {
			*slash = 0;
			SHCreateDirectoryExW(nullptr, parent, nullptr);
		}
		if (g_trace.files[i].is_dir) {
			SHCreateDirectoryExW(nullptr, path, nullptr);
			continue;
		}

		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return FAILED;
		}
		const DWORD chunk = 64 * 1024;
		unsigned char *buf = (unsigned char *)malloc(chunk);
		if (!buf) {
			CloseHandle(file);
			return FAILED;
		}
		uint64_t written = 0;
		while (written < g_trace.files[i].size) {
			DWORD count = (DWORD)MIN((uint64_t)chunk, g_trace.files[i].size - written);
			_spike_fill(buf, written, count, g_trace.files[i].seed);
			DWORD out = 0;
			if (!WriteFile(file, buf, count, &out, nullptr) || out == 0) {
				break;
			}
			written += out;
		}
		free(buf);
		CloseHandle(file);
	}
	return OK;
}

// ---------------------------------------------------------------------------
// Self-test: drive the data object exactly like DropTargetWindows does.
// ---------------------------------------------------------------------------

struct SpikeSelfTest {
	bool ok = true;
	String failure;
	int enumerated_formats = 0;
	bool descriptor_offered = false;
	bool contents_offered = false;
	bool hdrop_offered = false;
	uint64_t descriptor_usec = 0;
	uint64_t max_contents_usec = 0;
	uint64_t total_usec = 0;
	uint64_t bytes_verified = 0;
	int files_written = 0;
	bool nested_dir_reconstructed = false;
	String out_dir;
};

static SpikeSelfTest g_self_test;

#define SPIKE_TEST_FAIL(m_msg)             \
	{                                      \
		g_self_test.ok = false;            \
		g_self_test.failure = String(m_msg); \
		return false;                      \
	}

static bool _spike_self_test_impl() {
	SpikeDataObject *data_object = new SpikeDataObject();
	bool result = true;

	// 1. Enumerate, the way a target inspects an unknown data object.
	IEnumFORMATETC *penum = nullptr;
	if (data_object->EnumFormatEtc(DATADIR_GET, &penum) != S_OK || !penum) {
		data_object->Release();
		SPIKE_TEST_FAIL("EnumFormatEtc(DATADIR_GET) failed.");
	}
	FORMATETC fmt;
	ULONG fetched = 0;
	while (penum->Next(1, &fmt, &fetched) == S_OK && fetched == 1) {
		g_self_test.enumerated_formats++;
		if (fmt.cfFormat == g_cf_filedescriptor) {
			g_self_test.descriptor_offered = true;
		} else if (fmt.cfFormat == g_cf_filecontents) {
			g_self_test.contents_offered = true;
		} else if (fmt.cfFormat == CF_HDROP) {
			g_self_test.hdrop_offered = true;
		}
	}
	penum->Release();

	// 2. QueryGetData in DropTargetWindows' order: CF_HDROP first, descriptor
	//    second. Exactly one must answer -- the formats are exclusive per drag.
	FORMATETC hdrop_fmt = { (CLIPFORMAT)CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	FORMATETC desc_fmt = { (CLIPFORMAT)g_cf_filedescriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	bool has_hdrop = data_object->QueryGetData(&hdrop_fmt) == S_OK;
	bool has_desc = data_object->QueryGetData(&desc_fmt) == S_OK;

	if (has_hdrop == has_desc) {
		data_object->Release();
		SPIKE_TEST_FAIL("CF_HDROP and FILEGROUPDESCRIPTORW must be mutually exclusive per drag.");
	}
	if (g_trace.hdrop_mode != has_hdrop) {
		data_object->Release();
		SPIKE_TEST_FAIL("Offered format does not match the requested mode.");
	}

	// Prepare an output directory, mirroring DropTargetWindows' temp dir.
	wchar_t temp_root[MAX_PATH];
	GetTempPathW(MAX_PATH, temp_root);
	wchar_t out_dir[MAX_PATH];
	wsprintfW(out_dir, L"%sgodot_dragout_selftest_%u", temp_root, GetTickCount());
	SHCreateDirectoryExW(nullptr, out_dir, nullptr);
	g_self_test.out_dir = String::utf16((const char16_t *)out_dir);

	uint64_t test_start = _spike_now_usec();

	if (has_hdrop) {
		// CF_HDROP path: paths only, content already on disk.
		STGMEDIUM stg;
		if (data_object->GetData(&hdrop_fmt, &stg) != S_OK) {
			data_object->Release();
			SPIKE_TEST_FAIL("GetData(CF_HDROP) failed.");
		}
		HDROP hdrop = (HDROP)GlobalLock(stg.hGlobal);
		int count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
		if (count != g_trace.file_count) {
			GlobalUnlock(stg.hGlobal);
			ReleaseStgMedium(&stg);
			data_object->Release();
			SPIKE_TEST_FAIL("CF_HDROP file count mismatch.");
		}
		for (int i = 0; i < count; i++) {
			wchar_t path[MAX_PATH * 2] = { 0 };
			DragQueryFileW(hdrop, i, path, MAX_PATH * 2);
			if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
				GlobalUnlock(stg.hGlobal);
				ReleaseStgMedium(&stg);
				data_object->Release();
				SPIKE_TEST_FAIL("CF_HDROP referenced a staged file that does not exist.");
			}
			g_self_test.files_written++;
			g_self_test.bytes_verified += g_trace.files[i].size;
		}
		GlobalUnlock(stg.hGlobal);
		ReleaseStgMedium(&stg);
	} else {
		// Delayed-rendering path.
		STGMEDIUM stg;
		uint64_t t0 = _spike_now_usec();
		if (data_object->GetData(&desc_fmt, &stg) != S_OK) {
			data_object->Release();
			SPIKE_TEST_FAIL("GetData(FILEGROUPDESCRIPTORW) failed.");
		}
		g_self_test.descriptor_usec = _spike_now_usec() - t0;

		FILEGROUPDESCRIPTORW *group = (FILEGROUPDESCRIPTORW *)GlobalLock(stg.hGlobal);
		if (!group || (int)group->cItems != g_trace.file_count) {
			if (group) {
				GlobalUnlock(stg.hGlobal);
			}
			ReleaseStgMedium(&stg);
			data_object->Release();
			SPIKE_TEST_FAIL("Descriptor item count mismatch.");
		}

		for (int i = 0; i < (int)group->cItems; i++) {
			FILEDESCRIPTORW *fd = &group->fgd[i];
			if (lstrcmpW(fd->cFileName, g_trace.files[i].name) != 0) {
				result = false;
				break;
			}

			wchar_t full[MAX_PATH * 2];
			lstrcpynW(full, out_dir, MAX_PATH);
			lstrcatW(full, L"\\");
			lstrcatW(full, fd->cFileName);

			// Same reconstruction DropTargetWindows::save_as_file performs.
			wchar_t parent[MAX_PATH * 2];
			lstrcpynW(parent, full, MAX_PATH * 2);
			wchar_t *slash = wcsrchr(parent, L'\\');
			bool nested = false;
			if (slash) {
				*slash = 0;
				if (lstrcmpW(parent, out_dir) != 0) {
					nested = true;
				}
				SHCreateDirectoryExW(nullptr, parent, nullptr);
			}

			if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				SHCreateDirectoryExW(nullptr, full, nullptr);
				continue;
			}

			// Pull the content, only now.
			FORMATETC contents_fmt = { (CLIPFORMAT)g_cf_filecontents, nullptr, DVASPECT_CONTENT, i, TYMED_ISTREAM };
			STGMEDIUM content_stg;
			uint64_t c0 = _spike_now_usec();
			HRESULT content_hr = data_object->GetData(&contents_fmt, &content_stg);
			uint64_t c_elapsed = _spike_now_usec() - c0;
			if (c_elapsed > g_self_test.max_contents_usec) {
				g_self_test.max_contents_usec = c_elapsed;
			}
			if (content_hr != S_OK || content_stg.tymed != TYMED_ISTREAM || !content_stg.pstm) {
				result = false;
				break;
			}

			STATSTG stat;
			ZeroMemory(&stat, sizeof(stat));
			content_stg.pstm->Stat(&stat, STATFLAG_NONAME);
			if (stat.cbSize.QuadPart != (ULONGLONG)g_trace.files[i].size) {
				ReleaseStgMedium(&content_stg);
				result = false;
				break;
			}

			HANDLE out_file = CreateFileW(full, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (out_file == INVALID_HANDLE_VALUE) {
				ReleaseStgMedium(&content_stg);
				result = false;
				break;
			}

			// Read exactly as stream2file does, and verify the pattern.
			const ULONG bufsize = 4096;
			unsigned char buf[bufsize];
			ULONG nread = 0;
			uint64_t offset = 0;
			bool content_ok = true;
			while (true) {
				HRESULT rr = content_stg.pstm->Read(buf, bufsize, &nread);
				if (rr != S_OK && rr != S_FALSE) {
					content_ok = false;
					break;
				}
				if (!nread) {
					break;
				}
				for (ULONG b = 0; b < nread; b++) {
					if (buf[b] != _spike_byte(offset + b, g_trace.files[i].seed)) {
						content_ok = false;
						break;
					}
				}
				if (!content_ok) {
					break;
				}
				DWORD written = 0;
				WriteFile(out_file, buf, nread, &written, nullptr);
				offset += nread;
			}
			CloseHandle(out_file);
			ReleaseStgMedium(&content_stg);

			if (!content_ok || offset != g_trace.files[i].size) {
				result = false;
				break;
			}
			g_self_test.bytes_verified += offset;
			g_self_test.files_written++;
			if (nested) {
				g_self_test.nested_dir_reconstructed = true;
			}
		}

		GlobalUnlock(stg.hGlobal);
		ReleaseStgMedium(&stg);
	}

	g_self_test.total_usec = _spike_now_usec() - test_start;
	data_object->Release();

	if (!result) {
		SPIKE_TEST_FAIL("Content verification failed (see report for the per-format trace).");
	}
	if (g_trace.engine_free_violations != 0) {
		SPIKE_TEST_FAIL("The engine was re-entered from an engine-free zone.");
	}
	return true;
}

#endif // WINDOWS_ENABLED

// ---------------------------------------------------------------------------
// Bound API
// ---------------------------------------------------------------------------

Error DragOutSpike::start_drag(const PackedStringArray &p_names, const PackedInt64Array &p_sizes) {
#ifdef WINDOWS_ENABLED
	ERR_FAIL_COND_V_MSG(g_trace.drag_active, ERR_BUSY, "DragOutSpike: a drag is already running.");

	Error err = _spike_snapshot(p_names, p_sizes);
	if (err != OK) {
		return err;
	}
	g_trace.hdrop_mode = use_hdrop;
	_spike_reset_trace();

	if (use_hdrop) {
		err = _spike_stage_files();
		ERR_FAIL_COND_V_MSG(err != OK, err, "DragOutSpike: failed to stage temp files for CF_HDROP.");
	}

	if (!_spike_ensure_window()) {
		ERR_FAIL_V_MSG(FAILED, "DragOutSpike: failed to create the message-only window.");
	}

	if (start_mode == START_INLINE) {
		// Runs inside Main::iteration() when called from _gui_input: every
		// pump attempt will be rejected by the !Main::is_iterating() guard.
		_spike_run_drag();
	} else {
		PostMessageW(g_msg_window, SPIKE_WM_START_DRAG, 0, 0);
	}
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

bool DragOutSpike::run_self_test() {
#ifdef WINDOWS_ENABLED
	PackedStringArray names;
	PackedInt64Array sizes;
	names.push_back("brick_albedo.png");
	sizes.push_back(1024 * 1024);
	names.push_back("brick_normal.png");
	sizes.push_back(4 * 1024 * 1024);
	names.push_back("brick.tres");
	sizes.push_back(2048);
	names.push_back("textures/nested/brick_orm.png");
	sizes.push_back(512 * 1024);

	Error err = _spike_snapshot(names, sizes);
	ERR_FAIL_COND_V(err != OK, false);
	g_trace.hdrop_mode = use_hdrop;
	_spike_reset_trace();

	if (use_hdrop) {
		err = _spike_stage_files();
		ERR_FAIL_COND_V_MSG(err != OK, false, "DragOutSpike: failed to stage temp files for CF_HDROP.");
	}

	g_self_test = SpikeSelfTest();
	bool ok = _spike_self_test_impl();
	g_self_test.ok = ok && g_self_test.ok;
	return g_self_test.ok;
#else
	return false;
#endif
}

Dictionary DragOutSpike::get_report() const {
	Dictionary report;
#ifdef WINDOWS_ENABLED
	auto format_name = [](uint32_t p_cf) -> String {
		if (p_cf == CF_HDROP) {
			return "CF_HDROP";
		}
		wchar_t buf[128] = { 0 };
		if (GetClipboardFormatNameW(p_cf, buf, 128) > 0) {
			return String::utf16((const char16_t *)buf);
		}
		return "cf#" + itos(p_cf);
	};

	report["mode"] = g_trace.hdrop_mode ? "CF_HDROP" : "FILEGROUPDESCRIPTORW";
	report["start_mode"] = start_mode == START_INLINE ? "inline" : "posted";
	report["file_count"] = g_trace.file_count;

	Array offered;
	for (int i = 0; i < g_trace.file_count; i++) {
		Dictionary f;
		f["name"] = String::utf16((const char16_t *)g_trace.files[i].name);
		f["size"] = (int64_t)g_trace.files[i].size;
		f["is_dir"] = g_trace.files[i].is_dir;
		offered.push_back(f);
	}
	report["files"] = offered;

	Array enumerated;
	LONG enum_count = MIN(g_trace.enumerated_count, (LONG)32);
	for (LONG i = 0; i < enum_count; i++) {
		enumerated.push_back(format_name(g_trace.enumerated[i]));
	}
	report["formats_enumerated"] = enumerated;
	report["enum_format_etc_calls"] = (int)g_trace.enum_calls;

	auto events_to_array = [&](const SpikeEvent *p_buf, LONG p_count, int p_cap) {
		Array out;
		LONG count = MIN(p_count, (LONG)p_cap);
		for (LONG i = 0; i < count; i++) {
			Dictionary e;
			e["format"] = format_name(p_buf[i].cf);
			e["lindex"] = p_buf[i].lindex;
			e["tymed"] = (int)p_buf[i].tymed;
			e["hresult"] = p_buf[i].hr;
			e["ok"] = p_buf[i].hr == 0;
			e["usec"] = (int64_t)p_buf[i].usec;
			e["bytes"] = (int64_t)p_buf[i].bytes;
			out.push_back(e);
		}
		return out;
	};

	report["queries"] = events_to_array(g_trace.queries, g_trace.query_count, SPIKE_MAX_EVENTS);
	report["gets"] = events_to_array(g_trace.gets, g_trace.get_count, SPIKE_MAX_EVENTS);
	report["set_data"] = events_to_array(g_trace.sets, g_trace.set_count, 32);
	report["query_get_data_calls"] = (int)g_trace.query_count;
	report["get_data_calls"] = (int)g_trace.get_count;

	report["stream_reads"] = (int)g_trace.stream_reads;
	report["stream_bytes"] = (int64_t)g_trace.stream_bytes;
	report["streams_created"] = (int)g_trace.streams_created;
	report["stat_calls"] = (int)g_trace.stat_calls;

	// Hover-time probing vs. drop-time rendering.
	report["contents_get_calls"] = (int)g_trace.contents_gets;
	report["first_contents_ms_into_drag"] = (double)g_trace.first_contents_usec / 1000.0;
	report["last_contents_ms_into_drag"] = (double)g_trace.last_contents_usec / 1000.0;
	report["descriptor_get_calls"] = (int)g_trace.descriptor_gets;
	report["first_descriptor_ms_into_drag"] = (double)g_trace.first_descriptor_usec / 1000.0;
	report["last_descriptor_ms_into_drag"] = (double)g_trace.last_descriptor_usec / 1000.0;
	report["first_stream_read_ms_into_drag"] = (double)g_trace.first_stream_read_usec / 1000.0;
	report["last_stream_read_ms_into_drag"] = (double)g_trace.last_stream_read_usec / 1000.0;

	report["max_getdata_usec"] = (int64_t)g_trace.max_getdata_usec;
	report["max_getdata_msec"] = (double)g_trace.max_getdata_usec / 1000.0;
	report["descriptor_usec"] = (int64_t)g_trace.descriptor_usec;
	report["drag_msec"] = (double)g_trace.drag_usec / 1000.0;

	report["pump_iterations_timer"] = (int)g_trace.pump_iterations_timer;
	report["pump_iterations_give_feedback"] = (int)g_trace.pump_iterations_feedback;
	report["pump_iterations_total"] = (int)(g_trace.pump_iterations_timer + g_trace.pump_iterations_feedback);
	report["pump_skips_reentrant"] = (int)g_trace.pump_skips_reentrant;
	report["give_feedback_calls"] = (int)g_trace.give_feedback_calls;
	report["query_continue_calls"] = (int)g_trace.query_continue_calls;
	report["timer_ticks"] = (int)g_trace.timer_ticks;
	report["engine_free_violations"] = (int)g_trace.engine_free_violations;

	report["last_target_class"] = String::utf16((const char16_t *)g_trace.last_target_class);
	report["last_target_pid"] = (int64_t)g_trace.last_target_pid;
	report["self_pid"] = (int64_t)g_trace.self_pid;
	report["target_is_self"] = g_trace.last_target_pid == g_trace.self_pid;
	report["target_changes"] = (int)g_trace.target_changes;
	report["last_effect"] = (int)g_trace.last_effect;

	report["drag_completed"] = g_trace.drag_completed;
	report["dodragdrop_hresult"] = g_trace.dodragdrop_hr;
	report["final_effect"] = (int)g_trace.final_effect;
	report["staging_dir"] = String::utf16((const char16_t *)g_trace.staging_dir);

	Dictionary self_test;
	self_test["ok"] = g_self_test.ok;
	self_test["failure"] = g_self_test.failure;
	self_test["formats_enumerated"] = g_self_test.enumerated_formats;
	self_test["descriptor_offered"] = g_self_test.descriptor_offered;
	self_test["contents_offered"] = g_self_test.contents_offered;
	self_test["hdrop_offered"] = g_self_test.hdrop_offered;
	self_test["descriptor_msec"] = (double)g_self_test.descriptor_usec / 1000.0;
	self_test["max_contents_msec"] = (double)g_self_test.max_contents_usec / 1000.0;
	self_test["total_msec"] = (double)g_self_test.total_usec / 1000.0;
	self_test["bytes_verified"] = (int64_t)g_self_test.bytes_verified;
	self_test["files_written"] = g_self_test.files_written;
	self_test["nested_dir_reconstructed"] = g_self_test.nested_dir_reconstructed;
	self_test["out_dir"] = g_self_test.out_dir;
	report["self_test"] = self_test;
#endif
	return report;
}

void DragOutSpike::clear_report() {
#ifdef WINDOWS_ENABLED
	_spike_reset_trace();
	g_self_test = SpikeSelfTest();
#endif
}

void DragOutSpike::set_use_hdrop(bool p_enabled) {
	use_hdrop = p_enabled;
}

bool DragOutSpike::is_using_hdrop() const {
	return use_hdrop;
}

void DragOutSpike::set_start_mode(StartMode p_mode) {
	start_mode = p_mode;
}

DragOutSpike::StartMode DragOutSpike::get_start_mode() const {
	return start_mode;
}

void DragOutSpike::set_pump_from_timer(bool p_enabled) {
	pump_from_timer = p_enabled;
}

bool DragOutSpike::is_pumping_from_timer() const {
	return pump_from_timer;
}

void DragOutSpike::set_pump_from_give_feedback(bool p_enabled) {
	pump_from_give_feedback = p_enabled;
}

bool DragOutSpike::is_pumping_from_give_feedback() const {
	return pump_from_give_feedback;
}

bool DragOutSpike::is_dragging() const {
#ifdef WINDOWS_ENABLED
	return g_trace.drag_active;
#else
	return false;
#endif
}

void DragOutSpike::_emit_finished(int p_hresult, int p_effect) {
	emit_signal("drag_finished", p_hresult, p_effect);
}

void DragOutSpike::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_drag", "names", "sizes"), &DragOutSpike::start_drag);
	ClassDB::bind_method(D_METHOD("run_self_test"), &DragOutSpike::run_self_test);
	ClassDB::bind_method(D_METHOD("get_report"), &DragOutSpike::get_report);
	ClassDB::bind_method(D_METHOD("clear_report"), &DragOutSpike::clear_report);
	ClassDB::bind_method(D_METHOD("set_use_hdrop", "enabled"), &DragOutSpike::set_use_hdrop);
	ClassDB::bind_method(D_METHOD("is_using_hdrop"), &DragOutSpike::is_using_hdrop);
	ClassDB::bind_method(D_METHOD("set_start_mode", "mode"), &DragOutSpike::set_start_mode);
	ClassDB::bind_method(D_METHOD("get_start_mode"), &DragOutSpike::get_start_mode);
	ClassDB::bind_method(D_METHOD("set_pump_from_timer", "enabled"), &DragOutSpike::set_pump_from_timer);
	ClassDB::bind_method(D_METHOD("is_pumping_from_timer"), &DragOutSpike::is_pumping_from_timer);
	ClassDB::bind_method(D_METHOD("set_pump_from_give_feedback", "enabled"), &DragOutSpike::set_pump_from_give_feedback);
	ClassDB::bind_method(D_METHOD("is_pumping_from_give_feedback"), &DragOutSpike::is_pumping_from_give_feedback);
	ClassDB::bind_method(D_METHOD("is_dragging"), &DragOutSpike::is_dragging);

	ADD_SIGNAL(MethodInfo("drag_finished", PropertyInfo(Variant::INT, "hresult"), PropertyInfo(Variant::INT, "effect")));

	BIND_ENUM_CONSTANT(START_POSTED);
	BIND_ENUM_CONSTANT(START_INLINE);
}

DragOutSpike::DragOutSpike() {
#ifdef WINDOWS_ENABLED
	g_spike = this;
	// UNICODE is not defined for the engine build, so the CFSTR_* macros are
	// narrow strings; use the TCHAR-resolved entry point like
	// drop_target_windows.cpp does.
	g_cf_filedescriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
	g_cf_filecontents = RegisterClipboardFormat(CFSTR_FILECONTENTS);
	g_cf_preferred_drop_effect = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
	g_trace.self_pid = GetCurrentProcessId();
#endif
}

DragOutSpike::~DragOutSpike() {
#ifdef WINDOWS_ENABLED
	if (g_spike == this) {
		g_spike = nullptr;
	}
	if (g_msg_window) {
		DestroyWindow(g_msg_window);
		g_msg_window = nullptr;
	}
#endif
}
