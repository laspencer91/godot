/**************************************************************************/
/*  editor_simple_markdown.cpp                                            */
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

#include "editor_simple_markdown.h"

#include "scene/gui/rich_text_label.h"

void EditorSimpleMarkdown::_append_inline(Vector<Inline> &r_inlines, const String &p_text, bool p_bold, bool p_italic, bool p_code, const String &p_link) {
	if (p_text.is_empty()) {
		return;
	}
	if (!r_inlines.is_empty()) {
		Inline &last = r_inlines.write[r_inlines.size() - 1];
		if (last.bold == p_bold && last.italic == p_italic && last.code == p_code && last.link == p_link) {
			last.text += p_text;
			return;
		}
	}

	Inline span;
	span.text = p_text;
	span.link = p_link;
	span.bold = p_bold;
	span.italic = p_italic;
	span.code = p_code;
	r_inlines.push_back(span);
}

bool EditorSimpleMarkdown::is_safe_http_link(const String &p_url) {
	const String lower_url = p_url.to_lower();
	int authority_start = -1;
	if (lower_url.begins_with("https://")) {
		authority_start = 8;
	} else if (lower_url.begins_with("http://")) {
		authority_start = 7;
	} else {
		return false;
	}

	for (int i = 0; i < p_url.length(); i++) {
		if (p_url[i] <= 0x20) {
			return false;
		}
	}

	int authority_end = p_url.length();
	for (int i = authority_start; i < p_url.length(); i++) {
		if (p_url[i] == '/' || p_url[i] == '?' || p_url[i] == '#') {
			authority_end = i;
			break;
		}
	}
	return authority_end > authority_start;
}

bool EditorSimpleMarkdown::_parse_inline(const String &p_text, int p_from, int p_to, bool p_bold, bool p_italic, Vector<Inline> &r_inlines) {
	String plain;
	auto flush_plain = [&]() {
		_append_inline(r_inlines, plain, p_bold, p_italic);
		plain.clear();
	};

	int position = p_from;
	while (position < p_to) {
		const char32_t character = p_text[position];
		if (character == '\\' && position + 1 < p_to) {
			const char32_t escaped = p_text[position + 1];
			if (escaped == '\\' || escaped == '*' || escaped == '`' || escaped == '[' || escaped == ']' || escaped == '(' || escaped == ')') {
				plain += escaped;
				position += 2;
				continue;
			}
		}

		if (character == '`') {
			const int closing = p_text.find_char('`', position + 1);
			if (closing < 0 || closing >= p_to || closing == position + 1) {
				return false;
			}
			flush_plain();
			_append_inline(r_inlines, p_text.substr(position + 1, closing - position - 1), p_bold, p_italic, true);
			position = closing + 1;
			continue;
		}

		if (character == '*' && position + 1 < p_to && p_text[position + 1] == '*') {
			const int closing = p_text.find("**", position + 2);
			if (closing < 0 || closing >= p_to || closing == position + 2) {
				return false;
			}
			flush_plain();
			if (!_parse_inline(p_text, position + 2, closing, true, p_italic, r_inlines)) {
				return false;
			}
			position = closing + 2;
			continue;
		}

		if (character == '*') {
			const int closing = p_text.find_char('*', position + 1);
			if (closing < 0 || closing >= p_to || closing == position + 1) {
				return false;
			}
			flush_plain();
			if (!_parse_inline(p_text, position + 1, closing, p_bold, true, r_inlines)) {
				return false;
			}
			position = closing + 1;
			continue;
		}

		if (character == '!' && position + 1 < p_to && p_text[position + 1] == '[') {
			const int label_end = p_text.find("](", position + 2);
			const int link_end = label_end < 0 ? -1 : p_text.find_char(')', label_end + 2);
			if (label_end >= 0 && link_end >= 0 && link_end < p_to) {
				plain += p_text.substr(position, link_end - position + 1);
				position = link_end + 1;
				continue;
			}
		}

		if (character == '[') {
			const int label_end = p_text.find("](", position + 1);
			if (label_end < 0 || label_end >= p_to) {
				plain += character;
				position++;
				continue;
			}
			const int link_end = p_text.find_char(')', label_end + 2);
			if (link_end < 0 || link_end >= p_to || label_end == position + 1) {
				return false;
			}
			const String full_link = p_text.substr(position, link_end - position + 1);
			const String label = p_text.substr(position + 1, label_end - position - 1);
			const String url = p_text.substr(label_end + 2, link_end - label_end - 2);
			if (is_safe_http_link(url)) {
				flush_plain();
				_append_inline(r_inlines, label, p_bold, p_italic, false, url);
			} else {
				plain += full_link;
			}
			position = link_end + 1;
			continue;
		}

		plain += character;
		position++;
	}
	flush_plain();
	return true;
}

EditorSimpleMarkdown::Document EditorSimpleMarkdown::parse(const String &p_markdown) {
	Document document;
	Vector<String> lines = p_markdown.split("\n", true);
	String paragraph;

	auto fail_literal = [&]() {
		document.blocks.clear();
		document.literal_text = p_markdown;
		document.literal_fallback = true;
	};
	auto append_inline_block = [&](BlockType p_type, const String &p_text) -> bool {
		Block block;
		block.type = p_type;
		if (!_parse_inline(p_text, 0, p_text.length(), false, false, block.inlines)) {
			return false;
		}
		document.blocks.push_back(block);
		return true;
	};
	auto flush_paragraph = [&]() -> bool {
		if (paragraph.is_empty()) {
			return true;
		}
		const bool success = append_inline_block(BLOCK_PARAGRAPH, paragraph);
		paragraph.clear();
		return success;
	};

	for (int line_index = 0; line_index < lines.size(); line_index++) {
		String line = lines[line_index];
		if (line.ends_with("\r")) {
			line = line.left(-1);
		}

		if (line.begins_with("```")) {
			if (!flush_paragraph()) {
				fail_literal();
				return document;
			}
			String code;
			bool closed = false;
			for (line_index++; line_index < lines.size(); line_index++) {
				String code_line = lines[line_index];
				if (code_line.ends_with("\r")) {
					code_line = code_line.left(-1);
				}
				if (code_line.strip_edges() == "```") {
					closed = true;
					break;
				}
				if (!code.is_empty()) {
					code += "\n";
				}
				code += code_line;
			}
			if (!closed) {
				fail_literal();
				return document;
			}
			Block block;
			block.type = BLOCK_CODE;
			block.code = code;
			document.blocks.push_back(block);
			continue;
		}

		if (line.strip_edges().is_empty()) {
			if (!flush_paragraph()) {
				fail_literal();
				return document;
			}
			continue;
		}

		BlockType line_type = BLOCK_PARAGRAPH;
		String line_content;
		if (line.begins_with("### ")) {
			line_type = BLOCK_HEADING_3;
			line_content = line.substr(4);
		} else if (line.begins_with("## ")) {
			line_type = BLOCK_HEADING_2;
			line_content = line.substr(3);
		} else if (line.begins_with("# ")) {
			line_type = BLOCK_HEADING_1;
			line_content = line.substr(2);
		} else if (line.begins_with("- ") || line.begins_with("* ")) {
			line_type = BLOCK_UNORDERED_LIST_ITEM;
			line_content = line.substr(2);
		}

		if (line_type != BLOCK_PARAGRAPH) {
			if (!flush_paragraph() || line_content.is_empty() || !append_inline_block(line_type, line_content)) {
				fail_literal();
				return document;
			}
		} else {
			if (!paragraph.is_empty()) {
				paragraph += " ";
			}
			paragraph += line;
		}
	}

	if (!flush_paragraph()) {
		fail_literal();
	}
	return document;
}

void EditorSimpleMarkdown::render(RichTextLabel *p_label, const String &p_markdown, int p_base_font_size) {
	ERR_FAIL_NULL(p_label);
	p_label->clear();
	const Document document = parse(p_markdown);
	if (document.literal_fallback) {
		p_label->add_text(document.literal_text);
		return;
	}

	for (int block_index = 0; block_index < document.blocks.size(); block_index++) {
		const Block &block = document.blocks[block_index];
		int block_pushes = 0;
		if (block.type == BLOCK_HEADING_1 || block.type == BLOCK_HEADING_2 || block.type == BLOCK_HEADING_3) {
			if (p_base_font_size > 0) {
				const int size_offset = block.type == BLOCK_HEADING_1 ? 8 : (block.type == BLOCK_HEADING_2 ? 5 : 2);
				p_label->push_font_size(p_base_font_size + size_offset);
				block_pushes++;
			}
			p_label->push_bold();
			block_pushes++;
		} else if (block.type == BLOCK_UNORDERED_LIST_ITEM) {
			p_label->add_text(U"• ");
		} else if (block.type == BLOCK_CODE) {
			p_label->push_mono();
			p_label->add_text(block.code);
			p_label->pop();
		}

		for (const Inline &span : block.inlines) {
			int pushes = 0;
			if (!span.link.is_empty()) {
				p_label->push_meta(span.link, RichTextLabel::META_UNDERLINE_ON_HOVER, span.link);
				pushes++;
			}
			if (span.bold) {
				p_label->push_bold();
				pushes++;
			}
			if (span.italic) {
				p_label->push_italics();
				pushes++;
			}
			if (span.code) {
				p_label->push_mono();
				pushes++;
			}
			p_label->add_text(span.text);
			while (pushes-- > 0) {
				p_label->pop();
			}
		}

		while (block_pushes-- > 0) {
			p_label->pop();
		}
		if (block_index + 1 < document.blocks.size()) {
			p_label->add_newline();
			const BlockType next_type = document.blocks[block_index + 1].type;
			if (block.type != BLOCK_UNORDERED_LIST_ITEM && next_type != BLOCK_UNORDERED_LIST_ITEM) {
				p_label->add_newline();
			}
		}
	}
}
