/**************************************************************************/
/*  drag_source_windows.h                                                 */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/callable.h"

#include <windows.h>

// OS drag-OUT source: hands a set of *virtual* files to any OLE drop target
// (Explorer, a Godot editor, this fork's editor) using Win32 delayed rendering.
//
// Names are final when the drag starts (the shell pulls CFSTR_FILEDESCRIPTORW
// repeatedly during hover); bytes are produced only when the target actually
// reads a CFSTR_FILECONTENTS stream, which happens at drop time.
//
// Threading contract (this is the whole design in four lines):
//  - SHDoDragDrop runs on the main thread; the engine keeps rendering because
//    IDropSource::GiveFeedback and a WM_TIMER both pump Main::iteration().
//  - IDataObject::GetData and IStream::Read are an ENGINE-FREE ZONE. They read
//    a POD snapshot taken at drag start and block on a worker thread with a
//    COM-filtered message pump. They never touch a Godot type or the scene tree.
//  - The byte provider Callable is therefore invoked on that worker thread.
//  - Effects are restricted to DROPEFFECT_COPY (MOVE would let a target delete
//    whatever it thinks the source was).
class DragSourceWindows {
public:
	struct FileEntry {
		String name; // File name, no separators.
		String dir; // Relative directory inside the drop, "" for the drop root.
	};

	// Starts a modal OS drag. Returns only once the drag has finished (or was
	// cancelled); the completion callback is invoked deferred, through the
	// MessageQueue, so receivers run on a clean stack.
	// p_provider_timeout_ms is the PER-SLOT deadline (from the moment a target
	// first reads that slot) the provider gets before the slot is failed,
	// clamped to [500, 60000]. Snapshotted at drag start like file_count.
	static Error start_drag(HWND p_owner, const Vector<FileEntry> &p_files, const Callable &p_provider, const Callable &p_finished_callback, const Callable &p_target_changed_callback, const String &p_manifest, int p_provider_timeout_ms = 10000);

	static bool is_dragging();

	// True while the calling thread is inside GetData / IStream::Read. The pump
	// checks it: re-entering the engine from a content path is the one thing
	// that must never happen.
	static bool is_engine_free_zone();

	// Runs one engine iteration if it is safe to do so. Called from
	// GiveFeedback and from the WM_TIMER half of the pump.
	static void pump_engine();

	// Deletes any CF_HDROP staging tree left behind by the UNKNOWN-target
	// fallback tier.
	static void sweep_staging();
};
