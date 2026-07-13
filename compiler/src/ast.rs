//! AST for hotlang v0.2.
//!
//! The language is deliberately small: scalar math, fixed-size array
//! parameters, `for i in lo..hi` loops with compile-time bounds, immutable
//! bindings plus explicit `let mut` accumulators. There is still no
//! construct that heap-allocates or recurses; loops are statically bounded;
//! every array access is proven in-bounds at compile time (see `sema`).

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Elem {
    I64,
    F64,
}

impl Elem {
    pub fn name(&self) -> &'static str {
        match self {
            Elem::I64 => "i64",
            Elem::F64 => "f64",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ty {
    I64,
    F64,
    Bool,
    /// Fixed-size array, parameter-only in v0.2. `[f64; 256]`
    Arr(Elem, u32),
    /// Circular buffer (the tick-stream primitive). Capacity is a
    /// power-of-two so indexing masks (`& (N-1)`) instead of dividing, and
    /// every access is in-bounds by construction. Parameter-only, always
    /// mutable. `ring[f64; 1024]`
    Ring(Elem, u32),
}

impl Ty {
    pub fn name(&self) -> String {
        match self {
            Ty::I64 => "i64".to_string(),
            Ty::F64 => "f64".to_string(),
            Ty::Bool => "bool".to_string(),
            Ty::Arr(e, n) => format!("[{}; {}]", e.name(), n),
            Ty::Ring(e, n) => format!("ring[{}; {}]", e.name(), n),
        }
    }

    pub fn scalar_of(e: Elem) -> Ty {
        match e {
            Elem::I64 => Ty::I64,
            Elem::F64 => Ty::F64,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub ty: Ty,
    /// `mut` — only meaningful for array params (writable output buffers).
    pub mutable: bool,
    pub line: u32,
    pub col: u32,
}

#[derive(Debug, Clone)]
pub struct FnDef {
    pub name: String,
    pub params: Vec<Param>,
    pub ret: Ty,
    pub body: Vec<Stmt>,
    pub line: u32,
    pub col: u32,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let { name: String, mutable: bool, expr: Expr, line: u32, col: u32 },
    /// `x = expr;` — only valid for `let mut` bindings.
    Assign { name: String, expr: Expr, line: u32, col: u32 },
    /// `arr[idx] = expr;` — only valid for `mut` array params.
    Store { arr: String, idx: Expr, expr: Expr, line: u32, col: u32 },
    /// `push r, expr;` — append to a ring, O(1), overwrites the oldest slot
    /// when full and advances the head. Only valid for a `mut ring` param.
    Push { ring: String, expr: Expr, line: u32, col: u32 },
    /// `for i in lo..hi { ... }` — lo/hi are integer literals, so every
    /// loop's trip count is known at compile time.
    For { var: String, lo: i64, hi: i64, body: Vec<Stmt>, line: u32, col: u32 },
    Return { expr: Expr, line: u32, col: u32 },
}

#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub line: u32,
    pub col: u32,
}

#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(f64),
    Bool(bool),
    Var(String),
    /// `arr[idx]` — index must be provably in-bounds (see sema ranges).
    Index {
        arr: String,
        idx: Box<Expr>,
    },
    Unary {
        op: UnOp,
        rhs: Box<Expr>,
    },
    Binary {
        op: BinOp,
        lhs: Box<Expr>,
        rhs: Box<Expr>,
    },
    Call {
        name: String,
        args: Vec<Expr>,
    },
    /// `if c { a } else { b }` — an expression, compiled to a branchless
    /// LLVM `select`. Both arms are pure, so eager evaluation is sound.
    IfElse {
        cond: Box<Expr>,
        then: Box<Expr>,
        els: Box<Expr>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Neg,
    Not,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
}

impl BinOp {
    pub fn symbol(&self) -> &'static str {
        match self {
            BinOp::Add => "+",
            BinOp::Sub => "-",
            BinOp::Mul => "*",
            BinOp::Div => "/",
            BinOp::Rem => "%",
            BinOp::Eq => "==",
            BinOp::Ne => "!=",
            BinOp::Lt => "<",
            BinOp::Le => "<=",
            BinOp::Gt => ">",
            BinOp::Ge => ">=",
            BinOp::And => "&&",
            BinOp::Or => "||",
        }
    }
}
