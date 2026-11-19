# Rules for agents

This page lists the rules an agent follows when it builds, runs or tests a game. They keep agents from running out the
PC's memory or resetting the GPU under each other.

- Build, run and test with `--exclusive`: `ounce build <game> --exclusive`, `ounce run <game> --headless --seconds N
  --exclusive`, `ounce test <game> --exclusive`. It waits until no other build or game run holds the PC, and says who
  does every 30 seconds.
- Never wrap `ounce` in `flock` or `systemd-run`. `--exclusive` takes the lock itself, and an `--exclusive` build runs in
  a memory-capped scope (`limits.build_mem`, or `--mem <size>`). An `ounce` started under an outer `flock` of the lock
  stops with an error instead of waiting for ever.
- One game run at a time comes for free: the lock covers builds and game runs alike, and a run also waits until enough
  memory is free (`limits.run_free_mem_gb`).
- Run games headless and muted (`--headless`), with a time limit (`--seconds`), and read the verdict of `ounce test`,
  not the log.
- Work in your own worktree, made with `git worktree add work/wt/<name> -b agent/<name>`. It needs no links: the
  config, the object cache and the data folder are outside the checkout, and its first `ounce build` copies the main
  checkout's texture store by itself.
- To land a branch, rebase it onto the tracking branch and fast-forward the tracking branch to it:

  ```sh
  git -C work/wt/<name> rebase native-vk
  git merge --ff-only agent/<name>
  ```

## Changing code

- What the project supports is being narrowed on purpose (one renderer, no fallbacks). When a change needs it,
  overhaul, restructure or delete the code it touches instead of patching around it until it works: remove paths,
  switches and compatibility code that the new design no longer needs, and leave the code as if it had been written
  that way. Say in the report what was removed and why.
