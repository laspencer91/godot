/**************************************************************************/
/*  progress_dialog_windows.h                                             */
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

#include "core/string/ustring.h"
#include "core/variant/callable.h"

#include <windows.h>

// Native shell progress dialog (IProgressDialog), one Win32 concern per file
// pair like drag_source_windows.
//
// Threading contract (this is the whole design in four lines):
//  - Each dialog owns a worker thread; the IProgressDialog COM object lives on
//    that thread's own STA and is NEVER touched from any other thread.
//  - Every public method below is safe to call from ANY thread: mutators only
//    take a mutex, write plain desired state, and signal an event. Zero COM
//    contact, no marshalling — which is what lets updates land while the main
//    thread is parked inside SHDoDragDrop's streaming phase.
//  - The worker polls HasUserCancelled ~every 100 ms into a SafeFlag, so
//    is_cancelled() is a free atomic read callable per-slot from a provider.
//  - The cancelled Callable fires at most once, via call_deferred only.
class ProgressDialogWindows {
public:
	struct Params {
		String title;
		String line1;
		String line2;
		uint32_t flags = 0; // DisplayServerEnums::ProgressDialogFlags bits.
		HWND owner = nullptr;
	};

	// Returns a handle > 0, or 0 on failure. The callback, if valid, is
	// call_deferred once with the dialog id when the user clicks Cancel.
	static int create(const Params &p_params, const Callable &p_cancelled_callback);
	static void set_progress(int p_id, uint64_t p_completed, uint64_t p_total);
	static void set_lines(int p_id, const String &p_line1, const String &p_line2);
	static bool is_cancelled(int p_id);
	// Joins the dialog's worker thread. Idempotent, safe from any thread — but
	// must never be called from the dialog's own cancelled callback stack on
	// the worker (deferred delivery already prevents this).
	static void destroy(int p_id);
	// Called from the DisplayServerWindows destructor, BEFORE OleUninitialize,
	// so no worker is still inside COM when the apartment is torn down.
	static void shutdown_all();
};
