extends SceneTree
func _init() -> void:
	print("cpus=", OS.get_processor_count())
	SwarmFormatLoader.set_load_delay_ms(200)
	for n in [1, 2, 4, 8, 16]:
		var ps := []
		for i in n: ps.append("res://claimed/par_%d_%d.txt" % [n, i])
		var t0 := Time.get_ticks_msec()
		for p in ps: ResourceLoader.load_threaded_request(p)
		for p in ps:
			while ResourceLoader.load_threaded_get_status(p) == ResourceLoader.THREAD_LOAD_IN_PROGRESS: OS.delay_msec(2)
		print("RESULT N=%d total=%d ms" % [n, Time.get_ticks_msec() - t0])
	quit(0)
