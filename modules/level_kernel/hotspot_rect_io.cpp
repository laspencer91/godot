/**************************************************************************/
/*  hotspot_rect_io.cpp                                                   */
/**************************************************************************/
/*  G-Level LE3: tolerant Source KeyValues hotspot .rect interchange.     */
/**************************************************************************/

#include "hotspot_atlas.h"

#include "core/io/file_access.h"

namespace {

enum KVTokenType {
	KV_TOKEN_TEXT,
	KV_TOKEN_OPEN,
	KV_TOKEN_CLOSE,
};

struct KVToken {
	KVTokenType type = KV_TOKEN_TEXT;
	String text;
	int line = 1;
};

struct KVEntry {
	String key;
	String value;
	Vector<KVEntry> children;
	bool is_block = false;
};

class KVTokenizer {
	const String &source;
	int offset = 0;
	int line = 1;
	String error_message;

	bool _at_end() const { return offset >= source.length(); }
	char32_t _peek(int p_ahead = 0) const {
		const int at = offset + p_ahead;
		return at >= 0 && at < source.length() ? source[at] : 0;
	}

	Error _skip_layout() {
		while (!_at_end()) {
			const char32_t c = _peek();
			if (c <= 0x20) {
				if (c == '\n') {
					line++;
				}
				offset++;
				continue;
			}
			if (c == '/' && _peek(1) == '/') {
				offset += 2;
				while (!_at_end() && _peek() != '\n') {
					offset++;
				}
				continue;
			}
			if (c == '/' && _peek(1) == '*') {
				offset += 2;
				bool closed = false;
				while (!_at_end()) {
					if (_peek() == '\n') {
						line++;
					}
					if (_peek() == '*' && _peek(1) == '/') {
						offset += 2;
						closed = true;
						break;
					}
					offset++;
				}
				if (!closed) {
					error_message = vformat("Unterminated block comment at line %d.", line);
					return ERR_PARSE_ERROR;
				}
				continue;
			}
			break;
		}
		return OK;
	}

	Error _read_quoted(KVToken &r_token) {
		const int token_line = line;
		offset++; // Opening quote.
		String value;
		while (!_at_end()) {
			char32_t c = _peek();
			offset++;
			if (c == '"') {
				r_token.type = KV_TOKEN_TEXT;
				r_token.text = value;
				r_token.line = token_line;
				return OK;
			}
			if (c == '\n') {
				line++;
			}
			if (c != '\\') {
				value += c;
				continue;
			}
			if (_at_end()) {
				break;
			}
			c = _peek();
			offset++;
			switch (c) {
				case 'n':
					value += '\n';
					break;
				case 'r':
					value += '\r';
					break;
				case 't':
					value += '\t';
					break;
				case '"':
					value += '"';
					break;
				case '\\':
					value += '\\';
					break;
				default:
					// Source KeyValues commonly contains unescaped path separators.
					// Retain unknown escape sequences byte-for-byte in the value.
					value += '\\';
					value += c;
					break;
			}
		}
		error_message = vformat("Unterminated quoted string beginning at line %d.", token_line);
		return ERR_PARSE_ERROR;
	}

public:
	explicit KVTokenizer(const String &p_source) :
			source(p_source) {}

	Error tokenize(Vector<KVToken> &r_tokens) {
		while (true) {
			const Error layout_error = _skip_layout();
			if (layout_error != OK) {
				return layout_error;
			}
			if (_at_end()) {
				return OK;
			}
			const char32_t c = _peek();
			KVToken token;
			token.line = line;
			if (c == '{' || c == '}') {
				token.type = c == '{' ? KV_TOKEN_OPEN : KV_TOKEN_CLOSE;
				offset++;
				r_tokens.push_back(token);
				continue;
			}
			if (c == '"') {
				const Error quote_error = _read_quoted(token);
				if (quote_error != OK) {
					return quote_error;
				}
				r_tokens.push_back(token);
				continue;
			}
			String value;
			while (!_at_end()) {
				const char32_t next = _peek();
				if (next <= 0x20 || next == '{' || next == '}') {
					break;
				}
				value += next;
				offset++;
			}
			if (value.is_empty()) {
				error_message = vformat("Unexpected character at line %d.", line);
				return ERR_PARSE_ERROR;
			}
			token.type = KV_TOKEN_TEXT;
			token.text = value;
			r_tokens.push_back(token);
		}
	}

	String get_error_message() const { return error_message; }
};

class KVParser {
	const Vector<KVToken> &tokens;
	int cursor = 0;
	String error_message;

	Error _parse_entries(Vector<KVEntry> &r_entries, bool p_expect_close) {
		while (cursor < tokens.size()) {
			const KVToken &token = tokens[cursor];
			if (token.type == KV_TOKEN_CLOSE) {
				if (!p_expect_close) {
					error_message = vformat("Unexpected closing brace at line %d.", token.line);
					return ERR_PARSE_ERROR;
				}
				cursor++;
				return OK;
			}
			KVEntry entry;
			if (token.type == KV_TOKEN_OPEN) {
				// An anonymous outer block is accepted for exporters which omit the
				// conventional top-level catalog label.
				entry.is_block = true;
				cursor++;
				const Error nested_error = _parse_entries(entry.children, true);
				if (nested_error != OK) {
					return nested_error;
				}
				r_entries.push_back(entry);
				continue;
			}
			entry.key = token.text;
			cursor++;
			if (cursor >= tokens.size()) {
				error_message = vformat("Key '%s' at line %d has no value or block.", entry.key, token.line);
				return ERR_PARSE_ERROR;
			}
			const KVToken &value_token = tokens[cursor];
			if (value_token.type == KV_TOKEN_CLOSE) {
				error_message = vformat("Key '%s' at line %d has no value or block.", entry.key, token.line);
				return ERR_PARSE_ERROR;
			}
			if (value_token.type == KV_TOKEN_TEXT) {
				entry.value = value_token.text;
				cursor++;
			} else {
				entry.is_block = true;
				cursor++;
				const Error nested_error = _parse_entries(entry.children, true);
				if (nested_error != OK) {
					return nested_error;
				}
			}
			r_entries.push_back(entry);
		}
		if (p_expect_close) {
			error_message = "Unterminated KeyValues block at end of file.";
		return ERR_PARSE_ERROR;
		}
		return OK;
	}

public:
	explicit KVParser(const Vector<KVToken> &p_tokens) :
			tokens(p_tokens) {}

	Error parse(Vector<KVEntry> &r_entries) {
		if (tokens.is_empty()) {
			error_message = "The .rect file is empty.";
			return ERR_PARSE_ERROR;
		}
		return _parse_entries(r_entries, false);
	}

	String get_error_message() const { return error_message; }
};

const KVEntry *_find_direct_entry(const Vector<KVEntry> &p_entries, const String &p_key) {
	for (const KVEntry &entry : p_entries) {
		if (!entry.is_block && entry.key.to_lower() == p_key) {
			return &entry;
		}
	}
	return nullptr;
}

bool _parse_number(const String &p_text, double &r_number) {
	if (!p_text.is_valid_float()) {
		return false;
	}
	r_number = p_text.to_float();
	return Math::is_finite(r_number);
}

bool _find_dimensions(const Vector<KVEntry> &p_entries, Vector2 &r_dimensions) {
	const KVEntry *width = _find_direct_entry(p_entries, "width");
	const KVEntry *height = _find_direct_entry(p_entries, "height");
	if (width && height) {
		double parsed_width = 0.0;
		double parsed_height = 0.0;
		if (_parse_number(width->value, parsed_width) && _parse_number(height->value, parsed_height) && parsed_width > 0.0 && parsed_height > 0.0) {
			r_dimensions = Vector2(parsed_width, parsed_height);
			return true;
		}
		return false;
	}
	for (const KVEntry &entry : p_entries) {
		if (entry.is_block && _find_dimensions(entry.children, r_dimensions)) {
			return true;
		}
	}
	return false;
}

void _collect_patch_blocks(const Vector<KVEntry> &p_entries, Vector<const KVEntry *> &r_patches) {
	for (const KVEntry &entry : p_entries) {
		if (!entry.is_block) {
			continue;
		}
		const bool has_x = _find_direct_entry(entry.children, "x") != nullptr;
		const bool has_y = _find_direct_entry(entry.children, "y") != nullptr;
		const bool has_w = _find_direct_entry(entry.children, "w") != nullptr;
		const bool has_h = _find_direct_entry(entry.children, "h") != nullptr;
		// Any coordinate field makes this a subrect candidate. Collecting partial
		// candidates lets _parse_patch return a typed malformed-data error instead
		// of silently treating a misspelled/incomplete patch as metadata.
		if (has_x || has_y || has_w || has_h) {
			r_patches.push_back(&entry);
			continue;
		}
		_collect_patch_blocks(entry.children, r_patches);
	}
}

bool _parse_bool(const String &p_text, bool &r_value) {
	const String normalized = p_text.strip_edges().to_lower();
	if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
		r_value = true;
		return true;
	}
	if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
		r_value = false;
		return true;
	}
	return false;
}

bool _parse_axis(const String &p_text, int &r_axis) {
	const String normalized = p_text.strip_edges().to_lower();
	if (normalized == "0" || normalized == "u" || normalized == "x" || normalized == "h" || normalized == "horizontal") {
		r_axis = HotspotPatch::TILING_AXIS_U;
		return true;
	}
	if (normalized == "1" || normalized == "v" || normalized == "y" || normalized == "vertical") {
		r_axis = HotspotPatch::TILING_AXIS_V;
		return true;
	}
	return false;
}

bool _is_recognized_patch_key(const String &p_key) {
	const String key = p_key.to_lower();
	return key == "x" || key == "y" || key == "w" || key == "h" || key == "name" ||
			key == "allowrotation" || key == "rotate" || key == "mirrorhoriz" || key == "mirrorvert" ||
			key == "tile" || key == "infinite" || key == "tilingaxis" || key == "tileaxis" ||
			key == "inset" || key == "border";
}

Variant _entry_to_variant(const KVEntry &p_entry);

void _dictionary_insert_preserving_duplicates(Dictionary &r_dictionary, const String &p_key, const Variant &p_value) {
	if (!r_dictionary.has(p_key)) {
		r_dictionary[p_key] = p_value;
		return;
	}
	Variant existing = r_dictionary[p_key];
	Array values;
	if (existing.get_type() == Variant::ARRAY) {
		values = existing;
	} else {
		values.push_back(existing);
	}
	values.push_back(p_value);
	r_dictionary[p_key] = values;
}

Variant _entry_to_variant(const KVEntry &p_entry) {
	if (!p_entry.is_block) {
		return p_entry.value;
	}
	Dictionary result;
	for (const KVEntry &child : p_entry.children) {
		_dictionary_insert_preserving_duplicates(result, child.key, _entry_to_variant(child));
	}
	return result;
}

Error _parse_patch(const KVEntry &p_entry, const Vector2 &p_dimensions, int p_index, Ref<HotspotPatch> &r_patch, String &r_error) {
	const KVEntry *x_entry = _find_direct_entry(p_entry.children, "x");
	const KVEntry *y_entry = _find_direct_entry(p_entry.children, "y");
	const KVEntry *w_entry = _find_direct_entry(p_entry.children, "w");
	const KVEntry *h_entry = _find_direct_entry(p_entry.children, "h");
	double x = 0.0;
	double y = 0.0;
	double width = 0.0;
	double height = 0.0;
	if (!x_entry || !y_entry || !w_entry || !h_entry || !_parse_number(x_entry->value, x) || !_parse_number(y_entry->value, y) ||
			!_parse_number(w_entry->value, width) || !_parse_number(h_entry->value, height)) {
		r_error = vformat("Subrect %d has missing or non-numeric x/y/w/h fields.", p_index);
		return ERR_PARSE_ERROR;
	}
	if (x < 0.0 || y < 0.0 || width <= 0.0 || height <= 0.0 || x + width > double(p_dimensions.x) || y + height > double(p_dimensions.y)) {
		r_error = vformat("Subrect %d lies outside the declared atlas dimensions.", p_index);
		return ERR_INVALID_DATA;
	}

	Ref<HotspotPatch> patch;
	patch.instantiate();
	patch->set_rect_uv(Rect2(float(x / p_dimensions.x), float(y / p_dimensions.y), float(width / p_dimensions.x), float(height / p_dimensions.y)));

	const KVEntry *name_entry = _find_direct_entry(p_entry.children, "name");
	patch->set_patch_name(name_entry && !name_entry->value.is_empty() ? StringName(name_entry->value) : StringName("p" + itos(p_index)));

	struct BoolMapping {
		const char *primary;
		const char *alias;
		void (HotspotPatch::*setter)(bool);
	};
	const BoolMapping bool_mappings[] = {
		{ "allowrotation", "rotate", &HotspotPatch::set_allow_rotation },
		{ "mirrorhoriz", nullptr, &HotspotPatch::set_allow_mirror_x },
		{ "mirrorvert", nullptr, &HotspotPatch::set_allow_mirror_y },
	};
	for (const BoolMapping &mapping : bool_mappings) {
		const KVEntry *entry = _find_direct_entry(p_entry.children, mapping.primary);
		if (!entry && mapping.alias) {
			entry = _find_direct_entry(p_entry.children, mapping.alias);
		}
		if (!entry) {
			continue;
		}
		bool value = false;
		if (!_parse_bool(entry->value, value)) {
			r_error = vformat("Subrect %d has invalid boolean value '%s' for %s.", p_index, entry->value, entry->key);
			return ERR_PARSE_ERROR;
		}
		(patch.ptr()->*mapping.setter)(value);
	}

	int tiling_axis = width >= height ? HotspotPatch::TILING_AXIS_U : HotspotPatch::TILING_AXIS_V;
	const KVEntry *axis_entry = _find_direct_entry(p_entry.children, "tilingaxis");
	if (!axis_entry) {
		axis_entry = _find_direct_entry(p_entry.children, "tileaxis");
	}
	if (axis_entry && !_parse_axis(axis_entry->value, tiling_axis)) {
		r_error = vformat("Subrect %d has invalid tiling axis '%s'.", p_index, axis_entry->value);
		return ERR_PARSE_ERROR;
	}
	const KVEntry *tile_entry = _find_direct_entry(p_entry.children, "tile");
	if (!tile_entry) {
		tile_entry = _find_direct_entry(p_entry.children, "infinite");
	}
	if (tile_entry) {
		bool allow_tiling = false;
		if (!_parse_bool(tile_entry->value, allow_tiling)) {
			if (!_parse_axis(tile_entry->value, tiling_axis)) {
				r_error = vformat("Subrect %d has invalid tiling value '%s'.", p_index, tile_entry->value);
				return ERR_PARSE_ERROR;
			}
			allow_tiling = true;
		}
		patch->set_allow_tiling(allow_tiling);
	}
	patch->set_tiling_axis(tiling_axis);

	const KVEntry *inset_entry = _find_direct_entry(p_entry.children, "inset");
	if (!inset_entry) {
		inset_entry = _find_direct_entry(p_entry.children, "border");
	}
	if (inset_entry) {
		double inset = 0.0;
		if (!_parse_number(inset_entry->value, inset) || inset < 0.0) {
			r_error = vformat("Subrect %d has invalid inset value '%s'.", p_index, inset_entry->value);
			return ERR_PARSE_ERROR;
		}
		patch->set_inset_px(float(inset));
	}

	Dictionary extra;
	for (const KVEntry &entry : p_entry.children) {
		if (!_is_recognized_patch_key(entry.key)) {
			_dictionary_insert_preserving_duplicates(extra, entry.key, _entry_to_variant(entry));
		}
	}
	patch->set_extra(extra);
	r_patch = patch;
	return OK;
}

String _escape_kv(const String &p_value) {
	String result;
	for (int i = 0; i < p_value.length(); i++) {
		const char32_t c = p_value[i];
		switch (c) {
			case '\\':
				result += "\\\\";
				break;
			case '"':
				result += "\\\"";
				break;
			case '\n':
				result += "\\n";
				break;
			case '\r':
				result += "\\r";
				break;
			case '\t':
				result += "\\t";
				break;
			default:
				result += c;
				break;
		}
	}
	return result;
}

String _indent(int p_depth) {
	return String("\t").repeat(p_depth);
}

void _append_scalar(String &r_output, int p_depth, const String &p_key, const String &p_value) {
	r_output += _indent(p_depth) + "\"" + _escape_kv(p_key) + "\"\t\"" + _escape_kv(p_value) + "\"\n";
}

void _append_variant(String &r_output, int p_depth, const String &p_key, const Variant &p_value) {
	if (p_value.get_type() == Variant::ARRAY) {
		const Array values = p_value;
		for (int i = 0; i < values.size(); i++) {
			_append_variant(r_output, p_depth, p_key, values[i]);
		}
		return;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		r_output += _indent(p_depth) + "\"" + _escape_kv(p_key) + "\"\n" + _indent(p_depth) + "{\n";
		const Dictionary dictionary = p_value;
		const Array keys = dictionary.keys();
		for (int i = 0; i < keys.size(); i++) {
			const String key = keys[i];
			_append_variant(r_output, p_depth + 1, key, dictionary[keys[i]]);
		}
		r_output += _indent(p_depth) + "}\n";
		return;
	}
	String value;
	switch (p_value.get_type()) {
		case Variant::NIL:
			break;
		case Variant::BOOL:
			value = bool(p_value) ? "1" : "0";
			break;
		case Variant::FLOAT:
			value = String::num_real(double(p_value), false);
			break;
		default:
			value = String(p_value);
			break;
	}
	_append_scalar(r_output, p_depth, p_key, value);
}

} // namespace

Error HotspotAtlas::import_rect(const String &p_path) {
	last_rect_error.clear();
	if (p_path.is_empty()) {
		last_rect_error = "The .rect path is empty.";
		return ERR_INVALID_PARAMETER;
	}
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		last_rect_error = vformat("Could not open '%s' for reading.", p_path);
		return open_error == OK ? ERR_CANT_OPEN : open_error;
	}
	const String source = file->get_as_text();
	if (file->get_error() != OK && file->get_error() != ERR_FILE_EOF) {
		last_rect_error = vformat("Could not read '%s'.", p_path);
		return file->get_error();
	}

	Vector<KVToken> tokens;
	KVTokenizer tokenizer(source);
	const Error token_error = tokenizer.tokenize(tokens);
	if (token_error != OK) {
		last_rect_error = tokenizer.get_error_message();
		return token_error;
	}
	Vector<KVEntry> entries;
	KVParser parser(tokens);
	const Error parse_error = parser.parse(entries);
	if (parse_error != OK) {
		last_rect_error = parser.get_error_message();
		return parse_error;
	}

	Vector2 dimensions;
	if (!_find_dimensions(entries, dimensions)) {
		last_rect_error = "The .rect file has no valid positive atlas width and height.";
		return ERR_PARSE_ERROR;
	}
	Vector<const KVEntry *> patch_entries;
	_collect_patch_blocks(entries, patch_entries);
	TypedArray<HotspotPatch> imported_patches;
	for (int i = 0; i < patch_entries.size(); i++) {
		Ref<HotspotPatch> patch;
		String patch_error_message;
		const Error patch_error = _parse_patch(*patch_entries[i], dimensions, i, patch, patch_error_message);
		if (patch_error != OK) {
			last_rect_error = patch_error_message;
			return patch_error;
		}
		imported_patches.push_back(patch);
	}

	set_patches(imported_patches);
	last_rect_error.clear();
	return OK;
}

Error HotspotAtlas::export_rect(const String &p_path) {
	last_rect_error.clear();
	if (p_path.is_empty()) {
		last_rect_error = "The .rect path is empty.";
		return ERR_INVALID_PARAMETER;
	}
	_recompute_patch_metrics();
	if (reference_texture.is_null() || derived_texture_size.x <= 0 || derived_texture_size.y <= 0) {
		last_rect_error = "A reference_texture with positive dimensions is required to export .rect pixels.";
		return ERR_INVALID_DATA;
	}

	String output = "\"hotspot_atlas\"\n{\n";
	_append_scalar(output, 1, "width", itos(derived_texture_size.x));
	_append_scalar(output, 1, "height", itos(derived_texture_size.y));
	output += "\t\"subrects\"\n\t{\n";
	for (int i = 0; i < patches.size(); i++) {
		Ref<HotspotPatch> patch = patches[i];
		if (patch.is_null()) {
			last_rect_error = vformat("Patch %d is null and cannot be exported.", i);
			return ERR_INVALID_DATA;
		}
		const Rect2 rect_px = get_patch_rect_px(i);
		output += "\t\t\"subrect\"\n\t\t{\n";
		_append_scalar(output, 3, "name", patch->get_patch_name().is_empty() ? "p" + itos(i) : String(patch->get_patch_name()));
		_append_scalar(output, 3, "x", String::num_real(rect_px.position.x, false));
		_append_scalar(output, 3, "y", String::num_real(rect_px.position.y, false));
		_append_scalar(output, 3, "w", String::num_real(rect_px.size.x, false));
		_append_scalar(output, 3, "h", String::num_real(rect_px.size.y, false));
		_append_scalar(output, 3, "allowrotation", patch->is_rotation_allowed() ? "1" : "0");
		_append_scalar(output, 3, "mirrorhoriz", patch->is_mirror_x_allowed() ? "1" : "0");
		_append_scalar(output, 3, "mirrorvert", patch->is_mirror_y_allowed() ? "1" : "0");
		_append_scalar(output, 3, "tile", patch->is_tiling_allowed() ? "1" : "0");
		_append_scalar(output, 3, "tilingaxis", patch->get_tiling_axis() == HotspotPatch::TILING_AXIS_V ? "v" : "u");
		_append_scalar(output, 3, "inset", String::num_real(patch->get_inset_px(), false));
		const Dictionary extra = patch->get_extra();
		const Array keys = extra.keys();
		for (int extra_index = 0; extra_index < keys.size(); extra_index++) {
			const String key = keys[extra_index];
			if (!_is_recognized_patch_key(key)) {
				_append_variant(output, 3, key, extra[keys[extra_index]]);
			}
		}
		output += "\t\t}\n";
	}
	output += "\t}\n}\n";

	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &open_error);
	if (file.is_null()) {
		last_rect_error = vformat("Could not open '%s' for writing.", p_path);
		return open_error == OK ? ERR_CANT_OPEN : open_error;
	}
	file->store_string(output);
	const Error write_error = file->get_error();
	if (write_error != OK) {
		last_rect_error = vformat("Could not write '%s'.", p_path);
		return write_error;
	}
	return OK;
}
