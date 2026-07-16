/**************************************************************************/
/*  test_editor_simple_markdown.cpp                                       */
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

TEST_FORCE_LINK(test_editor_simple_markdown)

#ifdef TOOLS_ENABLED
#include "editor/gui/editor_simple_markdown.h"

namespace TestEditorSimpleMarkdown {

TEST_CASE("[Editor][EditorSimpleMarkdown] Supported block and inline constructs") {
	const String markdown = "# Heading one\n## Heading two\n### Heading three\n\nA paragraph with **bold**, *italic*, `inline code`, and [a link](https://godotengine.org/docs).\n\n- First item\n* Second item\n\n```gdscript\nvar unsafe = \"[b]literal[/b]\"\n```";
	const EditorSimpleMarkdown::Document document = EditorSimpleMarkdown::parse(markdown);

	REQUIRE_FALSE(document.literal_fallback);
	REQUIRE(document.blocks.size() == 7);
	CHECK(document.blocks[0].type == EditorSimpleMarkdown::BLOCK_HEADING_1);
	CHECK(document.blocks[1].type == EditorSimpleMarkdown::BLOCK_HEADING_2);
	CHECK(document.blocks[2].type == EditorSimpleMarkdown::BLOCK_HEADING_3);
	CHECK(document.blocks[3].type == EditorSimpleMarkdown::BLOCK_PARAGRAPH);
	CHECK(document.blocks[4].type == EditorSimpleMarkdown::BLOCK_UNORDERED_LIST_ITEM);
	CHECK(document.blocks[5].type == EditorSimpleMarkdown::BLOCK_UNORDERED_LIST_ITEM);
	CHECK(document.blocks[6].type == EditorSimpleMarkdown::BLOCK_CODE);
	CHECK(document.blocks[6].code == "var unsafe = \"[b]literal[/b]\"");

	bool found_bold = false;
	bool found_italic = false;
	bool found_code = false;
	bool found_link = false;
	for (const EditorSimpleMarkdown::Inline &span : document.blocks[3].inlines) {
		found_bold |= span.text == "bold" && span.bold;
		found_italic |= span.text == "italic" && span.italic;
		found_code |= span.text == "inline code" && span.code;
		found_link |= span.text == "a link" && span.link == "https://godotengine.org/docs";
	}
	CHECK(found_bold);
	CHECK(found_italic);
	CHECK(found_code);
	CHECK(found_link);
}

TEST_CASE("[Editor][EditorSimpleMarkdown] User markup stays literal by default") {
	const String markup = "[b]BBCode stays text[/b] <img src=bad> <script>alert(1)</script> | not | a | table |";
	const EditorSimpleMarkdown::Document document = EditorSimpleMarkdown::parse(markup);

	REQUIRE_FALSE(document.literal_fallback);
	REQUIRE(document.blocks.size() == 1);
	REQUIRE(document.blocks[0].inlines.size() == 1);
	CHECK(document.blocks[0].inlines[0].text == markup);
	CHECK(document.blocks[0].inlines[0].link.is_empty());

	const String image = "![alt text](https://example.com/image.png)";
	const EditorSimpleMarkdown::Document image_document = EditorSimpleMarkdown::parse(image);
	REQUIRE_FALSE(image_document.literal_fallback);
	REQUIRE(image_document.blocks[0].inlines.size() == 1);
	CHECK(image_document.blocks[0].inlines[0].text == image);
	CHECK(image_document.blocks[0].inlines[0].link.is_empty());
}

TEST_CASE("[Editor][EditorSimpleMarkdown] Malformed input falls back to its exact literal text") {
	const Vector<String> malformed_inputs = {
		"Text with **unclosed bold",
		"Text with *unclosed italic",
		"Text with `unclosed code",
		"Text with [an incomplete](https://example.com link",
		"```cpp\nint value = 1;",
	};
	for (const String &input : malformed_inputs) {
		const EditorSimpleMarkdown::Document document = EditorSimpleMarkdown::parse(input);
		CHECK(document.literal_fallback);
		CHECK(document.blocks.is_empty());
		CHECK(document.literal_text == input);
	}
}

TEST_CASE("[Editor][EditorSimpleMarkdown] Only HTTP and HTTPS links become metadata") {
	CHECK(EditorSimpleMarkdown::is_safe_http_link("http://example.com"));
	CHECK(EditorSimpleMarkdown::is_safe_http_link("HTTPS://example.com/path?q=1#fragment"));
	CHECK_FALSE(EditorSimpleMarkdown::is_safe_http_link("https://"));
	CHECK_FALSE(EditorSimpleMarkdown::is_safe_http_link("https://example.com/bad path"));
	CHECK_FALSE(EditorSimpleMarkdown::is_safe_http_link("mailto:person@example.com"));
	CHECK_FALSE(EditorSimpleMarkdown::is_safe_http_link("javascript:alert(1)"));
	CHECK_FALSE(EditorSimpleMarkdown::is_safe_http_link("res://icon.svg"));

	const String markdown = "[safe](http://example.com) [mail](mailto:person@example.com) [script](javascript:alert(1))";
	const EditorSimpleMarkdown::Document document = EditorSimpleMarkdown::parse(markdown);
	REQUIRE_FALSE(document.literal_fallback);
	int linked_span_count = 0;
	for (const EditorSimpleMarkdown::Inline &span : document.blocks[0].inlines) {
		if (!span.link.is_empty()) {
			linked_span_count++;
			CHECK(span.link == "http://example.com");
		}
	}
	CHECK(linked_span_count == 1);
}

} // namespace TestEditorSimpleMarkdown
#endif // TOOLS_ENABLED
