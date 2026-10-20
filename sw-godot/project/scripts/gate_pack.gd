# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
extends SceneTree
# Empacotador do portão da F1-20 (papel do sw-pack, que ainda não existe).
# Gera, em res://.swarm_gate/, os arquivos que o mock do sw-core serve por bytes:
#   rock_4k.swtex      textura BC1 (S3TC/DXT1) 4096x4096 com mipmaps (melhor nível)
#   rock_4k.low.swtex  a mesma textura em 256x256 com mipmaps (grupo core)
#   sphere.swmesh      esfera com 128x64 segmentos (LOD 0)
#   sphere.low.swmesh  esfera com 16x8 segmentos (LOD 1, grupo core)
# O áudio (tone.ogg) é gerado fora, pelo ffmpeg (sw-godot/tools/gate_f1_20.py).
# Este script roda na build e PODE usar FileAccess; o loader em runtime, não.
#
#   godot --headless --path sw-godot/project --script res://scripts/gate_pack.gd

const OUT_DIR := "res://.swarm_gate"
const SOURCE := "res://assets/regions/rock_path/rock_path_diff_8k.jpg"


func _init() -> void:
	var out := ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(out)
	# .gdignore: o editor não importa estes arquivos (são dados da "CDN" do mock).
	FileAccess.open(OUT_DIR.path_join(".gdignore"), FileAccess.WRITE).close()
	var ok := _pack_textures() and _pack_meshes()
	quit(0 if ok else 1)


func _pack_textures() -> bool:
	var started := Time.get_ticks_msec()
	var source := Image.load_from_file(ProjectSettings.globalize_path(SOURCE))
	if source == null or source.is_empty():
		push_error("falha ao ler %s" % SOURCE)
		return false
	var low := source.duplicate() as Image
	source.resize(4096, 4096, Image.INTERPOLATE_CUBIC)
	low.resize(256, 256, Image.INTERPOLATE_CUBIC)
	for pair in [[source, "rock_4k.swtex"], [low, "rock_4k.low.swtex"]]:
		var image: Image = pair[0]
		image.generate_mipmaps()
		if image.compress(Image.COMPRESS_S3TC) != OK:
			push_error("falha ao comprimir %s em S3TC" % pair[1])
			return false
		if not _write_tex(OUT_DIR.path_join(pair[1]), image):
			return false
		print(
			(
				"PACK %s %dx%d formato=%d mipmaps=%d bytes=%d"
				% [
					pair[1],
					image.get_width(),
					image.get_height(),
					image.get_format(),
					image.get_mipmap_count(),
					image.get_data().size()
				]
			)
		)
	print("PACK texturas em %.1f s" % ((Time.get_ticks_msec() - started) / 1000.0))
	return true


func _write_tex(path: String, image: Image) -> bool:
	var data := image.get_data()
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_error("falha ao abrir %s" % path)
		return false
	f.store_buffer("SWTX".to_ascii_buffer())
	f.store_32(1)
	f.store_32(image.get_width())
	f.store_32(image.get_height())
	f.store_32(image.get_format())
	f.store_32(1 if image.has_mipmaps() else 0)
	f.store_64(data.size())
	f.store_buffer(data)
	f.close()
	return FileAccess.get_file_as_bytes(path).size() == 32 + data.size()


func _pack_meshes() -> bool:
	for spec in [[128, 64, "sphere.swmesh"], [16, 8, "sphere.low.swmesh"]]:
		var sphere := SphereMesh.new()
		sphere.radial_segments = spec[0]
		sphere.rings = spec[1]
		var arrays := sphere.get_mesh_arrays()
		var pos: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var nrm: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
		var uv: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
		var idx: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		var path: String = OUT_DIR.path_join(spec[2])
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f == null:
			push_error("falha ao abrir %s" % path)
			return false
		f.store_buffer("SWMS".to_ascii_buffer())
		f.store_32(1)
		f.store_32(pos.size())
		f.store_32(idx.size())
		for v in pos:
			f.store_float(v.x)
			f.store_float(v.y)
			f.store_float(v.z)
		for n in nrm:
			f.store_float(n.x)
			f.store_float(n.y)
			f.store_float(n.z)
		for t in uv:
			f.store_float(t.x)
			f.store_float(t.y)
		for i in idx:
			f.store_32(i)
		f.close()
		print("PACK %s vertices=%d indices=%d" % [spec[2], pos.size(), idx.size()])
	return true
