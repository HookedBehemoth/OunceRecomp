//! runtime/os and the threads' host code (hos_fp.c, hos_setjmp.c, hos_thread.c, nn_os_sync.c, the file system's link
//! rules), compiled from the source as it is around authored inputs.

use ounce_tests::*;
use regex::Regex;
use std::collections::{BTreeSet, HashMap};
use std::path::PathBuf;

/// writes text as name into t and compiles it: flags, the source, then tail (more sources, libraries)
fn build(t: &Tmp, compiler: &str, name: &str, text: &str, flags: &[&str], tail: &[&str]) -> PathBuf {
    let src = t.write(name, text);
    let mut args: Vec<String> = flags.iter().map(|s| s.to_string()).collect();
    args.push(src.display().to_string());
    args.extend(tail.iter().map(|s| s.to_string()));
    compile(compiler, &args, &t.join("fixture"))
}

/// os.h and os_linux.c under load: the locks exclude, the condition variable, event and semaphore lose no wake-up,
/// timeouts end, sleeps are not rounded to milliseconds; on all cores and on one (every contended wait sleeps).
#[test]
fn os() {
    let t = Tmp::new("os");
    let os_linux = path("runtime/os/os_linux.c");
    let exe = compile(
        &cc(),
        &["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", &inc("runtime/os"), fixture_path("os_test.c").to_str().unwrap(),
          os_linux.to_str().unwrap(), "-lpthread", "-ldl"],
        &t.join("os_test"),
    );
    run(&mut clean(&exe));
    if let Some(taskset) = which("taskset") {
        run(std::process::Command::new(taskset).args(["-c", "0"]).arg(&exe));
    }
}

/// hos_thread.c syncs MXCSR to the guest FPCR before guest code runs: in thread_main, in fiber_main, and in fiber_switch
/// for the side that runs again (a new fiber inherits the FPCR).
#[test]
fn hos_fp_thread_syncs() {
    let s = read("runtime/hos_thread.c");
    let main = between(&s, "static void thread_main(void *p)", "\n}\n");
    assert!(main[..find(main, "f(c);")].contains("a64_fpcr_sync(c);"), "thread_main: MXCSR before guest code");
    let fm = between(&s, "static void fiber_main(void *p)", "\n}\n");
    assert!(find(fm, "a64_fpcr_sync(c);") < find(fm, "a64_call_received(c, f->fn);"), "fiber_main");
    let sw = between(&s, "static void fiber_switch(Ctx *from, HosFiber *to) {", "\n}\n");
    let re = Regex::new(r"(?s)os_fiber_switch\([^;]*\);\s*(/\*.*?\*/\s*)?a64_fpcr_sync\(from\);").unwrap();
    assert!(re.is_match(sw), "fiber_switch: MXCSR of `from` when it runs again");
    assert!(sw.contains("to->ctx.fpcr = from->fpcr;"));
}

/// hos_fp.c: MXCSR follows the guest FPCR (rounding, FZ as DAZ and FTZ), the MRS/MSR FPCR/FPSR helpers the lifted code
/// calls (the host's bits kept, sticky bits folded in, DE as IDC only under FZ), and MXCSR across os_linux.c's fibers
/// switched the way hos_thread.c's fiber_switch does it.
#[cfg(target_arch = "x86_64")]
#[test]
fn hos_fp() {
    let t = Tmp::new("hos-fp");
    let exe = build(&t, &cc(), "t.c", &fixture("hos_fp_test.c"),
                    &["-O1", "-w", "-D_GNU_SOURCE", &inc("runtime"), &inc("runtime/os")],
                    &[path("runtime/os/os_linux.c").to_str().unwrap(), "-lpthread"]);
    run(&mut clean(&exe));
}

/// hos_setjmp.c's hos_sj_call (the lifted code's call of a setjmp owner) at one optimisation level: an owner that
/// returns; one whose callee longjmps back to the owner's setjmp (the code at the return address runs through
/// a64_icall); nested owners where the inner one's callee longjmps to the outer owner (the inner slot dropped) or to
/// its own; many longjmps in a row without the host stack growing.
#[cfg(target_arch = "x86_64")]
fn sj_call_at(opt: &str) {
    let t = Tmp::new("sj-call");
    // (the stand-in hos.h beside hos_setjmp.c: its #include "hos.h" looks in its own folder first)
    t.write("hos.h", &fixture("hos_sj_call_hos.h"));
    t.write("hos_setjmp.c", &read("runtime/hos_setjmp.c"));
    let dir = format!("-I{}", t.path().display());
    let exe = build(&t, &cc(), "t.c", &fixture("hos_sj_call_test.c"), &[opt, "-w", &dir, &inc("runtime/os")], &[]);
    let out = run(&mut clean(&exe));
    assert_eq!(text(&out.stdout).trim(), "ok");
}

#[cfg(target_arch = "x86_64")]
#[test]
fn hos_sj_call_o0() {
    sj_call_at("-O0");
}

#[cfg(target_arch = "x86_64")]
#[test]
fn hos_sj_call_o2() {
    sj_call_at("-O2");
}

/// The game's file system stays inside its mounts and never works through links (nn_fs.c guest_path_ok and
/// delete_tree, mods.c's RomFS walk, over os_linux.c's file functions): guest paths with ".." or "." parts are refused;
/// deleting a save folder removes a link inside it, not what it points to; the mod index leaves out links to folders
/// (two links back up made the walk endless), lists file links, and indexes nothing through a romfs folder that is a
/// link; os_path_kind marks a link as one.
#[test]
fn fs_links() {
    let fs = read("runtime/nn_fs.c");
    let mods = read("runtime/mods.c");
    let pieces = [
        block(&fs, "static int guest_path_ok(const char *rest) {"),
        block(&fs, "static void delete_tree(const char *dir) {"),
        block(&mods, "static int index_romfs(const char *host_dir, const char *rel, int depth) {"),
    ]
    .join("\n");
    let t = Tmp::new("fs-links");
    let exe = build(&t, &cc(), "fixture.c", &fixture("fs_links_test.c").replace("@PIECES@", &pieces),
                    &["-std=gnu11", &inc("runtime/os"), "-Wno-unused-function"],
                    &[path("runtime/os/os_linux.c").to_str().unwrap(), "-lpthread", "-ldl"]);
    let work = t.join("work");
    std::fs::create_dir(&work).unwrap();
    run(clean(&exe).arg(&work));
}

/// The core number a guest thread is told (hos_thread.c hos_thread_core, hos_cores): a one-core mask's core; else the
/// ideal core when the mask has it (or there is no mask); else the mask's lowest core; never a core outside the mask;
/// six cores.
#[test]
fn core_number() {
    let src = read("runtime/hos_thread.c");
    let code = fixture("core_number_test.c")
        .replace("@CORES@", block(&src, "int hos_cores(void) {"))
        .replace("@CORE@", block(&src, "int hos_thread_core(HosThread *t) {"));
    let t = Tmp::new("core-number");
    let exe = build(&t, &cc(), "t.c", &code, &["-std=c11", "-Wall", "-Werror"], &[]);
    run(&mut clean(&exe));
}

/// The core hos_reserved_cpus keeps (hos_thread.c pick_reserved_core): the last core of the fastest kind. On a CPU of
/// one kind the last core; on hybrid CPUs (efficiency cores of a lower class, first or last) the last performance core;
/// none with fewer than 8 logical processors, nor on a CPU of more than one kind with fewer than 4 cores of the
/// fastest kind.
#[test]
fn reserved_core() {
    let helper = block(&read("runtime/hos_thread.c"), "static uint64_t pick_reserved_core(").to_string();
    let t = Tmp::new("reserved-core");
    let exe = build(&t, &cxx(), "fixture.cpp", &fixture("reserved_core_test.cpp").replace("@HELPER@", &helper),
                    &["-std=c++20", "-Wall", "-Wextra", "-Werror"], &[]);
    run(&mut clean(&exe));
}

/// nn::os mutexes keep their lock in the guest's MutexType (nn_os_sync.c, "mutex state in the guest's MutexType": the
/// owner's id, the recursion count and MX_SLOW in the u32 at +0x18), the section compiled with a pthread harness:
/// threads lock and unlock shared mutexes (recursively too) around plain counters, sleepers are woken, a mutex
/// reserved for a condition variable's waiter is not taken by others, counts past the field go to the object, and a
/// word naming no thread is taken as free.
#[test]
fn mutex_word() {
    let source = read("runtime/nn_os_sync.c");
    let section = between(&source, "/* ---- mutex state in the guest's MutexType", "HLE(_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi)");
    let t = Tmp::new("mutex-word");
    let exe = build(&t, &cc(), "fixture.c", &fixture("mutex_word_test.c").replace("@SECTION@", section),
                    &["-std=gnu11", "-O2", "-pthread", "-w"], &[]);
    run(&mut clean(&exe));
}

// ---- WaitAny's wake (nn_os_sync.c poke_multiwait) and the scheduling points under the locks its scan takes

/// comments blanked: words in them are not calls
fn code_of(s: &str) -> String {
    Regex::new(r"(?s)/\*.*?\*/|//[^\n]*").unwrap().replace_all(s, " ").into_owned()
}

/// every definition of each name (one per platform behind #if included): any `name(params) {`, params with one level
/// of parentheses; keywords are not functions
fn definitions(code: &str) -> HashMap<String, Vec<String>> {
    let def = Regex::new(r"\b(\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*\{").unwrap();
    let mut defs: HashMap<String, Vec<String>> = HashMap::new();
    for m in def.captures_iter(code) {
        let name = &m[1];
        if !["if", "for", "while", "switch", "return", "sizeof"].contains(&name) {
            let at = m.get(0).unwrap();
            defs.entry(name.to_string()).or_default().push(block(&code[at.start()..], at.as_str()).to_string());
        }
    }
    defs
}

fn calls(body: &str) -> Vec<String> {
    Regex::new(r"\b(\w+)\s*\(").unwrap().captures_iter(body).map(|c| c[1].to_string()).collect()
}

/// what wait_any's mw_lock region and everything of this file it reaches calls beyond the functions that keep the core
/// and never sleep (an allowlist: anything else, a wrapper in another file included, is reported)
fn lookup_graph_unsafe(text: &str) -> Vec<String> {
    const SAFE: &[&str] = &[
        "os_mutex_lock", "os_mutex_unlock", "os_rwlock_lock", "os_rwlock_unlock", "os_rwlock_lock_shared",
        "os_rwlock_unlock_shared", "os_cond_signal", "os_cond_broadcast", "os_atomic_inc", "os_atomic_dec", "os_atomic_add",
        "os_atomic_load", "os_atomic_store", "os_atomic_cas", "os_atomic_xchg", "__atomic_load_n", "__atomic_store_n",
        "malloc", "calloc", "realloc", "free", "memset", "memcpy", "memmove", "getenv", "hos_now_ns", "offsetof",
    ];
    const NOT_CALLS: &[&str] = &["if", "for", "while", "switch", "return", "sizeof", "case", "SLEEP_LETS_MW_LOCK_GO"];
    let defs = definitions(&code_of(text));
    // (wait_any's own mw_lock region: no call but its one sleep, which lets mw_lock go: the fixture checks that)
    let wa = defs.get("wait_any").map(|v| v.join("\n")).unwrap_or_default();
    let region = match (wa.find("os_mutex_lock(&mw_lock)"), wa.rfind("os_mutex_unlock(&mw_lock)")) {
        (Some(s), Some(e)) if s < e => &wa[s..e],
        _ => return vec!["wait_any (its mw_lock region was not found)".into()],
    };
    let rest = Regex::new(r"\bhos_cv_sleep\s*\(\s*&mw_cv\s*,\s*&mw_lock\s*,").unwrap().replacen(region, 1, "SLEEP_LETS_MW_LOCK_GO(");
    let indirect = Regex::new(r"\(\s*\*[^()]*\)\s*\(|(->|\.)\s*\w+\s*\(|\]\s*\(").unwrap();
    let (mut bad, mut seen) = (BTreeSet::new(), BTreeSet::new());
    let mut todo: Vec<(String, Option<String>)> =
        vec![("wait_any's mw_lock region".into(), Some(rest.into_owned())), ("holder_ready".into(), None)];
    while let Some((name, body)) = todo.pop() {
        let body = match body {
            Some(b) => b,
            None => {
                if seen.contains(&name) || !defs.contains_key(&name) {
                    continue;
                }
                seen.insert(name.clone());
                defs[&name].join("\n")
            }
        };
        // (a call through a pointer, a member or an array element: (*f)(), p->f(), s.f(), f[i]())
        if indirect.is_match(&body) {
            bad.insert(format!("{name} (an indirect call)"));
        }
        for c in calls(&body) {
            if defs.contains_key(&c) {
                todo.push((c, None));
            } else if !SAFE.contains(&c.as_str()) && !NOT_CALLS.contains(&c.as_str()) {
                bad.insert(format!("{name} (calls {c})"));
            }
        }
    }
    if !seen.contains("holder_ready") {
        bad.insert("holder_ready (not found)".into());
    }
    bad.into_iter().collect()
}

/// the functions of this file that, holding a lock WaitAny's scan takes (mw_lock, table_lock, an object's lk), wait
/// for a virtual core themselves or through what of this file they call (hos_cv_sleep is fine: it lets the lock go
/// before taking a core back)
fn locked_regions_unsafe(text: &str) -> Vec<String> {
    let core_wait =
        Regex::new(r"\b(hos_vcore_\w+|vc_\w+|a64_poll\w*|hos_yield\w*|hos_thread_wait|hos_sleep\w*|os_sleep\w*|os_yield)\s*\(").unwrap();
    let scan_lock = Regex::new(r"os_(?:mutex|rwlock)_lock(?:_shared)?\(\s*&\s*(mw_lock|table_lock|\w+\s*->\s*lk)\s*\)").unwrap();
    let defs = definitions(&code_of(text));
    fn waits_for_core(name: &str, stack: &mut Vec<String>, defs: &HashMap<String, Vec<String>>, core_wait: &Regex,
                      memo: &mut HashMap<String, bool>) -> bool {
        if !memo.contains_key(name) {
            let v = if stack.iter().any(|s| s == name) {
                false
            } else {
                stack.push(name.to_string());
                let mut any = false;
                for b in &defs[name] {
                    if core_wait.is_match(b)
                        || calls(b).iter().any(|c| defs.contains_key(c) && waits_for_core(c, stack, defs, core_wait, memo))
                    {
                        any = true;
                        break;
                    }
                }
                stack.pop();
                any
            };
            memo.insert(name.to_string(), v);
        }
        memo[name]
    }
    let mut memo = HashMap::new();
    let mut bad = BTreeSet::new();
    let ws = Regex::new(r"\s").unwrap();
    for (name, bodies) in &defs {
        for b in bodies {
            for m in scan_lock.captures_iter(b) {
                let lk = ws.replace_all(&m[1], "").into_owned();
                let name_pat = lk.split("->").map(regex::escape).collect::<Vec<_>>().join(r"\s*->\s*");   // (t->lk: any spacing)
                let rel = Regex::new(&format!(r"os_(?:mutex|rwlock)_unlock(?:_shared)?\(\s*&\s*{name_pat}\s*\)")).unwrap();
                let after = &b[m.get(0).unwrap().end()..];
                let region = rel.find(after).map(|r| &after[..r.start()]).unwrap_or(after);
                if core_wait.is_match(region)
                    || calls(region).iter().any(|c| defs.contains_key(c) && waits_for_core(c, &mut Vec::new(), &defs, &core_wait, &mut memo))
                {
                    bad.insert(format!("{name} (holding {lk})"));
                }
            }
        }
    }
    bad.into_iter().collect()
}

/// the fixture: the watched-address table, poke_multiwait, holder_watch, Link/Unlink/UnlinkAll/MoveAll, the real
/// event_wait (and event_peek), holder_ready and wait_any
fn multiwait_fixture(source: &str) -> String {
    let mut parts = vec![
        between(source, "typedef struct { uint64_t key; int n; } MwWatched;", "/* o: the object that changed").to_string(),
        block(source, "static void poke_multiwait(").to_string(),
        block(source, "static uint64_t holder_watch(").to_string(),
    ];
    let mut defines = Vec::new();
    for (alias, name) in [("LINK", "LinkMultiWaitHolder"), ("UNLINK", "UnlinkMultiWaitHolder"),
                          ("UNLINKALL", "UnlinkAllMultiWaitHolder"), ("MOVEALL", "MoveAllMultiWaitHolder")] {
        let m = Regex::new(&format!(r"HLE\((_ZN2nn2os\d+{name}E\w*)\)")).unwrap().captures(source).unwrap_or_else(|| panic!("no HLE of {name}"));
        parts.push(block(source, &m[0]).to_string());
        defines.push(format!("#define {alias} {}", &m[1]));
    }
    parts.push(block(source, "static int event_wait(Obj *e, int64_t timeout_ns, int try_only) {").to_string());
    if source.contains("static int event_peek(Obj *e) {") {
        parts.push(block(source, "static int event_peek(Obj *e) {").to_string());
    }
    parts.push(block(source, "static int holder_ready(").to_string());
    parts.push(block(source, "static uint64_t wait_any(").to_string());
    parts.extend(defines);
    let holder_enum = Regex::new(r"enum \{ H_EVENT[^}]*\};").unwrap().find(source).expect("the holders' enum").as_str().to_string();
    fixture("multiwait_wake_test.cpp").replace("@HOLDER_ENUM@", &holder_enum).replace("@FUNCTIONS@", &parts.join("\n"))
}

/// No scheduling point (a call that can give the virtual core up or wait for one, or sleep: what turns mw_lock into
/// the 250 ms vcore-escape stall) while holding a lock WaitAny's scan takes, nor in anything WaitAny's scan reaches
/// under mw_lock (an allowlist of the calls there). Each check is shown to catch its controls; a control whose
/// anchor is gone fails, so a reshaped function updates the check. Every poke_multiwait call passes the object that
/// changed.
#[test]
fn multiwait_scan_has_no_scheduling_point() {
    let source = read("runtime/nn_os_sync.c");
    let unsafe_ = locked_regions_unsafe(&source);
    assert!(unsafe_.is_empty(), "sleeps or yields while holding a lock WaitAny's scan takes: {}", unsafe_.join(", "));
    for (what, old, new) in [
        ("a checkpoint in event_signal holding e->lk", "    os_mutex_lock(&e->lk);\n    e->signaled = 1;",
         "    os_mutex_lock(&e->lk);\n    hos_vcore_checkpoint();\n    e->signaled = 1;"),
        ("a sleep under table_lock", "static Obj *obj_lookup(uint64_t key, int kind) {",
         "static Obj *obj_lookup(uint64_t key, int kind) { os_mutex_lock(&table_lock); os_sleep_ns(1000000); os_mutex_unlock(&table_lock);"),
    ] {
        assert!(source.contains(old), "the locked-region check's control '{what}' lost its anchor: update the test");
        assert!(!locked_regions_unsafe(&source.replacen(old, new, 1)).is_empty(), "the locked-region check missed {what}");
    }

    let unsafe_ = lookup_graph_unsafe(&source);
    assert!(unsafe_.is_empty(), "reached from WaitAny's scan under mw_lock, a scheduling point in {}", unsafe_.join(", "));
    let callee = Regex::new(r"(?m)^static [^;{}()]*?\bfind_unlocked\s*\([^;{]*\)\s*\{").unwrap().find(&source).map(|m| m.as_str().to_string());
    let nockpt = "static Obj *obj_nockpt(uint64_t key, int kind) {";
    let lookup = "static Obj *obj_lookup(uint64_t key, int kind) {";
    let controls: Vec<(&str, String, String)> = vec![
        ("a checkpoint in obj_nockpt", nockpt.into(), format!("{nockpt} hos_vcore_checkpoint();")),
        ("a vcore yield in obj_nockpt", nockpt.into(), format!("{nockpt} hos_vcore_yield();")),
        ("a sleep in obj_lookup", lookup.into(), format!("{lookup} os_sleep_ns(1000000);")),
        ("a call through a pointer in obj_nockpt", nockpt.into(), format!("{nockpt} (*lookup_hook)();")),
        ("a call through an array element in obj_nockpt", nockpt.into(), format!("{nockpt} lookup_observers[0]();")),
        ("event_wait back in the scan", "return event_peek(obj_nockpt(h->event, K_EVENT));".into(),
         "return event_wait(obj_nockpt(h->event, K_EVENT), 0, 1);".into()),
        ("a checkpoint in event_peek", "static int event_peek(Obj *e) {".into(), "static int event_peek(Obj *e) { hos_vcore_checkpoint();".into()),
        ("a periodic checkpoint in WaitAny's holder loop", "for (int i = 0; i < mw->nlinks && !found; i++) {".into(),
         "for (int i = 0; i < mw->nlinks && !found; i++) { if (i && (i & 63) == 0) hos_vcore_checkpoint();".into()),
        ("a second sleep in WaitAny's mw_lock region", "        if (found) break;".into(),
         "        if (found) break;\n        hos_cv_sleep(&mw_cv, &mw_lock, 0);".into()),
        ("a checkpoint in find_unlocked", callee.clone().unwrap_or_else(|| "static ... find_unlocked(...) {".into()),
         callee.as_ref().map(|c| format!("{c} hos_vcore_checkpoint();")).unwrap_or_default()),
    ];
    for (what, old, new) in &controls {
        assert!(source.contains(old.as_str()), "the check's control '{what}' lost its anchor {:?}: update the test", &old[..old.len().min(60)]);
        assert!(!lookup_graph_unsafe(&source.replacen(old.as_str(), new, 1)).is_empty(), "the check missed {what}");
    }
    let second = format!("{source}\n#if 0\nstatic Obj *find_unlocked(uint64_t key, int kind) {{ hos_vcore_checkpoint(); return 0; }}\n#endif\n");
    assert!(callee.is_none() || !lookup_graph_unsafe(&second).is_empty(), "the check missed a checkpoint in a second (#else) find_unlocked");

    let pokes: Vec<String> =
        Regex::new(r"poke_multiwait\(([^)]*)\);").unwrap().captures_iter(&source).map(|c| c[1].to_string()).collect();
    let word = Regex::new(r"^\w+$").unwrap();
    assert!(!pokes.is_empty() && pokes.iter().all(|c| word.is_match(c)), "a poke_multiwait call without the object: {pokes:?}");
}

/// A WaitAny is woken only by an object some holder links into a multi-wait (nn_os_sync.c poke_multiwait), compiled
/// with a harness that counts WakeAllConditionVariable and every object lookup: an unwatched change wakes no WaitAny;
/// link, unlink, unlink all and move all keep the counts by address; a re-initialized watched event still wakes;
/// finalize before unlink resurrects nothing; no watched-target lookups and no lookups under mw_lock; WaitAny's scan
/// (try, timed, untimed) reaches no spin, timed wait or vcore call under mw_lock; an allocation failure keeps wakes.
#[test]
fn multiwait_wake() {
    let source = read("runtime/nn_os_sync.c");
    let t = Tmp::new("multiwait-wake");
    let exe = build(&t, &cxx(), "fixture.cpp", &multiwait_fixture(&source), &["-std=c++17", "-O1", "-w"], &[]);
    run(&mut clean(&exe));
}
