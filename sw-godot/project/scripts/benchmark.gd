# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
extends Node3D

const REGION_SIZE := 96.0
const WARMUP_SECONDS := 3.0
const REGION_SPECS := [
	{"name": "Floresta", "color": Color(0.12, 0.32, 0.16), "assets": ["forest_leaves_04", "tree_bark_03", "grass_ground", "mossy_brick"]},
	{"name": "Pedreira", "color": Color(0.38, 0.38, 0.40), "assets": ["rock_path", "rock_01", "cobblestone_03", "gravel_road"]},
	{"name": "Costa", "color": Color(0.16, 0.36, 0.50), "assets": ["aerial_beach_01", "coast_sand_01", "aerial_rocks_01", "rock_wall_08"]},
	{"name": "Distrito", "color": Color(0.42, 0.32, 0.22), "assets": ["aerial_asphalt_01", "asphalt_01", "brick_floor", "concrete_moss"]},
]
const ROUTE := [Vector3(0, 5, 0), Vector3(80, 5, 0), Vector3(100, 5, 0), Vector3(100, 5, 80), Vector3(100, 5, 100), Vector3(20, 5, 100), Vector3(0, 5, 100), Vector3(0, 5, 20), Vector3(0, 5, 0)]

var camera: Camera3D
var robot_enabled := false
var route_seconds := 8.0
var route_elapsed := 0.0
var route_segment := 0
var last_logged_waypoint := -1
var route_log: FileAccess
var measure_enabled := false
var load_ms := 0.0
var texture_load_ms := 0.0
var texture_bytes := 0
var texture_read_ms := 0.0  # só leitura dos bytes do disco (isola I/O de decodificação e envio à GPU)
var texture_mode := "gpu"  # "gpu" = .res BC1 pré-comprimido (lê bytes); "jpg" = decodifica JPEG 8K na CPU (método antigo)
var frame_ms: PackedFloat64Array = PackedFloat64Array()
var last_frame_usec := 0


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	robot_enabled = "--robot" in args
	measure_enabled = "--measure" in args
	for index in args.size():
		if args[index] == "--route-seconds" and index + 1 < args.size():
			route_seconds = maxf(0.1, args[index + 1].to_float())
		if args[index] == "--tex" and index + 1 < args.size():
			texture_mode = args[index + 1]
	if texture_mode != "gpu" and texture_mode != "jpg":
		push_error("--tex deve ser gpu ou jpg")
		get_tree().quit(2)
		return
	var load_started := Time.get_ticks_usec()
	_build_regions()
	_build_camera()
	load_ms = (Time.get_ticks_usec() - load_started) / 1000.0
	if robot_enabled:
		_start_robot()


func _build_regions() -> void:
	for index in REGION_SPECS.size():
		var spec: Dictionary = REGION_SPECS[index]
		var origin := Vector3((index % 2) * REGION_SIZE, 0, floori(index / 2.0) * REGION_SIZE)
		var region := Node3D.new()
		region.name = "Region_%02d_%s" % [index + 1, spec.name]
		region.position = origin
		add_child(region)
		var textures: Array[Texture2D] = []
		for asset_id in spec.assets:
			var texture := _load_region_texture(asset_id)
			if texture:
				textures.append(texture)
		_add_surface(region, Vector3(REGION_SIZE / 2, -0.25, REGION_SIZE / 2), Vector3(REGION_SIZE, 0.5, REGION_SIZE), spec.color, textures[0] if not textures.is_empty() else null)
		for row in 3:
			for column in 3:
				var x := 12.0 + column * 36.0
				var z := 12.0 + row * 36.0
				var texture: Texture2D = textures[(row * 3 + column) % textures.size()] if not textures.is_empty() else null
				_add_surface(region, Vector3(x, 2.0 + (row + column) % 5, z), Vector3(7, 4 + (row * column) % 5, 7), spec.color.lightened(0.12 * ((row + column) % 3)), texture)
		var label := Label3D.new()
		label.text = "%s | %d texturas" % [spec.name, spec.assets.size()]
		label.position = Vector3(4, 8, 4)
		label.font_size = 64
		region.add_child(label)


func _load_region_texture(asset_id: String) -> Texture2D:
	var started := Time.get_ticks_usec()
	var texture := _load_region_texture_uncounted(asset_id)
	texture_load_ms += (Time.get_ticks_usec() - started) / 1000.0
	return texture


func _load_region_texture_uncounted(asset_id: String) -> Texture2D:
	if texture_mode == "gpu":
		var path := "res://assets/gpu/%s_diff.res" % asset_id
		var file := FileAccess.open(path, FileAccess.READ)
		if file == null:
			push_error("textura GPU ausente: %s (rode bake_gpu_textures.gd)" % path)
			get_tree().quit(3)
			return null
		var read_started := Time.get_ticks_usec()
		var raw := file.get_buffer(file.get_length())
		texture_read_ms += (Time.get_ticks_usec() - read_started) / 1000.0
		texture_bytes += raw.size()
		file.close()
		return ResourceLoader.load(path, "Texture2D", ResourceLoader.CACHE_MODE_IGNORE) as Texture2D
	var directory := "res://assets/regions/%s" % asset_id
	if DirAccess.open(directory) == null:
		return null
	for filename in DirAccess.get_files_at(directory):
		if filename.begins_with(asset_id + "_diff_") and filename.ends_with(".jpg"):
			var image := Image.new()
			var error := image.load(ProjectSettings.globalize_path(directory.path_join(filename)))
			if error == OK:
				var jpg := FileAccess.open(directory.path_join(filename), FileAccess.READ)
				texture_bytes += jpg.get_length() if jpg else 0
			if error == OK:
				return ImageTexture.create_from_image(image)
	return null


func _add_surface(parent: Node3D, position: Vector3, size: Vector3, tint: Color, texture: Texture2D) -> void:
	var mesh_instance := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh_instance.mesh = mesh
	mesh_instance.position = position
	var material := StandardMaterial3D.new()
	material.albedo_color = tint
	material.roughness = 0.9
	if texture:
		material.albedo_texture = texture
		material.uv1_scale = Vector3(8, 8, 1)
	mesh_instance.material_override = material
	parent.add_child(mesh_instance)


func _build_camera() -> void:
	camera = Camera3D.new()
	camera.name = "BenchmarkCamera"
	camera.position = ROUTE[0]
	add_child(camera)
	camera.look_at(Vector3(24, 0, 24))
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-55, 35, 0)
	light.light_energy = 1.5
	add_child(light)


func _start_robot() -> void:
	robot_enabled = true
	route_elapsed = 0.0
	route_segment = 0
	last_logged_waypoint = -1
	route_log = FileAccess.open("user://route.log", FileAccess.WRITE)
	if route_log == null:
		push_error("Não foi possível abrir user://route.log")
		get_tree().quit(2)
		return
	route_log.store_line("elapsed_s,x,y,z,waypoint")
	_record_position(0)


func _process(_delta: float) -> void:
	if not (robot_enabled and measure_enabled):
		return
	var now := Time.get_ticks_usec()
	if last_frame_usec > 0:
		frame_ms.append((now - last_frame_usec) / 1000.0)
	last_frame_usec = now


func _write_measurements() -> void:
	var total := 0.0
	var worst := 0.0
	var over_33 := 0
	var over_50 := 0
	var over_100 := 0
	var excess_ms := 0.0
	var over_33_steady := 0
	var elapsed := 0.0
	for value in frame_ms:
		elapsed += value
		total += value
		worst = maxf(worst, value)
		if value > 33.0:
			over_33 += 1
			if value > 50.0:
				over_50 += 1
			if value > 100.0:
				over_100 += 1
			excess_ms += value - 33.0
			if elapsed > WARMUP_SECONDS * 1000.0:
				over_33_steady += 1
	var sorted := frame_ms.duplicate()
	sorted.sort()
	var p99 := sorted[mini(int(sorted.size() * 0.99), sorted.size() - 1)] if sorted.size() > 0 else 0.0
	var report := {
		"load_ms": load_ms,
		"texture_load_ms": texture_load_ms,
		"scene_build_ms": load_ms - texture_load_ms,
		"texture_bytes": texture_bytes,
		"texture_read_ms": texture_read_ms,
		"texture_mode": texture_mode,
		"frames": frame_ms.size(),
		"avg_fps": 1000.0 * frame_ms.size() / total if total > 0.0 else 0.0,
		"frames_over_33ms": over_33,
		"frames_over_33ms_steady": over_33_steady,
		"frames_over_50ms": over_50,
		"frames_over_100ms": over_100,
		"excess_ms_over_33": excess_ms,
		"p99_frame_ms": p99,
		"worst_frame_ms": worst,
		"route_seconds": route_seconds,
		"renderer": RenderingServer.get_video_adapter_name(),
	}
	var file := FileAccess.open("user://measure.json", FileAccess.WRITE)
	if file:
		file.store_string(JSON.stringify(report))
		file.close()
	print("MEASURE_JSON ", JSON.stringify(report))


func _physics_process(delta: float) -> void:
	if not robot_enabled:
		return
	route_elapsed += delta
	var segment_count := ROUTE.size() - 1
	var segment_duration := route_seconds / segment_count
	var progress := minf(route_elapsed / segment_duration, float(segment_count))
	route_segment = mini(int(progress), segment_count - 1)
	var fraction := progress - route_segment
	camera.position = ROUTE[route_segment].lerp(ROUTE[route_segment + 1], fraction)
	camera.look_at(camera.position + Vector3(0.0, -0.1, -1.0), Vector3.UP)
	if route_segment > last_logged_waypoint:
		_record_position(route_segment)
	if progress >= segment_count:
		route_elapsed = route_seconds
		camera.position = ROUTE.back()
		if last_logged_waypoint < segment_count:
			_record_position(segment_count)
		route_log.close()
		robot_enabled = false
		if measure_enabled:
			_write_measurements()
		print("ROUTE_DONE seconds=%.6f waypoints=%d log=user://route.log" % [route_elapsed, ROUTE.size()])
		get_tree().quit()


func _record_position(waypoint: int) -> void:
	var point: Vector3 = ROUTE[waypoint]
	route_log.store_line("%.6f,%.3f,%.3f,%.3f,%d" % [route_seconds * waypoint / (ROUTE.size() - 1), point.x, point.y, point.z, waypoint])
	route_log.flush()
	last_logged_waypoint = waypoint
