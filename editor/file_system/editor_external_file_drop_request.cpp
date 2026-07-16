/**************************************************************************/
/*  editor_external_file_drop_request.cpp                                 */
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

#include "editor_external_file_drop_request.h"

#include "core/object/class_db.h"
#include "core/os/thread.h"
#include "editor/plugins/editor_plugin.h"

void EditorExternalFileDropRequest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_source_paths"), &EditorExternalFileDropRequest::get_source_paths);
	ClassDB::bind_method(D_METHOD("get_destination_directory"), &EditorExternalFileDropRequest::get_destination_directory);
	ClassDB::bind_method(D_METHOD("continue_default"), &EditorExternalFileDropRequest::continue_default);
	ClassDB::bind_method(D_METHOD("finish_handled"), &EditorExternalFileDropRequest::finish_handled);
	ClassDB::bind_method(D_METHOD("cancel"), &EditorExternalFileDropRequest::cancel);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "source_paths"), "", "get_source_paths");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "destination_directory"), "", "get_destination_directory");
}

PackedStringArray EditorExternalFileDropRequest::get_source_paths() const {
	return source_paths;
}

String EditorExternalFileDropRequest::get_destination_directory() const {
	return destination_directory;
}

void EditorExternalFileDropRequest::_mark_claimed() {
	ERR_FAIL_COND(claimed);
	ERR_FAIL_COND(resolved);
	ERR_FAIL_NULL(dispatcher);
	claimed = true;
}

void EditorExternalFileDropRequest::_detach_dispatcher() {
	dispatcher = nullptr;
}

void EditorExternalFileDropRequest::_cancel_from_dispatcher() {
	_resolve(RESOLUTION_CANCEL);
}

void EditorExternalFileDropRequest::_resolve(Resolution p_resolution) {
	ERR_FAIL_COND_MSG(resolved, "This external file drop request has already been resolved.");
	ERR_FAIL_COND_MSG(!claimed, "An external file drop request can only be resolved after an EditorPlugin claims it.");
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "External file drop requests must be resolved on the editor main thread. Use call_deferred() from a worker thread.");
	ERR_FAIL_NULL(dispatcher);

	resolved = true;
	dispatcher->_request_resolved(this, p_resolution);
}

void EditorExternalFileDropRequest::continue_default() {
	_resolve(RESOLUTION_CONTINUE_DEFAULT);
}

void EditorExternalFileDropRequest::finish_handled() {
	_resolve(RESOLUTION_FINISH_HANDLED);
}

void EditorExternalFileDropRequest::cancel() {
	_resolve(RESOLUTION_CANCEL);
}

EditorExternalFileDropRequest::EditorExternalFileDropRequest(const PackedStringArray &p_source_paths, const String &p_destination_directory, EditorExternalFileDropDispatcher *p_dispatcher) :
		source_paths(p_source_paths),
		destination_directory(p_destination_directory),
		dispatcher(p_dispatcher) {
}

void EditorExternalFileDropDispatcher::configure(void *p_userdata, GetPluginCountCallback p_get_plugin_count_callback, GetPluginCallback p_get_plugin_callback, PerformDefaultCallback p_perform_default_callback) {
	ERR_FAIL_COND(processing);
	ERR_FAIL_COND(pending_request.is_valid());
	ERR_FAIL_NULL(p_userdata);
	ERR_FAIL_NULL(p_get_plugin_count_callback);
	ERR_FAIL_NULL(p_get_plugin_callback);
	ERR_FAIL_NULL(p_perform_default_callback);

	callback_userdata = p_userdata;
	get_plugin_count_callback = p_get_plugin_count_callback;
	get_plugin_callback = p_get_plugin_callback;
	perform_default_callback = p_perform_default_callback;
}

void EditorExternalFileDropDispatcher::enqueue(const PackedStringArray &p_source_paths, const String &p_destination_directory) {
	enqueue(p_source_paths, p_destination_directory, p_destination_directory);
}

void EditorExternalFileDropDispatcher::enqueue(const PackedStringArray &p_source_paths, const String &p_request_destination_directory, const String &p_default_destination_directory) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "External file drops must be dispatched on the editor main thread.");
	ERR_FAIL_COND_MSG(shutting_down, "External file drops cannot be queued while the editor is shutting down.");
	ERR_FAIL_NULL(callback_userdata);
	ERR_FAIL_NULL(get_plugin_count_callback);
	ERR_FAIL_NULL(get_plugin_callback);
	ERR_FAIL_NULL(perform_default_callback);

	Drop drop;
	drop.source_paths = p_source_paths;
	drop.request_destination_directory = p_request_destination_directory;
	drop.default_destination_directory = p_default_destination_directory;
	queued_drops.push_back(drop);
	_process_queue();
}

bool EditorExternalFileDropDispatcher::_dispatch_drop(const Drop &p_drop) {
	Vector<PluginCandidate> candidates;
	const int plugin_count = get_plugin_count_callback(callback_userdata);
	candidates.reserve(plugin_count);
	for (int i = 0; i < plugin_count; i++) {
		EditorPlugin *plugin = get_plugin_callback(callback_userdata, i);
		if (!plugin || plugins_being_removed.has(plugin->get_instance_id())) {
			continue;
		}
		PluginCandidate candidate;
		candidate.plugin_id = plugin->get_instance_id();
		candidate.registration_order = i;
		candidates.push_back(candidate);
	}

	dispatching = true;
	plugins_removed_during_dispatch.clear();
	for (PluginCandidate &candidate : candidates) {
		EditorPlugin *plugin = Object::cast_to<EditorPlugin>(ObjectDB::get_instance(candidate.plugin_id));
		if (!plugin || plugins_being_removed.has(candidate.plugin_id) || plugins_removed_during_dispatch.has(candidate.plugin_id)) {
			continue;
		}
		candidate.priority = plugin->get_external_file_drop_priority();
		if (shutting_down) {
			break;
		}
	}
	candidates.sort_custom<PluginCandidateComparator>();

	Ref<EditorExternalFileDropRequest> request = memnew(EditorExternalFileDropRequest(p_drop.source_paths, p_drop.request_destination_directory, this));
	bool claimed = false;
	for (const PluginCandidate &candidate : candidates) {
		if (shutting_down) {
			break;
		}
		EditorPlugin *plugin = Object::cast_to<EditorPlugin>(ObjectDB::get_instance(candidate.plugin_id));
		if (!plugin || plugins_being_removed.has(candidate.plugin_id) || plugins_removed_during_dispatch.has(candidate.plugin_id)) {
			continue;
		}

		const EditorPlugin::ExternalFileDropClaim claim = plugin->intercept_external_file_drop(request);
		if (claim == EditorPlugin::EXTERNAL_FILE_DROP_PASS) {
			continue;
		}
		if (claim != EditorPlugin::EXTERNAL_FILE_DROP_CLAIM) {
			ERR_PRINT(vformat("EditorPlugin '%s' returned an invalid external file drop claim value (%d).", plugin->get_class(), int(claim)));
			continue;
		}

		claimed = true;
		pending_drop = p_drop;
		pending_request = request;
		pending_owner = candidate.plugin_id;
		request->_mark_claimed();
		if (shutting_down || plugins_removed_during_dispatch.has(candidate.plugin_id)) {
			request->_cancel_from_dispatcher();
		}
		break;
	}
	dispatching = false;
	plugins_removed_during_dispatch.clear();

	if (!claimed) {
		request->_detach_dispatcher();
	}
	return claimed;
}

void EditorExternalFileDropDispatcher::_process_queue() {
	if (processing || plugin_removal_depth > 0 || pending_request.is_valid() || shutting_down) {
		return;
	}

	processing = true;
	while (!queued_drops.is_empty() && pending_request.is_null() && !shutting_down && plugin_removal_depth == 0) {
		const Drop drop = queued_drops.front()->get();
		queued_drops.pop_front();
		if (!_dispatch_drop(drop) && !shutting_down) {
			perform_default_callback(callback_userdata, drop.source_paths, drop.default_destination_directory);
		}
	}
	processing = false;
}

void EditorExternalFileDropDispatcher::_request_resolved(EditorExternalFileDropRequest *p_request, EditorExternalFileDropRequest::Resolution p_resolution) {
	ERR_FAIL_COND(pending_request.ptr() != p_request);

	const Drop resolved_drop = pending_drop;
	pending_request->_detach_dispatcher();
	pending_request.unref();
	pending_owner = ObjectID();

	if (p_resolution == EditorExternalFileDropRequest::RESOLUTION_CONTINUE_DEFAULT && !shutting_down) {
		perform_default_callback(callback_userdata, resolved_drop.source_paths, resolved_drop.default_destination_directory);
	}
	_process_queue();
}

void EditorExternalFileDropDispatcher::begin_plugin_removal(EditorPlugin *p_plugin) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "EditorPlugins must be removed on the editor main thread.");
	ERR_FAIL_NULL(p_plugin);

	const ObjectID plugin_id = p_plugin->get_instance_id();
	plugin_removal_depth++;
	plugins_being_removed.insert(plugin_id);
	if (dispatching) {
		plugins_removed_during_dispatch.insert(plugin_id);
	}
	if (pending_request.is_valid() && pending_owner == plugin_id) {
		pending_request->_cancel_from_dispatcher();
	}
}

void EditorExternalFileDropDispatcher::end_plugin_removal(EditorPlugin *p_plugin) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "EditorPlugins must be removed on the editor main thread.");
	ERR_FAIL_NULL(p_plugin);
	ERR_FAIL_COND(plugin_removal_depth <= 0);

	plugins_being_removed.erase(p_plugin->get_instance_id());
	plugin_removal_depth--;
	_process_queue();
}

void EditorExternalFileDropDispatcher::shutdown() {
	if (shutting_down) {
		return;
	}
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "The external file drop dispatcher must shut down on the editor main thread.");

	shutting_down = true;
	queued_drops.clear();
	if (pending_request.is_valid()) {
		pending_request->_cancel_from_dispatcher();
	}
}

EditorExternalFileDropDispatcher::~EditorExternalFileDropDispatcher() {
	shutdown();
}
