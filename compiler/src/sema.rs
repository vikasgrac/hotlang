//! Semantic analysis: type checking + the hot-path verifier.
//!
//! Beyond ordinary type checking, this module enforces the guarantees that
//! make hotlang worth existing:
//!
//! 1. **No recursion.** The call graph must be a DAG.
//! 2. **Bounded loops.** `for` bounds are compile-time constants, so every
//!    function's total step count is statically known (the eBPF move).
//! 3. **Proven array bounds.** Every index expression carries a conservative
//!    integer range (interval analysis); an access compiles only if the
//!    range is provably inside the array. No runtime bounds checks, no UB —
//!    the third option is "the program doesn't compile."
//! 4. **No allocation.** No language construct allocates. Arrays live in
//!    caller-provided buffers; locals are scalars.

use crate::ast::*;
use crate::diag::Diag;
use std::collections::HashMap;

#[derive(Clone)]
pub struct VarInfo {
    pub ty: Ty,
    pub mutable: bool,
    /// Conservative value range for i64 expressions, used to prove indexes
    /// in-bounds. `None` = unknown.
    pub range: Option<(i64, i64)>,
}

pub struct FnSig {
    pub params: Vec<Ty>,
    pub ret: Ty,
}

pub struct Checked {
    pub sigs: HashMap<String, FnSig>,
}

/// C-runtime symbols a hotlang function must not shadow — the emitted
/// object exports functions under their source names (C ABI).
const RESERVED_SYMBOLS: &[&str] = &[
    "main", "malloc", "free", "calloc", "realloc", "memcpy", "memset",
    "memmove", "memcmp", "exit", "abort", "_init", "_fini", "errno",
];

/// Math builtins, lowered to LLVM intrinsics in codegen. Reserved names:
/// a user function cannot shadow them (which also prevents a hotlang
/// module from hijacking libm symbols like `sqrt` in the host process).
/// `f64`/`i64` are the explicit conversions (quantities are integers,
/// prices are doubles — `f64(qty) * px` is line one of real HFT code).
const BUILTIN_NAMES: &[&str] = &[
    "sqrt", "abs", "min", "max", "fma", "floor", "ceil", "exp", "log", "pow", "f64", "i64",
];

pub fn is_builtin(name: &str) -> bool {
    BUILTIN_NAMES.contains(&name)
}

/// Return type of a builtin for the given argument types, or None if no
/// overload matches. Kept in sema so type checking and codegen agree.
pub fn builtin_ret(name: &str, args: &[Ty]) -> Option<Ty> {
    match (name, args) {
        ("sqrt" | "floor" | "ceil" | "exp" | "log", [Ty::F64]) => Some(Ty::F64),
        ("abs", [Ty::F64]) => Some(Ty::F64),
        ("abs", [Ty::I64]) => Some(Ty::I64),
        ("min" | "max", [Ty::F64, Ty::F64]) => Some(Ty::F64),
        ("min" | "max", [Ty::I64, Ty::I64]) => Some(Ty::I64),
        ("pow", [Ty::F64, Ty::F64]) => Some(Ty::F64),
        ("fma", [Ty::F64, Ty::F64, Ty::F64]) => Some(Ty::F64),
        ("f64", [Ty::I64]) => Some(Ty::F64),
        ("i64", [Ty::F64]) => Some(Ty::I64),
        _ => None,
    }
}

const BUILTIN_HELP: &str = "builtins: sqrt/floor/ceil/exp/log (f64) -> f64, \
     abs (i64 -> i64, f64 -> f64), min/max (i64,i64 -> i64 or f64,f64 -> f64), \
     pow (f64,f64) -> f64, fma (f64,f64,f64) -> f64, \
     conversions f64(i64) and i64(f64) (i64() truncates toward zero, \
     saturates at the i64 range, NaN -> 0)";

pub fn check(fns: &[FnDef]) -> Result<Checked, Diag> {
    let mut sigs: HashMap<String, FnSig> = HashMap::new();
    for f in fns {
        if RESERVED_SYMBOLS.contains(&f.name.as_str()) {
            return Err(Diag::new(
                format!("function name `{}` collides with a C runtime symbol", f.name),
                f.line,
                f.col,
            )
            .with_note(
                "hotlang exports functions under their source names for host FFI; \
                 pick a name that is not a libc/runtime symbol",
            ));
        }
        if is_builtin(&f.name) {
            return Err(Diag::new(
                format!("`{}` is a hotlang builtin and cannot be redefined", f.name),
                f.line,
                f.col,
            )
            .with_note(BUILTIN_HELP));
        }
        if sigs.contains_key(&f.name) {
            return Err(Diag::new(
                format!("function `{}` is defined more than once", f.name),
                f.line,
                f.col,
            ));
        }
        if matches!(f.ret, Ty::Arr(..)) {
            return Err(Diag::new(
                format!("function `{}` cannot return an array; write into a `mut` array parameter instead", f.name),
                f.line,
                f.col,
            ));
        }
        sigs.insert(
            f.name.clone(),
            FnSig { params: f.params.iter().map(|p| p.ty).collect(), ret: f.ret },
        );
    }

    for f in fns {
        check_fn(f, &sigs)?;
    }

    check_no_recursion(fns)?;

    Ok(Checked { sigs })
}

fn check_fn(f: &FnDef, sigs: &HashMap<String, FnSig>) -> Result<(), Diag> {
    let mut env: HashMap<String, VarInfo> = HashMap::new();
    for p in &f.params {
        if env
            .insert(p.name.clone(), VarInfo { ty: p.ty, mutable: p.mutable, range: None })
            .is_some()
        {
            return Err(Diag::new(
                format!("duplicate parameter `{}` in function `{}`", p.name, f.name),
                p.line,
                p.col,
            ));
        }
    }

    if f.body.is_empty() {
        return Err(Diag::new(
            format!("function `{}` has an empty body", f.name),
            f.line,
            f.col,
        )
        .with_note("every hotlang function must end with a `return`"));
    }

    let (last, init) = f.body.split_last().unwrap();
    check_stmts(init, &mut env, sigs, f, false)?;

    match last {
        Stmt::Return { expr, line, col } => {
            let ty = type_of(expr, &env, sigs)?;
            if ty != f.ret {
                return Err(Diag::new(
                    format!(
                        "function `{}` declares return type `{}` but returns `{}`",
                        f.name,
                        f.ret.name(),
                        ty.name()
                    ),
                    *line,
                    *col,
                ));
            }
            Ok(())
        }
        other => {
            let (line, col) = stmt_pos(other);
            Err(Diag::new(
                format!("function `{}` must end with a `return`", f.name),
                line,
                col,
            ))
        }
    }
}

fn stmt_pos(s: &Stmt) -> (u32, u32) {
    match s {
        Stmt::Let { line, col, .. }
        | Stmt::Assign { line, col, .. }
        | Stmt::Store { line, col, .. }
        | Stmt::For { line, col, .. }
        | Stmt::Return { line, col, .. } => (*line, *col),
    }
}

/// Check a run of non-terminal statements. Bindings introduced here are
/// added to `env`; loop-scoped names are removed on exit.
fn check_stmts(
    stmts: &[Stmt],
    env: &mut HashMap<String, VarInfo>,
    sigs: &HashMap<String, FnSig>,
    f: &FnDef,
    in_loop: bool,
) -> Result<Vec<String>, Diag> {
    let mut introduced = Vec::new();
    for stmt in stmts {
        match stmt {
            Stmt::Let { name, mutable, expr, line, col } => {
                if env.contains_key(name) {
                    return Err(Diag::new(
                        format!("`{name}` is already bound; hotlang has no shadowing"),
                        *line,
                        *col,
                    ));
                }
                let ty = type_of(expr, env, sigs)?;
                if matches!(ty, Ty::Arr(..)) {
                    return Err(Diag::new(
                        format!("cannot bind `{name}` to an array; arrays are parameters only"),
                        *line,
                        *col,
                    ));
                }
                let range = if *mutable { None } else { infer_range(expr, env) };
                env.insert(name.clone(), VarInfo { ty, mutable: *mutable, range });
                introduced.push(name.clone());
            }
            Stmt::Assign { name, expr, line, col } => {
                let info = env.get(name).cloned().ok_or_else(|| {
                    Diag::new(format!("unknown variable `{name}`"), *line, *col)
                })?;
                if !info.mutable {
                    return Err(Diag::new(
                        format!("cannot assign to `{name}`: it is not declared `let mut`"),
                        *line,
                        *col,
                    ));
                }
                let ty = type_of(expr, env, sigs)?;
                if ty != info.ty {
                    return Err(Diag::new(
                        format!(
                            "cannot assign `{}` to `{name}` of type `{}`",
                            ty.name(),
                            info.ty.name()
                        ),
                        *line,
                        *col,
                    ));
                }
            }
            Stmt::Store { arr, idx, expr, line, col } => {
                let info = env.get(arr).cloned().ok_or_else(|| {
                    Diag::new(format!("unknown variable `{arr}`"), *line, *col)
                })?;
                let (elem, len) = match info.ty {
                    Ty::Arr(e, n) => (e, n),
                    other => {
                        return Err(Diag::new(
                            format!("`{arr}` has type `{}` and cannot be indexed", other.name()),
                            *line,
                            *col,
                        ))
                    }
                };
                if !info.mutable {
                    return Err(Diag::new(
                        format!("cannot write to `{arr}`: declare the parameter `mut` to make it an output buffer"),
                        *line,
                        *col,
                    ));
                }
                check_index(arr, idx, len, env, sigs)?;
                let ty = type_of(expr, env, sigs)?;
                if ty != Ty::scalar_of(elem) {
                    return Err(Diag::new(
                        format!(
                            "cannot store `{}` into `{arr}` of element type `{}`",
                            ty.name(),
                            elem.name()
                        ),
                        *line,
                        *col,
                    ));
                }
            }
            Stmt::For { var, lo, hi, body, line, col } => {
                if env.contains_key(var) {
                    return Err(Diag::new(
                        format!("loop variable `{var}` is already bound; hotlang has no shadowing"),
                        *line,
                        *col,
                    ));
                }
                env.insert(
                    var.clone(),
                    VarInfo { ty: Ty::I64, mutable: false, range: Some((*lo, *hi - 1)) },
                );
                let inner = check_stmts(body, env, sigs, f, true)?;
                for name in inner {
                    env.remove(&name);
                }
                env.remove(var);
            }
            Stmt::Return { line, col, .. } => {
                let msg = if in_loop {
                    "`return` inside a loop is not allowed"
                } else {
                    "unreachable code after `return`"
                };
                return Err(Diag::new(msg, *line, *col).with_note(
                    "hotlang functions have a single exit: the final `return`. \
                     Use an `if` expression to select the value instead",
                ));
            }
        }
    }
    Ok(introduced)
}

/// Prove `idx` lies in `[0, len)` at compile time, or reject.
fn check_index(
    arr: &str,
    idx: &Expr,
    len: u32,
    env: &HashMap<String, VarInfo>,
    sigs: &HashMap<String, FnSig>,
) -> Result<(), Diag> {
    let ty = type_of(idx, env, sigs)?;
    if ty != Ty::I64 {
        return Err(Diag::new(
            format!("array index must be `i64`, found `{}`", ty.name()),
            idx.line,
            idx.col,
        ));
    }
    match infer_range(idx, env) {
        Some((lo, hi)) if lo >= 0 && hi < len as i64 => Ok(()),
        Some((lo, hi)) => Err(Diag::new(
            format!(
                "index into `{arr}` may be out of bounds: value range is [{lo}, {hi}] but the array has {len} elements"
            ),
            idx.line,
            idx.col,
        )
        .with_note("hotlang proves every array access in-bounds at compile time — no runtime checks, no UB")),
        None => Err(Diag::new(
            format!("cannot prove index into `{arr}` is in bounds"),
            idx.line,
            idx.col,
        )
        .with_note(
            "indexes must have a statically known range: a loop variable, a constant, \
             or arithmetic over them (e.g. `i`, `i + 1`, `2 * i`)",
        )),
    }
}

/// Conservative interval analysis over i64 expressions. Public because
/// codegen uses the same ranges to decide when division guards can be
/// omitted — sema and codegen must always agree.
pub fn infer_range(e: &Expr, env: &HashMap<String, VarInfo>) -> Option<(i64, i64)> {
    match &e.kind {
        ExprKind::Int(n) => Some((*n, *n)),
        ExprKind::Var(name) => env.get(name)?.range,
        ExprKind::Unary { op: UnOp::Neg, rhs } => {
            let (lo, hi) = infer_range(rhs, env)?;
            Some((hi.checked_neg()?, lo.checked_neg()?))
        }
        ExprKind::Binary { op, lhs, rhs } => {
            let (a, b) = infer_range(lhs, env)?;
            let (c, d) = infer_range(rhs, env)?;
            match op {
                BinOp::Add => Some((a.checked_add(c)?, b.checked_add(d)?)),
                BinOp::Sub => Some((a.checked_sub(d)?, b.checked_sub(c)?)),
                BinOp::Mul => {
                    let products = [
                        a.checked_mul(c)?,
                        a.checked_mul(d)?,
                        b.checked_mul(c)?,
                        b.checked_mul(d)?,
                    ];
                    Some((
                        *products.iter().min().unwrap(),
                        *products.iter().max().unwrap(),
                    ))
                }
                _ => None,
            }
        }
        ExprKind::IfElse { then, els, .. } => {
            let (a, b) = infer_range(then, env)?;
            let (c, d) = infer_range(els, env)?;
            Some((a.min(c), b.max(d)))
        }
        _ => None,
    }
}

/// Infer the type of an expression. Also used by codegen.
pub fn type_of(
    e: &Expr,
    env: &HashMap<String, VarInfo>,
    sigs: &HashMap<String, FnSig>,
) -> Result<Ty, Diag> {
    match &e.kind {
        ExprKind::Int(_) => Ok(Ty::I64),
        ExprKind::Float(_) => Ok(Ty::F64),
        ExprKind::Bool(_) => Ok(Ty::Bool),
        ExprKind::Var(name) => {
            let info = env.get(name).ok_or_else(|| {
                Diag::new(format!("unknown variable `{name}`"), e.line, e.col)
            })?;
            if matches!(info.ty, Ty::Arr(..)) {
                return Err(Diag::new(
                    format!("`{name}` is an array; arrays can only be indexed (`{name}[i]`), not used as values"),
                    e.line,
                    e.col,
                ));
            }
            Ok(info.ty)
        }
        ExprKind::Index { arr, idx } => {
            let info = env.get(arr).ok_or_else(|| {
                Diag::new(format!("unknown variable `{arr}`"), e.line, e.col)
            })?;
            let (elem, len) = match info.ty {
                Ty::Arr(el, n) => (el, n),
                other => {
                    return Err(Diag::new(
                        format!("`{arr}` has type `{}` and cannot be indexed", other.name()),
                        e.line,
                        e.col,
                    ))
                }
            };
            check_index(arr, idx, len, env, sigs)?;
            Ok(Ty::scalar_of(elem))
        }
        ExprKind::Unary { op, rhs } => {
            let t = type_of(rhs, env, sigs)?;
            match (op, t) {
                (UnOp::Neg, Ty::I64) | (UnOp::Neg, Ty::F64) => Ok(t),
                (UnOp::Not, Ty::Bool) => Ok(Ty::Bool),
                (UnOp::Neg, other) => Err(Diag::new(
                    format!("cannot negate a `{}`", other.name()),
                    e.line,
                    e.col,
                )),
                (UnOp::Not, other) => Err(Diag::new(
                    format!("`!` requires `bool`, found `{}`", other.name()),
                    e.line,
                    e.col,
                )),
            }
        }
        ExprKind::Binary { op, lhs, rhs } => {
            let lt = type_of(lhs, env, sigs)?;
            let rt = type_of(rhs, env, sigs)?;
            if lt != rt {
                return Err(Diag::new(
                    format!(
                        "type mismatch: `{}` {} `{}` (hotlang has no implicit conversions)",
                        lt.name(),
                        op.symbol(),
                        rt.name()
                    ),
                    e.line,
                    e.col,
                ));
            }
            match op {
                BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem => {
                    if lt == Ty::Bool {
                        Err(Diag::new(
                            format!("arithmetic `{}` is not defined for `bool`", op.symbol()),
                            e.line,
                            e.col,
                        ))
                    } else {
                        Ok(lt)
                    }
                }
                BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                    if lt == Ty::Bool {
                        Err(Diag::new(
                            format!("ordering `{}` is not defined for `bool`", op.symbol()),
                            e.line,
                            e.col,
                        ))
                    } else {
                        Ok(Ty::Bool)
                    }
                }
                BinOp::Eq | BinOp::Ne => Ok(Ty::Bool),
                BinOp::And | BinOp::Or => {
                    if lt != Ty::Bool {
                        Err(Diag::new(
                            format!("`{}` requires `bool` operands, found `{}`", op.symbol(), lt.name()),
                            e.line,
                            e.col,
                        ))
                    } else {
                        Ok(lt)
                    }
                }
            }
        }
        ExprKind::Call { name, args } => {
            if !sigs.contains_key(name) && is_builtin(name) {
                let mut atys = Vec::new();
                for arg in args {
                    atys.push(type_of(arg, env, sigs)?);
                }
                return builtin_ret(name, &atys).ok_or_else(|| {
                    let list: Vec<String> = atys.iter().map(|t| t.name()).collect();
                    Diag::new(
                        format!("no builtin `{name}` takes ({})", list.join(", ")),
                        e.line,
                        e.col,
                    )
                    .with_note(BUILTIN_HELP)
                });
            }
            let sig = sigs.get(name).ok_or_else(|| {
                Diag::new(format!("unknown function `{name}`"), e.line, e.col).with_note(
                    "hotlang functions can only call other functions in the same module — \
                     the whole call graph must be visible to the verifier",
                )
            })?;
            if args.len() != sig.params.len() {
                return Err(Diag::new(
                    format!(
                        "`{name}` expects {} argument(s), found {}",
                        sig.params.len(),
                        args.len()
                    ),
                    e.line,
                    e.col,
                ));
            }
            for (i, (arg, want)) in args.iter().zip(&sig.params).enumerate() {
                if matches!(want, Ty::Arr(..)) {
                    return Err(Diag::new(
                        format!("cannot call `{name}`: passing arrays between hotlang functions is not yet supported"),
                        e.line,
                        e.col,
                    ));
                }
                let got = type_of(arg, env, sigs)?;
                if got != *want {
                    return Err(Diag::new(
                        format!(
                            "argument {} of `{name}` expects `{}`, found `{}`",
                            i + 1,
                            want.name(),
                            got.name()
                        ),
                        arg.line,
                        arg.col,
                    ));
                }
            }
            Ok(sig.ret)
        }
        ExprKind::IfElse { cond, then, els } => {
            let ct = type_of(cond, env, sigs)?;
            if ct != Ty::Bool {
                return Err(Diag::new(
                    format!("`if` condition must be `bool`, found `{}`", ct.name()),
                    cond.line,
                    cond.col,
                ));
            }
            let tt = type_of(then, env, sigs)?;
            let et = type_of(els, env, sigs)?;
            if tt != et {
                return Err(Diag::new(
                    format!(
                        "`if` arms have different types: `{}` vs `{}`",
                        tt.name(),
                        et.name()
                    ),
                    e.line,
                    e.col,
                ));
            }
            Ok(tt)
        }
    }
}

/// The bounded-execution verifier: reject any cycle in the call graph.
fn check_no_recursion(fns: &[FnDef]) -> Result<(), Diag> {
    let mut calls: HashMap<&str, Vec<(&str, u32, u32)>> = HashMap::new();
    for f in fns {
        let mut out = Vec::new();
        collect_calls_stmts(&f.body, &mut out);
        calls.insert(&f.name, out);
    }

    #[derive(Clone, Copy, PartialEq)]
    enum State {
        Unvisited,
        InProgress,
        Done,
    }
    let mut state: HashMap<&str, State> =
        fns.iter().map(|f| (f.name.as_str(), State::Unvisited)).collect();

    fn dfs<'a>(
        name: &'a str,
        calls: &HashMap<&'a str, Vec<(&'a str, u32, u32)>>,
        state: &mut HashMap<&'a str, State>,
        path: &mut Vec<&'a str>,
    ) -> Result<(), Diag> {
        state.insert(name, State::InProgress);
        path.push(name);
        if let Some(edges) = calls.get(name) {
            for (callee, line, col) in edges {
                match state.get(callee).copied().unwrap_or(State::Done) {
                    State::InProgress => {
                        let start = path.iter().position(|&n| n == *callee).unwrap_or(0);
                        let cycle: Vec<&str> =
                            path[start..].iter().copied().chain(std::iter::once(*callee)).collect();
                        return Err(Diag::new(
                            format!("recursion detected: {}", cycle.join(" -> ")),
                            *line,
                            *col,
                        )
                        .with_note(
                            "hot paths must have statically bounded execution time; \
                             hotlang rejects all recursion at compile time",
                        ));
                    }
                    State::Unvisited => dfs(callee, calls, state, path)?,
                    State::Done => {}
                }
            }
        }
        path.pop();
        state.insert(name, State::Done);
        Ok(())
    }

    for f in fns {
        if state[f.name.as_str()] == State::Unvisited {
            let mut path = Vec::new();
            dfs(&f.name, &calls, &mut state, &mut path)?;
        }
    }
    Ok(())
}

fn collect_calls_stmts<'a>(stmts: &'a [Stmt], out: &mut Vec<(&'a str, u32, u32)>) {
    for stmt in stmts {
        match stmt {
            Stmt::Let { expr, .. } | Stmt::Assign { expr, .. } | Stmt::Return { expr, .. } => {
                collect_calls(expr, out)
            }
            Stmt::Store { idx, expr, .. } => {
                collect_calls(idx, out);
                collect_calls(expr, out);
            }
            Stmt::For { body, .. } => collect_calls_stmts(body, out),
        }
    }
}

fn collect_calls<'a>(e: &'a Expr, out: &mut Vec<(&'a str, u32, u32)>) {
    match &e.kind {
        ExprKind::Call { name, args } => {
            out.push((name, e.line, e.col));
            for a in args {
                collect_calls(a, out);
            }
        }
        ExprKind::Unary { rhs, .. } => collect_calls(rhs, out),
        ExprKind::Index { idx, .. } => collect_calls(idx, out),
        ExprKind::Binary { lhs, rhs, .. } => {
            collect_calls(lhs, out);
            collect_calls(rhs, out);
        }
        ExprKind::IfElse { cond, then, els } => {
            collect_calls(cond, out);
            collect_calls(then, out);
            collect_calls(els, out);
        }
        _ => {}
    }
}
