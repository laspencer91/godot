/**************************************************************************/
/*  editor_external_file_drop_request.h                                   */
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

#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"

class EditorPlugin;
class EditorExternalFileDropDispatcher;

class EditorExternalFileDropRequest : public RefCounted {
	GDCLASS(EditorExternalFileDropRequest, RefCounted);

	friend class EditorExternalFileDropDispatcher;

	enum Resolution {
		RESOLUTION_CONTINUE_DEFAULT,
		RESOLUTION_FINISH_HANDLED,
		RESOLUTION_CANCEL,
	};

	PackedStringArray source_paths;
	String destination_directory;
	EditorExternalFileDropDispatcher *dispatcher = nullptr;
	bool claimed = false;
	bool resolved = false;

	void _mark_claimed();
	void _detach_dispatcher();
	void _cancel_from_dispatcher();
	void _resolve(Resolution p_resolution);

protected:
	static void _bind_methods();

public:
	PackedStringArray get_source_paths() const;
	String get_destination_directory() const;

	void continue_default();
	void finish_handled();
	void cancel();

	EditorExternalFileDropRequest(const PackedStringArray &p_source_paths, const String &p_destination_directory, EditorExternalFileDropDispatcher *p_dispatcher);
};

class EditorExternalFileDropDispatcher {
public:
	typedef int (*GetPluginCountCallback)(void *p_userdata);
	typedef EditorPlugin *(*GetPluginCallback)(void *p_userdata, int p_index);
	typedef void (*PerformDefaultCallback)(void *p_userdata, const PackedStringArray &p_source_paths, const String &p_destination_directory);

private:
	struct Drop {
		PackedStringArray source_paths;
		String request_destination_directory;
		String default_destination_directory;
	};

	struct PluginCandidate {
		ObjectID plugin_id;
		int priority = 0;
		int registration_order = 0;
	};

	struct PluginCandidateComparator {
		_FORCE_INLINE_ bool operator()(const PluginCandidate &p_left, const PluginCandidate &p_right) const {
			if (p_left.priority != p_right.priority) {
				return p_left.priority > p_right.priority;
			}
			return p_left.registration_order < p_right.registration_order;
		}
	};

	void *callback_userdata = nullptr;
	GetPluginCountCallback get_plugin_count_callback = nullptr;
	GetPluginCallback get_plugin_callback = nullptr;
	PerformDefaultCallback perform_default_callback = nullptr;

	List<Drop> queued_drops;
	Drop pending_drop;
	Ref<EditorExternalFileDropRequest> pending_request;
	ObjectID pending_owner;

	HashSet<ObjectID> plugins_being_removed;
	HashSet<ObjectID> plugins_removed_during_dispatch;
	int plugin_removal_depth = 0;
	bool processing = false;
	bool dispatching = false;
	bool shutting_down = false;

	bool _dispatch_drop(const Drop &p_drop);
	void _process_queue();
	void _request_resolved(EditorExternalFileDropRequest *p_request, EditorExternalFileDropRequest::Resolution p_resolution);

	friend class EditorExternalFileDropRequest;

public:
	void configure(void *p_userdata, GetPluginCountCallback p_get_plugin_count_callback, GetPluginCallback p_get_plugin_callback, PerformDefaultCallback p_perform_default_callback);
	void enqueue(const PackedStringArray &p_source_paths, const String &p_destination_directory);
	void enqueue(const PackedStringArray &p_source_paths, const String &p_request_destination_directory, const String &p_default_destination_directory);

	void begin_plugin_removal(EditorPlugin *p_plugin);
	void end_plugin_removal(EditorPlugin *p_plugin);
	void shutdown();

	~EditorExternalFileDropDispatcher();
};
