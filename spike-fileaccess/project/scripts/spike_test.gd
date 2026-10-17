extends SceneTree
# Roda: godot --headless --path project --script res://scripts/spike_test.gd

func r(name: String, v) -> void:
	print("RESULT ", name, ": ", v)

func _init() -> void:
	SwarmFormatLoader.reset_load_calls()

	# T1: caminho swarm:// via loader
	var b1 = load("swarm://hello.txt")
	r("T1 load(swarm://hello.txt)", "%s | %s" % [b1.get_class() if b1 else "null", b1.as_text() if b1 else ""])

	# T2: res://claimed/ via ResourceLoader -> loader na frente
	var calls0 := SwarmFormatLoader.get_load_calls()
	var b2 = load("res://claimed/a.txt")
	r("T2 load(res://claimed/a.txt)", "%s | %s | loader_calls+%d" % [b2.get_class() if b2 else "null", b2.as_text() if b2 and b2.has_method("as_text") else "?", SwarmFormatLoader.get_load_calls() - calls0])

	# T3: FileAccess direto no mesmo caminho -> NAO passa pelo loader
	calls0 = SwarmFormatLoader.get_load_calls()
	var f = FileAccess.open("res://claimed/a.txt", FileAccess.READ)
	r("T3 FileAccess.open(res://claimed/a.txt)", "%s | loader_calls+%d" % [f.get_as_text() if f else "null", SwarmFormatLoader.get_load_calls() - calls0])

	# T4: FileAccess/DirAccess em swarm:// (backend inexistente)
	var f4 = FileAccess.open("swarm://hello.txt", FileAccess.READ)
	r("T4 FileAccess.open(swarm://hello.txt)", "null, err=%d" % FileAccess.get_open_error() if f4 == null else "ABRIU")
	var d4 = DirAccess.open("swarm://")
	r("T4b DirAccess.open(swarm://)", "null" if d4 == null else "ABRIU")
	r("T4c FileAccess.file_exists(swarm://hello.txt)", FileAccess.file_exists("swarm://hello.txt"))

	# T5: ConfigFile em res://claimed
	calls0 = SwarmFormatLoader.get_load_calls()
	var cf := ConfigFile.new()
	var e5 := cf.load("res://claimed/c.cfg")
	r("T5 ConfigFile.load(res://claimed/c.cfg)", "err=%d k=%s loader_calls+%d" % [e5, cf.get_value("", "k", "?"), SwarmFormatLoader.get_load_calls() - calls0])

	# T6: cena com ext_resource apontando para claimed
	calls0 = SwarmFormatLoader.get_load_calls()
	var ps = load("res://plain/scene.tscn")
	var n = ps.instantiate() if ps else null
	var meta = n.get_meta("blob") if n else null
	if n: n.free()
	r("T6 scene ext_resource claimed", "%s | loader_calls+%d" % [meta.get_class() if meta else "null", SwarmFormatLoader.get_load_calls() - calls0])

	# T8: textura importada
	calls0 = SwarmFormatLoader.get_load_calls()
	var t_plain = load("res://plain/tex.png")
	r("T8a load(res://plain/tex.png)", "%s | loader_calls+%d" % [t_plain.get_class() if t_plain else "null", SwarmFormatLoader.get_load_calls() - calls0])
	calls0 = SwarmFormatLoader.get_load_calls()
	var t_cl = load("res://claimed/tex.png")
	r("T8b load(res://claimed/tex.png)", "%s | loader_calls+%d" % [t_cl.get_class() if t_cl else "null", SwarmFormatLoader.get_load_calls() - calls0])


	# T9: arquivo que NAO existe no disco (so no "swarm")
	calls0 = SwarmFormatLoader.get_load_calls()
	r("T9a ResourceLoader.exists(virtual)", ResourceLoader.exists("res://claimed/only_in_swarm.txt"))
	r("T9b FileAccess.file_exists(virtual)", FileAccess.file_exists("res://claimed/only_in_swarm.txt"))
	var v = load("res://claimed/only_in_swarm.txt")
	r("T9c load(virtual, sem arquivo no disco)", "%s | loader_calls+%d" % [v.get_class() if v else "null", SwarmFormatLoader.get_load_calls() - calls0])
	var e9 := ResourceLoader.load_threaded_request("res://claimed/only_in_swarm_thr.txt")
	var st9 := ResourceLoader.THREAD_LOAD_IN_PROGRESS
	while st9 == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
		st9 = ResourceLoader.load_threaded_get_status("res://claimed/only_in_swarm_thr.txt")
		OS.delay_msec(5)
	r("T9d threaded(virtual)", "req_err=%d status=%d (0=INVALID,1=IN_PROGRESS,2=FAILED,3=LOADED)" % [e9, st9])
	calls0 = SwarmFormatLoader.get_load_calls()
	var ps2 = load("res://plain/scene_virtual.tscn")
	var n2 = ps2.instantiate() if ps2 else null
	var m2 = n2.get_meta("blob") if n2 else null
	r("T9e scene com ext_resource virtual", "%s | loader_calls+%d" % [m2.get_class() if m2 else "null", SwarmFormatLoader.get_load_calls() - calls0])
	if n2: n2.free()

	# T7: threaded, 4 pedidos em paralelo, _load dorme 200 ms
	SwarmFormatLoader.set_load_delay_ms(200)
	var paths := ["res://claimed/t1.txt", "res://claimed/t2.txt", "res://claimed/t3.txt", "res://claimed/t4.txt"]
	var t0 := Time.get_ticks_msec()
	for p in paths:
		var err := ResourceLoader.load_threaded_request(p)
		if err != OK: r("T7 request err", err)
	var pending := paths.duplicate()
	var main_ticks := 0
	while not pending.is_empty():
		for p in pending.duplicate():
			var st := ResourceLoader.load_threaded_get_status(p)
			if st == ResourceLoader.THREAD_LOAD_LOADED:
				pending.erase(p)
			elif st == ResourceLoader.THREAD_LOAD_FAILED or st == ResourceLoader.THREAD_LOAD_INVALID_RESOURCE:
				r("T7 status", "%s st=%d" % [p, st]); pending.erase(p)
		main_ticks += 1
		OS.delay_msec(5)
	var dt := Time.get_ticks_msec() - t0
	var got = ResourceLoader.load_threaded_get(paths[0])
	r("T7 threaded x4 (200ms cada)", "total=%d ms | main thread livre (polls=%d) | classe=%s" % [dt, main_ticks, got.get_class() if got else "null"])
	SwarmFormatLoader.set_load_delay_ms(0)
	quit(0)
