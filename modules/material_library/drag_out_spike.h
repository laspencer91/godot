/**************************************************************************/
/*  drag_out_spike.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Phase 0 spike S1: OS-level drag-OUT of virtual files with delayed rendering.
//
// Offers CFSTR_FILEDESCRIPTORW at drag start (names final, sizes declared) and
// synthesizes file bytes only when the drop target pulls CFSTR_FILECONTENTS.
// The content path (IDataObject::GetData / IStream::Read) is an engine-free
// zone: it reads a POD snapshot marshalled at drag start and touches no Godot
// API. `set_use_hdrop()` flips to an exclusive CF_HDROP mode (real temp files
// written up front) so the two strategies can be compared.
//
// Everything is throwaway: single active drag, fixed-size trace buffers, no
// error recovery beyond what the spike needs to produce a verdict.
//
// Other platforms: every entry point is a no-op / ERR_UNAVAILABLE stub.
class DragOutSpike : public Object {
	GDCLASS(DragOutSpike, Object);

public:
	// Where SHDoDragDrop is invoked from. This is the crux of the spike: the
	// engine's modal pump is guarded by `!Main::is_iterating()`, so a drag begun
	// from _gui_input (inside Main::iteration) can never pump and freezes the
	// app for its whole duration.
	enum StartMode {
		START_POSTED, // Post to a message-only window; the drag runs from its
					  // WndProc during DisplayServer::process_events(), which
					  // os_windows.cpp calls outside Main::iteration(). Pumps work.
		START_INLINE, // Call SHDoDragDrop directly. Demonstrates the freeze.
	};

private:
	bool use_hdrop = false;
	StartMode start_mode = START_POSTED;
	bool pump_from_timer = true;
	bool pump_from_give_feedback = true;

	void _emit_finished(int p_hresult, int p_effect);

protected:
	static void _bind_methods();

public:
	// Begins the drag. `p_sizes` declares the byte length of each file; content
	// for that many bytes is generated on demand at drop time.
	// A name containing '/' or '\\' is offered as a nested path (the receiver is
	// expected to reconstruct the directories).
	Error start_drag(const PackedStringArray &p_names, const PackedInt64Array &p_sizes);

	// Drives the data object in-process the way DropTargetWindows does
	// (EnumFormatEtc / QueryGetData / GetData descriptor + contents), asserting
	// contents, timing and the engine-free property. No mouse, no shell.
	bool run_self_test();

	Dictionary get_report() const;
	void clear_report();

	void set_use_hdrop(bool p_enabled);
	bool is_using_hdrop() const;
	void set_start_mode(StartMode p_mode);
	StartMode get_start_mode() const;
	void set_pump_from_timer(bool p_enabled);
	bool is_pumping_from_timer() const;
	void set_pump_from_give_feedback(bool p_enabled);
	bool is_pumping_from_give_feedback() const;

	bool is_dragging() const;

	DragOutSpike();
	~DragOutSpike();
};

VARIANT_ENUM_CAST(DragOutSpike::StartMode);
