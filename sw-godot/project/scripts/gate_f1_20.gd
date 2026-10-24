# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
extends Node3D
# Portão obrigatório do ADR-0016 (F1-20). Prova, em Godot estoque, que o loader
# da GDExtension materializa a partir de BYTES do mock do sw-core, sem FileAccess:
#   1) textura BC1 4096x4096 com mipmaps, 2) malha com 2 LODs, 3) áudio OGG,
# e troca o mip/LOD (e o áudio) NO MESMO recurso, na thread principal.
#
#   godot --path sw-godot/project res://scenes/gate_f1_20.tscn -- --gate-auto --out <pasta>
#
# Sai com código 0 se tudo passar; 1 caso contrário, com a lista do que falhou.

const TEX := "swarm://gate/rock_4k.swtex"
const MESH := "swarm://gate/sphere.swmesh"
const AUDIO := "swarm://gate/tone.ogg"
const LATENCY_MS := 600  # latência simulada da "CDN" do mock por peça
const LOAD_BUDGET_US := 100_000  # _load acima disto conta como espera
const TIMEOUT_S := 15.0

var _failures: PackedStringArray = []
var _report: Array = []
var _out_dir := ""


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var i := args.find("--out")
	if i >= 0 and i + 1 < args.size():
		_out_dir = args[i + 1]
	_build_scene()
	await _run()
	_finish()


func _check(ok: bool, what: String) -> void:
	_report.append("%s %s" % ["OK  " if ok else "FAIL", what])
	if not ok:
		_failures.append(what)


func _build_scene() -> void:
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0, 2.4)
	add_child(cam)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-40, 30, 0)
	add_child(light)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.ambient_light_color = Color(0.4, 0.4, 0.45)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	add_child(env)


func _wait_loaded(paths: Array, timeout_s: float) -> Dictionary:
	var started := Time.get_ticks_usec()
	var done := {}
	while done.size() < paths.size():
		for p in paths:
			if done.has(p):
				continue
			var st := ResourceLoader.load_threaded_get_status(p)
			if st == ResourceLoader.THREAD_LOAD_LOADED:
				done[p] = Time.get_ticks_usec() - started
			elif st == ResourceLoader.THREAD_LOAD_FAILED or st == ResourceLoader.THREAD_LOAD_INVALID_RESOURCE:
				done[p] = -1
		if (Time.get_ticks_usec() - started) / 1e6 > timeout_s:
			break
		await get_tree().process_frame
	return done


func _shot(name: String) -> void:
	if _out_dir == "":
		return
	# Com --headless o renderizador é falso e frame_post_draw nunca dispara: esperar por ele trava a cena.
	if DisplayServer.get_name() == "headless":
		_report.append("info captura %s pulada: sem janela (headless)" % name)
		return
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_dir.path_join(name))


func _events(kind: String) -> Dictionary:
	var by_path := {}
	for e in SwarmGate.get_log():
		if e["event"] == kind:
			by_path[e["path"]] = e
	return by_path


func _run() -> void:
	_check(ClassDB.class_exists("SwarmGate") and ClassDB.class_exists("SwarmLoader"), "GDExtension carregada")
	if not ClassDB.class_exists("SwarmGate"):
		return
	var pack := ProjectSettings.globalize_path("res://.swarm_gate")
	var rc: int = SwarmGate.init(pack, LATENCY_MS)
	_check(rc == 0, "mock do sw-core iniciado a partir de %s (resultado %d)" % [pack, rc])
	if rc != 0:
		return
	_report.append("info s3tc_na_gpu=%s renderer=%s driver_de_audio=%s dispositivo=%s" % [
		RenderingServer.has_os_feature("s3tc"), RenderingServer.get_video_adapter_name(),
		AudioServer.get_driver_name() if AudioServer.has_method("get_driver_name") else "?",
		AudioServer.get_output_device()])

	# Pré-condições: o melhor nível NÃO está local; o nível do core está.
	_check(not SwarmGate.is_local("rock_4k.swtex") and SwarmGate.is_local("rock_4k.low.swtex"),
		"textura 4K fora do cache, 256 px no core")
	_check(not SwarmGate.is_local("sphere.swmesh") and SwarmGate.is_local("sphere.low.swmesh"),
		"malha LOD 0 fora do cache, LOD 1 no core")
	_check(not SwarmGate.is_local("tone.ogg"), "áudio fora do cache")
	_check(not FileAccess.file_exists(TEX) and FileAccess.open(TEX, FileAccess.READ) == null,
		"swarm:// não existe para FileAccess (o loader não pode estar lendo por ele)")

	# 1. Carga em thread: o _load roda em worker e não pode esperar a rede.
	var paths := [TEX, MESH, AUDIO]
	for p in paths:
		ResourceLoader.load_threaded_request(p)
	var loaded := await _wait_loaded(paths, 5.0)
	var loads := _events("load")
	for p in paths:
		var t: int = loaded.get(p, -1)
		_check(t >= 0 and t < LATENCY_MS * 1000 / 2,
			"%s carregado em %.1f ms (< metade da latência de %d ms)" % [p, t / 1000.0, LATENCY_MS])
		var e: Dictionary = loads.get(p, {})
		_check(e.get("thread", "") == "WORKER", "%s: _load rodou em thread de worker (%s)" % [p, e.get("thread", "?")])
		_check(e.get("read_best", "") == "SW_E_NOT_LOCAL", "%s: leitura do melhor nível deu %s" % [p, e.get("read_best", "?")])
		_check(int(e.get("dur_us", 1 << 40)) < LOAD_BUDGET_US,
			"%s: _load durou %.1f ms (< %d ms, não esperou a rede)" % [p, int(e.get("dur_us", 0)) / 1000.0, LOAD_BUDGET_US / 1000])
	if loaded.values().has(-1) or loaded.size() < paths.size():
		return
	var tex: ImageTexture = ResourceLoader.load_threaded_get(TEX)
	var mesh: ArrayMesh = ResourceLoader.load_threaded_get(MESH)
	var audio: AudioStreamOggVorbis = ResourceLoader.load_threaded_get(AUDIO)
	var ids := [tex.get_instance_id(), mesh.get_instance_id(), audio.get_instance_id()]

	# 2. Estado inicial = política de falta (nível do core).
	var low_vertices := mesh.surface_get_array_len(0)
	_check(tex.get_width() == 256 and tex.get_height() == 256, "textura entregue com o mip do core (%dx%d)" % [tex.get_width(), tex.get_height()])
	_check(audio.packet_sequence == null, "áudio entregue vazio, sem pacotes (começa atrasado)")
	var quad := MeshInstance3D.new()
	var quad_mesh := QuadMesh.new()
	quad_mesh.size = Vector2(1.5, 1.5)
	quad.mesh = quad_mesh
	quad.position = Vector3(-0.85, 0, 0)
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = tex
	quad.material_override = mat
	add_child(quad)
	var ball := MeshInstance3D.new()
	ball.mesh = mesh
	ball.position = Vector3(0.95, 0, 0)
	ball.scale = Vector3(0.7, 0.7, 0.7)
	var ball_mat := StandardMaterial3D.new()
	ball_mat.albedo_texture = tex
	ball.material_override = ball_mat
	add_child(ball)
	var player := AudioStreamPlayer.new()
	player.stream = audio
	add_child(player)
	await _shot("f1_20_antes.png")

	# 3. Espera as trocas no lugar.
	var started := Time.get_ticks_msec()
	while SwarmGate.pending_swaps() > 0 and (Time.get_ticks_msec() - started) / 1000.0 < TIMEOUT_S:
		await get_tree().process_frame
	_check(SwarmGate.pending_swaps() == 0, "todas as trocas terminaram em %.2f s" % ((Time.get_ticks_msec() - started) / 1000.0))
	var swaps := _events("swap")
	for f in _events("swap_failed").values():
		_check(false, "%s: troca falhou (%s)" % [f["path"], f["error"]])
	for k in range(paths.size()):
		var e: Dictionary = swaps.get(paths[k], {})
		_check(e.get("thread", "") == "MAIN", "%s: troca rodou na thread principal (%s)" % [paths[k], e.get("thread", "?")])
		_check(int(e.get("instance_id", 0)) == ids[k], "%s: troca no MESMO objeto (id %d)" % [paths[k], ids[k]])
		_report.append("info %s: espera até a troca %.1f ms" % [paths[k], int(e.get("wait_us", 0)) / 1000.0])

	# 4. Estado final: melhor nível, no mesmo recurso, sem recarregar.
	var te: Dictionary = swaps.get(TEX, {})
	_check(tex.get_width() == 4096 and tex.get_height() == 4096, "textura trocada para 4096x4096 (%dx%d)" % [tex.get_width(), tex.get_height()])
	_check(te.get("format", -1) == Image.FORMAT_DXT1 and te.get("mipmaps", false) and int(te.get("mip_count", 0)) == 12,
		"textura BC1 (DXT1) com 12 níveis de mip (formato %s, mips %s)" % [te.get("format", "?"), te.get("mip_count", "?")])
	var high_vertices := mesh.surface_get_array_len(0)
	_check(high_vertices > low_vertices * 10, "malha trocada de LOD 1 (%d vértices) para LOD 0 (%d vértices)" % [low_vertices, high_vertices])
	_check(audio.get_length() > 2.5, "áudio OGG materializado (%.2f s)" % audio.get_length())
	_check(ResourceLoader.load(TEX).get_instance_id() == ids[0], "o cache do ResourceLoader devolve o objeto já trocado")
	await get_tree().process_frame
	await _shot("f1_20_depois.png")

	# 5. O áudio toca (a sessão só vê o estado do player, não ouve o som).
	player.play()
	await get_tree().create_timer(0.6).timeout
	_check(player.playing and player.get_playback_position() > 0.2,
		"áudio tocando (posição %.2f s)" % player.get_playback_position())
	var st: Dictionary = SwarmGate.stats()
	_report.append("info mock: %s" % [st])
	# Limpeza: o player tocando seguraria a reprodução do OGG no AudioServer na saída.
	player.stop()
	for node in [quad, ball, player]:
		node.queue_free()
	tex = null
	mesh = null
	audio = null
	await get_tree().process_frame
	await get_tree().process_frame


func _finish() -> void:
	for line in _report:
		print("[gate] ", line)
	if _failures.is_empty():
		print("[gate] RESULTADO: PASSOU (", _report.size(), " linhas)")
	else:
		print("[gate] RESULTADO: FALHOU em ", _failures.size(), ": ", ", ".join(_failures))
	if ClassDB.class_exists("SwarmGate"):
		SwarmGate.shutdown()
	get_tree().quit(0 if _failures.is_empty() else 1)
