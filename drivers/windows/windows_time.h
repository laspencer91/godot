/**************************************************************************/
/*  windows_time.h                                                       */
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

#ifdef WINDOWS_ENABLED

#include <windows.h>

// Shared FILETIME -> Unix epoch (seconds) conversion, used by both
// FileAccessWindows::_get_modified_time (a CreateFileW + GetFileTime stat)
// and DirAccessWindows (which already has the FILETIME from directory
// enumeration and would otherwise need a redundant stat call). Both call
// sites MUST produce identical results for the same file, since cached
// modified-time values (e.g. in the editor's filesystem cache) are compared
// across whichever code path produced them.
static inline uint64_t windows_filetime_to_unix_time(const FILETIME &p_ft) {
	uint64_t ticks = (uint64_t)p_ft.dwHighDateTime << 32 | p_ft.dwLowDateTime;

	const uint64_t WINDOWS_TICKS_PER_SECOND = 10000000;
	const uint64_t TICKS_TO_UNIX_EPOCH = 116444736000000000LL;

	if (ticks >= TICKS_TO_UNIX_EPOCH) {
		return (ticks - TICKS_TO_UNIX_EPOCH) / WINDOWS_TICKS_PER_SECOND;
	}

	return 0;
}

#endif // WINDOWS_ENABLED
