//! Verilog backend — the answer to "we can't accept a tie."
//!
//! On a CPU, hotlang and hand-tuned C++ both lower through LLVM to the same
//! silicon, so they tie. The way to stop tying is to stop targeting a CPU.
//! The fastest HFT systems are FPGAs: a tick becomes a circuit, evaluated in
//! one clock cycle with no instruction fetch — 10-100x lower latency than
//! any software.
//!
//! General C++ cannot be synthesized to hardware (unbounded loops, dynamic
//! allocation, recursion, pointers). hotlang's guarantees — bounded, no
//! allocation, no recursion, static everything — are *exactly* what makes a
//! program a circuit. And the bit-squeeze feeds straight in: a narrow `i32`
//! is 32 gates of adder, not 64.
//!
//! This backend emits combinational Verilog for loop-free integer functions
//! — the branchy tick-to-trade decision logic (mid, spread, imbalance, the
//! trade decision). Loops, arrays, rings and floating point map to
//! sequential logic / DSP blocks / memories and are future work; they are
//! rejected here with a clear message rather than emitted wrongly.

use crate::ast::*;
use crate::diag::Diag;
use crate::sema::{type_of, FnSig, VarInfo};
use std::collections::HashMap;

fn width(ty: Ty) -> Result<u32, String> {
    match ty {
        Ty::I16 => Ok(16),
        Ty::I32 => Ok(32),
        Ty::I64 => Ok(64),
        Ty::Bool => Ok(1),
        other => Err(format!(
            "type `{}` cannot be synthesized to hardware yet (integers and bool only)",
            other.name()
        )),
    }
}

pub fn generate(fns: &[FnDef], module_name: &str) -> Result<String, Diag> {
    let mut sigs: HashMap<String, FnSig> = HashMap::new();
    for f in fns {
        sigs.insert(
            f.name.clone(),
            FnSig { params: f.params.iter().map(|p| p.ty).collect(), ret: f.ret },
        );
    }

    let mut out = String::new();
    out.push_str(&format!("// hotlang -> Verilog — module set `{module_name}`\n"));
    out.push_str("// Combinational hardware. Each function is a circuit: inputs in,\n");
    out.push_str("// decision out, one clock cycle, no CPU. Synthesize for an FPGA.\n\n");

    let mut emitted = 0;
    for f in fns {
        match gen_fn(f, &sigs) {
            Ok(v) => {
                out.push_str(&v);
                out.push('\n');
                emitted += 1;
            }
            Err(reason) => {
                out.push_str(&format!("// SKIPPED `{}`: {reason}\n\n", f.name));
            }
        }
    }
    if emitted == 0 {
        return Err(Diag::new(
            "no function could be synthesized to hardware".to_string(),
            1,
            1,
        )
        .with_note("the Verilog backend currently supports loop-free integer/bool functions"));
    }
    Ok(out)
}

fn gen_fn(f: &FnDef, sigs: &HashMap<String, FnSig>) -> Result<String, String> {
    // Only combinational integer/bool functions for now.
    let mut env: HashMap<String, VarInfo> = HashMap::new();
    let mut ports = Vec::new();
    for p in &f.params {
        let w = width(p.ty)?;
        ports.push(format!("  input  signed [{}:0] {}", w - 1, p.name));
        env.insert(p.name.clone(), VarInfo { ty: p.ty, mutable: false, range: None });
    }
    let rw = width(f.ret)?;
    ports.push(format!("  output signed [{}:0] out", rw - 1));

    let mut body = String::new();
    for stmt in &f.body {
        match stmt {
            Stmt::Let { name, expr, .. } => {
                let ty = type_of(expr, &env, sigs).map_err(|d| d.msg.clone())?;
                let w = width(ty)?;
                let e = gen_expr(expr, &env, sigs)?;
                body.push_str(&format!("  wire signed [{}:0] {name} = {e};\n", w - 1));
                env.insert(name.clone(), VarInfo { ty, mutable: false, range: None });
            }
            Stmt::Return { expr, .. } => {
                let e = gen_expr(expr, &env, sigs)?;
                body.push_str(&format!("  assign out = {e};\n"));
            }
            Stmt::For { .. } => {
                return Err("contains a loop (sequential logic — not yet synthesized)".into())
            }
            Stmt::Push { .. } | Stmt::Store { .. } => {
                return Err("writes memory (rings/arrays — not yet synthesized)".into())
            }
            Stmt::Assign { .. } => {
                return Err("uses mutable state (a register — not yet synthesized)".into())
            }
        }
    }

    Ok(format!(
        "module {}(\n{}\n);\n{}endmodule\n",
        f.name,
        ports.join(",\n"),
        body
    ))
}

fn gen_expr(
    e: &Expr,
    env: &HashMap<String, VarInfo>,
    sigs: &HashMap<String, FnSig>,
) -> Result<String, String> {
    match &e.kind {
        ExprKind::Int(n) => Ok(n.to_string()),
        ExprKind::Bool(b) => Ok(if *b { "1'b1" } else { "1'b0" }.to_string()),
        ExprKind::Float(_) => Err("floating point is not synthesized (needs a DSP block)".into()),
        ExprKind::Var(name) => Ok(name.clone()),
        ExprKind::Unary { op, rhs } => {
            let r = gen_expr(rhs, env, sigs)?;
            Ok(match op {
                UnOp::Neg => format!("(-{r})"),
                UnOp::Not => format!("(!{r})"),
            })
        }
        ExprKind::Binary { op, lhs, rhs } => {
            let l = gen_expr(lhs, env, sigs)?;
            let r = gen_expr(rhs, env, sigs)?;
            let sym = match op {
                BinOp::Add => "+",
                BinOp::Sub => "-",
                BinOp::Mul => "*",
                // Total division: a/0 == 0, mirrors the CPU semantics.
                BinOp::Div => return Ok(format!("(({r}) == 0 ? 0 : ({l}) / ({r}))")),
                BinOp::Rem => return Ok(format!("(({r}) == 0 ? ({l}) : ({l}) % ({r}))")),
                BinOp::Eq => "==",
                BinOp::Ne => "!=",
                BinOp::Lt => "<",
                BinOp::Le => "<=",
                BinOp::Gt => ">",
                BinOp::Ge => ">=",
                BinOp::And => "&&",
                BinOp::Or => "||",
            };
            Ok(format!("({l} {sym} {r})"))
        }
        ExprKind::IfElse { cond, then, els } => {
            let c = gen_expr(cond, env, sigs)?;
            let t = gen_expr(then, env, sigs)?;
            let el = gen_expr(els, env, sigs)?;
            Ok(format!("({c} ? {t} : {el})"))
        }
        ExprKind::Call { name, args } => {
            // Builtins map to combinational logic; conversions are width casts
            // handled by the enclosing wire width, so pass the value through.
            let a: Result<Vec<String>, String> =
                args.iter().map(|x| gen_expr(x, env, sigs)).collect();
            let a = a?;
            match name.as_str() {
                "min" => Ok(format!("(({}) < ({}) ? ({}) : ({}))", a[0], a[1], a[0], a[1])),
                "max" => Ok(format!("(({}) > ({}) ? ({}) : ({}))", a[0], a[1], a[0], a[1])),
                "abs" => Ok(format!("(({}) < 0 ? (-({})) : ({}))", a[0], a[0], a[0])),
                "i16" | "i32" | "i64" => Ok(a[0].clone()),
                other => Err(format!(
                    "call to `{other}` cannot be synthesized (only min/max/abs and integer casts)"
                )),
            }
        }
        ExprKind::Index { .. } => Err("array/ring indexing needs a memory (not yet synthesized)".into()),
    }
}
