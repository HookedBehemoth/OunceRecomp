//! The host's patch declarations, read from its C source (nx2/lift2/patch_scan.py; host/patch.h says how each is
//! written): a token scanner of a fixed grammar (comments, literals, line continuations, nested parentheses; no C
//! parser), the declarations' grammar and cross-checks, each error with file:line exactly as the Python side words
//! it; the rows they make (probes.Value, probes.At) resolved to their functions over the plan with the lift's checks
//! (probes.resolve, errors); the HLE'd functions' declared effects (host_effects); the PATCH_FUNCs against the plan
//! (check_funcs).

use crate::export::{GuestCalls, HleFact, MetaAt, MetaValue, PatchDecl, RowEffect};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::Path;

pub const KINDS: [&str; 8] = ["PATCH_AT", "PATCH_AT_SITE", "PATCH_FUNC", "PATCH_FUNC_SYM", "HLE", "NV", "HLE_EFFECTS", "PATCH_ANSWER"];

/// (file, line) of a declaration
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Where(pub String, pub u32);

pub type Res<T> = Result<T, String>;

pub fn fail<T>(w: &Where, msg: impl AsRef<str>) -> Res<T> {
    Err(format!("patch declarations: {}:{}: {}", w.0, w.1, msg.as_ref()))
}

// ---- tokens

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TK {
    Id,
    Num,
    Str,
    Chr,
    P,
    Pp,
}

#[derive(Clone, Debug)]
pub struct Tok {
    pub kind: TK,
    pub text: String,
    pub line: u32,
}

const PUNCT3: [&str; 3] = ["<<=", ">>=", "..."];
const PUNCT2: [&str; 20] = ["->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##"];

fn starts(t: &[char], i: usize, s: &str) -> bool {
    (i..).zip(s.chars()).all(|(k, c)| t.get(k) == Some(&c))
}

fn find(t: &[char], from: usize, s: &str) -> Option<usize> {
    (from..t.len()).find(|&k| starts(t, k, s))
}

/// patch_scan.tokens: comments and line continuations go; a directive is one "pp" token
pub fn tokens(text: &str, path: &str) -> Res<Vec<Tok>> {
    let t: Vec<char> = text.chars().collect();
    let n = t.len();
    let mut out = Vec::new();
    let (mut i, mut line, mut bol) = (0usize, 1u32, true);
    let splice = |mut j: usize, line: &mut u32| {
        while starts(&t, j, "\\\n") {
            j += 2;
            *line += 1;
        }
        j
    };
    while i < n {
        i = splice(i, &mut line);
        if i >= n {
            break;
        }
        let c = t[i];
        if c == '\n' {
            line += 1;
            bol = true;
            i += 1;
            continue;
        }
        if matches!(c, ' ' | '\t' | '\r' | '\u{c}' | '\u{b}') {
            i += 1;
            continue;
        }
        if starts(&t, i, "//") {
            while i < n && t[i] != '\n' {
                i = if t[i] != '\\' {
                    splice(i + 1, &mut line)
                } else if starts(&t, i, "\\\n") {
                    splice(i, &mut line)
                } else {
                    i + 1
                };
            }
            continue;
        }
        if starts(&t, i, "/*") {
            let j = match find(&t, i + 2, "*/") {
                Some(j) => j,
                None => return fail(&Where(path.into(), line), "a comment that does not end"),
            };
            line += t[i..j].iter().filter(|&&c| c == '\n').count() as u32;
            i = j + 2;
            continue;
        }
        let start = line;
        if c == '#' && bol {
            let mut buf = String::new();
            i += 1;
            while i < n && t[i] != '\n' {
                if starts(&t, i, "\\\n") {
                    i = splice(i, &mut line);
                    buf.push(' ');
                    continue;
                }
                if starts(&t, i, "/*") {
                    let j = match find(&t, i + 2, "*/") {
                        Some(j) => j,
                        None => return fail(&Where(path.into(), line), "a comment that does not end"),
                    };
                    line += t[i..j].iter().filter(|&&c| c == '\n').count() as u32;
                    i = j + 2;
                    buf.push(' ');
                    continue;
                }
                if starts(&t, i, "//") {
                    while i < n && t[i] != '\n' {
                        i += 1;
                    }
                    break;
                }
                buf.push(t[i]);
                i += 1;
            }
            out.push(Tok { kind: TK::Pp, text: buf.trim().to_string(), line: start });
            bol = false;
            continue;
        }
        if c == '"' || c == '\'' {
            let mut j = i + 1;
            loop {
                if j >= n || t[j] == '\n' {
                    return fail(&Where(path.into(), start), "a literal that does not end");
                }
                if t[j] == '\\' {
                    j += 2;
                    continue;
                }
                if t[j] == c {
                    break;
                }
                j += 1;
            }
            out.push(Tok { kind: if c == '"' { TK::Str } else { TK::Chr }, text: t[i..j + 1].iter().collect(), line: start });
            i = j + 1;
        } else if c.is_alphabetic() || c == '_' {
            let mut j = i;
            while j < n && (t[j].is_alphanumeric() || t[j] == '_') {
                j += 1;
            }
            out.push(Tok { kind: TK::Id, text: t[i..j].iter().collect(), line: start });
            i = j;
        } else if c.is_ascii_digit() || (c == '.' && i + 1 < n && t[i + 1].is_ascii_digit()) {
            let mut j = i;
            while j < n && (t[j].is_alphanumeric() || t[j] == '.' || t[j] == '_' || ((t[j] == '+' || t[j] == '-') && matches!(t[j - 1], 'e' | 'E' | 'p' | 'P'))) {
                j += 1;
            }
            out.push(Tok { kind: TK::Num, text: t[i..j].iter().collect(), line: start });
            i = j;
        } else {
            let p = PUNCT3.iter().chain(PUNCT2.iter()).find(|q| starts(&t, i, q)).map(|q| q.to_string()).unwrap_or_else(|| c.to_string());
            i += p.chars().count();
            out.push(Tok { kind: TK::P, text: p, line: start });
        }
        bol = false;
    }
    Ok(out)
}

// ---- declarations

#[derive(Clone, Debug)]
pub struct Decl {
    pub kind: &'static str,
    pub args: Vec<Vec<Tok>>,
    pub at: Where,
}

fn words(s: &str) -> Vec<String> {
    let mut w = Vec::new();
    let mut cur = String::new();
    for c in s.chars() {
        if c.is_alphanumeric() || c == '_' {
            cur.push(c);
        } else if !cur.is_empty() {
            w.push(std::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() {
        w.push(cur);
    }
    w
}

/// patch_scan.declarations: one file's
pub fn declarations(text: &str, path: &str) -> Res<Vec<Decl>> {
    let toks = tokens(text, path)?;
    let (mut out, mut depth, mut cond) = (Vec::new(), 0i64, Vec::<u32>::new());
    let mut i = 0;
    while i < toks.len() {
        let tk = &toks[i];
        match tk.kind {
            TK::Pp => {
                let word = tk.text.split_whitespace().next().unwrap_or("");
                if matches!(word, "if" | "ifdef" | "ifndef") {
                    cond.push(tk.line);
                } else if word == "endif" {
                    if cond.pop().is_none() {
                        return fail(&Where(path.into(), tk.line), "#endif without #if");
                    }
                } else if word == "define" {
                    let w = words(&tk.text);
                    if w.len() > 2 && !KINDS.contains(&w[1].as_str()) && w[2..].iter().any(|x| KINDS.contains(&x.as_str())) {
                        return fail(&Where(path.into(), tk.line), format!("a declaration made by another macro (#define {})", w[1]));
                    }
                }
                i += 1;
                continue;
            }
            TK::P => {
                depth += match tk.text.as_str() {
                    "{" => 1,
                    "}" => -1,
                    _ => 0,
                };
                i += 1;
                continue;
            }
            TK::Id if KINDS.contains(&tk.text.as_str()) => {
                let kind = *KINDS.iter().find(|k| **k == tk.text).unwrap();
                let at = Where(path.into(), tk.line);
                if depth != 0 {
                    return fail(&at, format!("{kind} inside a function or an initializer: declarations stand at file scope"));
                }
                if let Some(c) = cond.last() {
                    return fail(&at, format!("{kind} inside conditional compilation (#if at line {c})"));
                }
                if i + 1 >= toks.len() || toks[i + 1].text != "(" {
                    return fail(&at, format!("{kind} without its arguments"));
                }
                let (mut args, mut cur, mut par, mut j) = (Vec::new(), Vec::new(), 0i64, i + 2);
                loop {
                    if j >= toks.len() {
                        return fail(&at, format!("{kind}'s arguments do not end"));
                    }
                    let t = &toks[j];
                    if t.kind == TK::Pp {
                        return fail(&at, format!("a directive inside {kind}'s arguments"));
                    }
                    if t.text == "(" {
                        par += 1;
                    } else if t.text == ")" {
                        if par == 0 {
                            break;
                        }
                        par -= 1;
                    }
                    if t.text == "," && par == 0 {
                        args.push(std::mem::take(&mut cur));
                    } else {
                        cur.push(t.clone());
                    }
                    j += 1;
                }
                if !cur.is_empty() || !args.is_empty() {
                    args.push(cur);
                }
                out.push(Decl { kind, args, at });
                i = j + 1;
                continue;
            }
            _ => {}
        }
        i += 1;
    }
    if let Some(c) = cond.last() {
        return fail(&Where(path.into(), *c), "#if without #endif");
    }
    Ok(out)
}

// ---- the grammar

fn joined(arg: &[Tok]) -> String {
    arg.iter().map(|t| t.text.as_str()).collect::<Vec<_>>().join(" ")
}

/// Python's int(text.rstrip("uUlL"), 0)
fn py_int(s: &str) -> Option<i128> {
    let s = s.trim_end_matches(['u', 'U', 'l', 'L']);
    let (radix, body) = match s.get(..2).map(|p| p.to_ascii_lowercase()) {
        Some(p) if p == "0x" => (16, &s[2..]),
        Some(p) if p == "0o" => (8, &s[2..]),
        Some(p) if p == "0b" => (2, &s[2..]),
        _ => (10, s),
    };
    let body = body.strip_prefix('_').filter(|_| radix != 10).unwrap_or(body);
    if body.is_empty() || body.starts_with('_') || body.ends_with('_') || body.contains("__") {
        return None;
    }
    let digits: String = body.chars().filter(|&c| c != '_').collect();
    if radix == 10 && digits.len() > 1 && digits.starts_with('0') && digits.chars().any(|c| c != '0') {
        return None;
    }
    i128::from_str_radix(&digits, radix).ok()
}

fn num(arg: &[Tok], at: &Where, what: &str) -> Res<i128> {
    if arg.len() != 1 || arg[0].kind != TK::Num {
        return fail(at, format!("{what}: a number, not {}", py_repr(&joined(arg))));
    }
    match py_int(&arg[0].text) {
        Some(v) => Ok(v),
        None => fail(at, format!("{what}: not a number: {}", arg[0].text)),
    }
}

/// Python's repr of a str (the error messages' %r)
fn py_repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut o = String::new();
    o.push(q);
    for c in s.chars() {
        match c {
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\t' => o.push_str("\\t"),
            '\r' => o.push_str("\\r"),
            c if c == q => {
                o.push('\\');
                o.push(c);
            }
            c => o.push(c),
        }
    }
    o.push(q);
    o
}

/// the arguments of `name(...)` written as one argument, or None
fn call<'a>(arg: &'a [Tok], name: &str) -> Option<Vec<Vec<&'a Tok>>> {
    if arg.len() < 3 || arg[0].text != name || arg[1].text != "(" || arg[arg.len() - 1].text != ")" {
        return None;
    }
    let (mut inner, mut cur, mut par) = (Vec::new(), Vec::new(), 0i64);
    for t in &arg[2..arg.len() - 1] {
        if t.text == "(" {
            par += 1;
        } else if t.text == ")" {
            par -= 1;
        }
        if t.text == "," && par == 0 {
            inner.push(std::mem::take(&mut cur));
        } else {
            cur.push(t);
        }
    }
    inner.push(cur);
    Some(inner)
}

fn owned(a: &[&Tok]) -> Vec<Tok> {
    a.iter().map(|t| (*t).clone()).collect()
}

fn ident(arg: &[Tok], at: &Where, what: &str) -> Res<String> {
    if arg.len() != 1 || arg[0].kind != TK::Id {
        return fail(at, format!("{what}: a name, not {}", py_repr(&joined(arg))));
    }
    Ok(arg[0].text.clone())
}

fn reg_in(r: &str, prefix: char, n: u32) -> bool {
    let mut c = r.chars();
    c.next() == Some(prefix) && {
        let d = c.as_str();
        !d.is_empty() && d.chars().all(|x| x.is_ascii_digit()) && (d == "0" || !d.starts_with('0')) && d.parse::<u32>().is_ok_and(|v| v < n)
    }
}
fn is_x(r: &str) -> bool {
    reg_in(r, 'x', 31)
}
fn is_w(r: &str) -> bool {
    reg_in(r, 'w', 31)
}
fn is_s(r: &str) -> bool {
    reg_in(r, 's', 32)
}
fn is_q(r: &str) -> bool {
    reg_in(r, 'q', 32)
}
fn is_v(r: &str) -> bool {
    reg_in(r, 'v', 32)
}

#[derive(Clone, Debug, PartialEq)]
pub enum Guest {
    Computed,
    List(Vec<u64>),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Operand {
    Arg(String),
    Guest(Guest),
    Io(Vec<String>),
}

/// patch_scan.operand
fn operand(arg: &[Tok], at: &Where) -> Res<Operand> {
    if let Some(inner) = call(arg, "as") {
        if inner.len() != 2 {
            return fail(at, "as(<operand>, name)");
        }
        ident(&owned(&inner[1]), at, "as's name")?;
        let o = operand(&owned(&inner[0]), at)?;
        if !matches!(o, Operand::Arg(_)) {
            return fail(at, format!("as() names a value operand, not {}", match o {
                Operand::Guest(_) => "guest",
                _ => "io",
            }));
        }
        return Ok(o);
    }
    if let Some(inner) = call(arg, "ld") {
        if inner.len() != 2 {
            return fail(at, "ld(xN or sp, K)");
        }
        let base = ident(&owned(&inner[0]), at, "ld's base")?;
        if !is_x(&base) && base != "sp" {
            return fail(at, format!("ld's base: xN or sp, not {base}"));
        }
        let off = if inner[1].len() == 1 { inner[1][0].text.clone() } else { num(&owned(&inner[1]), at, "ld's offset")?.to_string() };
        return Ok(Operand::Arg(format!("[{base}+{off}]")));
    }
    if let Some(inner) = call(arg, "add") {
        if inner.len() != 2 {
            return fail(at, "add(xN, K)");
        }
        let base = ident(&owned(&inner[0]), at, "add's base")?;
        if !is_x(&base) {
            return fail(at, format!("add's base: xN, not {base}"));
        }
        return Ok(Operand::Arg(format!("{base}+{}", num(&owned(&inner[1]), at, "add's offset")?)));
    }
    if let Some(inner) = call(arg, "k") {
        if inner.len() != 1 {
            return fail(at, "k(K)");
        }
        num(&owned(&inner[0]), at, "k's value")?;
        return Ok(Operand::Arg(format!("#{}", inner[0][0].text)));
    }
    if let Some(inner) = call(arg, "guest") {
        if inner.len() == 1 && inner[0].len() == 1 && inner[0][0].text == "computed" {
            return Ok(Operand::Guest(Guest::Computed));
        }
        let mut v = Vec::new();
        for a in &inner {
            v.push(num(&owned(a), at, "a guest callee")? as u64);
        }
        return Ok(Operand::Guest(Guest::List(v)));
    }
    if let Some(inner) = call(arg, "io") {
        let mut regs = Vec::new();
        for a in &inner {
            regs.push(ident(&owned(a), at, "an io register")?);
        }
        let bad: Vec<&String> = regs.iter().filter(|r| !(is_x(r) || is_w(r) || is_q(r))).collect();
        if !bad.is_empty() {
            return fail(at, format!("io registers: xN, wN or qN, not {}", bad.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")));
        }
        return Ok(Operand::Io(regs));
    }
    let r = ident(arg, at, "an operand")?;
    if !(is_x(&r) || is_w(&r) || is_s(&r) || is_q(&r)) && r != "sp" {
        return fail(at, format!("unknown operand {r}"));
    }
    Ok(Operand::Arg(r))
}

/// a PATCH_AT / PATCH_AT_SITE, parsed
#[derive(Clone, Debug)]
pub struct Site {
    pub kind: &'static str,
    pub at: Where,
    pub pc: u64,
    pub word: u32,
    pub mode: String,
    pub result: Option<String>,
    pub fname: String,
    pub args: Vec<String>,
    pub guest: Option<Guest>,
    pub io: Vec<String>,
}

fn is_result(r: &str) -> bool {
    is_x(r) || is_w(r) || is_s(r) || is_q(r) || r == "none"
}

impl Site {
    fn new(d: &Decl) -> Res<Site> {
        let (a, at) = (&d.args, &d.at);
        if a.len() < 5 {
            return fail(at, format!("{}(pc, word(W), mode, result, fn, operand...)", d.kind));
        }
        let pc = num(&a[0], at, "pc")? as u64;
        let w = call(&a[1], "word");
        let w = match w {
            Some(w) if w.len() == 1 => w,
            _ => return fail(at, format!("the instruction word is written word(0x...), not {}", py_repr(&joined(&a[1])))),
        };
        let word = num(&owned(&w[0]), at, "word")? as u32;
        let mode = ident(&a[2], at, "mode")?;
        if !matches!(mode.as_str(), "after" | "before" | "instead") {
            return fail(at, format!("mode: after, before or instead, not {mode}"));
        }
        let res = ident(&a[3], at, "result")?;
        if !is_result(&res) {
            return fail(at, format!("unknown result {res}"));
        }
        let fname = ident(&a[4], at, "the host function")?;
        let mut s = Site { kind: d.kind, at: at.clone(), pc, word, mode, result: (res != "none").then_some(res), fname, args: Vec::new(), guest: None, io: Vec::new() };
        let mut ops = Vec::new();
        for x in &a[5..] {
            if !x.is_empty() {
                ops.push(operand(x, at)?);
            }
        }
        let nops = ops.len();
        for (k, o) in ops.into_iter().enumerate() {
            match o {
                Operand::Guest(g) => {
                    if k != 0 {
                        return fail(at, "guest(...) comes first");
                    }
                    s.guest = Some(g);
                }
                Operand::Io(r) => {
                    if k != nops - 1 {
                        return fail(at, "io(...) comes last");
                    }
                    if d.kind == "PATCH_AT_SITE" {
                        return fail(at, "io(...) only in the PATCH_AT of its function");
                    }
                    s.io = r;
                }
                Operand::Arg(v) => s.args.push(v),
            }
        }
        Ok(s)
    }
}

/// an operand's or result's C type (patch_scan.ctype)
fn ctype(a: Option<&str>) -> &'static str {
    let Some(a) = a else { return "void" };
    let mut c = a.chars();
    let f = c.next().unwrap_or(' ');
    let rest = c.as_str();
    if (matches!(f, 'w' | 's' | 'q') && !rest.is_empty() && rest.chars().all(|x| x.is_ascii_digit())) || f == '#' {
        return match f {
            'w' | '#' => "uint32_t",
            's' => "float",
            _ => "hos_q",
        };
    }
    "uint64_t"
}

fn signature(s: &Site) -> (&'static str, bool, Vec<&'static str>, Vec<String>) {
    (ctype(s.result.as_deref()), s.guest.is_some(), s.args.iter().map(|a| ctype(Some(a))).collect(), s.io.clone())
}

/// (reads, clobbers) of a declaration's effect clauses (patch_scan._effects, allowed: reads, clobbers)
fn effects(args: &[&Vec<Tok>], at: &Where) -> Res<(Vec<String>, Option<String>)> {
    let allowed = ["reads", "clobbers"];
    let mut reads: Option<Vec<String>> = None;
    let mut clobbers: Option<String> = None;
    for a in args {
        let name = a.first().filter(|t| t.kind == TK::Id).map(|t| t.text.clone());
        let inner = name.as_ref().and_then(|n| call(a, n));
        let (Some(name), Some(inner)) = (name, inner) else {
            return fail(at, format!("an effect: reads(...) or clobbers(...), not {}", py_repr(&joined(a))));
        };
        if !allowed.contains(&name.as_str()) {
            return fail(at, format!("an effect: reads(...) or clobbers(...), not {}", py_repr(&joined(a))));
        }
        if (name == "reads" && reads.is_some()) || (name == "clobbers" && clobbers.is_some()) {
            return fail(at, format!("{name}(...) twice"));
        }
        if name == "reads" {
            let last = inner.last().unwrap();
            if inner.len() < 2 || last.len() != 1 || last[0].kind != TK::Str {
                return fail(at, "reads(reg..., \"why\"): the registers, then the reason as a string");
            }
            let mut regs = Vec::new();
            for r in &inner[..inner.len() - 1] {
                regs.push(ident(&owned(r), at, "a register read")?);
            }
            let bad: Vec<&String> = regs.iter().filter(|r| !(is_x(r) || is_v(r) || *r == "sp")).collect();
            if !bad.is_empty() {
                return fail(at, format!("reads: xN, vN or sp, not {}", bad.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")));
            }
            reads = Some(regs);
        } else {
            if inner.len() != 1 || inner[0].len() != 1 || inner[0][0].kind != TK::Str {
                return fail(at, "clobbers(\"why\"): the reason as a string");
            }
            clobbers = Some(inner[0][0].text.clone());
        }
    }
    Ok((reads.unwrap_or_default(), clobbers))
}

/// a PATCH_FUNC / PATCH_FUNC_SYM, parsed
#[derive(Clone, Debug)]
pub struct Func {
    pub kind: &'static str,
    pub at: Where,
    pub addr: Option<u64>,
    pub word: Option<u32>,
    pub body: Option<u64>,
    pub symbol: Option<String>,
    pub name: String,
    pub orig_name: String,
    pub result: Option<String>,
    pub fname: String,
    pub orig: bool,
    pub guest: Vec<u64>,
    pub args: Vec<String>,
}

fn is_func_operand(r: &str) -> bool {
    reg_in(r, 'x', 9) || reg_in(r, 'w', 9) || reg_in(r, 's', 8) || reg_in(r, 'q', 8) || r == "x30" || r == "sp"
}

impl Func {
    fn new(d: &Decl) -> Res<Func> {
        let (a, at) = (&d.args, &d.at);
        let (mut addr, mut word, mut body, mut symbol) = (None, None, None, None);
        let (key, rest) = if d.kind == "PATCH_FUNC" {
            if a.len() < 6 {
                return fail(at, "PATCH_FUNC(addr, word(W), body(H), aapcs, result, fn, operand...)");
            }
            addr = Some(num(&a[0], at, "the function's address")? as u64);
            let w = match call(&a[1], "word") {
                Some(w) if w.len() == 1 => w,
                _ => return fail(at, format!("the function's first word is written word(0x...), not {}", py_repr(&a[1].iter().map(|t| t.text.as_str()).collect::<Vec<_>>().join(" ")))),
            };
            word = Some(num(&owned(&w[0]), at, "word")? as u32);
            let b = match call(&a[2], "body") {
                Some(b) if b.len() == 1 => b,
                _ => return fail(at, format!("the function's body hash is written body(0x...), not {}", py_repr(&a[2].iter().map(|t| t.text.as_str()).collect::<Vec<_>>().join(" ")))),
            };
            body = Some(num(&owned(&b[0]), at, "body")? as u64);
            (a[0][0].text.clone(), &a[3..])
        } else {
            if a.len() < 4 || a[0].is_empty() {
                return fail(at, "PATCH_FUNC_SYM(symbol, aapcs, result, fn, operand...)");
            }
            let s = ident(&a[0], at, "the symbol")?;
            symbol = Some(s.clone());
            (format!("sym_{s}"), &a[1..])
        };
        let abi = ident(&rest[0], at, "abi")?;
        if abi != "aapcs" {
            return fail(at, format!("abi: aapcs, not {abi} (a function of the context is an HLE(...))"));
        }
        let res = ident(&rest[1], at, "result")?;
        let results = ["none", "x0", "w0", "s0", "q0"];
        if !results.contains(&res.as_str()) {
            return fail(at, format!("a function's result: {}, not {res}", results.join(", ")));
        }
        let fname = ident(&rest[2], at, "the host function")?;
        let mut f = Func {
            kind: d.kind,
            at: at.clone(),
            addr,
            word,
            body,
            symbol,
            name: format!("hos_func_{key}"),
            orig_name: format!("hos_orig_{key}"),
            result: (res != "none").then_some(res),
            fname,
            orig: false,
            guest: Vec::new(),
            args: Vec::new(),
        };
        let ops: Vec<&Vec<Tok>> = rest[3..].iter().filter(|x| !x.is_empty()).collect();
        if ops.len() > 16 {
            return fail(at, format!("{} operands: host/patch.h takes at most 16", ops.len()));
        }
        for (k, x) in ops.iter().enumerate() {
            if x.len() == 1 && x[0].text == "orig" {
                if k != 0 {
                    return fail(at, "orig comes first");
                }
                f.orig = true;
                continue;
            }
            match operand(x, at)? {
                Operand::Guest(g) => {
                    if k != f.orig as usize {
                        return fail(at, "guest(...) comes first (after orig)");
                    }
                    match g {
                        Guest::Computed => return fail(at, "a function names the guest functions it calls"),
                        Guest::List(l) => f.guest = l,
                    }
                }
                Operand::Io(_) => return fail(at, "io(...) is a PATCH_AT's: a function returns its result"),
                Operand::Arg(v) if !is_func_operand(&v) => {
                    return fail(at, format!("a function's operand: x0-x8, w0-w8, s0-s7, q0-q7, x30 or sp, not {v}"))
                }
                Operand::Arg(v) => f.args.push(v),
            }
        }
        if !f.guest.is_empty() && !f.args.iter().any(|a| a == "sp") {
            return fail(at, "guest(...) runs on the call's stack: sp is an operand (guest_call's sp)");
        }
        Ok(f)
    }
}

/// an HLE / NV / HLE_EFFECTS
#[derive(Clone, Debug)]
pub struct Replaced {
    pub kind: &'static str,
    pub at: Where,
    pub symbol: String,
    pub reads: Vec<String>,
    pub clobbers: Option<String>,
}

impl Replaced {
    fn new(d: &Decl) -> Res<Replaced> {
        let (a, at) = (&d.args, &d.at);
        if a.is_empty() || a[0].is_empty() {
            return fail(at, format!("{}(symbol, effect...)", d.kind));
        }
        let symbol = ident(&a[0], at, "the symbol")?;
        let eff: Vec<&Vec<Tok>> = a[1..].iter().filter(|x| !x.is_empty()).collect();
        let (reads, clobbers) = effects(&eff, at)?;
        if d.kind == "HLE_EFFECTS" && reads.is_empty() && clobbers.is_none() {
            return fail(at, "HLE_EFFECTS without an effect");
        }
        Ok(Replaced { kind: d.kind, at: at.clone(), symbol, reads, clobbers })
    }
}

/// a PATCH_ANSWER
#[derive(Clone, Debug)]
pub struct Answer {
    pub at: Where,
    pub symbol: String,
    pub value: i128,
}

impl Answer {
    fn new(d: &Decl) -> Res<Answer> {
        let (a, at) = (&d.args, &d.at);
        if a.len() != 3 {
            return fail(at, "PATCH_ANSWER(symbol, value, \"why\")");
        }
        let symbol = ident(&a[0], at, "the symbol")?;
        let value = num(&a[1], at, "the answer")?;
        if a[2].len() != 1 || a[2][0].kind != TK::Str || a[2][0].text.chars().count() <= 2 {
            return fail(at, "PATCH_ANSWER(symbol, value, \"why\"): the reason as a string");
        }
        Ok(Answer { at: at.clone(), symbol, value })
    }
}

/// the scan's result (patch_scan.PatchSet)
#[derive(Default, Debug)]
pub struct PatchSet {
    pub sites: Vec<Site>,
    pub funcs: Vec<Func>,
    pub hle: BTreeMap<String, Replaced>,
    pub nv: BTreeMap<String, Replaced>,
    pub declared: BTreeMap<String, Replaced>,
    pub answers: BTreeMap<String, Answer>,
}

/// patch_scan.scan_texts: [(file name, text)]
pub fn scan_texts(texts: &[(String, String)]) -> Res<PatchSet> {
    let (mut sites, mut replaced, mut funcs, mut answers) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
    for (rel, text) in texts {
        if !KINDS.iter().any(|k| text.contains(k)) {
            continue;
        }
        for d in declarations(text, rel)? {
            match d.kind {
                "PATCH_AT" | "PATCH_AT_SITE" => sites.push(Site::new(&d)?),
                "PATCH_FUNC" | "PATCH_FUNC_SYM" => funcs.push(Func::new(&d)?),
                "PATCH_ANSWER" => answers.push(Answer::new(&d)?),
                _ => replaced.push(Replaced::new(&d)?),
            }
        }
    }
    let mut first: HashMap<&str, &Site> = HashMap::new();
    for s in &sites {
        if s.kind == "PATCH_AT" {
            if let Some(f) = first.get(s.fname.as_str()) {
                return fail(&s.at, format!("a second PATCH_AT of {} (the first at {}:{}): further sites are PATCH_AT_SITE", s.fname, f.at.0, f.at.1));
            }
            first.insert(&s.fname, s);
        }
    }
    for s in &sites {
        if s.kind == "PATCH_AT_SITE" {
            let f = first.get(s.fname.as_str());
            let f = match f {
                Some(f) if !(f.at.0 == s.at.0 && f.at.1 > s.at.1) => f,
                _ => return fail(&s.at, format!("PATCH_AT_SITE of {} before or without its PATCH_AT", s.fname)),
            };
            if signature(f) != signature(s) {
                return fail(&s.at, format!("PATCH_AT_SITE of {}: its result or operands differ from the PATCH_AT at {}:{}", s.fname, f.at.0, f.at.1));
            }
        }
    }
    type SiteKey<'a> = (u64, &'a str, &'a str, &'a [String], Option<&'a str>);
    let mut seen: HashMap<SiteKey, &Where> = HashMap::new();
    for s in &sites {
        let k = (s.pc, s.mode.as_str(), s.fname.as_str(), &s.args[..], s.result.as_deref());
        if let Some(w) = seen.get(&k) {
            return fail(&s.at, format!("the same site as {}:{}", w.0, w.1));
        }
        seen.insert(k, &s.at);
    }
    let mut seen: HashMap<String, &Where> = HashMap::new();
    for h in &funcs {
        let k = match (h.addr, &h.symbol) {
            (Some(a), _) => format!("#{a}"),
            (None, Some(s)) => s.clone(),
            _ => unreachable!(),
        };
        if let Some(w) = seen.get(&k) {
            return fail(&h.at, format!("a second declaration of {} (the first at {}:{})", h.name, w.0, w.1));
        }
        seen.insert(k, &h.at);
    }
    let mut fns: HashMap<&str, &Where> = HashMap::new();
    for f in &funcs {
        if let Some(w) = fns.get(f.fname.as_str()) {
            return fail(&f.at, format!("{} replaces a second function (the first at {}:{})", f.fname, w.0, w.1));
        }
        fns.insert(&f.fname, &f.at);
    }
    let mut seen: HashMap<(&str, &str), &Where> = HashMap::new();
    for r in &replaced {
        if let Some(w) = seen.get(&(r.kind, r.symbol.as_str())) {
            return fail(&r.at, format!("{} declared again (the first at {}:{})", r.symbol, w.0, w.1));
        }
        seen.insert((r.kind, &r.symbol), &r.at);
    }
    let hle: HashMap<&str, &Replaced> = replaced.iter().filter(|r| r.kind == "HLE").map(|r| (r.symbol.as_str(), r)).collect();
    for r in &replaced {
        if r.kind == "HLE_EFFECTS" {
            if let Some(h) = hle.get(r.symbol.as_str()) {
                return fail(&r.at, format!("HLE_EFFECTS of {}, which HLE declares at {}:{}: its effects go there", r.symbol, h.at.0, h.at.1));
            }
        }
    }
    let mut seen: HashMap<&str, &Where> = HashMap::new();
    for a in &answers {
        if let Some(w) = seen.get(a.symbol.as_str()) {
            return fail(&a.at, format!("PATCH_ANSWER of {} again (the first at {}:{})", a.symbol, w.0, w.1));
        }
        seen.insert(&a.symbol, &a.at);
        let other = hle.get(a.symbol.as_str()).copied().or_else(|| replaced.iter().find(|r| r.kind == "HLE_EFFECTS" && r.symbol == a.symbol));
        if let Some(o) = other {
            return fail(&a.at, format!("PATCH_ANSWER of {}, which {} declares at {}:{}", a.symbol, o.kind, o.at.0, o.at.1));
        }
    }
    let mut ps = PatchSet { sites, funcs, ..Default::default() };
    // (dict by symbol: a later declaration of one kind would replace an earlier, but the checks above refuse those)
    for r in replaced {
        let m = match r.kind {
            "HLE" => &mut ps.hle,
            "NV" => &mut ps.nv,
            _ => &mut ps.declared,
        };
        m.insert(r.symbol.clone(), r);
    }
    for a in answers {
        ps.answers.insert(a.symbol.clone(), a);
    }
    Ok(ps)
}

/// the host's declarations: game.toml's host files under root/host (patch_scan.scan), read as UTF-8 with replacement
pub fn scan(root: &Path, files: &[String]) -> Res<PatchSet> {
    let mut texts = Vec::new();
    for f in files {
        let p = root.join("host").join(f);
        let b = std::fs::read(&p).map_err(|e| format!("patch declarations: {}: {e}", p.display()))?;
        texts.push((format!("host/{f}"), String::from_utf8_lossy(&b).into_owned()));
    }
    scan_texts(&texts)
}

impl PatchSet {
    /// the symbols HLE(...) and PATCH_ANSWER(...) replace
    pub fn hle_symbols(&self) -> BTreeSet<String> {
        self.hle.keys().chain(self.answers.keys()).cloned().collect()
    }
    fn effects_of(&self, symbol: &str) -> Option<&Replaced> {
        self.hle.get(symbol).or_else(|| self.declared.get(symbol)).or_else(|| self.nv.get(symbol))
    }
    /// host_effects' facts of a hooked symbol (stops, answer, clobbers, hle_reads)
    pub fn hle_fact(&self, symbol: &str) -> HleFact {
        let stops = if self.hle.contains_key(symbol) || self.answers.contains_key(symbol) || self.declared.contains_key(symbol) {
            false
        } else {
            match crate::eden::nvn_proc_name(symbol) {
                Some(p) => !self.nv.contains_key(p),
                None => true,
            }
        };
        let answer = self.answers.get(symbol).map(|a| (a.value & 0xFFFF_FFFF_FFFF_FFFF) as u64);
        let d = self.effects_of(symbol);
        // (host_effects.HLE_ARGS + the declared reads, summed as Python sums the bits: a declared read of an argument
        // register counts twice)
        let mut reads: u128 = (0..9).map(|i| 1u128 << i).sum::<u128>() + (0..8).map(|i| 1u128 << (36 + i)).sum::<u128>() + (1 << 31) + (1 << 30);
        if let Some(d) = d {
            for r in &d.reads {
                reads = reads.wrapping_add(var_bit(r));
            }
        }
        HleFact(symbol.to_string(), stops, answer, d.is_some_and(|d| d.clobbers.is_some()), reads)
    }
    /// [symbol, value] of the answers
    pub fn answers(&self) -> Vec<(String, u64)> {
        self.answers.values().map(|a| (a.symbol.clone(), (a.value & 0xFFFF_FFFF_FFFF_FFFF) as u64)).collect()
    }
}

/// probes._var: xN, wN -> xN; sN, qN, vN -> vN; sp
pub fn var(a: &str) -> String {
    if a == "sp" {
        return "sp".into();
    }
    let (f, rest) = a.split_at(1);
    format!("{}{rest}", if f == "x" || f == "w" { "x" } else { "v" })
}

/// a guest variable's summary bit: xN N, sp 31, vN 36+N
pub fn var_bit(v: &str) -> u128 {
    if v == "sp" {
        return 1 << 31;
    }
    let n: u32 = v[1..].parse().unwrap_or_else(|_| panic!("register {v}"));
    if v.starts_with('x') {
        1 << n
    } else {
        1 << (36 + n)
    }
}

// ---- rows (patch_scan.rows, probes.Value / At)

#[derive(Clone, Debug)]
pub struct Row {
    pub at: Where,
    /// a Value probe (an instruction's result replaced), else an At row
    pub value: bool,
    pub pc: u64,
    pub word: u32,
    pub fname: String,
    pub args: Vec<String>,
    pub result: Option<String>,
    pub mode: String,
    pub io: Vec<String>,
    pub guest: Option<Guest>,
    /// its function, once resolved
    pub func: u64,
}

impl Row {
    /// (the guest variables the row reads, those it writes) (Value.effects, At.effects)
    fn effects(&self) -> (BTreeSet<String>, BTreeSet<String>) {
        if self.value {
            return (self.args.iter().map(|a| var(a.split('+').next().unwrap())).collect(), BTreeSet::new());
        }
        let mut reads = BTreeSet::new();
        for a in &self.args {
            if a.starts_with('#') {
                continue;
            }
            reads.insert(var(if let Some(m) = a.strip_prefix('[') { m.split('+').next().unwrap() } else { a }));
        }
        reads.extend(self.io.iter().map(|r| var(r)));
        let mut writes: BTreeSet<String> = self.io.iter().map(|r| var(r)).collect();
        if let Some(r) = &self.result {
            writes.insert(var(r));
        }
        (reads, writes)
    }
}

fn is_call(w: u32) -> bool {
    match ounce_decode::decode(w) {
        Some(i) => i.enc.template == "blr" || (i.enc.template == "b" && i.f(ounce_decode::Fld::op) == 1),
        None => false,
    }
}
fn is_tail(w: u32) -> bool {
    ounce_decode::decode(w).is_some_and(|i| i.enc.template == "br")
}

/// (Value probes and At rows) of the sites, in declaration order (patch_scan.rows)
pub fn rows(sites: &[Site]) -> Res<Vec<Row>> {
    let mut out = Vec::new();
    for s in sites {
        let callish = is_call(s.word) || is_tail(s.word);
        let value = s.mode == "instead" && !callish;
        if value {
            if s.guest.is_some() || !s.io.is_empty() {
                return fail(&s.at, "an instruction's result replaced (instead, not a call) takes no guest or io");
            }
            if s.result.is_none() {
                return fail(&s.at, "an instruction's result replaced (instead, not a call) names its destination as result");
            }
        }
        out.push(Row {
            at: s.at.clone(),
            value,
            pc: s.pc,
            word: s.word,
            fname: s.fname.clone(),
            args: s.args.clone(),
            result: s.result.clone(),
            mode: s.mode.clone(),
            io: s.io.clone(),
            guest: s.guest.clone(),
            func: 0,
        });
    }
    Ok(out)
}

/// each row's function over the plan's bodies (probes.resolve: a site in no function or in several stops)
pub fn resolve(rows: &mut [Row], bodies: &[(u64, &[u64])]) -> Res<()> {
    for r in rows.iter_mut() {
        // (a body's span first: most bodies are far from the pc)
        let mut hits: Vec<u64> = bodies
            .iter()
            .filter(|(_, p)| !p.is_empty() && p[0] <= r.pc && r.pc <= p[p.len() - 1] && p.binary_search(&r.pc).is_ok())
            .map(|(e, _)| *e)
            .collect();
        hits.sort_unstable();
        if hits.len() != 1 {
            return fail(&r.at, format!("{:#x} is in {}", r.pc, if hits.is_empty() {
                "no lifted function".to_string()
            } else {
                format!("more than one function: {}", hits.iter().map(|e| format!("f_{e:08x}")).collect::<Vec<_>>().join(", "))
            }));
        }
        r.func = hits[0];
    }
    Ok(())
}

/// the template's writes of a word (scratch-builder run)
fn template_run(w: u32) -> Option<(crate::ir::Function, crate::ir::BlockId)> {
    use crate::ir::{BKey, Builder, Function};
    let i = ounce_decode::decode(w)?;
    let crate::sem::Kind::Sem(t, _) = crate::sem::kind(&i) else { return None };
    let mut f = Function::new(0);
    let blk = f.block(BKey::Pc(0));
    {
        let mut b = Builder::new(&mut f);
        b.at(blk);
        t(&mut b, &i, 0x1000);
    }
    Some((f, blk))
}

const TERMINATORS: [&str; 9] = ["b", "bcond", "cbz", "tbz", "ret", "br", "blr", "udf", "exc"];

/// (register, bits) of the one general register the word writes as its Rd (probes.dest)
fn dest(w: u32) -> Option<(u32, u32)> {
    use ounce_decode::Fld;
    let i = ounce_decode::decode(w)?;
    if !i.has(Fld::Rd) || TERMINATORS.contains(&i.enc.template) {
        return None;
    }
    let (f, blk) = template_run(w)?;
    let rd = i.f(Fld::Rd);
    if f.written != 1u128 << rd {
        return None;
    }
    if i.has(Fld::sf) {
        return Some((rd, if i.f(Fld::sf) != 0 { 64 } else { 32 }));
    }
    let v = f.b(blk).defs[rd as usize]?;
    let z = matches!(f.v(v).op, crate::ir::Op::Cast(crate::ir::CastOp::Zext)) && f.ty(f.arg(v, 0)) == crate::ty::I32;
    Some((rd, if z { 32 } else { 64 }))
}

/// probes.CALLER_SAVED: x0-x18, v0-v7, v16-v31
fn caller_saved(v: &str) -> bool {
    let n: u32 = v[1..].parse().unwrap_or(99);
    (v.starts_with('x') && n < 19) || (v.starts_with('v') && (n < 8 || (16..32).contains(&n)))
}

/// the guest variables the word writes (probes._written)
fn written(w: u32) -> Res<BTreeSet<String>> {
    let i = ounce_decode::decode(w).ok_or("probes: a row's word does not decode")?;
    let t = i.enc.template;
    let mut out = BTreeSet::new();
    if (t == "b" && i.f(ounce_decode::Fld::op) != 0) || t == "blr" {
        out.insert("x30".to_string());
        out.extend((0..19).map(|k| format!("x{k}")));
        out.extend((0..8).chain(16..32).map(|k| format!("v{k}")));
        return Ok(out);
    }
    if matches!(t, "b" | "bcond" | "cbz" | "tbz" | "ret" | "br" | "udf" | "exc") {
        return Ok(out);
    }
    let (f, _) = template_run(w).ok_or("probes: a row's instruction has no template")?;
    for v in 0..crate::ir::NVARS as u8 {
        if f.written >> v & 1 != 0 {
            out.insert(crate::ir::var_name(v));
        }
    }
    Ok(out)
}

/// what is wrong with one row against its function (probes.errors); jt and entries as the lift passes them
pub fn errors(r: &Row, pcs: &[u64], words: &[u32], jt: Option<&HashMap<u64, Vec<u64>>>, entries: Option<&HashSet<u64>>) -> Res<Vec<String>> {
    let word = |pc: u64| pcs.binary_search(&pc).ok().map(|k| words[k]);
    let kind = if r.value { "Value" } else { "At" };
    match word(r.pc) {
        Some(w) if w == r.word => {}
        got => {
            return Ok(vec![format!(
                "{kind} f_{:08x} at {:#x} holds {}, not {:08x} (another game version?)",
                r.func,
                r.pc,
                got.map_or("nothing".to_string(), |w| format!("{w:08x}")),
                r.word
            )])
        }
    }
    let mut out = Vec::new();
    if r.value {
        let ok = |a: &str| {
            let (b, rest) = match a.split_once('+') {
                Some((b, k)) => (b, Some(k)),
                None => (a, None),
            };
            let digits = |s: &str| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit());
            (b.starts_with('x') && digits(&b[1..]) && rest.is_none_or(digits)) || (b.starts_with('s') && digits(&b[1..]) && rest.is_none())
        };
        let bad: Vec<&String> = r.args.iter().filter(|a| !ok(a)).collect();
        if !bad.is_empty() {
            out.push(format!("Value at {:#x}: operands xN, add(xN, K) or sN, not {}", r.pc, bad.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")));
        }
        match dest(r.word) {
            None => out.push(format!("Value at {:#x}: the instruction writes no single general register Rd to replace", r.pc)),
            Some((rd, bits)) => {
                let want = format!("{}{rd}", if bits == 32 { "w" } else { "x" });
                if r.result.as_deref() != Some(want.as_str()) {
                    out.push(format!("Value at {:#x}: the result is {}, the instruction's destination is {want}", r.pc, r.result.as_deref().unwrap_or("None")));
                }
            }
        }
        return Ok(out);
    }
    let replace = r.mode == "instead";
    let before = r.mode == "before";
    if replace && !is_call(r.word) && !is_tail(r.word) {
        out.push(format!("At f_{:08x} replacing {:#x}: not a BL, BLR or BR", r.func, r.pc));
    }
    if replace && is_tail(r.word) && jt.is_some_and(|j| j.get(&r.pc).is_some_and(|t| !t.is_empty())) {
        out.push(format!("At f_{:08x} replacing {:#x}: a jump table's BR, not a tail call", r.func, r.pc));
    }
    if before {
        for (&pc, &w) in pcs.iter().zip(words) {
            if let Some(i) = ounce_decode::decode(w) {
                if crate::build::target(&i, pc) == Some(r.pc) {
                    out.push(format!("At f_{:08x} before {:#x}: a branch target (from {pc:#x})", r.func, r.pc));
                    break;
                }
            }
            if jt.is_some_and(|j| j.get(&pc).is_some_and(|t| t.contains(&r.pc))) {
                out.push(format!("At f_{:08x} before {:#x}: a jump-table target (the BR at {pc:#x})", r.func, r.pc));
                break;
            }
        }
        if r.pc != r.func && entries.is_some_and(|e| e.contains(&r.pc)) {
            out.push(format!("At f_{:08x} before {:#x}: another function's entry", r.func, r.pc));
        }
    }
    let wr = if before { BTreeSet::new() } else { written(r.word)? };
    let bad: Vec<String> = r.effects().1.into_iter().filter(|v| !caller_saved(v) && !wr.contains(v)).collect();
    if !bad.is_empty() {
        out.push(format!("At f_{:08x} at {:#x} writes {}: callee-saved, not written by the instruction", r.func, r.pc, bad.join(", ")));
    }
    Ok(out)
}

/// the rows as the lift reads them (the export's values / rows)
pub fn meta_rows(rows: &[Row]) -> (Vec<MetaValue>, Vec<MetaAt>) {
    let (mut v, mut a) = (Vec::new(), Vec::new());
    for r in rows {
        if r.value {
            v.push(MetaValue { pc: r.pc, word: r.word, fname: r.fname.clone(), args: r.args.clone(), result: r.result.clone().unwrap(), func: r.func });
        } else {
            a.push(MetaAt {
                pc: r.pc,
                word: r.word,
                fname: r.fname.clone(),
                mode: r.mode.clone(),
                args: r.args.clone(),
                result: r.result.clone(),
                io: r.io.clone(),
                guest: r.guest.as_ref().map(|g| match g {
                    Guest::Computed => serde_json::Value::String("computed".into()),
                    Guest::List(l) => serde_json::json!(l),
                }),
                func: r.func,
            });
        }
    }
    (v, a)
}

/// summary.rows: each pc's rows' declared reads and writes before and after its instruction (Value probes first, then
/// At rows, each in declaration order)
pub fn row_effects(rows: &[Row]) -> Vec<RowEffect> {
    // (before reads, after reads, before writes, after writes, guest calls (None: computed), replace, any)
    type Acc = (u128, u128, u128, u128, Option<Vec<u64>>, bool, bool);
    let mut by: BTreeMap<u64, Acc> = BTreeMap::new();
    let order = rows.iter().filter(|r| r.value).chain(rows.iter().filter(|r| !r.value));
    for r in order {
        let e = by.entry(r.pc).or_insert((0, 0, 0, 0, Some(Vec::new()), false, false));
        let (rd, wr) = r.effects();
        let (rm, wm) = (rd.iter().map(|v| var_bit(v)).sum::<u128>(), wr.iter().map(|v| var_bit(v)).sum::<u128>());
        if !r.value && r.mode == "after" {
            e.1 |= rm;
            e.3 |= wm;
        } else {
            e.0 |= rm;
            e.2 |= wm;
        }
        // (calls: the tuple of guest callees, or "computed" (None here) once any row's is)
        let guest = if r.value { None } else { r.guest.clone() };
        let computed_now = e.4.is_none();
        if matches!(guest, Some(Guest::Computed)) || computed_now {
            if guest.is_some() {
                e.4 = None;
            }
        } else if let Some(Guest::List(l)) = guest {
            if let Some(c) = &mut e.4 {
                c.extend(l);
            }
        }
        e.5 = e.5 || (!r.value && r.mode == "instead");
        e.6 = true;
    }
    by.into_iter()
        .map(|(pc, (bu, au, bd, ad, calls, repl, _))| {
            RowEffect(pc, bu, au, bd, ad, match calls {
                None => GuestCalls::Computed("computed".into()),
                Some(l) => GuestCalls::List(l),
            }, repl)
        })
        .collect()
}

// ---- PATCH_FUNC (patch_scan.funcs, check_funcs, summary.func_facts)

/// a body's hash as body(H) names it: the first 64 bits of the SHA-1 of its words (little-endian)
pub fn body_hash(words: &[u32]) -> u64 {
    use sha1::{Digest, Sha1};
    let mut h = Sha1::new();
    for w in words {
        h.update(w.to_le_bytes());
    }
    u64::from_be_bytes(h.finalize()[..8].try_into().unwrap())
}

/// {address: Func} of the PATCH_FUNCs (a PATCH_FUNC_SYM through the image's function exports), checked against the
/// plan's bodies: the first error stops with file:line
pub fn check_funcs<'a>(ps: &'a PatchSet, exports: &[(String, u64)], bodies: &dyn Fn(u64) -> Option<(Vec<u64>, Vec<u32>)>) -> Res<BTreeMap<u64, &'a Func>> {
    let ex: HashMap<&str, u64> = exports.iter().map(|(n, a)| (n.as_str(), *a)).collect();
    let mut found = BTreeMap::new();
    let mut errs: Vec<(Where, String)> = Vec::new();
    for f in &ps.funcs {
        let a = match &f.symbol {
            None => f.addr,
            Some(s) => ex.get(s.as_str()).copied(),
        };
        match a {
            Some(a) => {
                found.insert(a, f);
            }
            None => errs.push((f.at.clone(), format!("{}: the image exports no {}", f.kind, f.symbol.as_deref().unwrap_or("")))),
        }
    }
    for (&a, f) in &found {
        let b = bodies(a);
        let (pcs, words) = match b {
            Some((p, w)) if p.first() == Some(&a) => (p, w),
            _ => {
                errs.push((f.at.clone(), format!("{} at {a:#x}: no lifted function there", f.kind)));
                continue;
            }
        };
        let _ = pcs;
        if let Some(w) = f.word.filter(|&w| words[0] != w) {
            errs.push((f.at.clone(), format!("PATCH_FUNC at {a:#x}: the function's first word is {:#010x}, not {w:#010x}", words[0])));
        } else if let Some(h) = f.body.filter(|&h| body_hash(&words) != h) {
            errs.push((f.at.clone(), format!(
                "PATCH_FUNC at {a:#x}: the function's body is body({:#018x}), not body({h:#018x}) ({} instructions)",
                body_hash(&words),
                words.len()
            )));
        }
    }
    if let Some((w, m)) = errs.first() {
        return fail(w, m);
    }
    Ok(found)
}

/// the PATCH_FUNCs' facts as the summaries take them (summary.func_facts) and their names
pub fn func_facts(found: &BTreeMap<u64, &Func>) -> (Vec<PatchDecl>, Vec<(u64, String, String)>) {
    let decls = found
        .iter()
        .map(|(&e, f)| {
            let reads: BTreeSet<String> = f.args.iter().map(|a| var(a)).collect();
            PatchDecl(e, reads.iter().map(|v| var_bit(v)).sum(), f.result.as_deref().map_or(0, |r| var_bit(&var(r))), f.orig, f.guest.clone())
        })
        .collect();
    let names = found.iter().map(|(&e, f)| (e, f.name.clone(), f.orig_name.clone())).collect();
    (decls, names)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn err(text: &str) -> String {
        scan_texts(&[("t.c".into(), text.into())]).unwrap_err()
    }

    #[test]
    fn scans_a_site_and_a_replacement() {
        let ps = scan_texts(&[(
            "t.c".into(),
            "/* x */ PATCH_AT(0x80001000, word(0x94000010), after, none, f, x0, ld(sp, 0x10), k(3)) { }\n\
             HLE(_ZN2nn2oe1fEv, reads(x9, \"why\")) { }\nPATCH_ANSWER(g, 1, \"why\");"
                .into(),
        )])
        .unwrap();
        assert_eq!(ps.sites[0].args, vec!["x0", "[sp+0x10]", "#3"]);
        assert_eq!(ps.hle["_ZN2nn2oe1fEv"].reads, vec!["x9"]);
        assert_eq!(ps.hle_symbols().len(), 2);
    }

    #[test]
    fn errors_name_file_and_line() {
        assert_eq!(err("\n#if X\nHLE(f) {}\n#endif"), "patch declarations: t.c:3: HLE inside conditional compilation (#if at line 2)");
        assert_eq!(err("void g(void) { HLE(f); }"), "patch declarations: t.c:1: HLE inside a function or an initializer: declarations stand at file scope");
        assert_eq!(err("HLE /* open"), "patch declarations: t.c:1: a comment that does not end");
        assert_eq!(err("PATCH_ANSWER(f, 1, \"why\");\nPATCH_ANSWER(f, 2, \"why\");"), "patch declarations: t.c:2: PATCH_ANSWER of f again (the first at t.c:1)");
        assert_eq!(err("HLE_EFFECTS(f);"), "patch declarations: t.c:1: HLE_EFFECTS without an effect");
        assert_eq!(
            err("PATCH_AT(1, word(2), sideways, none, f);"),
            "patch declarations: t.c:1: mode: after, before or instead, not sideways"
        );
        assert_eq!(err("PATCH_AT(1, 2, after, none, f);"), "patch declarations: t.c:1: the instruction word is written word(0x...), not '2'");
        assert_eq!(err("PATCH_AT(1, word(2), after, none, f, x31);"), "patch declarations: t.c:1: unknown operand x31");
        assert_eq!(
            err("PATCH_AT_SITE(1, word(2), after, none, f, x0);"),
            "patch declarations: t.c:1: PATCH_AT_SITE of f before or without its PATCH_AT"
        );
        assert_eq!(err("#define MINE(x) HLE(x)"), "patch declarations: t.c:1: a declaration made by another macro (#define MINE)");
        assert_eq!(
            err("PATCH_FUNC_SYM(s, aapcs, x0, f, guest(0x10), x0);"),
            "patch declarations: t.c:1: guest(...) runs on the call's stack: sp is an operand (guest_call's sp)"
        );
    }

    #[test]
    fn numbers_as_python_reads_them() {
        assert_eq!(py_int("0x10u"), Some(16));
        assert_eq!(py_int("010"), None);
        assert_eq!(py_int("1_000"), Some(1000));
    }

    #[test]
    fn hash_of_a_body() {
        // (sha1 of 4 zero bytes: 9069ca78e7450a28...)
        assert_eq!(body_hash(&[0]), 0x9069ca78e7450a28);
    }
}

/// the platform's facts (ounce/lift/data/horizon.toml)
#[derive(serde::Deserialize, Debug)]
pub struct Horizon {
    pub setjmp_names: Vec<String>,
    pub noreturn_names: Vec<String>,
    pub noreturn_parts: Vec<String>,
    pub os_namespaces: Vec<String>,
}

impl Horizon {
    pub fn load(root: &Path) -> Res<Horizon> {
        let p = root.join("ounce/lift/data/horizon.toml");
        let t = std::fs::read_to_string(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        toml::from_str(&t).map_err(|e| format!("{}: {e}", p.display()))
    }
}
