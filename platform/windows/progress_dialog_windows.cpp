/**************************************************************************/
/*  progress_dialog_windows.cpp                                           */
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

#include "progress_dialog_windows.h"

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"
#include "servers/display/display_server_enums.h"

#include <shlobj.h>
#include <shobjidl.h>

// ---------------------------------------------------------------------------
// Instance state.
//
// Public mutators only take a mutex, write the desired state, and set an
// event. The worker thread is the ONLY code that ever holds a COM pointer:
// CLSID_ProgressDialog is ThreadingModel=Apartment, so the raw interface is
// legal on its creating STA alone. That creating STA is the worker's own,
// which is what keeps the dialog updatable while the engine's main thread is
// blocked (the whole point of the feature).
// ---------------------------------------------------------------------------

struct ProgressDialogInstance {
	int id = 0;

	// Creation snapshot, read once by the worker.
	String title;
	uint32_t flags = 0;
	HWND owner = nullptr;
	Callable cancelled_callback;

	// Desired state, guarded by `mutex`.
	Mutex mutex;
	String line1;
	String line2;
	uint64_t completed = 0;
	uint64_t total = 1;
	bool lines_dirty = false;
	bool progress_dirty = false;

	SafeFlag stop_requested;
	SafeFlag cancelled;
	SafeFlag started;

	HANDLE update_event = nullptr; // Manual-reset.
	Thread thread;
};

static Mutex g_pd_mutex;
static HashMap<int, ProgressDialogInstance *> g_pd_instances;
static int g_pd_next_id = 1;

static DWORD _map_flags(uint32_t p_flags) {
	DWORD out = PROGDLG_NORMAL;
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_MODAL) {
		out |= PROGDLG_MODAL;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_MARQUEE) {
		out |= PROGDLG_MARQUEEPROGRESS;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_NO_TIME) {
		out |= PROGDLG_NOTIME;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_NO_MINIMIZE) {
		out |= PROGDLG_NOMINIMIZE;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_NO_CANCEL) {
		out |= PROGDLG_NOCANCEL;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_NO_PROGRESS_BAR) {
		out |= PROGDLG_NOPROGRESSBAR;
	}
	if (p_flags & DisplayServerEnums::PROGRESS_DIALOG_FLAG_AUTO_TIME) {
		out |= PROGDLG_AUTOTIME;
	}
	return out;
}

// Copies the desired state under the mutex and applies it to the COM object
// outside it. `p_force` replays everything once right after StartProgressDialog
// returns: SetProgress/SetLine calls made before the dialog window exists are
// silently dropped by the shell.
static void _apply_state(ProgressDialogInstance *inst, IProgressDialog *dlg, bool p_force) {
	String line1, line2;
	uint64_t completed = 0, total = 1;
	bool apply_lines = false, apply_progress = false;
	{
		MutexLock lock(inst->mutex);
		apply_lines = inst->lines_dirty || p_force;
		apply_progress = inst->progress_dirty || p_force;
		inst->lines_dirty = false;
		inst->progress_dirty = false;
		if (apply_lines) {
			line1 = inst->line1;
			line2 = inst->line2;
		}
		if (apply_progress) {
			completed = inst->completed;
			total = inst->total;
		}
	}
	if (apply_lines) {
		Char16String l1 = line1.utf16();
		Char16String l2 = line2.utf16();
		dlg->SetLine(1, (LPCWSTR)l1.get_data(), FALSE, nullptr);
		dlg->SetLine(2, (LPCWSTR)l2.get_data(), FALSE, nullptr);
	}
	if (apply_progress) {
		dlg->SetProgress64(completed, total);
	}
}

static void _progress_dialog_thread_func(void *p_ud) {
	ProgressDialogInstance *inst = (ProgressDialogInstance *)p_ud;

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	IProgressDialog *dlg = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
	if (SUCCEEDED(hr) && dlg) {
		{
			Char16String title16 = inst->title.utf16();
			dlg->SetTitle((LPCWSTR)title16.get_data());
		}
		// SetAnimation is deliberately skipped: it needs an AVI resource and is
		// ignored on modern Windows anyway.
		dlg->SetCancelMsg(L"Finishing up...", nullptr);
		// punkEnableModless stays null: the shell must never disable our
		// windows through IOleInPlaceActiveObject.
		dlg->StartProgressDialog(inst->owner, nullptr, _map_flags(inst->flags), nullptr);
		inst->started.set();
		// Replay the current desired state once: anything set between create()
		// and the dialog window existing was dropped by the shell.
		_apply_state(inst, dlg, true);

		bool callback_fired = false;
		while (!inst->stop_requested.is_set()) {
			// QS_ALLINPUT + a drain, not a bare WaitForSingleObject: the shell's
			// internal SendMessages to this apartment deadlock unless this
			// thread keeps pumping.
			MsgWaitForMultipleObjectsEx(1, &inst->update_event, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			ResetEvent(inst->update_event);
			_apply_state(inst, dlg, false);
			// HasUserCancelled is too expensive for callers' tight loops; this
			// ~100 ms poll caches it into a flag that is a free atomic read.
			if (!inst->cancelled.is_set() && dlg->HasUserCancelled()) {
				inst->cancelled.set();
				if (!callback_fired && inst->cancelled_callback.is_valid()) {
					callback_fired = true;
					// Deferred only — never a direct call from this thread.
					inst->cancelled_callback.call_deferred(inst->id);
				}
			}
		}
		dlg->StopProgressDialog();
		dlg->Release();
	}
	CoUninitialize();
}

int ProgressDialogWindows::create(const Params &p_params, const Callable &p_cancelled_callback) {
	ProgressDialogInstance *inst = memnew(ProgressDialogInstance);
	inst->title = p_params.title;
	inst->line1 = p_params.line1;
	inst->line2 = p_params.line2;
	inst->lines_dirty = true;
	inst->flags = p_params.flags;
	inst->owner = p_params.owner;
	inst->cancelled_callback = p_cancelled_callback;
	inst->update_event = CreateEventW(nullptr, TRUE, FALSE, nullptr); // Manual-reset.
	if (!inst->update_event) {
		memdelete(inst);
		return DisplayServerEnums::INVALID_PROGRESS_DIALOG_ID;
	}

	{
		MutexLock lock(g_pd_mutex);
		inst->id = g_pd_next_id++;
		g_pd_instances[inst->id] = inst;
	}
	inst->thread.start(_progress_dialog_thread_func, inst);
	return inst->id;
}

void ProgressDialogWindows::set_progress(int p_id, uint64_t p_completed, uint64_t p_total) {
	// g_pd_mutex is held for the whole (cheap, COM-free) body so destroy()
	// cannot free the instance from under us.
	MutexLock lock(g_pd_mutex);
	ProgressDialogInstance **inst_ptr = g_pd_instances.getptr(p_id);
	if (!inst_ptr) {
		return;
	}
	ProgressDialogInstance *inst = *inst_ptr;
	{
		MutexLock state_lock(inst->mutex);
		inst->completed = p_completed;
		inst->total = p_total;
		inst->progress_dirty = true;
	}
	SetEvent(inst->update_event);
}

void ProgressDialogWindows::set_lines(int p_id, const String &p_line1, const String &p_line2) {
	MutexLock lock(g_pd_mutex);
	ProgressDialogInstance **inst_ptr = g_pd_instances.getptr(p_id);
	if (!inst_ptr) {
		return;
	}
	ProgressDialogInstance *inst = *inst_ptr;
	{
		MutexLock state_lock(inst->mutex);
		inst->line1 = p_line1;
		inst->line2 = p_line2;
		inst->lines_dirty = true;
	}
	SetEvent(inst->update_event);
}

bool ProgressDialogWindows::is_cancelled(int p_id) {
	MutexLock lock(g_pd_mutex);
	ProgressDialogInstance **inst_ptr = g_pd_instances.getptr(p_id);
	if (!inst_ptr) {
		return false;
	}
	return (*inst_ptr)->cancelled.is_set();
}

void ProgressDialogWindows::destroy(int p_id) {
	ProgressDialogInstance *inst = nullptr;
	{
		MutexLock lock(g_pd_mutex);
		ProgressDialogInstance **inst_ptr = g_pd_instances.getptr(p_id);
		if (!inst_ptr) {
			return; // Idempotent: the delete-on-finish / delete-on-drain race is harmless.
		}
		inst = *inst_ptr;
		g_pd_instances.erase(p_id);
	}
	// Join outside the map lock; any mutator racing us either completed while
	// we waited for g_pd_mutex or no longer finds the id.
	inst->stop_requested.set();
	SetEvent(inst->update_event);
	inst->thread.wait_to_finish();
	CloseHandle(inst->update_event);
	memdelete(inst);
}

void ProgressDialogWindows::shutdown_all() {
	while (true) {
		int id = 0;
		{
			MutexLock lock(g_pd_mutex);
			if (g_pd_instances.is_empty()) {
				return;
			}
			id = g_pd_instances.begin()->key;
		}
		destroy(id);
	}
}
