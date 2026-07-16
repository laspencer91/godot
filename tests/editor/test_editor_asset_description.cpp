/**************************************************************************/
/*  test_editor_asset_description.cpp                                     */
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

TEST_FORCE_LINK(test_editor_asset_description)

#ifdef TOOLS_ENABLED
#include "core/io/resource.h"
#include "editor/file_system/editor_asset_description.h"
#include "scene/main/node.h"

namespace TestEditorAssetDescription {

TEST_CASE("[Editor][EditorAssetDescription] Text scene description insert, replace, read, and remove") {
	const String source = "[gd_scene format=3]\n\n[node name=\"Root\" type=\"Node\"]\nscript = null\n\n[node name=\"Child\" type=\"Node\" parent=\".\"]\neditor_description = \"child only\"\n";
	String edited;
	REQUIRE(EditorAssetDescription::edit_text_description(source, EditorAssetDescription::TEXT_ASSET_SCENE, "Root description", edited) == OK);
	CHECK(edited.contains("[node name=\"Root\" type=\"Node\"]\neditor_description = \"Root description\"\nscript = null"));

	String read_back;
	REQUIRE(EditorAssetDescription::read_text_description(edited, EditorAssetDescription::TEXT_ASSET_SCENE, read_back) == OK);
	CHECK(read_back == "Root description");

	String replaced;
	REQUIRE(EditorAssetDescription::edit_text_description(edited, EditorAssetDescription::TEXT_ASSET_SCENE, "Replacement", replaced) == OK);
	CHECK(replaced.count("editor_description =") == 2); // Root and pre-existing child.
	REQUIRE(EditorAssetDescription::read_text_description(replaced, EditorAssetDescription::TEXT_ASSET_SCENE, read_back) == OK);
	CHECK(read_back == "Replacement");

	String removed;
	REQUIRE(EditorAssetDescription::edit_text_description(replaced, EditorAssetDescription::TEXT_ASSET_SCENE, "", removed) == OK);
	REQUIRE(EditorAssetDescription::read_text_description(removed, EditorAssetDescription::TEXT_ASSET_SCENE, read_back) == OK);
	CHECK(read_back.is_empty());
	CHECK(removed.contains("editor_description = \"child only\""));
}

TEST_CASE("[Editor][EditorAssetDescription] Text resource preserves line endings and handles an empty block") {
	const String source = "[gd_resource type=\"Resource\" format=3]\r\n\r\n[resource]\r\nresource_name = \"Example\"\r\n";
	String inserted;
	REQUIRE(EditorAssetDescription::edit_text_description(source, EditorAssetDescription::TEXT_ASSET_RESOURCE, "Resource notes", inserted) == OK);
	CHECK(inserted.contains("[resource]\r\neditor_description = \"Resource notes\"\r\nresource_name"));
	CHECK_FALSE(inserted.replace("\r\n", "").contains_char('\n'));

	String replaced;
	REQUIRE(EditorAssetDescription::edit_text_description(inserted, EditorAssetDescription::TEXT_ASSET_RESOURCE, "Updated", replaced) == OK);
	CHECK(replaced.contains("editor_description = \"Updated\"\r\n"));

	String removed;
	REQUIRE(EditorAssetDescription::edit_text_description(replaced, EditorAssetDescription::TEXT_ASSET_RESOURCE, "", removed) == OK);
	CHECK(removed == source);

	String empty_block;
	REQUIRE(EditorAssetDescription::edit_text_description("[resource]\n", EditorAssetDescription::TEXT_ASSET_RESOURCE, "At EOF", empty_block) == OK);
	CHECK(empty_block == "[resource]\neditor_description = \"At EOF\"\n");
}

TEST_CASE("[Editor][EditorAssetDescription] Godot string escaping round-trips exactly") {
	const String description = "Quote: \" Backslash: \\ Newline:\nTab:\t Snowman: ☃";
	const String source = "[gd_resource type=\"Resource\" format=3]\n\n[resource]\n";
	String edited;
	REQUIRE(EditorAssetDescription::edit_text_description(source, EditorAssetDescription::TEXT_ASSET_RESOURCE, description, edited) == OK);
	CHECK(edited.contains("\\\""));
	CHECK(edited.contains("\\\\"));
	CHECK(edited.contains("\\n"));
	CHECK(edited.contains("\\t"));

	String read_back;
	REQUIRE(EditorAssetDescription::read_text_description(edited, EditorAssetDescription::TEXT_ASSET_RESOURCE, read_back) == OK);
	CHECK(read_back == description);
}

TEST_CASE("[Editor][EditorAssetDescription] Malformed or ambiguous blocks are rejected") {
	String result;
	CHECK(EditorAssetDescription::edit_text_description("[gd_scene format=3]\n", EditorAssetDescription::TEXT_ASSET_SCENE, "No root", result) == ERR_FILE_CORRUPT);
	CHECK(EditorAssetDescription::read_text_description("[resource]\neditor_description = \"one\"\neditor_description = \"two\"\n", EditorAssetDescription::TEXT_ASSET_RESOURCE, result) == ERR_FILE_CORRUPT);
	CHECK(EditorAssetDescription::read_text_description("[resource]\neditor_description = not_a_string\n", EditorAssetDescription::TEXT_ASSET_RESOURCE, result) == ERR_PARSE_ERROR);
}

TEST_CASE("[Editor][EditorAssetDescription] Resource property metadata mirrors Node") {
	Ref<Resource> resource;
	resource.instantiate();
	Node *node = memnew(Node);
	List<PropertyInfo> resource_properties;
	List<PropertyInfo> node_properties;
	resource->get_property_list(&resource_properties);
	node->get_property_list(&node_properties);

	PropertyInfo resource_description;
	PropertyInfo node_description;
	bool found_resource_description = false;
	bool found_node_description = false;
	for (const PropertyInfo &property : resource_properties) {
		if (property.name == "editor_description") {
			resource_description = property;
			found_resource_description = true;
			break;
		}
	}
	for (const PropertyInfo &property : node_properties) {
		if (property.name == "editor_description") {
			node_description = property;
			found_node_description = true;
			break;
		}
	}

	REQUIRE(found_resource_description);
	REQUIRE(found_node_description);
	CHECK(resource_description.type == Variant::STRING);
	CHECK(resource_description.hint == PROPERTY_HINT_MULTILINE_TEXT);
	CHECK(resource_description.usage == node_description.usage);
	CHECK(resource_description.usage == PROPERTY_USAGE_DEFAULT);

	memdelete(node);
}

TEST_CASE("[Editor][EditorAssetDescription] Binary resources are an explicit V1 limitation") {
	CHECK(EditorAssetDescription::get_asset_kind("res://example.res") == EditorAssetDescription::ASSET_KIND_RESOURCE_BINARY);
	CHECK_FALSE(EditorAssetDescription::is_supported("res://example.res"));
	String error_message;
	CHECK(EditorAssetDescription::write_description("res://example.res", "Not written", error_message) == ERR_UNAVAILABLE);
	CHECK(error_message.contains("Binary .res"));
}

} // namespace TestEditorAssetDescription
#endif // TOOLS_ENABLED
