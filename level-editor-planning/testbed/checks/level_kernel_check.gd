extends SceneTree
## LE0 kernel-skeleton check for `modules/level_kernel` (see PLAN.md §1 KERNEL, §4 LE0).
## Exercises: LevelMesh transaction -> create_box -> commit; LevelMeshData vertex/edge/face/loop
## counts; LevelMeshBaker triangle count; diff revert/apply round-trip.
##
## Run: bin/godot.windows.editor.dev.x86_64.console.exe --headless \
##      --path level-editor-planning/testbed --script res://checks/level_kernel_check.gd
##
## The module does not exist yet (it is being built in parallel with this check), so every call
## below goes through ClassDB reflection (instantiate/has_method/call) instead of static
## `LevelMesh.new()` references — a static reference to an undeclared global class would fail
## GDScript parsing even inside a branch that never runs, breaking the SKIP guard below.
##
## REFLECTED API (kept reflection-based so this project still parses in builds without the module):
##   LevelMesh (RefCounted)
##     .begin_transaction() -> void
##     .create_box(transform: Transform3D, size: Vector3, material_index: int) -> bool
##     .commit() -> LevelMeshDiff
##     .revert_diff(diff: LevelMeshDiff) -> bool
##     .apply_diff(diff: LevelMeshDiff) -> bool
##     .data : LevelMeshData
##   LevelMeshData (Resource)
##     .vertex_count() -> int   [alt candidate: "get_vertex_count()"]
##     .edge_count() -> int
##     .face_count() -> int
##     .loop_count() -> int     [total per-face-vertex loop entries; alt candidate: "loop_count()"
##                               may instead be named "corner_count()"]
##   LevelMeshBaker (RefCounted)
##     .bake(data: LevelMeshData) -> ArrayMesh
## `reconcile_face_uv` is an internal kernel post-step invoked by the ops themselves; this check
## does not call it directly.

const REQUIRED_CLASSES: PackedStringArray = [
	"LevelMesh", "LevelMeshData", "LevelMeshDiff", "LevelMeshBaker",
]

const BOX_SIZE := Vector3(4, 4, 4)
const BOX_MATERIAL_INDEX := 0
const EXPECTED_VERTS := 8
const EXPECTED_EDGES := 12
const EXPECTED_FACES := 6
const EXPECTED_LOOPS := 24
const EXPECTED_TRIS := 12


func _initialize() -> void:
	for cls in REQUIRED_CLASSES:
		if not ClassDB.class_exists(cls):
			print("SKIP: level_kernel module not built (missing class: %s)" % cls)
			quit(0)
			return

	var failures: Array[String] = []
	_run_check(failures)

	if failures.is_empty():
		print(("LEVEL_KERNEL_CHECK: PASS (box: %d verts/%d edges/%d faces/%d loops, " +
				"baked %d tris, diff round-trip ok)") % [
				EXPECTED_VERTS, EXPECTED_EDGES, EXPECTED_FACES, EXPECTED_LOOPS, EXPECTED_TRIS])
		quit(0)
	else:
		for f in failures:
			printerr("LEVEL_KERNEL_CHECK FAIL: " + f)
		quit(1)


func _run_check(failures: Array[String]) -> void:
	var mesh: Object = ClassDB.instantiate("LevelMesh")
	if mesh == null:
		failures.append("LevelMesh.new() failed to instantiate")
		return

	if not mesh.has_method("begin_transaction") or not mesh.has_method("create_box") \
			or not mesh.has_method("commit"):
		failures.append("LevelMesh is missing begin_transaction()/create_box()/commit()")
		return

	mesh.call("begin_transaction")
	mesh.call("create_box", Transform3D.IDENTITY, BOX_SIZE, BOX_MATERIAL_INDEX)
	var diff: Variant = mesh.call("commit")
	if diff == null:
		failures.append("commit() returned null — expected a LevelMeshDiff")
		return

	var data: Object = _get_data(mesh)
	if data == null:
		failures.append("could not obtain LevelMeshData from LevelMesh (tried '.data' and 'get_data()')")
		return

	_expect_count(failures, data, "vertex_count", EXPECTED_VERTS, "vertices")
	_expect_count(failures, data, "edge_count", EXPECTED_EDGES, "edges")
	_expect_count(failures, data, "face_count", EXPECTED_FACES, "faces")
	_expect_count(failures, data, "loop_count", EXPECTED_LOOPS, "loops")

	_check_bake(failures, data)
	_check_round_trip(failures, mesh, data, diff)


func _check_bake(failures: Array[String], data: Object) -> void:
	var baker: Object = ClassDB.instantiate("LevelMeshBaker")
	if baker == null:
		failures.append("LevelMeshBaker.new() failed to instantiate")
		return
	if not baker.has_method("bake"):
		failures.append("LevelMeshBaker has no bake() method")
		return

	var array_mesh: Variant = baker.call("bake", data)
	if array_mesh == null or not (array_mesh is ArrayMesh):
		failures.append("LevelMeshBaker.bake() did not return an ArrayMesh")
		return

	var tri_count := _triangle_count(array_mesh)
	if tri_count != EXPECTED_TRIS:
		failures.append("baked box had %d triangles, expected %d" % [tri_count, EXPECTED_TRIS])


func _check_round_trip(failures: Array[String], mesh: Object, data: Object, diff: Variant) -> void:
	if not mesh.has_method("revert_diff") or not mesh.has_method("apply_diff"):
		failures.append("LevelMesh is missing revert_diff()/apply_diff() — cannot verify diff round-trip")
		return

	mesh.call("revert_diff", diff)
	var reverted_verts := _count_or(data, "vertex_count")
	if reverted_verts != 0:
		failures.append("revert_diff(diff) did not empty the mesh (got %d vertices, expected 0)" % reverted_verts)

	mesh.call("apply_diff", diff)
	var restored_verts := _count_or(data, "vertex_count")
	var restored_edges := _count_or(data, "edge_count")
	var restored_faces := _count_or(data, "face_count")
	var restored_loops := _count_or(data, "loop_count")
	if restored_verts != EXPECTED_VERTS or restored_edges != EXPECTED_EDGES \
			or restored_faces != EXPECTED_FACES or restored_loops != EXPECTED_LOOPS:
		failures.append(("apply_diff(diff) did not round-trip the box topology " +
				"(got %d/%d/%d/%d, expected %d/%d/%d/%d)") % [
				restored_verts, restored_edges, restored_faces, restored_loops,
				EXPECTED_VERTS, EXPECTED_EDGES, EXPECTED_FACES, EXPECTED_LOOPS])


func _get_data(mesh: Object) -> Object:
	if mesh.has_method("get_data"):
		var via_method: Variant = mesh.call("get_data")
		if via_method is Object:
			return via_method
	var via_property: Variant = mesh.get("data")
	if via_property is Object:
		return via_property
	return null


func _expect_count(failures: Array[String], data: Object, method: String, expected: int, label: String) -> void:
	if not data.has_method(method):
		failures.append("LevelMeshData has no %s() method" % method)
		return
	var actual: int = int(data.call(method))
	if actual != expected:
		failures.append("box has %d %s, expected %d" % [actual, label, expected])


func _count_or(data: Object, method: String) -> int:
	if data.has_method(method):
		return int(data.call(method))
	return -1


func _triangle_count(array_mesh: ArrayMesh) -> int:
	var total := 0
	for surface_index in range(array_mesh.get_surface_count()):
		var arrays: Array = array_mesh.surface_get_arrays(surface_index)
		var indices: PackedInt32Array = arrays[ArrayMesh.ARRAY_INDEX]
		if indices.size() > 0:
			total += indices.size() / 3
		else:
			var verts: PackedVector3Array = arrays[ArrayMesh.ARRAY_VERTEX]
			total += verts.size() / 3
	return total
