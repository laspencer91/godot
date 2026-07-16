/**************************************************************************/
/*  editor_simple_markdown.h                                              */
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
#include "core/templates/vector.h"

class RichTextLabel;

// A deliberately small, editor-only Markdown subset. Rendering uses RichTextLabel's
// push/pop API so user text is never interpreted as BBCode.
class EditorSimpleMarkdown {
public:
	enum BlockType {
		BLOCK_PARAGRAPH,
		BLOCK_HEADING_1,
		BLOCK_HEADING_2,
		BLOCK_HEADING_3,
		BLOCK_UNORDERED_LIST_ITEM,
		BLOCK_CODE,
	};

	struct Inline {
		String text;
		String link;
		bool bold = false;
		bool italic = false;
		bool code = false;
	};

	struct Block {
		BlockType type = BLOCK_PARAGRAPH;
		Vector<Inline> inlines;
		String code;
	};

	struct Document {
		Vector<Block> blocks;
		String literal_text;
		bool literal_fallback = false;
	};

	static Document parse(const String &p_markdown);
	static bool is_safe_http_link(const String &p_url);
	static void render(RichTextLabel *p_label, const String &p_markdown, int p_base_font_size);

private:
	static bool _parse_inline(const String &p_text, int p_from, int p_to, bool p_bold, bool p_italic, Vector<Inline> &r_inlines);
	static void _append_inline(Vector<Inline> &r_inlines, const String &p_text, bool p_bold, bool p_italic, bool p_code = false, const String &p_link = String());
};
