//! LLVM IR code generator (textual .ll output).
//!
//! This is where hotlang's semantics turn into optimizer freedom that C++
//! cannot legally grant by default:
//!
//! - **Array params are `noalias`** — hotlang has no pointers and arrays
//!   never overlap, so every array parameter is `restrict` for free. C/C++
//!   must assume aliasing unless the programmer hand-writes `restrict`.
//! - **FP arithmetic carries `reassoc nsz contract`** — hotlang defines
//!   floating-point accumulation as reassociable, so LLVM may vectorize
//!   reductions. C/C++ strict IEEE ordering forbids this without the global
//!   `-ffast-math` hammer.
//! - **Functions are `nounwind willreturn norecurse`** with tight `memory`
//!   attributes — claims the verifier in `sema` has already proven.
//! - **Inner loops of loop nests carry `!llvm.loop` shaping metadata** —
//!   because trip counts are compile-time facts, the compiler can also tell
//!   LLVM *how to spend* the freedom the flags grant: keep nested reductions
//!   rolled for the loop vectorizer, interleave accumulators, then unroll
//!   the vectorized loop. See `LoopMd` for the measured rationale.
//!
//! `let mut` and loop counters lower to entry-block allocas; LLVM's mem2reg
//! rewrites them into SSA registers, so the emitted IR stays simple.

use crate::ast::*;
use crate::diag::Diag;
use crate::sema::{builtin_ret, infer_range, type_of, Checked, VarInfo};
use std::collections::{BTreeSet, HashMap};

fn llty(t: Ty) -> &'static str {
    match t {
        Ty::I64 => "i64",
        Ty::F64 => "double",
        Ty::Bool => "i1",
        Ty::Arr(..) => "ptr",
    }
}

fn ll_elem(e: Elem) -> &'static str {
    match e {
        Elem::I64 => "i64",
        Elem::F64 => "double",
    }
}

/// Fast-math flags applied to f64 arithmetic. `reassoc` lets LLVM vectorize
/// reductions; `nsz`/`contract` allow sign-of-zero freedom and FMA fusion.
/// No `nnan`/`ninf`: hotlang does not pretend NaNs can't happen.
const FMF: &str = "reassoc nsz contract ";

/// LLVM's textual parser is picky about decimal float syntax; the raw-bits
/// hex form is exact and always accepted.
fn f64_const(x: f64) -> String {
    format!("0x{:016X}", x.to_bits())
}

enum Loc {
    /// Immutable SSA value (params, plain `let`).
    Ssa(String),
    /// Stack slot (`let mut`, loop counters).
    Slot(String),
    /// Array parameter pointer.
    Arr(String),
}

struct FnCtx<'a> {
    checked: &'a Checked,
    env: HashMap<String, VarInfo>,
    loc: HashMap<String, Loc>,
    tmp: u32,
    label: u32,
    allocas: String,
    body: String,
    /// LLVM intrinsic declarations required by builtins used in this module.
    intrinsics: &'a mut BTreeSet<&'static str>,
    /// Nesting depth of the `for` currently being emitted (0 = not in a loop).
    loop_depth: u32,
    /// Module-level `!llvm.loop` metadata for nested inner loops.
    loop_md: &'a mut LoopMd,
}

/// Loop metadata emitted on the backedge of every inner loop of a loop nest.
///
/// Why: with exact trip counts and `noalias` params, LLVM's early full-unroll
/// flattens a nested reduction (e.g. matvec's inner dot) into straight-line
/// code, and on wide targets (AVX-512) SLP then vectorizes it *across* the
/// outer loop — broadcasting every element of the shared operand into its own
/// 512-bit register and spilling catastrophically (measured 3x slower than
/// plain -O3 on Zen 5). The recipe below is target-neutral and was measured
/// best at both 128-bit and 512-bit widths:
///   - `unroll.disable`      : keep the inner loop rolled so the loop
///                             vectorizer, not SLP, owns the reduction;
///   - `interleave.count 4`  : four parallel vector accumulators break the
///                             FP-add latency chain (legal: hotlang FP is
///                             reassociable by definition);
///   - `followup_vectorized -> isvectorized + unroll.enable` : the followup
///                             list REPLACES the vectorized loop's metadata,
///                             so the vector loop sheds `unroll.disable` and
///                             may be (fully) unrolled — this recovers the
///                             straight-line code that narrow targets want.
///                             Properties are listed flat in the followup
///                             wrapper (not nested in a self-referential
///                             node) so every LLVM property scan sees them.
///
/// Known trade-off: an inner loop the vectorizer REJECTS (e.g. a first-order
/// recurrence like `e = a*e + x[j]`) never gets its followup applied and
/// keeps `unroll.disable`. Such loops are latency-chain-bound anyway —
/// unrolling cannot overlap a serial dependency — so the cost is small, and
/// recurrences in the *inner* position of a nest are rare in practice.
///
/// Only the two self-referential per-loop nodes must be distinct; all
/// property leaves and the followup wrapper are shared.
struct LoopMd {
    next_id: u32,
    lines: String,
    /// (unroll.disable, interleave.count, followup wrapper) shared ids.
    leaves: Option<(u32, u32, u32)>,
}

impl LoopMd {
    fn new() -> Self {
        LoopMd { next_id: 0, lines: String::new(), leaves: None }
    }

    fn alloc(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    /// Register metadata for one innermost nested loop; returns the id for
    /// its backedge `!llvm.loop !N` annotation.
    fn tag_inner_loop(&mut self) -> u32 {
        let (dis, ilc, wrap) = match self.leaves {
            Some(t) => t,
            None => {
                let dis = self.alloc();
                let ilc = self.alloc();
                let isv = self.alloc();
                let uen = self.alloc();
                let wrap = self.alloc();
                self.lines.push_str(&format!(
                    "!{dis} = !{{!\"llvm.loop.unroll.disable\"}}\n\
                     !{ilc} = !{{!\"llvm.loop.interleave.count\", i32 4}}\n\
                     !{isv} = !{{!\"llvm.loop.isvectorized\", i32 1}}\n\
                     !{uen} = !{{!\"llvm.loop.unroll.enable\"}}\n\
                     !{wrap} = !{{!\"llvm.loop.vectorize.followup_vectorized\", !{isv}, !{uen}}}\n"
                ));
                self.leaves = Some((dis, ilc, wrap));
                (dis, ilc, wrap)
            }
        };
        let main = self.alloc();
        self.lines.push_str(&format!(
            "!{main} = distinct !{{!{main}, !{dis}, !{ilc}, !{wrap}}}\n"
        ));
        main
    }
}

impl<'a> FnCtx<'a> {
    fn fresh(&mut self) -> String {
        let t = format!("%t{}", self.tmp);
        self.tmp += 1;
        t
    }

    fn fresh_label(&mut self) -> u32 {
        self.label += 1;
        self.label
    }

    fn emit(&mut self, line: String) {
        self.body.push_str("  ");
        self.body.push_str(&line);
        self.body.push('\n');
    }

    fn emit_label(&mut self, name: &str) {
        self.body.push_str(name);
        self.body.push_str(":\n");
    }

    fn alloca(&mut self, name: &str, ty: &str) -> String {
        let slot = format!("%slot.{}.{}", self.tmp, name);
        self.tmp += 1;
        self.allocas.push_str(&format!("  {slot} = alloca {ty}, align 8\n"));
        slot
    }
}

pub fn generate(fns: &[FnDef], checked: &Checked, module_name: &str) -> Result<String, Diag> {
    let mut out = String::new();
    out.push_str(&format!("; hotlang v0.2 — module `{module_name}`\n"));
    out.push_str("; generated by hotc; every function is verified: zero allocation,\n");
    out.push_str("; statically bounded loops, no recursion, all array accesses proven\n");
    out.push_str("; in-bounds at compile time. Array params are noalias by construction.\n\n");

    let mut intrinsics: BTreeSet<&'static str> = BTreeSet::new();
    let mut loop_md = LoopMd::new();
    for f in fns {
        out.push_str(&gen_fn(f, checked, &mut intrinsics, &mut loop_md)?);
        out.push('\n');
    }

    for decl in &intrinsics {
        out.push_str(decl);
        out.push('\n');
    }
    if !intrinsics.is_empty() {
        out.push('\n');
    }

    out.push_str("attributes #0 = { nounwind willreturn norecurse memory(none) }\n");
    out.push_str("attributes #1 = { nounwind willreturn norecurse memory(argmem: readwrite) }\n");
    if !loop_md.lines.is_empty() {
        out.push('\n');
        out.push_str(&loop_md.lines);
    }
    Ok(out)
}

fn gen_fn(
    f: &FnDef,
    checked: &Checked,
    intrinsics: &mut BTreeSet<&'static str>,
    loop_md: &mut LoopMd,
) -> Result<String, Diag> {
    let mut ctx = FnCtx {
        checked,
        env: HashMap::new(),
        loc: HashMap::new(),
        tmp: 0,
        label: 0,
        allocas: String::new(),
        body: String::new(),
        intrinsics,
        loop_depth: 0,
        loop_md,
    };

    let mut sig_params = Vec::new();
    let mut has_arrays = false;
    for p in &f.params {
        let reg = format!("%arg.{}", p.name);
        match p.ty {
            Ty::Arr(..) => {
                has_arrays = true;
                let ro = if p.mutable { "" } else { " readonly" };
                sig_params.push(format!("ptr noalias nocapture{ro} align 8 {reg}"));
                ctx.loc.insert(p.name.clone(), Loc::Arr(reg));
            }
            ty => {
                sig_params.push(format!("{} {}", llty(ty), reg));
                ctx.loc.insert(p.name.clone(), Loc::Ssa(reg));
            }
        }
        ctx.env
            .insert(p.name.clone(), VarInfo { ty: p.ty, mutable: p.mutable, range: None });
    }

    gen_stmts(&mut ctx, &f.body)?;

    let attr = if has_arrays { "#1" } else { "#0" };
    Ok(format!(
        "define {} @{}({}) local_unnamed_addr {} {{\nentry:\n{}{}}}\n",
        llty(f.ret),
        f.name,
        sig_params.join(", "),
        attr,
        ctx.allocas,
        ctx.body
    ))
}

fn gen_stmts(ctx: &mut FnCtx, stmts: &[Stmt]) -> Result<Vec<String>, Diag> {
    let mut introduced = Vec::new();
    for stmt in stmts {
        match stmt {
            Stmt::Let { name, mutable, expr, .. } => {
                let ty = type_of(expr, &ctx.env, &ctx.checked.sigs)?;
                // Ranges must match sema exactly (immutable lets keep their
                // inferred range) or bounds checks would diverge between
                // verification and emission.
                let range = if *mutable { None } else { infer_range(expr, &ctx.env) };
                let val = gen_expr(ctx, expr)?;
                if *mutable {
                    let slot = ctx.alloca(name, llty(ty));
                    ctx.emit(format!("store {} {}, ptr {}", llty(ty), val, slot));
                    ctx.loc.insert(name.clone(), Loc::Slot(slot));
                } else {
                    ctx.loc.insert(name.clone(), Loc::Ssa(val));
                }
                ctx.env
                    .insert(name.clone(), VarInfo { ty, mutable: *mutable, range });
                introduced.push(name.clone());
            }
            Stmt::Assign { name, expr, .. } => {
                let ty = type_of(expr, &ctx.env, &ctx.checked.sigs)?;
                let val = gen_expr(ctx, expr)?;
                let slot = match &ctx.loc[name] {
                    Loc::Slot(s) => s.clone(),
                    _ => unreachable!("sema verified mutability"),
                };
                ctx.emit(format!("store {} {}, ptr {}", llty(ty), val, slot));
            }
            Stmt::Store { arr, idx, expr, .. } => {
                let (elem, ptr) = array_of(ctx, arr);
                let iv = gen_expr(ctx, idx)?;
                let val = gen_expr(ctx, expr)?;
                let gep = ctx.fresh();
                ctx.emit(format!(
                    "{gep} = getelementptr inbounds {}, ptr {}, i64 {}",
                    ll_elem(elem),
                    ptr,
                    iv
                ));
                ctx.emit(format!("store {} {}, ptr {gep}, align 8", ll_elem(elem), val));
            }
            Stmt::For { var, lo, hi, body, .. } => {
                // Tag only INNERMOST nested loops: inside another loop, and
                // containing no loop. Middle loops of deeper nests are never
                // vectorized, so tagging them would just pin unroll.disable.
                // (Any nested For appears at the top level of `body` — only
                // For introduces a statement scope — so this scan is deep.)
                let is_inner = ctx.loop_depth > 0
                    && !body.iter().any(|s| matches!(s, Stmt::For { .. }));
                let k = ctx.fresh_label();
                let slot = ctx.alloca(var, "i64");
                ctx.emit(format!("store i64 {lo}, ptr {slot}"));
                ctx.emit(format!("br label %loop.cond{k}"));

                ctx.emit_label(&format!("loop.cond{k}"));
                let iv = ctx.fresh();
                ctx.emit(format!("{iv} = load i64, ptr {slot}"));
                let cmp = ctx.fresh();
                ctx.emit(format!("{cmp} = icmp slt i64 {iv}, {hi}"));
                ctx.emit(format!("br i1 {cmp}, label %loop.body{k}, label %loop.end{k}"));

                ctx.emit_label(&format!("loop.body{k}"));
                ctx.loc.insert(var.clone(), Loc::Slot(slot.clone()));
                ctx.env.insert(
                    var.clone(),
                    VarInfo { ty: Ty::I64, mutable: false, range: Some((*lo, *hi - 1)) },
                );
                ctx.loop_depth += 1;
                let inner = gen_stmts(ctx, body)?;
                ctx.loop_depth -= 1;
                for name in inner {
                    ctx.env.remove(&name);
                    ctx.loc.remove(&name);
                }
                let iv2 = ctx.fresh();
                ctx.emit(format!("{iv2} = load i64, ptr {slot}"));
                let next = ctx.fresh();
                ctx.emit(format!("{next} = add nsw i64 {iv2}, 1"));
                ctx.emit(format!("store i64 {next}, ptr {slot}"));
                // Inner loops of a nest carry `!llvm.loop` metadata on the
                // backedge; see `LoopMd` for the measured rationale.
                if is_inner {
                    let id = ctx.loop_md.tag_inner_loop();
                    ctx.emit(format!("br label %loop.cond{k}, !llvm.loop !{id}"));
                } else {
                    ctx.emit(format!("br label %loop.cond{k}"));
                }

                ctx.emit_label(&format!("loop.end{k}"));
                ctx.env.remove(var);
                ctx.loc.remove(var);
            }
            Stmt::Return { expr, .. } => {
                let ty = type_of(expr, &ctx.env, &ctx.checked.sigs)?;
                let val = gen_expr(ctx, expr)?;
                ctx.emit(format!("ret {} {}", llty(ty), val));
            }
        }
    }
    Ok(introduced)
}

const I64_MIN: &str = "-9223372036854775808";

/// Emit total integer division/remainder. hotlang defines:
///   a / 0 == 0        a % 0 == a        (identity a == (a/b)*b + a%b holds)
///   MIN / -1 == MIN   MIN % -1 == 0     (wrapping negation)
/// The guards are branchless selects; when the divisor's proven range
/// excludes 0 (and the MIN/-1 pair is impossible), raw sdiv/srem is emitted
/// with zero overhead — proving safety statically is the fast path.
fn gen_int_divrem(
    ctx: &mut FnCtx,
    op: BinOp,
    lv: String,
    rv: String,
    lhs_range: Option<(i64, i64)>,
    rhs_range: Option<(i64, i64)>,
) -> Result<String, Diag> {
    let instr = if op == BinOp::Div { "sdiv" } else { "srem" };
    let nonzero = rhs_range.map_or(false, |(lo, hi)| lo > 0 || hi < 0);
    let no_neg1 = rhs_range.map_or(false, |(lo, hi)| lo > -1 || hi < -1);
    let no_min = lhs_range.map_or(false, |(lo, _)| lo > i64::MIN);
    if nonzero && (no_neg1 || no_min) {
        let dst = ctx.fresh();
        ctx.emit(format!("{dst} = {instr} i64 {lv}, {rv}"));
        return Ok(dst);
    }
    let bz = ctx.fresh();
    ctx.emit(format!("{bz} = icmp eq i64 {rv}, 0"));
    let bm1 = ctx.fresh();
    ctx.emit(format!("{bm1} = icmp eq i64 {rv}, -1"));
    let amin = ctx.fresh();
    ctx.emit(format!("{amin} = icmp eq i64 {lv}, {I64_MIN}"));
    let ovf = ctx.fresh();
    ctx.emit(format!("{ovf} = and i1 {amin}, {bm1}"));
    let bad = ctx.fresh();
    ctx.emit(format!("{bad} = or i1 {bz}, {ovf}"));
    let bsafe = ctx.fresh();
    ctx.emit(format!("{bsafe} = select i1 {bad}, i64 1, i64 {rv}"));
    let raw = ctx.fresh();
    ctx.emit(format!("{raw} = {instr} i64 {lv}, {bsafe}"));
    let dst = ctx.fresh();
    if op == BinOp::Div {
        let z = ctx.fresh();
        ctx.emit(format!("{z} = select i1 {bz}, i64 0, i64 {raw}"));
        ctx.emit(format!("{dst} = select i1 {ovf}, i64 {I64_MIN}, i64 {z}"));
    } else {
        let z = ctx.fresh();
        ctx.emit(format!("{z} = select i1 {bz}, i64 {lv}, i64 {raw}"));
        ctx.emit(format!("{dst} = select i1 {ovf}, i64 0, i64 {z}"));
    }
    Ok(dst)
}

/// Lower a math builtin to its LLVM intrinsic. sqrt/abs/min/max/fma/floor/
/// ceil are single instructions on arm64; exp/log/pow lower to alloc-free,
/// errno-free libm-equivalent intrinsics (the only calls hotlang emits).
fn gen_builtin(ctx: &mut FnCtx, name: &str, args: &[Expr]) -> Result<String, Diag> {
    let mut atys = Vec::new();
    let mut vals = Vec::new();
    for arg in args {
        atys.push(type_of(arg, &ctx.env, &ctx.checked.sigs)?);
        vals.push(gen_expr(ctx, arg)?);
    }
    let ret = builtin_ret(name, &atys).expect("sema verified builtin overload");
    // Conversions are single instructions / saturating intrinsics, total:
    // f64(i64) is exact sitofp; i64(f64) truncates toward zero, saturates
    // at the i64 range, and maps NaN to 0 (llvm.fptosi.sat semantics).
    if name == "f64" {
        let dst = ctx.fresh();
        ctx.emit(format!("{dst} = sitofp i64 {} to double", vals[0]));
        return Ok(dst);
    }
    if name == "i64" {
        ctx.intrinsics
            .insert("declare i64 @llvm.fptosi.sat.i64.f64(double)");
        let dst = ctx.fresh();
        ctx.emit(format!(
            "{dst} = call i64 @llvm.fptosi.sat.i64.f64(double {})",
            vals[0]
        ));
        return Ok(dst);
    }
    // (intrinsic call target, module-level declaration, trailing extra args)
    let (target, decl, extra): (&str, &'static str, &str) = match (name, &atys[..]) {
        ("sqrt", _) => ("llvm.sqrt.f64", "declare double @llvm.sqrt.f64(double)", ""),
        ("floor", _) => ("llvm.floor.f64", "declare double @llvm.floor.f64(double)", ""),
        ("ceil", _) => ("llvm.ceil.f64", "declare double @llvm.ceil.f64(double)", ""),
        ("exp", _) => ("llvm.exp.f64", "declare double @llvm.exp.f64(double)", ""),
        ("log", _) => ("llvm.log.f64", "declare double @llvm.log.f64(double)", ""),
        ("abs", [Ty::F64]) => ("llvm.fabs.f64", "declare double @llvm.fabs.f64(double)", ""),
        // i1 false: INT64_MIN input is NOT poison — abs(MIN) == MIN, total.
        ("abs", [Ty::I64]) => ("llvm.abs.i64", "declare i64 @llvm.abs.i64(i64, i1)", ", i1 false"),
        ("min", [Ty::F64, _]) => ("llvm.minnum.f64", "declare double @llvm.minnum.f64(double, double)", ""),
        ("max", [Ty::F64, _]) => ("llvm.maxnum.f64", "declare double @llvm.maxnum.f64(double, double)", ""),
        ("min", [Ty::I64, _]) => ("llvm.smin.i64", "declare i64 @llvm.smin.i64(i64, i64)", ""),
        ("max", [Ty::I64, _]) => ("llvm.smax.i64", "declare i64 @llvm.smax.i64(i64, i64)", ""),
        ("pow", _) => ("llvm.pow.f64", "declare double @llvm.pow.f64(double, double)", ""),
        ("fma", _) => ("llvm.fma.f64", "declare double @llvm.fma.f64(double, double, double)", ""),
        _ => unreachable!("sema verified builtin overload"),
    };
    ctx.intrinsics.insert(decl);
    let lowered: Vec<String> = atys
        .iter()
        .zip(&vals)
        .map(|(t, v)| format!("{} {}", llty(*t), v))
        .collect();
    let fmf = if ret == Ty::F64 { FMF } else { "" };
    let dst = ctx.fresh();
    ctx.emit(format!(
        "{dst} = call {fmf}{} @{}({}{extra})",
        llty(ret),
        target,
        lowered.join(", ")
    ));
    Ok(dst)
}

fn array_of(ctx: &FnCtx, name: &str) -> (Elem, String) {
    let elem = match ctx.env[name].ty {
        Ty::Arr(e, _) => e,
        _ => unreachable!("sema verified array type"),
    };
    let ptr = match &ctx.loc[name] {
        Loc::Arr(p) => p.clone(),
        _ => unreachable!(),
    };
    (elem, ptr)
}

fn gen_expr(ctx: &mut FnCtx, e: &Expr) -> Result<String, Diag> {
    match &e.kind {
        ExprKind::Int(n) => Ok(n.to_string()),
        ExprKind::Float(x) => Ok(f64_const(*x)),
        ExprKind::Bool(b) => Ok(if *b { "true" } else { "false" }.to_string()),
        ExprKind::Var(name) => match &ctx.loc[name] {
            Loc::Ssa(v) => Ok(v.clone()),
            Loc::Slot(slot) => {
                let ty = llty(ctx.env[name].ty);
                let slot = slot.clone();
                let dst = ctx.fresh();
                ctx.emit(format!("{dst} = load {ty}, ptr {slot}"));
                Ok(dst)
            }
            Loc::Arr(_) => unreachable!("sema rejects bare array use"),
        },
        ExprKind::Index { arr, idx } => {
            let (elem, ptr) = array_of(ctx, arr);
            let iv = gen_expr(ctx, idx)?;
            let gep = ctx.fresh();
            ctx.emit(format!(
                "{gep} = getelementptr inbounds {}, ptr {}, i64 {}",
                ll_elem(elem),
                ptr,
                iv
            ));
            let dst = ctx.fresh();
            ctx.emit(format!("{dst} = load {}, ptr {gep}, align 8", ll_elem(elem)));
            Ok(dst)
        }
        ExprKind::Unary { op, rhs } => {
            let rt = type_of(rhs, &ctx.env, &ctx.checked.sigs)?;
            let rv = gen_expr(ctx, rhs)?;
            let dst = ctx.fresh();
            match (op, rt) {
                (UnOp::Neg, Ty::I64) => ctx.emit(format!("{dst} = sub i64 0, {rv}")),
                (UnOp::Neg, Ty::F64) => ctx.emit(format!("{dst} = fneg {FMF}double {rv}")),
                (UnOp::Not, _) => ctx.emit(format!("{dst} = xor i1 {rv}, true")),
                _ => unreachable!("sema rejected"),
            }
            Ok(dst)
        }
        ExprKind::Binary { op, lhs, rhs } => {
            let lt = type_of(lhs, &ctx.env, &ctx.checked.sigs)?;
            // Integer division/remainder are TOTAL in hotlang: a/0 = 0,
            // a%0 = a, MIN/-1 = MIN, MIN%-1 = 0. Emitted branchlessly; the
            // guards are omitted entirely when interval analysis proves the
            // divisor can never be 0 (and the MIN/-1 pair is impossible).
            if lt == Ty::I64 && matches!(op, BinOp::Div | BinOp::Rem) {
                let lhs_range = infer_range(lhs, &ctx.env);
                let rhs_range = infer_range(rhs, &ctx.env);
                let lv = gen_expr(ctx, lhs)?;
                let rv = gen_expr(ctx, rhs)?;
                return gen_int_divrem(ctx, *op, lv, rv, lhs_range, rhs_range);
            }
            let lv = gen_expr(ctx, lhs)?;
            let rv = gen_expr(ctx, rhs)?;
            let dst = ctx.fresh();
            let ty = llty(lt);
            let is_f = lt == Ty::F64;
            let instr: String = match (op, is_f) {
                (BinOp::Add, true) => format!("fadd {FMF}"),
                (BinOp::Add, false) => "add ".into(),
                (BinOp::Sub, true) => format!("fsub {FMF}"),
                (BinOp::Sub, false) => "sub ".into(),
                (BinOp::Mul, true) => format!("fmul {FMF}"),
                (BinOp::Mul, false) => "mul ".into(),
                (BinOp::Div, true) => format!("fdiv {FMF}"),
                (BinOp::Div, false) => "sdiv ".into(),
                (BinOp::Rem, true) => format!("frem {FMF}"),
                (BinOp::Rem, false) => "srem ".into(),
                (BinOp::And, _) => "and ".into(),
                (BinOp::Or, _) => "or ".into(),
                (BinOp::Eq, true) => "fcmp oeq ".into(),
                (BinOp::Eq, false) => "icmp eq ".into(),
                // `une` (unordered not-equal), NOT `one`: matches C/Java/IEEE
                // so `x != x` is the canonical NaN test (true for NaN) and
                // `==`/`!=` stay complementary. With `one`, LLVM folds
                // `x != x` to false and deletes NaN guards as dead code.
                (BinOp::Ne, true) => "fcmp une ".into(),
                (BinOp::Ne, false) => "icmp ne ".into(),
                (BinOp::Lt, true) => "fcmp olt ".into(),
                (BinOp::Lt, false) => "icmp slt ".into(),
                (BinOp::Le, true) => "fcmp ole ".into(),
                (BinOp::Le, false) => "icmp sle ".into(),
                (BinOp::Gt, true) => "fcmp ogt ".into(),
                (BinOp::Gt, false) => "icmp sgt ".into(),
                (BinOp::Ge, true) => "fcmp oge ".into(),
                (BinOp::Ge, false) => "icmp sge ".into(),
            };
            ctx.emit(format!("{dst} = {instr}{ty} {lv}, {rv}"));
            Ok(dst)
        }
        ExprKind::Call { name, args } => {
            if !ctx.checked.sigs.contains_key(name) {
                return gen_builtin(ctx, name, args);
            }
            let ret = ctx.checked.sigs[name].ret;
            let mut lowered = Vec::new();
            for arg in args {
                let at = type_of(arg, &ctx.env, &ctx.checked.sigs)?;
                let av = gen_expr(ctx, arg)?;
                lowered.push(format!("{} {}", llty(at), av));
            }
            let dst = ctx.fresh();
            ctx.emit(format!(
                "{dst} = call {} @{}({})",
                llty(ret),
                name,
                lowered.join(", ")
            ));
            Ok(dst)
        }
        ExprKind::IfElse { cond, then, els } => {
            // Pure arms → eager evaluation + branchless `select`.
            let ty = type_of(then, &ctx.env, &ctx.checked.sigs)?;
            let cv = gen_expr(ctx, cond)?;
            let tv = gen_expr(ctx, then)?;
            let ev = gen_expr(ctx, els)?;
            let dst = ctx.fresh();
            ctx.emit(format!(
                "{dst} = select i1 {cv}, {ty} {tv}, {ty} {ev}",
                ty = llty(ty)
            ));
            Ok(dst)
        }
    }
}
