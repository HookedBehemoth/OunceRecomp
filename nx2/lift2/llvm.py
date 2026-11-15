"""The LLVM tools the lifter depends on, pinned to one major version.

The emitter writes textual LLVM IR, whose syntax is only guaranteed for the LLVM major it was written for, and the
Linux clang and the pinned llvm-mingw must read the same IR. check() refuses any other major with a message that says
which one was found and what to do, before anything is compiled.
"""
import os
import re
import shutil
import subprocess

PINNED_MAJOR = 23
MC_ATTRS = "+v8.5a,+lse,+rcpc,+pauth,+dotprod,+fullfp16,+fp16fml,+i8mm,+bf16,+crc,+mte,+aes,+sha2"

_checked = {}


class VersionError(SystemExit):
    pass


def tool(name):
    """the path of an LLVM tool (HOOKSHOT_LLVM_BIN first, then PATH), or None"""
    d = os.environ.get("HOOKSHOT_LLVM_BIN")
    if d:
        p = os.path.join(d, name + (".exe" if os.name == "nt" else ""))
        if os.path.exists(p):
            return p
    return shutil.which(name)


def major(path):
    out = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(?:clang|LLVM) version (\d+)\.", out)
    return int(m.group(1)) if m else None


def check(name="clang"):
    """the tool's path, after checking that its major is PINNED_MAJOR (VersionError otherwise)"""
    if name in _checked:
        return _checked[name]
    path = tool(name)
    if path is None:
        raise VersionError("lift: %s not found (LLVM %d is needed; HOOKSHOT_LLVM_BIN names its bin folder)"
                           % (name, PINNED_MAJOR))
    got = major(path)
    if got != PINNED_MAJOR:
        raise VersionError("lift: %s is LLVM %s, but the emitted IR is written for LLVM %d. Install LLVM %d "
                           "(HOOKSHOT_LLVM_BIN=<its bin folder>), or move nx2/lift2/llvm.py PINNED_MAJOR and check "
                           "the emitter against the new version's IR syntax." % (path, got, PINNED_MAJOR, PINNED_MAJOR))
    _checked[name] = path
    return path


def mc_disassemble(words):
    """llvm-mc's text for each word ("mnemonic operands", comments dropped, single spaces), None for an invalid one"""
    mc = check("llvm-mc")
    src = "".join("0x%02x 0x%02x 0x%02x 0x%02x\n" % (w & 255, w >> 8 & 255, w >> 16 & 255, w >> 24) for w in words)
    r = subprocess.run([mc, "--disassemble", "-triple=aarch64", "-mattr=" + MC_ATTRS, "-M", "no-aliases"],
                       input=src, capture_output=True, text=True)
    bad = {int(m.group(1)) - 1 for m in re.finditer(r"<stdin>:(\d+):\d+: warning: invalid instruction encoding",
                                                    r.stderr)}
    lines = [l for l in r.stdout.split("\n") if l.startswith("\t")]
    out, k = [], 0
    for i in range(len(words)):
        if i in bad:
            out.append(None)
            continue
        t = lines[k].split("//")[0].strip()
        k += 1
        mn, _, ops = t.partition("\t")
        out.append((mn + " " + re.sub(r"\s+", " ", ops.strip())).strip())
    if k != len(lines):
        raise RuntimeError("llvm-mc: %d lines for %d valid words" % (len(lines), k))
    return out
