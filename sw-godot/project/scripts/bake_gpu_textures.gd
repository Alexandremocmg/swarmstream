# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
extends SceneTree
# Converte os diffuse JPEG 8K em texturas GPU comprimidas (BC1/S3TC (DXT1), com mipmaps),
# gravadas como PortableCompressedTexture2D em res://assets/gpu/<id>_diff.res.
# Assim a carga do benchmark lê bytes já prontos para a GPU, sem decodificar JPEG na CPU.
#
#   godot --headless --path sw-godot/project --script res://scripts/bake_gpu_textures.gd [-- --only id]

const OUT_DIR := "res://assets/gpu"


func _init() -> void:
	var specs: Array = load("res://scripts/benchmark.gd").get_script_constant_map()["REGION_SPECS"]
	var only := ""
	var args := OS.get_cmdline_user_args()
	if args.size() >= 2 and args[0] == "--only":
		only = args[1]
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))
	var failures := 0
	for spec in specs:
		for asset_id in spec.assets:
			if only != "" and asset_id != only:
				continue
			if not _bake(asset_id):
				failures += 1
	quit(1 if failures > 0 else 0)


func _bake(asset_id: String) -> bool:
	var directory := "res://assets/regions/%s" % asset_id
	var source := ""
	for filename in DirAccess.get_files_at(directory):
		if filename.begins_with(asset_id + "_diff_") and filename.ends_with(".jpg"):
			source = directory.path_join(filename)
	if source == "":
		push_error("sem diffuse para %s" % asset_id)
		return false
	var started := Time.get_ticks_msec()
	var image := Image.load_from_file(ProjectSettings.globalize_path(source))
	if image == null or image.is_empty():
		push_error("falha ao ler %s" % source)
		return false
	image.generate_mipmaps()
	var texture := PortableCompressedTexture2D.new()
	texture.keep_compressed_buffer = true  # sem isso o .res sai sem os dados
	texture.create_from_image(image, PortableCompressedTexture2D.COMPRESSION_MODE_S3TC, false)
	var target := "%s/%s_diff.res" % [OUT_DIR, asset_id]
	var error := ResourceSaver.save(texture, target)
	if error != OK:
		push_error("falha ao salvar %s (erro %d)" % [target, error])
		return false
	var size := FileAccess.get_file_as_bytes(target).size()
	print("BAKED %s %dx%d -> %.1f MiB em %.1f s" % [asset_id, image.get_width(), image.get_height(), size / 1048576.0, (Time.get_ticks_msec() - started) / 1000.0])
	return true
