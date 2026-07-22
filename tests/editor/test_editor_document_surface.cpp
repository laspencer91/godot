/**************************************************************************/
/*  test_editor_document_surface.cpp                                      */
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

TEST_FORCE_LINK(test_editor_document_surface)

#ifdef TOOLS_ENABLED
#include "editor/editor_document.h"
#include "editor/gui/editor_document_surface.h"
#include "scene/gui/control.h"

namespace TestEditorDocumentSurface {

static int created_instance_count = 0;
static bool cleanup_called[2] = {};

class TestSurfaceInstance : public EditorDocumentSurfaceInstance {
	int index = 0;

public:
	virtual void pre_delete_cleanup() override {
		cleanup_called[index] = true;
	}

	TestSurfaceInstance(int p_index) {
		index = p_index;
		set_root_control(memnew(Control));
	}
};

class TestSurfaceProvider : public EditorDocumentSurfaceProvider {
public:
	virtual StringName get_surface_id() const override { return SNAME("test_document_surface"); }

	virtual bool supports(EditorDocument *p_document) const override {
		return p_document && p_document->get_type() == EditorDocument::TYPE_UNKNOWN;
	}

	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override {
		if (!supports(p_context.document) || created_instance_count >= 2) {
			return nullptr;
		}
		const int index = created_instance_count++;
		return memnew(TestSurfaceInstance(index));
	}
};

class RoutingSurfaceProvider : public EditorDocumentSurfaceProvider {
	StringName surface_id;

public:
	virtual StringName get_surface_id() const override { return surface_id; }
	virtual bool supports(EditorDocument *p_document) const override { return true; }
	virtual EditorDocumentSurfaceInstance *create(const EditorDocumentSurfaceContext &p_context) const override { return nullptr; }

	RoutingSurfaceProvider(const StringName &p_surface_id) {
		surface_id = p_surface_id;
	}
};

TEST_CASE("[Editor][EditorDocumentSurface] Providers create independent instances and can be removed safely") {
	EditorDocumentSurfaceRegistry *registry = EditorDocumentSurfaceRegistry::get_singleton();
	REQUIRE(registry != nullptr);

	created_instance_count = 0;
	cleanup_called[0] = false;
	cleanup_called[1] = false;
	EditorDocument document;
	TestSurfaceProvider *provider = memnew(TestSurfaceProvider);
	REQUIRE(registry->register_provider(provider));

	EditorDocumentSurfaceProvider *resolved = registry->resolve_surface(SNAME("test_document_surface"), &document);
	REQUIRE(resolved == provider);
	EditorDocumentSurfaceContext context;
	context.document = &document;
	EditorDocumentSurfaceInstance *first = resolved->create(context);
	EditorDocumentSurfaceInstance *second = resolved->create(context);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	CHECK(first != second);
	CHECK(first->get_root_control() != second->get_root_control());

	// CSG-3A: Unregister and destroy the stateless provider before its live instances.
	// Their cleanup virtuals must have no provider dependency or stale registry pointer.
	CHECK(registry->unregister_provider(provider->get_surface_id(), provider));
	CHECK(registry->resolve_surface(SNAME("test_document_surface"), &document) == nullptr);
	memdelete(provider);

	Control *first_root = first->get_root_control();
	Control *second_root = second->get_root_control();
	first->pre_delete_cleanup();
	second->pre_delete_cleanup();
	CHECK(cleanup_called[0]);
	CHECK(cleanup_called[1]);
	memdelete(first);
	memdelete(second);
	memdelete(first_root);
	memdelete(second_root);
}

TEST_CASE("[Editor][EditorDocumentSurface] Default surface IDs preserve DocumentView routing") {
	EditorDocumentSurfaceRegistry registry;
	RoutingSurfaceProvider resource(SNAME("resource"));
	RoutingSurfaceProvider script(SNAME("script"));
	RoutingSurfaceProvider shader(SNAME("shader"));
	RoutingSurfaceProvider help(SNAME("help"));
	RoutingSurfaceProvider screen_host(SNAME("screen_host"));
	RoutingSurfaceProvider scene(SNAME("scene"));
	REQUIRE(registry.register_provider(&resource));
	REQUIRE(registry.register_provider(&script));
	REQUIRE(registry.register_provider(&shader));
	REQUIRE(registry.register_provider(&help));
	REQUIRE(registry.register_provider(&screen_host));
	REQUIRE(registry.register_provider(&scene));

	EditorDocument document;
	document.set_type(EditorDocument::TYPE_RESOURCE);
	CHECK(registry.resolve_default_surface(&document) == &resource);
	document.set_type(EditorDocument::TYPE_SCRIPT);
	CHECK(registry.resolve_default_surface(&document) == &script);
	document.set_type(EditorDocument::TYPE_SHADER);
	CHECK(registry.resolve_default_surface(&document) == &shader);
	document.set_type(EditorDocument::TYPE_HELP);
	CHECK(registry.resolve_default_surface(&document) == &help);
	document.set_type(EditorDocument::TYPE_SCREEN_HOST);
	CHECK(registry.resolve_default_surface(&document) == &screen_host);
	document.set_type(EditorDocument::TYPE_SCENE_2D);
	CHECK(registry.resolve_default_surface(&document) == &scene);
	document.set_type(EditorDocument::TYPE_SCENE_3D);
	CHECK(registry.resolve_default_surface(&document) == &scene);
	document.set_type(EditorDocument::TYPE_SCENE_MIXED);
	CHECK(registry.resolve_default_surface(&document) == &scene);
	document.set_type(EditorDocument::TYPE_UNKNOWN);
	CHECK(registry.resolve_default_surface(&document) == &scene);
	CHECK(registry.resolve_default_surface(nullptr) == &scene);

	CHECK(registry.unregister_provider(resource.get_surface_id(), &resource));
	CHECK(registry.unregister_provider(script.get_surface_id(), &script));
	CHECK(registry.unregister_provider(shader.get_surface_id(), &shader));
	CHECK(registry.unregister_provider(help.get_surface_id(), &help));
	CHECK(registry.unregister_provider(screen_host.get_surface_id(), &screen_host));
	CHECK(registry.unregister_provider(scene.get_surface_id(), &scene));

	// Constructing a full DocumentView requires the live EditorNode and editor-plugin graph.
	// This direct registry test stays headless while exercising the extension seam without
	// modifying DocumentView for the test provider.
}

} // namespace TestEditorDocumentSurface
#endif // TOOLS_ENABLED
