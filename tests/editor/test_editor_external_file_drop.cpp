/**************************************************************************/
/*  test_editor_external_file_drop.cpp                                    */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_external_file_drop)

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_external_file_drop_request.h"
#include "editor/plugins/editor_plugin.h"

namespace TestEditorExternalFileDrop {

struct DefaultDrop {
	PackedStringArray source_paths;
	String destination_directory;
};

struct DropHost {
	Vector<EditorPlugin *> plugins;
	Vector<DefaultDrop> default_drops;

	static int get_plugin_count(void *p_userdata) {
		return static_cast<DropHost *>(p_userdata)->plugins.size();
	}

	static EditorPlugin *get_plugin(void *p_userdata, int p_index) {
		return static_cast<DropHost *>(p_userdata)->plugins[p_index];
	}

	static void perform_default(void *p_userdata, const PackedStringArray &p_source_paths, const String &p_destination_directory) {
		DropHost *host = static_cast<DropHost *>(p_userdata);
		DefaultDrop drop;
		drop.source_paths = p_source_paths;
		drop.destination_directory = p_destination_directory;
		host->default_drops.push_back(drop);
	}
};

class MockExternalFileDropPlugin : public EditorPlugin {
	int priority = 0;
	ExternalFileDropClaim claim = EXTERNAL_FILE_DROP_PASS;
	String label;
	Vector<String> *call_log = nullptr;

public:
	Vector<Ref<EditorExternalFileDropRequest>> requests;

	virtual int get_external_file_drop_priority() const override {
		return priority;
	}

	virtual ExternalFileDropClaim intercept_external_file_drop(const Ref<EditorExternalFileDropRequest> &p_request) override {
		requests.push_back(p_request);
		if (call_log) {
			call_log->push_back(label);
		}
		return claim;
	}

	MockExternalFileDropPlugin(int p_priority, ExternalFileDropClaim p_claim, const String &p_label = String(), Vector<String> *p_call_log = nullptr) :
			priority(p_priority),
			claim(p_claim),
			label(p_label),
			call_log(p_call_log) {
	}
};

struct DropHarness {
	DropHost host;
	EditorExternalFileDropDispatcher dispatcher;
	Vector<MockExternalFileDropPlugin *> owned_plugins;

	MockExternalFileDropPlugin *add_plugin(int p_priority, EditorPlugin::ExternalFileDropClaim p_claim, const String &p_label = String(), Vector<String> *p_call_log = nullptr) {
		MockExternalFileDropPlugin *plugin = memnew(MockExternalFileDropPlugin(p_priority, p_claim, p_label, p_call_log));
		host.plugins.push_back(plugin);
		owned_plugins.push_back(plugin);
		return plugin;
	}

	void remove_plugin_from_host(EditorPlugin *p_plugin) {
		host.plugins.erase(p_plugin);
	}

	DropHarness() {
		dispatcher.configure(&host, &DropHost::get_plugin_count, &DropHost::get_plugin, &DropHost::perform_default);
	}

	~DropHarness() {
		dispatcher.shutdown();
		for (MockExternalFileDropPlugin *plugin : owned_plugins) {
			memdelete(plugin);
		}
	}
};

TEST_CASE("[Editor][EditorExternalFileDrop] Request payload and default continuation are one-shot") {
	DropHarness harness;
	MockExternalFileDropPlugin *owner = harness.add_plugin(10, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM);
	const PackedStringArray sources({ "C:/incoming/albedo.png", "D:/incoming/normal.png" });
	harness.dispatcher.enqueue(sources, "res://art/materials/", "res://art/materials");

	REQUIRE(owner->requests.size() == 1);
	Ref<EditorExternalFileDropRequest> request = owner->requests[0];
	CHECK(request->get_source_paths() == sources);
	CHECK(request->get_destination_directory() == "res://art/materials/");
	CHECK(harness.host.default_drops.is_empty());

	request->continue_default();
	REQUIRE(harness.host.default_drops.size() == 1);
	CHECK(harness.host.default_drops[0].source_paths == sources);
	CHECK(harness.host.default_drops[0].destination_directory == "res://art/materials");

	ERR_PRINT_OFF;
	request->finish_handled();
	request->cancel();
	ERR_PRINT_ON;
	CHECK(harness.host.default_drops.size() == 1);
}

TEST_CASE("[Editor][EditorExternalFileDrop] Handled and canceled requests suppress the default path") {
	DropHarness harness;
	MockExternalFileDropPlugin *owner = harness.add_plugin(0, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM);

	SUBCASE("finish handled") {
		harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/handled.glb" }), "res://models/");
		REQUIRE(owner->requests.size() == 1);
		owner->requests[0]->finish_handled();
		CHECK(harness.host.default_drops.is_empty());
	}

	SUBCASE("cancel") {
		harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/canceled.glb" }), "res://models/");
		REQUIRE(owner->requests.size() == 1);
		owner->requests[0]->cancel();
		CHECK(harness.host.default_drops.is_empty());
	}
}

TEST_CASE("[Editor][EditorExternalFileDrop] No handler and all-pass dispatch run default exactly once") {
	SUBCASE("no handlers") {
		DropHarness harness;
		harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/plain.txt" }), "res://data/");
		REQUIRE(harness.host.default_drops.size() == 1);
		CHECK(harness.host.default_drops[0].source_paths[0] == "C:/incoming/plain.txt");
	}

	SUBCASE("all handlers pass") {
		DropHarness harness;
		MockExternalFileDropPlugin *first = harness.add_plugin(5, EditorPlugin::EXTERNAL_FILE_DROP_PASS);
		MockExternalFileDropPlugin *second = harness.add_plugin(-2, EditorPlugin::EXTERNAL_FILE_DROP_PASS);
		harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/plain.txt" }), "res://data/");
		CHECK(first->requests.size() == 1);
		CHECK(second->requests.size() == 1);
		CHECK(first->requests[0].ptr() == second->requests[0].ptr());
		CHECK(harness.host.default_drops.size() == 1);
	}
}

TEST_CASE("[Editor][EditorExternalFileDrop] Priority, registration-order ties, and first claim determine ownership") {
	DropHarness harness;
	Vector<String> call_log;
	MockExternalFileDropPlugin *tie_first = harness.add_plugin(10, EditorPlugin::EXTERNAL_FILE_DROP_PASS, "tie_first", &call_log);
	MockExternalFileDropPlugin *low = harness.add_plugin(-5, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM, "low", &call_log);
	MockExternalFileDropPlugin *high = harness.add_plugin(20, EditorPlugin::EXTERNAL_FILE_DROP_PASS, "high", &call_log);
	MockExternalFileDropPlugin *tie_second = harness.add_plugin(10, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM, "tie_second", &call_log);

	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/ordered.png" }), "res://textures/");
	REQUIRE(call_log.size() == 3);
	CHECK(call_log[0] == "high");
	CHECK(call_log[1] == "tie_first");
	CHECK(call_log[2] == "tie_second");
	CHECK(low->requests.is_empty());
	CHECK(high->requests[0].ptr() == tie_first->requests[0].ptr());
	CHECK(tie_first->requests[0].ptr() == tie_second->requests[0].ptr());
	CHECK(harness.host.default_drops.is_empty());

	tie_second->requests[0]->finish_handled();
	CHECK(harness.host.default_drops.is_empty());
}

TEST_CASE("[Editor][EditorExternalFileDrop] Drops queue FIFO until the pending claim resolves") {
	DropHarness harness;
	MockExternalFileDropPlugin *owner = harness.add_plugin(0, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM);

	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/first.png" }), "res://first/");
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/second.png" }), "res://second/");
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/third.png" }), "res://third/");
	REQUIRE(owner->requests.size() == 1);

	owner->requests[0]->finish_handled();
	REQUIRE(owner->requests.size() == 2);
	CHECK(owner->requests[1]->get_source_paths()[0] == "C:/incoming/second.png");

	owner->requests[1]->cancel();
	REQUIRE(owner->requests.size() == 3);
	CHECK(owner->requests[2]->get_source_paths()[0] == "C:/incoming/third.png");

	owner->requests[2]->continue_default();
	REQUIRE(harness.host.default_drops.size() == 1);
	CHECK(harness.host.default_drops[0].source_paths[0] == "C:/incoming/third.png");
}

TEST_CASE("[Editor][EditorExternalFileDrop] Removing the owner cancels pending and resumes after removal") {
	DropHarness harness;
	MockExternalFileDropPlugin *owner = harness.add_plugin(10, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM);
	MockExternalFileDropPlugin *fallback = harness.add_plugin(0, EditorPlugin::EXTERNAL_FILE_DROP_PASS);
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/owned.png" }), "res://owned/");
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/queued.png" }), "res://queued/");
	REQUIRE(owner->requests.size() == 1);
	Ref<EditorExternalFileDropRequest> canceled_request = owner->requests[0];

	harness.dispatcher.begin_plugin_removal(owner);
	CHECK(fallback->requests.is_empty());
	CHECK(harness.host.default_drops.is_empty());
	harness.remove_plugin_from_host(owner);
	harness.dispatcher.end_plugin_removal(owner);

	REQUIRE(fallback->requests.size() == 1);
	CHECK(fallback->requests[0]->get_source_paths()[0] == "C:/incoming/queued.png");
	REQUIRE(harness.host.default_drops.size() == 1);
	CHECK(harness.host.default_drops[0].source_paths[0] == "C:/incoming/queued.png");

	ERR_PRINT_OFF;
	canceled_request->continue_default();
	ERR_PRINT_ON;
	CHECK(harness.host.default_drops.size() == 1);
}

TEST_CASE("[Editor][EditorExternalFileDrop] Shutdown cancels pending and discards queued drops") {
	DropHarness harness;
	MockExternalFileDropPlugin *owner = harness.add_plugin(0, EditorPlugin::EXTERNAL_FILE_DROP_CLAIM);
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/owned.png" }), "res://owned/");
	harness.dispatcher.enqueue(PackedStringArray({ "C:/incoming/queued.png" }), "res://queued/");
	REQUIRE(owner->requests.size() == 1);
	Ref<EditorExternalFileDropRequest> canceled_request = owner->requests[0];

	harness.dispatcher.shutdown();
	CHECK(owner->requests.size() == 1);
	CHECK(harness.host.default_drops.is_empty());

	ERR_PRINT_OFF;
	canceled_request->continue_default();
	ERR_PRINT_ON;
	CHECK(harness.host.default_drops.is_empty());
}

} // namespace TestEditorExternalFileDrop
#endif // TOOLS_ENABLED
