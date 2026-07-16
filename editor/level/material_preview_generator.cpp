/**************************************************************************/
/*  material_preview_generator.cpp                                        */
/**************************************************************************/
/*  G-Level LE2: private flat-material preview queue and cache.           */
/**************************************************************************/

#include "material_preview_generator.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/file_system/editor_paths.h"
#include "editor/inspector/editor_resource_preview.h"
#include "editor/level/texel_density_scanner.h"
#include "scene/resources/image_texture.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

class MaterialFlatPreviewGenerator : public EditorResourcePreviewGenerator {
	RID scenario;
	RID quad;
	RID quad_instance;
	RID viewport;
	RID viewport_texture;
	RID light;
	RID light_instance;
	RID light2;
	RID light_instance2;
	RID camera;
	RID camera_attributes;
	mutable DrawRequester draw_requester;
	bool available = false;

public:
	bool is_available() const { return available; }

	Ref<Texture2D> generate_flat(const Ref<Material> &p_material, const Size2i &p_dimensions, int p_size) const {
		if (!available || p_material.is_null() || p_material->get_shader_mode() != Shader::MODE_SPATIAL) {
			return Ref<Texture2D>();
		}
		RenderingServer *rs = RS::get_singleton();
		ERR_FAIL_NULL_V(rs, Ref<Texture2D>());

		const float texture_aspect = p_dimensions.y > 0 ? float(p_dimensions.x) / float(p_dimensions.y) : 1.0f;
		const Vector3 scale = texture_aspect >= 1.0f ? Vector3(1.0f, 1.0f / texture_aspect, 1.0f) : Vector3(texture_aspect, 1.0f, 1.0f);
		rs->viewport_set_size(viewport, p_size, p_size);
		rs->instance_set_transform(quad_instance, Transform3D(Basis().scaled(scale), Vector3()));
		rs->mesh_surface_set_material(quad, 0, p_material->get_rid());
		draw_requester.request_and_wait(viewport);
		Ref<Image> image = rs->texture_2d_get(viewport_texture);
		rs->mesh_surface_set_material(quad, 0, RID());
		if (image.is_null()) {
			return Ref<Texture2D>();
		}
		image->convert(Image::FORMAT_RGBA8);
		if (image->get_width() != p_size || image->get_height() != p_size) {
			image->resize(p_size, p_size, Image::INTERPOLATE_CUBIC);
		}
		return ImageTexture::create_from_image(image);
	}

	void abort() override {
		draw_requester.abort();
	}

	MaterialFlatPreviewGenerator() {
		RenderingServer *rs = RS::get_singleton();
		DisplayServer *display = DisplayServer::get_singleton();
		if (!rs || !display || display->get_name() == "headless") {
			return;
		}

		scenario = rs->scenario_create();
		viewport = rs->viewport_create();
		rs->viewport_set_update_mode(viewport, RSE::VIEWPORT_UPDATE_DISABLED);
		rs->viewport_set_scenario(viewport, scenario);
		rs->viewport_set_size(viewport, MaterialBrowserPreviewQueue::THUMBNAIL_SIZE, MaterialBrowserPreviewQueue::THUMBNAIL_SIZE);
		rs->viewport_set_transparent_background(viewport, false);
		rs->viewport_set_active(viewport, true);
		viewport_texture = rs->viewport_get_texture(viewport);

		camera = rs->camera_create();
		rs->viewport_attach_camera(viewport, camera);
		rs->camera_set_transform(camera, Transform3D(Basis(), Vector3(0, 0, 2)));
		rs->camera_set_orthogonal(camera, 1.02f, 0.01f, 10.0f);
		if (GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units")) {
			camera_attributes = rs->camera_attributes_create();
			rs->camera_attributes_set_exposure(camera_attributes, 1.0, 0.000032552);
			rs->camera_set_camera_attributes(camera, camera_attributes);
		}

		light = rs->directional_light_create();
		light_instance = rs->instance_create2(light, scenario);
		rs->instance_set_transform(light_instance, Transform3D().looking_at(Vector3(-1, -1, -1), Vector3(0, 1, 0)));
		light2 = rs->directional_light_create();
		rs->light_set_color(light2, Color(0.7, 0.7, 0.7));
		light_instance2 = rs->instance_create2(light2, scenario);
		rs->instance_set_transform(light_instance2, Transform3D().looking_at(Vector3(0, 1, 0), Vector3(0, 0, 1)));

		// Clockwise as seen from the camera at +Z — Godot front faces are
		// clockwise, so CCW here backface-culls every opaque material to a
		// black tile (only cull-disabled foliage survived).
		Vector<Vector3> vertices;
		vertices.push_back(Vector3(-0.5f, -0.5f, 0));
		vertices.push_back(Vector3(0.5f, 0.5f, 0));
		vertices.push_back(Vector3(0.5f, -0.5f, 0));
		vertices.push_back(Vector3(-0.5f, -0.5f, 0));
		vertices.push_back(Vector3(-0.5f, 0.5f, 0));
		vertices.push_back(Vector3(0.5f, 0.5f, 0));
		Vector<Vector3> normals;
		Vector<Vector2> uvs;
		Vector<real_t> tangents;
		static const Vector2 uv_values[6] = {
			Vector2(0, 1), Vector2(1, 0), Vector2(1, 1),
			Vector2(0, 1), Vector2(0, 0), Vector2(1, 0)
		};
		for (int i = 0; i < 6; i++) {
			normals.push_back(Vector3(0, 0, 1));
			uvs.push_back(uv_values[i]);
			tangents.push_back(1.0f);
			tangents.push_back(0.0f);
			tangents.push_back(0.0f);
			tangents.push_back(1.0f);
		}
		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		arrays[RSE::ARRAY_VERTEX] = vertices;
		arrays[RSE::ARRAY_NORMAL] = normals;
		arrays[RSE::ARRAY_TANGENT] = tangents;
		arrays[RSE::ARRAY_TEX_UV] = uvs;
		quad = rs->mesh_create();
		rs->mesh_add_surface_from_arrays(quad, RSE::PRIMITIVE_TRIANGLES, arrays);
		quad_instance = rs->instance_create2(quad, scenario);
		available = true;
	}

	~MaterialFlatPreviewGenerator() {
		RenderingServer *rs = RS::get_singleton();
		if (!rs) {
			return;
		}
		const RID rids[] = { quad, quad_instance, viewport, light, light_instance, light2, light_instance2, camera, camera_attributes, scenario };
		for (const RID &rid : rids) {
			if (rid.is_valid()) {
				rs->free_rid(rid);
			}
		}
	}
};

void MaterialBrowserPreviewQueue::_thread_func(void *p_userdata) {
	static_cast<MaterialBrowserPreviewQueue *>(p_userdata)->_thread_loop();
}

void MaterialBrowserPreviewQueue::_thread_loop() {
	while (true) {
		semaphore.wait();
		{
			MutexLock lock(mutex);
			if (stopping) {
				return;
			}
		}
		PreviewRequest request;
		while (_pop_request(request)) {
			_finish_request(request, _process_request(request));
		}
	}
}

void MaterialBrowserPreviewQueue::_deferred_iterate() {
	{
		MutexLock lock(mutex);
		deferred_iteration_queued = false;
		if (stopping) {
			return;
		}
	}
	PreviewRequest request;
	if (_pop_request(request)) {
		_finish_request(request, _process_request(request));
	}
	MutexLock lock(mutex);
	if (!requests.is_empty() && !deferred_iteration_queued) {
		deferred_iteration_queued = true;
		callable_mp(this, &MaterialBrowserPreviewQueue::_deferred_iterate).call_deferred();
	}
}

void MaterialBrowserPreviewQueue::_schedule_iteration() {
	if (use_background_thread) {
		semaphore.post();
		return;
	}
	if (!deferred_iteration_queued) {
		deferred_iteration_queued = true;
		callable_mp(this, &MaterialBrowserPreviewQueue::_deferred_iterate).call_deferred();
	}
}

bool MaterialBrowserPreviewQueue::_pop_request(PreviewRequest &r_request) {
	MutexLock lock(mutex);
	if (requests.is_empty() || stopping) {
		return false;
	}
	r_request = requests.front()->get();
	requests.pop_front();
	return true;
}

Ref<Texture2D> MaterialBrowserPreviewQueue::_process_request(const PreviewRequest &p_request) {
	Ref<Texture2D> texture = _load_disk_cache(p_request.path);
	if (texture.is_valid() || generator.is_null() || !generator->is_available()) {
		return texture;
	}
	Ref<Material> material = ResourceLoader::load(p_request.path);
	if (material.is_null()) {
		return Ref<Texture2D>();
	}
	Size2i dimensions(1, 1);
	if (scanner.is_valid()) {
		const std::optional<TexelDensityResult> scan = scanner->scan(material, p_request.path);
		if (scan.has_value()) {
			dimensions = scan->dimensions;
		}
	}
	texture = generator->generate_flat(material, dimensions, THUMBNAIL_SIZE);
	if (texture.is_valid()) {
		_store_disk_cache(p_request.path, texture);
	}
	return texture;
}

void MaterialBrowserPreviewQueue::_finish_request(const PreviewRequest &p_request, const Ref<Texture2D> &p_texture) {
	Vector<Callable> ready_callbacks;
	bool requeue = false;
	{
		MutexLock lock(mutex);
		const uint64_t *revision = revisions.getptr(p_request.path);
		const uint64_t current_revision = revision ? *revision : 0;
		pending_paths.erase(p_request.path);
		if (current_revision != p_request.revision) {
			if (callbacks.has(p_request.path)) {
				PreviewRequest replacement;
				replacement.path = p_request.path;
				replacement.revision = current_revision;
				requests.push_back(replacement);
				pending_paths.insert(p_request.path);
				requeue = true;
			}
		} else {
			memory_cache.insert(p_request.path, p_texture);
			completed_paths.insert(p_request.path);
			Vector<Callable> *stored_callbacks = callbacks.getptr(p_request.path);
			if (stored_callbacks) {
				ready_callbacks = *stored_callbacks;
				callbacks.erase(p_request.path);
			}
		}
	}
	for (const Callable &callback : ready_callbacks) {
		callback.call_deferred(p_request.path, p_texture);
	}
	if (requeue) {
		MutexLock lock(mutex);
		_schedule_iteration();
	}
}

String MaterialBrowserPreviewQueue::_cache_base(const String &p_path) {
	EditorPaths *paths = EditorPaths::get_singleton();
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (!paths || !project_settings) {
		return String();
	}
	// v2: quad-winding fix — the version bump orphans v1's backface-culled
	// (black) thumbnails instead of serving them from the mtime-valid cache.
	return paths->get_cache_dir().path_join("matbrowser2-" + project_settings->globalize_path(p_path).md5_text());
}

uint64_t MaterialBrowserPreviewQueue::_modified_time(const String &p_path) {
	uint64_t modified = FileAccess::get_modified_time(p_path);
	const String import_path = p_path + ".import";
	if (FileAccess::exists(import_path)) {
		modified = MAX(modified, FileAccess::get_modified_time(import_path));
	}
	return modified;
}

Ref<Texture2D> MaterialBrowserPreviewQueue::_load_disk_cache(const String &p_path) {
	const String base = _cache_base(p_path);
	if (base.is_empty()) {
		return Ref<Texture2D>();
	}
	Ref<FileAccess> metadata = FileAccess::open(base + ".txt", FileAccess::READ);
	if (metadata.is_null()) {
		return Ref<Texture2D>();
	}
	const int stored_size = metadata->get_line().to_int();
	const uint64_t stored_modified = metadata->get_line().to_int();
	const String stored_md5 = metadata->get_line();
	const uint64_t current_modified = _modified_time(p_path);
	if (stored_size != THUMBNAIL_SIZE) {
		return Ref<Texture2D>();
	}
	if (stored_modified != current_modified) {
		const String current_md5 = FileAccess::get_md5(p_path);
		if (current_md5 != stored_md5) {
			return Ref<Texture2D>();
		}
		Ref<FileAccess> update = FileAccess::open(base + ".txt", FileAccess::WRITE);
		if (update.is_valid()) {
			update->store_line(itos(THUMBNAIL_SIZE));
			update->store_line(uitos(current_modified));
			update->store_line(current_md5);
		}
	}
	Ref<Image> image;
	image.instantiate();
	if (image->load(base + ".png") != OK) {
		return Ref<Texture2D>();
	}
	return ImageTexture::create_from_image(image);
}

void MaterialBrowserPreviewQueue::_store_disk_cache(const String &p_path, const Ref<Texture2D> &p_texture) {
	const String base = _cache_base(p_path);
	if (base.is_empty() || p_texture.is_null()) {
		return;
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null() || image->save_png(base + ".png") != OK) {
		return;
	}
	Ref<FileAccess> metadata = FileAccess::open(base + ".txt", FileAccess::WRITE);
	if (metadata.is_null()) {
		return;
	}
	metadata->store_line(itos(THUMBNAIL_SIZE));
	metadata->store_line(uitos(_modified_time(p_path)));
	metadata->store_line(FileAccess::get_md5(p_path));
}

void MaterialBrowserPreviewQueue::_remove_disk_cache(const String &p_path) {
	const String base = _cache_base(p_path);
	if (base.is_empty()) {
		return;
	}
	DirAccess::remove_absolute(base + ".png");
	DirAccess::remove_absolute(base + ".txt");
}

void MaterialBrowserPreviewQueue::_bind_methods() {
	ClassDB::bind_method(D_METHOD("request", "path"), &MaterialBrowserPreviewQueue::request_for_test);
	ClassDB::bind_method(D_METHOD("invalidate", "path"), &MaterialBrowserPreviewQueue::invalidate);
	ClassDB::bind_method(D_METHOD("get_cached_preview", "path"), &MaterialBrowserPreviewQueue::get_cached_preview);
	ClassDB::bind_method(D_METHOD("has_completed", "path"), &MaterialBrowserPreviewQueue::has_completed);
	ClassDB::bind_method(D_METHOD("get_pending_count"), &MaterialBrowserPreviewQueue::get_pending_count);
	ClassDB::bind_method(D_METHOD("is_rendering_available"), &MaterialBrowserPreviewQueue::is_rendering_available);
}

void MaterialBrowserPreviewQueue::initialize(const Ref<TexelDensityScanner> &p_scanner) {
	if (initialized) {
		return;
	}
	initialized = true;
	scanner = p_scanner;
	generator.instantiate();
	EditorResourcePreview *shared_preview = EditorResourcePreview::get_singleton();
	use_background_thread = !generator->is_available() || (shared_preview && shared_preview->is_threaded());
	if (use_background_thread) {
		thread.start(_thread_func, this);
	}
}

void MaterialBrowserPreviewQueue::request_preview(const String &p_path, const Callable &p_callback) {
	ERR_FAIL_COND(p_path.is_empty());
	Ref<Texture2D> cached;
	bool cache_hit = false;
	bool schedule = false;
	{
		MutexLock lock(mutex);
		const Ref<Texture2D> *cached_ptr = memory_cache.getptr(p_path);
		if (cached_ptr) {
			cached = *cached_ptr;
			cache_hit = true;
		} else {
			if (p_callback.is_valid()) {
				callbacks[p_path].push_back(p_callback);
			}
			if (!pending_paths.has(p_path)) {
				PreviewRequest request;
				request.path = p_path;
				const uint64_t *revision = revisions.getptr(p_path);
				request.revision = revision ? *revision : 0;
				requests.push_back(request);
				pending_paths.insert(p_path);
				schedule = true;
			}
		}
		if (schedule) {
			_schedule_iteration();
		}
	}
	if (cache_hit) {
		if (p_callback.is_valid()) {
			p_callback.call_deferred(p_path, cached);
		}
	}
}

void MaterialBrowserPreviewQueue::invalidate(const String &p_path) {
	{
		MutexLock lock(mutex);
		const uint64_t *revision = revisions.getptr(p_path);
		revisions[p_path] = (revision ? *revision : 0) + 1;
		memory_cache.erase(p_path);
		completed_paths.erase(p_path);
	}
	_remove_disk_cache(p_path);
}

Ref<Texture2D> MaterialBrowserPreviewQueue::get_cached_preview(const String &p_path) {
	MutexLock lock(mutex);
	const Ref<Texture2D> *cached = memory_cache.getptr(p_path);
	return cached ? *cached : Ref<Texture2D>();
}

bool MaterialBrowserPreviewQueue::has_completed(const String &p_path) const {
	MutexLock lock(mutex);
	return completed_paths.has(p_path);
}

int MaterialBrowserPreviewQueue::get_pending_count() const {
	MutexLock lock(mutex);
	return pending_paths.size();
}

bool MaterialBrowserPreviewQueue::is_rendering_available() const {
	return generator.is_valid() && generator->is_available();
}

void MaterialBrowserPreviewQueue::stop() {
	{
		MutexLock lock(mutex);
		if (stopping) {
			return;
		}
		stopping = true;
	}
	if (thread.is_started()) {
		if (generator.is_valid()) {
			generator->abort();
		}
		semaphore.post();
		thread.wait_to_finish();
	}
}

MaterialBrowserPreviewQueue::~MaterialBrowserPreviewQueue() {
	stop();
	generator.unref();
}
