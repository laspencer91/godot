/**************************************************************************/
/*  editor_resource_preview.cpp                                           */
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

#include "editor_resource_preview.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/missing_resource.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/variant/variant_utility.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/2d/audio_stream_player_2d.h"
#include "scene/2d/camera_2d.h"
#include "scene/2d/cpu_particles_2d.h"
#include "scene/2d/gpu_particles_2d.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/cpu_particles_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/animation/animation_mixer.h"
#include "scene/animation/animation_player.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/video_stream_player.h"
#include "scene/main/canvas_item.h"
#include "scene/main/missing_node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/timer.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/packed_scene.h"
#include "servers/display/display_server.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

namespace {

class ScenePreviewScriptGuard {
	bool scripting_enabled = false;
	bool recovery_mode = false;

public:
	ScenePreviewScriptGuard() {
		scripting_enabled = ScriptServer::is_scripting_enabled();
		recovery_mode = Engine::get_singleton()->is_recovery_mode_hint();
		ScriptServer::set_scripting_enabled(false);
		Engine::get_singleton()->set_recovery_mode_hint(true);
	}

	~ScenePreviewScriptGuard() {
		Engine::get_singleton()->set_recovery_mode_hint(recovery_mode);
		ScriptServer::set_scripting_enabled(scripting_enabled);
	}
};

} // namespace

bool EditorResourcePreviewGenerator::handles(const String &p_type) const {
	bool success = false;
	GDVIRTUAL_CALL(_handles, p_type, success);
	return success;
}

Ref<Texture2D> EditorResourcePreviewGenerator::generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<Texture2D> preview;
	GDVIRTUAL_CALL(_generate, p_from, p_size, p_metadata, preview);
	return preview;
}

Ref<Texture2D> EditorResourcePreviewGenerator::generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const {
	Ref<Texture2D> preview;
	if (GDVIRTUAL_CALL(_generate_from_path, p_path, p_size, p_metadata, preview)) {
		return preview;
	}

	Ref<Resource> res = ResourceLoader::load(p_path);
	if (res.is_null()) {
		return res;
	}
	return generate(res, p_size, p_metadata);
}

bool EditorResourcePreviewGenerator::generate_small_preview_automatically() const {
	bool success = false;
	GDVIRTUAL_CALL(_generate_small_preview_automatically, success);
	return success;
}

bool EditorResourcePreviewGenerator::can_generate_small_preview() const {
	bool success = false;
	GDVIRTUAL_CALL(_can_generate_small_preview, success);
	return success;
}

void EditorResourcePreviewGenerator::_bind_methods() {
	GDVIRTUAL_BIND(_handles, "type");
	GDVIRTUAL_BIND(_generate, "resource", "size", "metadata");
	GDVIRTUAL_BIND(_generate_from_path, "path", "size", "metadata");
	GDVIRTUAL_BIND(_generate_small_preview_automatically);
	GDVIRTUAL_BIND(_can_generate_small_preview);

	ClassDB::bind_method(D_METHOD("request_draw_and_wait", "viewport"), &EditorResourcePreviewGenerator::request_draw_and_wait);
}

void EditorResourcePreviewGenerator::DrawRequester::request_and_wait(RID p_viewport) {
	if (EditorResourcePreview::get_singleton()->is_threaded()) {
		RS::get_singleton()->connect(SNAME("frame_pre_draw"), callable_mp(this, &EditorResourcePreviewGenerator::DrawRequester::_prepare_draw).bind(p_viewport), Object::CONNECT_ONE_SHOT);
		semaphore.wait();
	} else {
		// Avoid the main viewport and children being redrawn.
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		ERR_FAIL_NULL_MSG(st, "Editor's MainLoop is not a SceneTree. This is a bug.");
		RID root_vp = st->get_root()->get_viewport_rid();
		RenderingServer::get_singleton()->viewport_set_active(root_vp, false);

		RS::get_singleton()->viewport_set_update_mode(p_viewport, RSE::VIEWPORT_UPDATE_ONCE);
		RS::get_singleton()->draw(false);

		// Let main viewport and children be drawn again.
		RenderingServer::get_singleton()->viewport_set_active(root_vp, true);
	}
}

void EditorResourcePreviewGenerator::DrawRequester::abort() {
	if (EditorResourcePreview::get_singleton()->is_threaded()) {
		semaphore.post();
	}
}

void EditorResourcePreviewGenerator::request_draw_and_wait(RID viewport) const {
	DrawRequester draw_requester;
	draw_requester.request_and_wait(viewport);
}

void EditorResourcePreviewGenerator::DrawRequester::_prepare_draw(RID p_viewport) {
	RS::get_singleton()->viewport_set_update_mode(p_viewport, RSE::VIEWPORT_UPDATE_ONCE);
	RS::get_singleton()->request_frame_drawn_callback(callable_mp(this, &EditorResourcePreviewGenerator::DrawRequester::_post_semaphore));
}

void EditorResourcePreviewGenerator::DrawRequester::_post_semaphore() {
	semaphore.post();
}

bool EditorResourcePreview::is_threaded() const {
	return RSG::rasterizer->can_create_resources_async();
}

void EditorResourcePreview::_thread_func(void *ud) {
	EditorResourcePreview *erp = (EditorResourcePreview *)ud;
	erp->_thread();
}

void EditorResourcePreview::_preview_ready(const String &p_path, int p_hash, const Ref<Texture2D> &p_texture, const Ref<Texture2D> &p_small_texture, const Callable &p_callback, const Dictionary &p_metadata) {
	{
		MutexLock lock(preview_mutex);

		Item item;
		item.preview = p_texture;
		item.small_preview = p_small_texture;
		item.last_hash = p_hash;
		item.modified_time = p_path.begins_with("ID:") ? 0 : _get_preview_modified_time(p_path);
		item.preview_metadata = p_metadata;

		cache[p_path] = item;
	}
	if (p_callback.is_valid()) {
		p_callback.call_deferred(p_path, p_texture, p_small_texture);
	}
}

void EditorResourcePreview::_generate_preview(Ref<ImageTexture> &r_texture, Ref<ImageTexture> &r_small_texture, const QueueItem &p_item, Dictionary &p_metadata) {
	String type;

	uint64_t started_at = OS::get_singleton()->get_ticks_usec();

	if (p_item.resource.is_valid()) {
		type = p_item.resource->get_class();
	} else {
		type = ResourceLoader::get_resource_type(p_item.path);
	}

	if (type.is_empty()) {
		r_texture = Ref<ImageTexture>();
		r_small_texture = Ref<ImageTexture>();

		if (is_print_verbose_enabled()) {
			print_line(vformat("Generated '%s' preview in %d usec", p_item.path, OS::get_singleton()->get_ticks_usec() - started_at));
		}
		return; //could not guess type
	}

	int thumbnail_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	thumbnail_size *= EDSCALE;

	r_texture = Ref<ImageTexture>();
	r_small_texture = Ref<ImageTexture>();

	for (int i = 0; i < preview_generators.size(); i++) {
		if (!preview_generators[i]->handles(type)) {
			continue;
		}

		Ref<Texture2D> generated;
		if (p_item.resource.is_valid()) {
			generated = preview_generators.write[i]->generate(p_item.resource, Vector2(thumbnail_size, thumbnail_size), p_metadata);
		} else {
			generated = preview_generators.write[i]->generate_from_path(p_item.path, Vector2(thumbnail_size, thumbnail_size), p_metadata);
		}
		r_texture = generated;

		if (preview_generators[i]->can_generate_small_preview()) {
			Ref<Texture2D> generated_small;
			Dictionary d;
			if (p_item.resource.is_valid()) {
				generated_small = preview_generators.write[i]->generate(p_item.resource, Vector2(small_thumbnail_size, small_thumbnail_size), d);
			} else {
				generated_small = preview_generators.write[i]->generate_from_path(p_item.path, Vector2(small_thumbnail_size, small_thumbnail_size), d);
			}
			r_small_texture = generated_small;
		}

		if (r_small_texture.is_null() && r_texture.is_valid() && preview_generators[i]->generate_small_preview_automatically()) {
			Ref<Image> small_image = r_texture->get_image()->duplicate();
			Vector2i new_size = Vector2i(1, 1) * small_thumbnail_size;
			const real_t aspect = small_image->get_size().aspect();
			if (aspect > 1.0) {
				new_size.y = MAX(1, new_size.y / aspect);
			} else if (aspect < 1.0) {
				new_size.x = MAX(1, new_size.x * aspect);
			}
			small_image->resize(new_size.x, new_size.y, Image::INTERPOLATE_CUBIC);

			// Make sure the image is always square.
			if (aspect != 1.0) {
				Ref<Image> rect = small_image;
				const Vector2i rect_size = rect->get_size();
				small_image = Image::create_empty(small_thumbnail_size, small_thumbnail_size, false, rect->get_format());
				// Blit the rectangle in the center of the square.
				small_image->blit_rect(rect, Rect2i(Vector2i(), rect_size), (Vector2i(1, 1) * small_thumbnail_size - rect_size) / 2);
			}

			r_small_texture.instantiate();
			r_small_texture->set_image(small_image);
		}

		if (generated.is_valid()) {
			break;
		}
	}

	if (p_item.resource.is_null()) {
		// Cache the preview in case it's a resource on disk.
		if (r_texture.is_valid()) {
			Ref<Image> preview = r_texture->get_image();
			Ref<Image> small_preview;
			if (r_small_texture.is_valid()) {
				small_preview = r_small_texture->get_image();
			}
			Error err = save_preview_cache(p_item.path, preview, small_preview, p_metadata);
			ERR_FAIL_COND_MSG(err != OK, vformat("Cannot save preview cache for '%s'.", p_item.path));
		}
	}

	if (is_print_verbose_enabled()) {
		print_line(vformat("Generated '%s' preview in %d usec", p_item.path, OS::get_singleton()->get_ticks_usec() - started_at));
	}
}

uint64_t EditorResourcePreview::_get_preview_modified_time(const String &p_path) {
	uint64_t modified_time = FileAccess::get_modified_time(p_path);
	const String import_path = p_path + ".import";
	if (FileAccess::exists(import_path)) {
		modified_time = MAX(modified_time, FileAccess::get_modified_time(import_path));
	}
	return modified_time;
}

bool EditorResourcePreview::_is_preview_cache_valid(int p_cached_size, int p_expected_size, bool p_outdated, uint64_t p_cached_modified_time, uint64_t p_modified_time, const String &p_cached_hash, const String &p_current_hash) {
	if (p_cached_size != p_expected_size || p_outdated) {
		return false;
	}
	if (p_cached_modified_time == p_modified_time) {
		return true;
	}
	return !p_current_hash.is_empty() && p_cached_hash == p_current_hash;
}

bool EditorResourcePreview::_load_cached_preview(const String &p_path, Ref<ImageTexture> &r_texture, Ref<ImageTexture> &r_small_texture, Dictionary &r_metadata) {
	r_texture.unref();
	r_small_texture.unref();

	String cache_base = ProjectSettings::get_singleton()->globalize_path(p_path).md5_text();
	cache_base = EditorPaths::get_singleton()->get_cache_dir().path_join("resthumb-" + cache_base);
	const String metadata_path = cache_base + ".txt";
	Ref<FileAccess> metadata_file = FileAccess::open(metadata_path, FileAccess::READ);
	if (metadata_file.is_null()) {
		return false;
	}

	int cached_size;
	bool has_small_texture;
	uint64_t cached_modified_time;
	String cached_hash;
	bool outdated;
	_read_preview_cache(metadata_file, &cached_size, &has_small_texture, &cached_modified_time, &cached_hash, &r_metadata, &outdated);
	metadata_file.unref();

	int thumbnail_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	thumbnail_size *= EDSCALE;
	const uint64_t modified_time = _get_preview_modified_time(p_path);
	const String current_hash = cached_modified_time == modified_time ? String() : FileAccess::get_md5(p_path);
	if (!_is_preview_cache_valid(cached_size, thumbnail_size, outdated, cached_modified_time, modified_time, cached_hash, current_hash)) {
		return false;
	}

	if (cached_modified_time != modified_time) {
		Ref<FileAccess> updated_metadata = FileAccess::open(metadata_path, FileAccess::WRITE);
		if (updated_metadata.is_null()) {
			ERR_PRINT("Cannot create file '" + metadata_path + "'. Check user write permissions.");
		} else {
			_write_preview_cache(updated_metadata, thumbnail_size, has_small_texture, modified_time, current_hash, r_metadata);
		}
	}

	Ref<Image> image;
	image.instantiate();
	if (image->load(cache_base + ".png") != OK) {
		return false;
	}
	r_texture.instantiate();
	r_texture->set_image(image);

	if (has_small_texture) {
		Ref<Image> small_image;
		small_image.instantiate();
		if (small_image->load(cache_base + "_small.png") != OK) {
			r_texture.unref();
			return false;
		}
		r_small_texture.instantiate();
		r_small_texture->set_image(small_image);
	}
	return true;
}

const Dictionary EditorResourcePreview::get_preview_metadata(const String &p_path) const {
	ERR_FAIL_COND_V(!cache.has(p_path), Dictionary());
	return cache[p_path].preview_metadata;
}

void EditorResourcePreview::_iterate() {
	preview_mutex.lock();

	if (queue.is_empty()) {
		preview_mutex.unlock();
		if (thread.is_started()) {
			// Resource previews drained; spend this wake-up probing a queued scene preview.
			_probe_scene_preview();
		}
		return;
	}

	QueueItem item = queue.front()->get();
	queue.pop_front();

	if (cache.has(item.path)) {
		Item cached_item = cache[item.path];
		// Already has it because someone loaded it, just let it know it's ready.
		_preview_ready(item.path, cached_item.last_hash, cached_item.preview, cached_item.small_preview, item.callback, cached_item.preview_metadata);
		preview_mutex.unlock();
		return;
	}
	preview_mutex.unlock();

	Ref<ImageTexture> texture;
	Ref<ImageTexture> small_texture;

	if (item.resource.is_valid()) {
		Dictionary preview_metadata;
		_generate_preview(texture, small_texture, item, preview_metadata);
		_preview_ready(item.path, item.resource->hash_edited_version_for_preview(), texture, small_texture, item.callback, preview_metadata);
		return;
	}

	Dictionary preview_metadata;
	if (!_load_cached_preview(item.path, texture, small_texture, preview_metadata)) {
		_generate_preview(texture, small_texture, item, preview_metadata);
	}
	_preview_ready(item.path, 0, texture, small_texture, item.callback, preview_metadata);
}

bool EditorResourcePreview::_count_scene_nodes(const Ref<SceneState> &p_state, int p_limit, int &r_count, HashSet<ObjectID> &r_stack) {
	if (p_state.is_null() || r_stack.has(p_state->get_instance_id())) {
		return false;
	}

	const int node_count = p_state->get_node_count();
	if (node_count < 1 || node_count > p_limit - r_count) {
		return false;
	}
	r_count += node_count;
	r_stack.insert(p_state->get_instance_id());
	for (int i = 0; i < node_count; i++) {
		const Ref<PackedScene> instance = p_state->get_node_instance(i);
		if (instance.is_valid() && !_count_scene_nodes(instance->get_state(), p_limit, r_count, r_stack)) {
			r_stack.erase(p_state->get_instance_id());
			return false;
		}
	}
	r_stack.erase(p_state->get_instance_id());
	return true;
}

bool EditorResourcePreview::_is_scene_node_count_within_limit(const Ref<SceneState> &p_state, int p_limit) {
	int node_count = 0;
	HashSet<ObjectID> stack;
	return p_limit > 0 && _count_scene_nodes(p_state, p_limit, node_count, stack);
}

bool EditorResourcePreview::_sanitize_scene_variant(Variant &r_variant, HashSet<ObjectID> &r_visited, String &r_failure_reason, int p_depth) {
	if (p_depth > MAX_SCENE_PREVIEW_RESOURCE_DEPTH) {
		r_failure_reason = "the scene resource graph exceeds the preview depth limit";
		return false;
	}
	if (r_variant.get_type() == Variant::ARRAY) {
		Array values = r_variant;
		for (int i = 0; i < values.size(); i++) {
			Variant value = values[i];
			if (!_sanitize_scene_variant(value, r_visited, r_failure_reason, p_depth + 1)) {
				return false;
			}
			values[i] = value;
		}
		r_variant = values;
		return true;
	}
	if (r_variant.get_type() == Variant::DICTIONARY) {
		Dictionary values = r_variant;
		const Array keys = values.keys();
		for (int i = 0; i < keys.size(); i++) {
			Variant key = keys[i];
			Object *key_object = nullptr;
			if (key.get_type() == Variant::OBJECT) {
				key_object = key;
			}
			if (Object::cast_to<Script>(key_object)) {
				values.erase(key);
				continue;
			}
			Variant value = values[key];
			if (!_sanitize_scene_variant(value, r_visited, r_failure_reason, p_depth + 1)) {
				return false;
			}
			values[key] = value;
		}
		r_variant = values;
		return true;
	}
	if (r_variant.get_type() == Variant::CALLABLE || r_variant.get_type() == Variant::SIGNAL) {
		r_variant = Variant();
		return true;
	}
	if (r_variant.get_type() != Variant::OBJECT) {
		return true;
	}

	Object *object = r_variant;
	if (!object) {
		return true;
	}
	if (Object::cast_to<Script>(object)) {
		r_variant = Variant();
		return true;
	}
	if (Object::cast_to<MissingResource>(object)) {
		r_failure_reason = "a scene resource dependency is missing";
		return false;
	}
	if (PackedScene *packed_scene = Object::cast_to<PackedScene>(object)) {
		return _sanitize_packed_scene(Ref<PackedScene>(packed_scene), r_visited, r_failure_reason, p_depth + 1);
	}

	Resource *resource = Object::cast_to<Resource>(object);
	if (!resource || r_visited.has(resource->get_instance_id())) {
		return true;
	}
	r_visited.insert(resource->get_instance_id());
	if (resource->get_script().get_type() != Variant::NIL) {
		resource->set_script(Variant());
	}

	List<PropertyInfo> properties;
	resource->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE) || property.name == SNAME("script")) {
			continue;
		}
		Variant value = resource->get(property.name);
		const Variant original_value = value;
		if (!_sanitize_scene_variant(value, r_visited, r_failure_reason, p_depth + 1)) {
			return false;
		}
		if (value != original_value) {
			resource->set(property.name, value);
		}
	}
	return true;
}

bool EditorResourcePreview::_sanitize_packed_scene(const Ref<PackedScene> &p_scene, HashSet<ObjectID> &r_visited, String &r_failure_reason, int p_depth) {
	if (p_depth > MAX_SCENE_PREVIEW_RESOURCE_DEPTH) {
		r_failure_reason = "the nested scene graph exceeds the preview depth limit";
		return false;
	}
	if (p_scene.is_null()) {
		r_failure_reason = "a nested scene dependency is invalid";
		return false;
	}
	if (r_visited.has(p_scene->get_instance_id())) {
		return true;
	}
	r_visited.insert(p_scene->get_instance_id());

	const Ref<SceneState> state = p_scene->get_state();
	if (state.is_null()) {
		r_failure_reason = "the scene has no state";
		return false;
	}
	Dictionary bundle = state->get_bundled_scene();
	if (!bundle.has("variants") || !bundle.has("nodes") || !bundle.has("node_count")) {
		r_failure_reason = "the scene state is malformed";
		return false;
	}

	Array variants = bundle["variants"];
	for (int i = 0; i < variants.size(); i++) {
		Variant value = variants[i];
		if (!_sanitize_scene_variant(value, r_visited, r_failure_reason, p_depth + 1)) {
			return false;
		}
		variants[i] = value;
	}
	bundle["variants"] = variants;
	bundle["conns"] = PackedInt32Array();
	bundle["conn_count"] = 0;

	Ref<SceneState> sanitized_state;
	sanitized_state.instantiate();
	sanitized_state->set_bundled_scene(bundle);
	sanitized_state->set_path(state->get_path());
	p_scene->replace_state(sanitized_state);
	return true;
}

bool EditorResourcePreview::_sanitize_proxy_node(Node *p_node, int &r_node_count, String &r_failure_reason) {
	if (++r_node_count > MAX_SCENE_PREVIEW_NODES) {
		r_failure_reason = "the instantiated scene exceeds the preview node limit";
		return false;
	}
	if (Object::cast_to<MissingNode>(p_node)) {
		r_failure_reason = "the instantiated scene contains a missing node";
		return false;
	}

	p_node->set_script(Variant());
	p_node->set_process_mode(Node::PROCESS_MODE_DISABLED);
	p_node->set_process(false);
	p_node->set_physics_process(false);
	p_node->set_process_internal(false);
	p_node->set_physics_process_internal(false);
	p_node->set_process_input(false);
	p_node->set_process_shortcut_input(false);
	p_node->set_process_unhandled_input(false);
	p_node->set_process_unhandled_key_input(false);

	if (AnimationPlayer *animation_player = Object::cast_to<AnimationPlayer>(p_node)) {
		animation_player->set_autoplay(StringName());
		animation_player->stop();
	}
	if (AnimationMixer *animation_mixer = Object::cast_to<AnimationMixer>(p_node)) {
		animation_mixer->set_active(false);
	}
	if (Timer *timer = Object::cast_to<Timer>(p_node)) {
		timer->set_autostart(false);
		timer->stop();
	}
	if (AudioStreamPlayer *player = Object::cast_to<AudioStreamPlayer>(p_node)) {
		player->set_autoplay(false);
		player->stop();
	}
	if (AudioStreamPlayer2D *player = Object::cast_to<AudioStreamPlayer2D>(p_node)) {
		player->set_autoplay(false);
		player->stop();
	}
	if (AudioStreamPlayer3D *player = Object::cast_to<AudioStreamPlayer3D>(p_node)) {
		player->set_autoplay(false);
		player->stop();
	}
	if (VideoStreamPlayer *player = Object::cast_to<VideoStreamPlayer>(p_node)) {
		player->set_autoplay(false);
		player->stop();
	}
	if (CPUParticles2D *particles = Object::cast_to<CPUParticles2D>(p_node)) {
		particles->set_emitting(false);
	}
	if (GPUParticles2D *particles = Object::cast_to<GPUParticles2D>(p_node)) {
		particles->set_emitting(false);
	}
	if (CPUParticles3D *particles = Object::cast_to<CPUParticles3D>(p_node)) {
		particles->set_emitting(false);
	}
	if (GPUParticles3D *particles = Object::cast_to<GPUParticles3D>(p_node)) {
		particles->set_emitting(false);
	}
	if (Camera2D *camera = Object::cast_to<Camera2D>(p_node)) {
		camera->set_enabled(false);
	}
	if (Camera3D *camera = Object::cast_to<Camera3D>(p_node)) {
		camera->set_current(false);
	}

	for (int i = p_node->get_child_count() - 1; i >= 0; i--) {
		Node *child = p_node->get_child(i);
		// A Window child is NOT safe to delete. Several built-in controls keep a raw pointer to an internal
		// Window they created -- OptionButton::popup, MenuButton, ColorPickerButton -- and freeing it here
		// leaves that pointer dangling; the very next layout or notification pass dereferences it and the
		// editor dies. (OptionButton::get_minimum_size() reads popup->get_contents_minimum_size() whenever
		// fit_to_longest_item is set, which is the default, so merely adding the scene to the preview
		// viewport was enough.) A Window contributes nothing to a thumbnail anyway -- it is a separate
		// surface and starts hidden -- so hide it and sanitise its contents instead of freeing it.
		if (Window *window = Object::cast_to<Window>(child)) {
			window->hide();
			if (!_sanitize_proxy_node(child, r_node_count, r_failure_reason)) {
				return false;
			}
			continue;
		}
		// A plain Viewport/SubViewport is a nested render target with no such owner, so it still goes.
		if (Object::cast_to<Viewport>(child)) {
			p_node->remove_child(child);
			memdelete(child);
			continue;
		}
		if (!_sanitize_proxy_node(child, r_node_count, r_failure_reason)) {
			return false;
		}
	}
	return true;
}

void EditorResourcePreview::_find_scene_bounds(Node *p_node, AABB &r_aabb, bool &r_has_3d, Rect2 &r_rect, bool &r_has_2d) {
	GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(p_node);
	if (geometry && geometry->is_visible_in_tree() && geometry->get_base().is_valid()) {
		const AABB aabb = geometry->get_global_transform().xform(geometry->get_aabb());
		if (aabb.is_finite() && aabb.size.length_squared() > CMP_EPSILON2) {
			r_aabb = r_has_3d ? r_aabb.merge(aabb) : aabb;
			r_has_3d = true;
		}
	}

	CanvasItem *canvas_item = Object::cast_to<CanvasItem>(p_node);
	if (canvas_item && canvas_item->is_visible_in_tree() && canvas_item->_edit_use_rect()) {
		const Rect2 rect = canvas_item->get_global_transform().xform(canvas_item->_edit_get_rect());
		if (rect.is_finite() && rect.size.length_squared() > CMP_EPSILON2) {
			r_rect = r_has_2d ? r_rect.merge(rect) : rect;
			r_has_2d = true;
		}
	}

	for (int i = 0; i < p_node->get_child_count(); i++) {
		_find_scene_bounds(p_node->get_child(i), r_aabb, r_has_3d, r_rect, r_has_2d);
	}
}

void EditorResourcePreview::_hide_canvas_items(Node *p_node) {
	if (CanvasItem *canvas_item = Object::cast_to<CanvasItem>(p_node)) {
		canvas_item->set_visible(false);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_hide_canvas_items(p_node->get_child(i));
	}
}

void EditorResourcePreview::_queue_scene_preview(const SceneQueueItem &p_item) {
	MutexLock lock(preview_mutex);
	for (const SceneQueueItem &queued_item : scene_queue) {
		if (queued_item.path == p_item.path && queued_item.callback == p_item.callback) {
			return;
		}
	}
	for (const SceneQueueItem &queued_item : scene_generate_queue) {
		if (queued_item.path == p_item.path && queued_item.callback == p_item.callback) {
			return;
		}
	}
	if (scene_probe_active && scene_probe_item.path == p_item.path && scene_probe_item.callback == p_item.callback) {
		return;
	}

	if (scene_queue.size() >= MAX_SCENE_PREVIEW_QUEUE_SIZE) {
		List<SceneQueueItem>::Element *last = scene_queue.back();
		if (!last || last->get().priority >= p_item.priority) {
			return;
		}
		scene_queue.erase(last);
	}

	bool inserted = false;
	for (List<SceneQueueItem>::Element *element = scene_queue.front(); element; element = element->next()) {
		if (element->get().priority < p_item.priority) {
			scene_queue.insert_before(element, p_item);
			inserted = true;
			break;
		}
	}
	if (!inserted) {
		scene_queue.push_back(p_item);
	}
	if (thread.is_started()) {
		// Wake the worker so it can probe the disk cache off the main thread.
		preview_sem.post();
	}
}

bool EditorResourcePreview::_pop_scene_preview(SceneQueueItem &r_item) {
	MutexLock lock(preview_mutex);
	if (scene_queue.is_empty()) {
		return false;
	}
	r_item = scene_queue.front()->get();
	scene_queue.pop_front();
	return true;
}

// Worker-thread stage: probe the disk cache for the next queued scene so the stat and PNG reads
// never block a frame. Hits complete here; misses move to scene_generate_queue for the main thread.
void EditorResourcePreview::_probe_scene_preview() {
	SceneQueueItem item;
	{
		MutexLock lock(preview_mutex);
		if (scene_queue.is_empty()) {
			return;
		}
		item = scene_queue.front()->get();
		scene_queue.pop_front();
		scene_probe_item = item;
		scene_probe_active = true;
	}

	Ref<ImageTexture> texture;
	Ref<ImageTexture> small_texture;
	Dictionary preview_metadata;
	const bool cache_hit = _load_cached_preview(item.path, texture, small_texture, preview_metadata);

	{
		MutexLock lock(preview_mutex);
		// cancel_scene_preview() may have nulled the callback while the probe ran.
		item.callback = scene_probe_item.callback;
		scene_probe_item = SceneQueueItem();
		scene_probe_active = false;
		if (!cache_hit) {
			item.cache_probed = true;
			scene_generate_queue.push_back(item);
		}
	}
	if (cache_hit) {
		_preview_ready(item.path, 0, texture, small_texture, item.callback, preview_metadata);
	}
}

void EditorResourcePreview::_begin_scene_preview(const SceneQueueItem &p_item) {
	Item cached_item;
	bool has_cached_item = false;
	{
		MutexLock lock(preview_mutex);
		const Item *cached = cache.getptr(p_item.path);
		if (cached && cached->modified_time == _get_preview_modified_time(p_item.path)) {
			if (_cached_preview_suppresses_scene_generation(*cached)) {
				cached_item = *cached;
				has_cached_item = true;
			}
		} else if (cached) {
			cache.erase(p_item.path);
		}
	}
	if (has_cached_item) {
		if (p_item.callback.is_valid()) {
			p_item.callback.call_deferred(p_item.path, cached_item.preview, cached_item.small_preview);
		}
		return;
	}

	if (!p_item.cache_probed) {
		Ref<ImageTexture> texture;
		Ref<ImageTexture> small_texture;
		Dictionary preview_metadata;
		if (_load_cached_preview(p_item.path, texture, small_texture, preview_metadata)) {
			_preview_ready(p_item.path, 0, texture, small_texture, p_item.callback, preview_metadata);
			return;
		}
	}

	active_scene_preview = ActiveScenePreview();
	active_scene_preview.item = p_item;
	active_scene_preview.source_modified_time = _get_preview_modified_time(p_item.path);

	ScenePreviewScriptGuard script_guard;
	Error load_error = OK;
	Ref<PackedScene> packed_scene = ResourceLoader::load(p_item.path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP, &load_error);
	if (load_error != OK || packed_scene.is_null()) {
		_finish_scene_preview(Ref<Image>(), vformat("the scene could not be loaded (error %d)", load_error));
		return;
	}
	if (!packed_scene->can_instantiate() || !_is_scene_node_count_within_limit(packed_scene->get_state())) {
		_finish_scene_preview(Ref<Image>(), "the scene is invalid, cyclic, or exceeds the preview node limit");
		return;
	}

	HashSet<ObjectID> sanitized_resources;
	String failure_reason;
	if (!_sanitize_packed_scene(packed_scene, sanitized_resources, failure_reason)) {
		_finish_scene_preview(Ref<Image>(), failure_reason);
		return;
	}

	Node *scene = packed_scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
	if (!scene) {
		_finish_scene_preview(Ref<Image>(), "the stripped scene could not be instantiated");
		return;
	}
	if (Object::cast_to<Viewport>(scene)) {
		memdelete(scene);
		_finish_scene_preview(Ref<Image>(), "the scene root is a viewport");
		return;
	}

	int node_count = 0;
	if (!_sanitize_proxy_node(scene, node_count, failure_reason)) {
		memdelete(scene);
		_finish_scene_preview(Ref<Image>(), failure_reason);
		return;
	}

	int thumbnail_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	thumbnail_size = MAX(1, int(thumbnail_size * EDSCALE));
	SubViewport *viewport = memnew(SubViewport);
	viewport->set_size(Vector2i(thumbnail_size, thumbnail_size));
	viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	viewport->set_transparent_background(true);
	viewport->set_disable_input(true);
	viewport->set_handle_input_locally(false);
	viewport->set_physics_object_picking(false);
	viewport->set_as_audio_listener_2d(false);
	viewport->set_as_audio_listener_3d(false);
	viewport->set_use_own_world_3d(true);
	// Window children now survive sanitising (see _sanitize_proxy_node), so the preview viewport has to be
	// able to host them: without embedding, a Window here has no embedder and no OS window to attach to.
	viewport->set_embedding_subwindows(true);
	viewport->add_child(scene);
	active_scene_preview.viewport = viewport;
	add_child(viewport);

	node_count = 0;
	if (!_sanitize_proxy_node(scene, node_count, failure_reason)) {
		_finish_scene_preview(Ref<Image>(), failure_reason);
	}
}

void EditorResourcePreview::_setup_scene_preview_frame() {
	SubViewport *viewport = active_scene_preview.viewport;
	ERR_FAIL_NULL(viewport);
	ERR_FAIL_COND(viewport->get_child_count() == 0);
	Node *scene = viewport->get_child(0);

	AABB scene_aabb;
	Rect2 scene_rect;
	bool has_3d = false;
	bool has_2d = false;
	_find_scene_bounds(scene, scene_aabb, has_3d, scene_rect, has_2d);

	if (has_3d) {
		_hide_canvas_items(scene);
		viewport->set_disable_3d(false);

		Camera3D *camera = memnew(Camera3D);
		viewport->add_child(camera);
		const real_t radius = MAX(scene_aabb.size.length() * 0.5, real_t(0.01));
		const real_t fov = 30.0;
		const real_t distance = radius / Math::sin(Math::deg_to_rad(fov) * 0.5) * 1.15;
		const Vector3 center = scene_aabb.get_center();
		Transform3D camera_transform;
		camera_transform.origin = center + Vector3(1.0, 0.65, 1.0).normalized() * distance;
		camera_transform.set_look_at(camera_transform.origin, center, Vector3(0, 1, 0));
		camera->set_perspective(fov, MAX(0.01, distance - radius * 2.0), distance + radius * 4.0);
		camera->set_transform(camera_transform);
		camera->set_current(true);

		DirectionalLight3D *key_light = memnew(DirectionalLight3D);
		key_light->set_rotation_degrees(Vector3(-45, -35, 0));
		viewport->add_child(key_light);
		DirectionalLight3D *fill_light = memnew(DirectionalLight3D);
		fill_light->set_color(Color(0.65, 0.7, 0.8));
		fill_light->set_param(Light3D::PARAM_ENERGY, 0.7);
		fill_light->set_rotation_degrees(Vector3(35, 145, 0));
		viewport->add_child(fill_light);
	} else if (has_2d) {
		viewport->set_disable_3d(true);
		Camera2D *camera = memnew(Camera2D);
		viewport->add_child(camera);
		camera->set_position(scene_rect.get_center());
		const Vector2 viewport_size = viewport->get_size();
		const real_t zoom = CLAMP(0.9 * MIN(viewport_size.x / MAX(scene_rect.size.x, real_t(1.0)), viewport_size.y / MAX(scene_rect.size.y, real_t(1.0))), real_t(0.0001), real_t(10000.0));
		camera->set_zoom(Vector2(zoom, zoom));
		camera->set_enabled(true);
		camera->make_current();
	} else {
		_finish_scene_preview(Ref<Image>(), "the scene has no renderable 2D or 3D content");
		return;
	}

	viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	active_scene_preview.phase = SCENE_PREVIEW_PHASE_RENDER;
}

void EditorResourcePreview::_capture_scene_preview() {
	SubViewport *viewport = active_scene_preview.viewport;
	ERR_FAIL_NULL(viewport);
	Ref<ViewportTexture> viewport_texture = viewport->get_texture();
	Ref<Image> image = viewport_texture.is_valid() ? viewport_texture->get_image() : Ref<Image>();
	if (image.is_null() || image->is_empty() || image->is_invisible()) {
		_finish_scene_preview(Ref<Image>(), "the offscreen render produced no visible pixels");
		return;
	}
	_finish_scene_preview(image);
}

void EditorResourcePreview::_clear_active_scene_preview() {
	if (active_scene_preview.viewport) {
		remove_child(active_scene_preview.viewport);
		memdelete(active_scene_preview.viewport);
	}
	active_scene_preview = ActiveScenePreview();
}

bool EditorResourcePreview::_cached_preview_suppresses_scene_generation(const Item &p_item) {
	// A null entry the generic worker cached (it has no scene generator) must not
	// suppress generation; a scene-pipeline result, including a negative one, must.
	return p_item.preview.is_valid() || p_item.scene_preview_attempted;
}

void EditorResourcePreview::_scene_preview_ready(const String &p_path, const Ref<Texture2D> &p_texture, const Ref<Texture2D> &p_small_texture, const Callable &p_callback) {
	_preview_ready(p_path, 0, p_texture, p_small_texture, p_callback, Dictionary());
	MutexLock lock(preview_mutex);
	Item *cached = cache.getptr(p_path);
	if (cached) {
		cached->scene_preview_attempted = true;
	}
}

void EditorResourcePreview::_finish_scene_preview(const Ref<Image> &p_image, const String &p_failure_reason) {
	const SceneQueueItem item = active_scene_preview.item;
	const uint64_t source_modified_time = active_scene_preview.source_modified_time;
	_clear_active_scene_preview();

	if (p_image.is_null()) {
		if (!p_failure_reason.is_empty()) {
			print_verbose(vformat("Scene preview for '%s' was skipped: %s.", item.path, p_failure_reason));
		}
		_scene_preview_ready(item.path, Ref<Texture2D>(), Ref<Texture2D>(), item.callback);
		return;
	}

	// A save during generation bumps the mtime; content hashing is left to the cache write itself.
	if (_get_preview_modified_time(item.path) != source_modified_time) {
		if (item.callback.is_valid()) {
			item.callback.call_deferred(item.path, Ref<Texture2D>(), Ref<Texture2D>());
		}
		return;
	}

	Ref<Image> preview = p_image->duplicate();
	if (preview->is_compressed() && preview->decompress() != OK) {
		_scene_preview_ready(item.path, Ref<Texture2D>(), Ref<Texture2D>(), item.callback);
		return;
	}
	if (preview->get_format() != Image::FORMAT_RGBA8) {
		preview->convert(Image::FORMAT_RGBA8);
	}
	Ref<Image> small_preview = preview->duplicate();
	small_preview->resize(small_thumbnail_size, small_thumbnail_size, Image::INTERPOLATE_CUBIC);

	const Error save_error = save_preview_cache(item.path, preview, small_preview);
	if (save_error != OK) {
		print_verbose(vformat("Scene preview for '%s' could not be cached (error %d).", item.path, save_error));
		_scene_preview_ready(item.path, Ref<Texture2D>(), Ref<Texture2D>(), item.callback);
		return;
	}

	Ref<ImageTexture> texture = ImageTexture::create_from_image(preview);
	Ref<ImageTexture> small_texture = ImageTexture::create_from_image(small_preview);
	_scene_preview_ready(item.path, texture, small_texture, item.callback);
}

void EditorResourcePreview::_process_scene_preview() {
	DEV_ASSERT(Thread::get_caller_id() == Thread::get_main_id());
	if (active_scene_preview.viewport) {
		if (active_scene_preview.phase == SCENE_PREVIEW_PHASE_SETUP) {
			_setup_scene_preview_frame();
		} else {
			_capture_scene_preview();
		}
		return;
	}

	if (OS::get_singleton()->get_ticks_msec() < scene_preview_defer_until_ms) {
		return;
	}

	SceneQueueItem item;
	bool has_item = false;
	{
		MutexLock lock(preview_mutex);
		if (!scene_generate_queue.is_empty()) {
			item = scene_generate_queue.front()->get();
			scene_generate_queue.pop_front();
			has_item = true;
		}
	}
	if (!has_item && !thread.is_started()) {
		// Without a worker (OpenGL and tests), the main thread drains scene_queue directly.
		has_item = _pop_scene_preview(item);
	}
	if (has_item) {
		_begin_scene_preview(item);
	}
}

void EditorResourcePreview::defer_scene_preview_generation(double p_seconds) {
	ERR_FAIL_COND(p_seconds < 0.0);
	const uint64_t until_ms = OS::get_singleton()->get_ticks_msec() + uint64_t(p_seconds * 1000.0);
	scene_preview_defer_until_ms = MAX(scene_preview_defer_until_ms, until_ms);
}

Error EditorResourcePreview::save_preview_cache(const String &p_path, const Ref<Image> &p_preview, const Ref<Image> &p_small_preview, const Dictionary &p_metadata) {
	ERR_FAIL_COND_V(p_path.is_empty(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_preview.is_null() || p_preview->is_empty(), ERR_INVALID_PARAMETER);

	MutexLock lock(preview_mutex);

	String cache_base = ProjectSettings::get_singleton()->globalize_path(p_path).md5_text();
	cache_base = EditorPaths::get_singleton()->get_cache_dir().path_join("resthumb-" + cache_base);

	const String temporary_suffix = ".tmp-" + itos(OS::get_singleton()->get_ticks_usec());
	const String preview_path = cache_base + ".png";
	const String small_preview_path = cache_base + "_small.png";
	const String metadata_path = cache_base + ".txt";
	const String temporary_preview_path = cache_base + temporary_suffix + ".png";
	const String temporary_small_preview_path = cache_base + temporary_suffix + "_small.png";
	const String temporary_metadata_path = cache_base + temporary_suffix + ".txt";

	auto cleanup_temporary_files = [&]() {
		if (FileAccess::exists(temporary_preview_path)) {
			DirAccess::remove_absolute(temporary_preview_path);
		}
		if (FileAccess::exists(temporary_small_preview_path)) {
			DirAccess::remove_absolute(temporary_small_preview_path);
		}
		if (FileAccess::exists(temporary_metadata_path)) {
			DirAccess::remove_absolute(temporary_metadata_path);
		}
	};

	Error err = p_preview->save_png(temporary_preview_path);
	if (err != OK) {
		cleanup_temporary_files();
		return err;
	}

	const bool has_small_preview = p_small_preview.is_valid() && !p_small_preview->is_empty();
	if (has_small_preview) {
		err = p_small_preview->save_png(temporary_small_preview_path);
		if (err != OK) {
			cleanup_temporary_files();
			return err;
		}
	}

	Ref<FileAccess> metadata_file = FileAccess::open(temporary_metadata_path, FileAccess::WRITE, &err);
	if (metadata_file.is_null()) {
		cleanup_temporary_files();
		return err;
	}
	int thumbnail_size = EDITOR_GET("filesystem/file_dialog/thumbnail_size");
	thumbnail_size *= EDSCALE;
	_write_preview_cache(metadata_file, thumbnail_size, has_small_preview, _get_preview_modified_time(p_path), FileAccess::get_md5(p_path), p_metadata);
	metadata_file.unref();

	// The metadata file is the cache's commit marker. Remove it before replacing the images, and
	// publish the new metadata only after every image is in place, so readers never accept a partial set.
	if (FileAccess::exists(metadata_path)) {
		err = DirAccess::remove_absolute(metadata_path);
		if (err != OK) {
			cleanup_temporary_files();
			return err;
		}
	}
	err = DirAccess::rename_absolute(temporary_preview_path, preview_path);
	if (err != OK) {
		cleanup_temporary_files();
		return err;
	}
	if (has_small_preview) {
		err = DirAccess::rename_absolute(temporary_small_preview_path, small_preview_path);
		if (err != OK) {
			cleanup_temporary_files();
			return err;
		}
	} else if (FileAccess::exists(small_preview_path)) {
		DirAccess::remove_absolute(small_preview_path);
	}
	err = DirAccess::rename_absolute(temporary_metadata_path, metadata_path);
	if (err != OK) {
		cleanup_temporary_files();
		return err;
	}

	const Item *cached_item = cache.getptr(p_path);
	if (cached_item && cached_item->preview.is_null()) {
		cache.erase(p_path);
	}

	return OK;
}

void EditorResourcePreview::_write_preview_cache(Ref<FileAccess> p_file, int p_thumbnail_size, bool p_has_small_texture, uint64_t p_modified_time, const String &p_hash, const Dictionary &p_metadata) {
	p_file->store_line(itos(p_thumbnail_size));
	p_file->store_line(itos(p_has_small_texture));
	p_file->store_line(itos(p_modified_time));
	p_file->store_line(p_hash);
	p_file->store_line(VariantUtilityFunctions::var_to_str(p_metadata).replace_char('\n', ' '));
	p_file->store_line(itos(CURRENT_METADATA_VERSION));
}

void EditorResourcePreview::_read_preview_cache(Ref<FileAccess> p_file, int *r_thumbnail_size, bool *r_has_small_texture, uint64_t *r_modified_time, String *r_hash, Dictionary *r_metadata, bool *r_outdated) {
	*r_thumbnail_size = p_file->get_line().to_int();
	*r_has_small_texture = p_file->get_line().to_int();
	*r_modified_time = p_file->get_line().to_int();
	*r_hash = p_file->get_line();
	*r_metadata = VariantUtilityFunctions::str_to_var(p_file->get_line());
	*r_outdated = p_file->get_line().to_int() < CURRENT_METADATA_VERSION;
}

void EditorResourcePreview::_thread() {
	exited.clear();
	while (!exiting.is_set()) {
		preview_sem.wait();
		_iterate();
	}
	exited.set();
}

void EditorResourcePreview::_idle_callback() {
	if (!singleton) {
		// Just in case the shutdown of the editor involves the deletion of the singleton
		// happening while additional idle callbacks can happen.
		return;
	}

	// Process preview tasks, trying to leave a little bit of responsiveness worst case.
	uint64_t start = OS::get_singleton()->get_ticks_msec();
	while (!singleton->queue.is_empty() && OS::get_singleton()->get_ticks_msec() - start < 100) {
		singleton->_iterate();
	}
}

void EditorResourcePreview::_update_thumbnail_sizes() {
	if (small_thumbnail_size == -1) {
		// Kind of a workaround to retrieve the default icon size.
		small_thumbnail_size = EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Object"), EditorStringName(EditorIcons))->get_width();
	}
}

EditorResourcePreview::PreviewItem EditorResourcePreview::get_resource_preview_if_available(const String &p_path) {
	PreviewItem item;
	{
		MutexLock lock(preview_mutex);

		HashMap<String, EditorResourcePreview::Item>::Iterator I = cache.find(p_path);
		if (!I) {
			return item;
		}

		EditorResourcePreview::Item &cached_item = I->value;
		item.preview = cached_item.preview;
		item.small_preview = cached_item.small_preview;
	}
	preview_sem.post();
	return item;
}

void EditorResourcePreview::_queue_edited_resource_preview(const Ref<Resource> &p_res, Object *p_receiver, const StringName &p_receiver_func, const Variant &p_userdata) {
	ERR_FAIL_NULL(p_receiver);
	queue_edited_resource_preview(p_res, Callable(p_receiver, p_receiver_func).bind(p_userdata));
}

void EditorResourcePreview::queue_edited_resource_preview(const Ref<Resource> &p_res, const Callable &p_callback) {
	ERR_FAIL_COND(p_res.is_null());
	_update_thumbnail_sizes();

	{
		MutexLock lock(preview_mutex);

		String path_id = "ID:" + itos(p_res->get_instance_id());
		HashMap<String, EditorResourcePreview::Item>::Iterator I = cache.find(path_id);

		if (I && I->value.last_hash == p_res->hash_edited_version_for_preview()) {
			p_callback.call(path_id, I->value.preview, I->value.small_preview);
			return;
		}

		if (I) {
			cache.remove(I); // Erase if exists, since it will be regen.
		}

		QueueItem item;
		item.resource = p_res;
		item.path = path_id;
		item.callback = p_callback;
		queue.push_back(item);
	}
	preview_sem.post();
}

void EditorResourcePreview::_queue_resource_preview(const String &p_path, Object *p_receiver, const StringName &p_receiver_func, const Variant &p_userdata) {
	ERR_FAIL_NULL(p_receiver);
	queue_resource_preview(p_path, Callable(p_receiver, p_receiver_func).bind(p_userdata));
}

void EditorResourcePreview::queue_resource_preview(const String &p_path, const Callable &p_callback) {
	_update_thumbnail_sizes();

	{
		MutexLock lock(preview_mutex);

		const Item *cached_item = cache.getptr(p_path);
		if (cached_item) {
			p_callback.call(p_path, cached_item->preview, cached_item->small_preview);
			return;
		}

		QueueItem item;
		item.path = p_path;
		item.callback = p_callback;
		queue.push_back(item);
	}
	preview_sem.post();
}

void EditorResourcePreview::queue_scene_preview(const String &p_path, const Callable &p_callback, PreviewPriority p_priority) {
	ERR_FAIL_COND(p_path.is_empty());
	ERR_FAIL_COND(!p_callback.is_valid());
	ERR_FAIL_INDEX(int(p_priority), int(PREVIEW_PRIORITY_HIGH) + 1);

	Item cached_item;
	bool has_cached_item = false;
	{
		MutexLock lock(preview_mutex);
		const Item *cached = cache.getptr(p_path);
		if (cached && cached->modified_time != _get_preview_modified_time(p_path)) {
			cache.erase(p_path);
		} else if (cached && _cached_preview_suppresses_scene_generation(*cached)) {
			cached_item = *cached;
			has_cached_item = true;
		}
	}
	if (has_cached_item) {
		p_callback.call(p_path, cached_item.preview, cached_item.small_preview);
		return;
	}

	_update_thumbnail_sizes();
	SceneQueueItem item;
	item.path = p_path;
	item.callback = p_callback;
	item.priority = p_priority;
	_queue_scene_preview(item);
}

void EditorResourcePreview::cancel_scene_preview(const String &p_path, const Callable &p_callback) {
	MutexLock lock(preview_mutex);
	for (List<SceneQueueItem>::Element *element = scene_queue.front(); element;) {
		List<SceneQueueItem>::Element *next = element->next();
		if (element->get().path == p_path && element->get().callback == p_callback) {
			scene_queue.erase(element);
		}
		element = next;
	}
	for (List<SceneQueueItem>::Element *element = scene_generate_queue.front(); element;) {
		List<SceneQueueItem>::Element *next = element->next();
		if (element->get().path == p_path && element->get().callback == p_callback) {
			scene_generate_queue.erase(element);
		}
		element = next;
	}
	if (scene_probe_active && scene_probe_item.path == p_path && scene_probe_item.callback == p_callback) {
		scene_probe_item.callback = Callable();
	}
	if (active_scene_preview.item.path == p_path && active_scene_preview.item.callback == p_callback) {
		active_scene_preview.item.callback = Callable();
	}
}

void EditorResourcePreview::add_preview_generator(const Ref<EditorResourcePreviewGenerator> &p_generator) {
	preview_generators.push_back(p_generator);
}

void EditorResourcePreview::remove_preview_generator(const Ref<EditorResourcePreviewGenerator> &p_generator) {
	preview_generators.erase(p_generator);
}

EditorResourcePreview *EditorResourcePreview::get_singleton() {
	return singleton;
}

void EditorResourcePreview::_bind_methods() {
	ClassDB::bind_method(D_METHOD("queue_resource_preview", "path", "receiver", "receiver_func", "userdata"), &EditorResourcePreview::_queue_resource_preview);
	ClassDB::bind_method(D_METHOD("queue_edited_resource_preview", "resource", "receiver", "receiver_func", "userdata"), &EditorResourcePreview::_queue_edited_resource_preview);
	ClassDB::bind_method(D_METHOD("add_preview_generator", "generator"), &EditorResourcePreview::add_preview_generator);
	ClassDB::bind_method(D_METHOD("remove_preview_generator", "generator"), &EditorResourcePreview::remove_preview_generator);
	ClassDB::bind_method(D_METHOD("check_for_invalidation", "path"), &EditorResourcePreview::check_for_invalidation);

	ADD_SIGNAL(MethodInfo("preview_invalidated", PropertyInfo(Variant::STRING, "path")));
}

void EditorResourcePreview::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			_process_scene_preview();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;
	}
}

void EditorResourcePreview::check_for_invalidation(const String &p_path) {
	bool call_invalidated = false;
	{
		MutexLock lock(preview_mutex);

		if (cache.has(p_path)) {
			if (_get_preview_modified_time(p_path) != cache[p_path].modified_time) {
				cache.erase(p_path);
				call_invalidated = true;
			}
		}
		for (List<SceneQueueItem>::Element *element = scene_queue.front(); element;) {
			List<SceneQueueItem>::Element *next = element->next();
			if (element->get().path == p_path) {
				scene_queue.erase(element);
				call_invalidated = true;
			}
			element = next;
		}
		for (List<SceneQueueItem>::Element *element = scene_generate_queue.front(); element;) {
			List<SceneQueueItem>::Element *next = element->next();
			if (element->get().path == p_path) {
				scene_generate_queue.erase(element);
				call_invalidated = true;
			}
			element = next;
		}
		if (scene_probe_active && scene_probe_item.path == p_path) {
			scene_probe_item.callback = Callable();
			call_invalidated = true;
		}
		if (active_scene_preview.item.path == p_path) {
			active_scene_preview.item.callback = Callable();
			call_invalidated = true;
		}
	}

	if (call_invalidated) { //do outside mutex
		call_deferred(SNAME("emit_signal"), "preview_invalidated", p_path);
	}
}

void EditorResourcePreview::start() {
	if (DisplayServer::get_singleton()->get_name() != "headless") {
		set_process_internal(true);
	}

	if (is_threaded()) {
		ERR_FAIL_COND_MSG(thread.is_started(), "Thread already started.");
		thread.start(_thread_func, this);
	} else {
		SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
		ERR_FAIL_NULL_MSG(st, "Editor's MainLoop is not a SceneTree. This is a bug.");
		st->add_idle_callback(&_idle_callback);
	}
}

void EditorResourcePreview::stop() {
	set_process_internal(false);
	_clear_active_scene_preview();
	if (is_threaded()) {
		if (thread.is_started()) {
			exiting.set();
			preview_sem.post();

			for (int i = 0; i < preview_generators.size(); i++) {
				preview_generators.write[i]->abort();
			}

			while (!exited.is_set()) {
				// Sync pending work.
				OS::get_singleton()->delay_usec(10000);
				RenderingServer::get_singleton()->sync();
				MessageQueue::get_singleton()->flush();
			}

			thread.wait_to_finish();
		}
	}
	// After the join, so a mid-probe worker cannot repopulate the generate queue behind the clear.
	{
		MutexLock lock(preview_mutex);
		scene_queue.clear();
		scene_generate_queue.clear();
	}
}

EditorResourcePreview::EditorResourcePreview() {
	singleton = this;
}

EditorResourcePreview::~EditorResourcePreview() {
	stop();
}
