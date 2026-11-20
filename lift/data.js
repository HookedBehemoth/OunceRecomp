window.BENCHMARK_DATA = {
  "lastUpdate": 1795202391747,
  "repoUrl": "https://github.com/HookedBehemoth/OunceRecomp",
  "entries": {
    "lift oot": [
      {
        "commit": {
          "author": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "committer": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "distinct": true,
          "id": "a95921c19f579b60f216bf9e4ae86070a1116fb7",
          "message": "ci: gh-pages holds only the lift charts (lift/data.js, lift/index.html, .nojekyll) in a history of its own: github-action-benchmark had made the missing branch from the checked-out commit, the whole repository. lift-history makes a branch holding anything else again as one commit of those files, the charts' data kept, before it adds the numbers\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_014NsvxGVC4JxyJvKExGrCh4",
          "timestamp": "2026-11-19T17:53:07+01:00",
          "tree_id": "bd07ebd77e38b29bd47900cea4142d5abf3318a8",
          "url": "https://github.com/HookedBehemoth/OunceRecomp/commit/a95921c19f579b60f216bf9e4ae86070a1116fb7"
        },
        "date": 1795108259542,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "compile.sum_s",
            "value": 2285.969,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.found",
            "value": 294504,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.lifted",
            "value": 294504,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.skipped",
            "value": 0,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.cpu_s",
            "value": 73.68,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.discovery_s",
            "value": 0.817,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.image_s",
            "value": 0.438,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.partitions_s",
            "value": 13.12,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.plan_s",
            "value": 0.61,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.summaries_s",
            "value": 5.231,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.wall_s",
            "value": 20.944,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects",
            "value": 154,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.bytes",
            "value": 306715216,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.cold_bytes",
            "value": 297002536,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.hot_bytes",
            "value": 9712680,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "partitions",
            "value": 147,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.cpu_s",
            "value": 2239.12,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.peak_mb",
            "value": 4034.898,
            "unit": "MB",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.wall_s",
            "value": 584.277,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "committer": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "distinct": true,
          "id": "3de2091a21d362099af5f1bae0ebcae8f5257ba0",
          "message": "culling lists: object a's resets are buffer clears recorded at the culling pass, before its work, and the CPU stores and fills go. pass_vegetation_compute's compute (frame_compute_list_begin's list, the culling calls cull_pass_calls_a, cull_pass_vegetation, cull_pass_calls_c) is the only GPU work that writes object a's lists and headers (CU_STORAGE_WRITTEN in a run); a row after frame_compute_list_begin (0x80291310, its command buffer at sp + 0xe8) and one on the other path (0x802917c0, x19's) record nvnCommandBufferClearBuffer of every copy a flip made current since the pass last ran: counts 0, the set count as it was at the flip, mesh records {0, 1, 1, w3}, dispatch records {0, 1, 1, 0}. cull_gpu_lists_reset's host code flips and reads the copied-out records only; a copy no pass reset since it was last current reads 0 (host state), no guest store. game_indirect_mesh_draw and its runtime hook go: the culling records get y = z at the pass, and actor_records_read_b's CPU records {0, n, n, 0} get 1 where n = 0 at a row after its stores. Object b's gather lists stay on fills after actor_records_read_a's zeroes: render-job mesh draws write two of them and their order against the culling pass is not established; the patch model cannot remove the game's stores. Kokiri bridge (outdoor, frame_rate 60): counts as before (op 3 6068 vs 6076, op 8 874 vs 873, no zeros, no doubling, 41 indirect mesh draws a frame); with the job sync fix before it, 0 one-present vegetation glitches in 40 s (before the fix the in-pass resets showed the race as pale ivy instead of missing ivy). ounce test oot PASS\n\nCo-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>",
          "timestamp": "2026-11-20T19:54:49+01:00",
          "tree_id": "db15de27772641a3477c9566d3f85a23f9f191f6",
          "url": "https://github.com/HookedBehemoth/OunceRecomp/commit/3de2091a21d362099af5f1bae0ebcae8f5257ba0"
        },
        "date": 1795202391356,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "compile.sum_s",
            "value": 2277.532,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.found",
            "value": 294481,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.lifted",
            "value": 294481,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.skipped",
            "value": 0,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.cpu_s",
            "value": 74.21,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.discovery_s",
            "value": 0.829,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.image_s",
            "value": 0.441,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.partitions_s",
            "value": 13.215,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.plan_s",
            "value": 0.697,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.summaries_s",
            "value": 5.208,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.wall_s",
            "value": 21.1,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects",
            "value": 154,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.bytes",
            "value": 306732720,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.cold_bytes",
            "value": 297018024,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.hot_bytes",
            "value": 9714696,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "partitions",
            "value": 147,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.cpu_s",
            "value": 2237.3,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.peak_mb",
            "value": 4025.543,
            "unit": "MB",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.wall_s",
            "value": 583.068,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          }
        ]
      }
    ],
    "lift dkb": [
      {
        "commit": {
          "author": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "committer": {
            "email": "luis@scheurenbrand.me",
            "name": "HookedBehemoth",
            "username": "HookedBehemoth"
          },
          "distinct": true,
          "id": "a95921c19f579b60f216bf9e4ae86070a1116fb7",
          "message": "ci: gh-pages holds only the lift charts (lift/data.js, lift/index.html, .nojekyll) in a history of its own: github-action-benchmark had made the missing branch from the checked-out commit, the whole repository. lift-history makes a branch holding anything else again as one commit of those files, the charts' data kept, before it adds the numbers\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_014NsvxGVC4JxyJvKExGrCh4",
          "timestamp": "2026-11-19T17:53:07+01:00",
          "tree_id": "bd07ebd77e38b29bd47900cea4142d5abf3318a8",
          "url": "https://github.com/HookedBehemoth/OunceRecomp/commit/a95921c19f579b60f216bf9e4ae86070a1116fb7"
        },
        "date": 1795108271415,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "compile.sum_s",
            "value": 1031.763,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.found",
            "value": 188647,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.lifted",
            "value": 188647,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "functions.skipped",
            "value": 0,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.cpu_s",
            "value": 37.07,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.discovery_s",
            "value": 0.538,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.image_s",
            "value": 0.254,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.partitions_s",
            "value": 6.337,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.plan_s",
            "value": 0.247,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.summaries_s",
            "value": 3.32,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "lift.wall_s",
            "value": 11.099,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects",
            "value": 83,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.bytes",
            "value": 171645304,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.cold_bytes",
            "value": 171645304,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "objects.hot_bytes",
            "value": 0,
            "unit": "bytes",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "partitions",
            "value": 76,
            "unit": "count",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.cpu_s",
            "value": 1036.6,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.peak_mb",
            "value": 3091.699,
            "unit": "MB",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          },
          {
            "name": "total.wall_s",
            "value": 267.526,
            "unit": "s",
            "extra": "4 jobs on Intel(R) Xeon(R) 6975P-C"
          }
        ]
      }
    ]
  }
}