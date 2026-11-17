"""The Legend of Zelda: Ocarina of Time (the Switch 2 build), as the lift and the build take it (nx2/gamedesc.py).
Data and constructor calls only."""
from gamedesc import Game, Identity

GAME = Game(
    name="oot",
    # (the main module's build id and the SHA-256 of exefs/main: another dump stops the lift and the build)
    identity=Identity(build_id="e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348",
                      main_sha256="2591fb99008d0814f8056c3f36b36790048ed857ff0ae2bb3a4c1e37b960a4de"),
    # (guest address -> our label, nx2/label_syms.py, nx2/lift2/dump.py)
    labels="labels.txt",
    # (the hot list and the link order: docs/lifter.md, "Game description files")
    hot="hot_funcs.bin",
    hot_order="hot_order.bin",
    # (the host's C files the build compiles and the patch scan reads, in host/)
    host=(
        "bench.c", "code_ptrs.c", "cxx_throw.c", "direct.c", "direct_assets.c", "direct_crit.c", "direct_depth_copy.c",
        "direct_diag_env.c", "direct_diag_nodata.c", "direct_job_sync.c", "direct_layer_ahead.c", "direct_mesh_args.c",
        "direct_mesh_binds.c", "direct_mesh_lists.c", "direct_mesh_system.c", "direct_movie.c", "direct_passes.c",
        "direct_patches.c", "direct_rt_ids.c", "direct_sky_lut.c", "direct_tex_dedup.c", "direct_texdata.c",
        "direct_texwatch.c", "eng_jobs.c", "hos_fp.c", "hos_hostcall.c", "hos_main.c", "hos_mem.c",
        "hos_process.c", "hos_sdl.c", "hos_setjmp.c", "hos_svc.c", "hos_thread.c", "lift2_verify.c",
        "mods.c", "mouse_native.c", "nn_answers.c", "nn_applet.c", "nn_audio.c", "nn_audio_in.c", "nn_codec.c",
        "nn_diag.c", "nn_fs.c", "nn_gs.c", "nn_hid.c", "nn_libc.c", "nn_os_sync.c", "nn_syncpt.c", "nn_vi.c",
        "nn_video.c", "nn_zstd.c", "nvdrv.c", "nvn_layout.c", "nvn_list.c", "nvn_native.c", "oot_probes.c", "pad_sdl.c",
        "prompts.c", "rt_slots.c", "rx_backend.c",
    ),
    # (zstd_decompress_sequences' local continuations: `adr x7, <label>` ... `br x7` inside the function, pc: word; the
    # code pointer audit accepts them, nx2/lift2/code_ptrs.py)
    local_continuations={0x8011060C: 0x10FFFCA7, 0x80110694: 0x10000047},
)
