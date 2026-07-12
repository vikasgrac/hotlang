//! Recursive-descent parser.
//!
//! Grammar (v0):
//! ```text
//! program  := fn*
//! fn       := "fn" IDENT "(" params? ")" "->" type "{" stmt* "}"
//! param    := IDENT ":" type
//! type     := "i64" | "f64" | "bool"
//! stmt     := "let" IDENT "=" expr ";"
//!           | "return" expr ";"
//! expr     := or
//! or       := and ("||" and)*
//! and      := cmp ("&&" cmp)*
//! cmp      := add (("=="|"!="|"<"|"<="|">"|">=") add)?
//! add      := mul (("+"|"-") mul)*
//! mul      := unary (("*"|"/"|"%") unary)*
//! unary    := ("-"|"!") unary | primary
//! primary  := INT | FLOAT | "true" | "false"
//!           | IDENT | IDENT "(" args ")"
//!           | "(" expr ")"
//!           | "if" expr "{" expr "}" "else" "{" expr "}"
//! ```

use crate::ast::*;
use crate::diag::Diag;
use crate::lexer::{Tok, Token};

pub struct Parser {
    toks: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(toks: Vec<Token>) -> Self {
        Parser { toks, pos: 0 }
    }

    fn peek(&self) -> &Token {
        &self.toks[self.pos]
    }

    fn peek2(&self) -> Option<&Token> {
        self.toks.get(self.pos + 1)
    }

    fn advance(&mut self) -> Token {
        let t = self.toks[self.pos].clone();
        if self.pos < self.toks.len() - 1 {
            self.pos += 1;
        }
        t
    }

    fn eat(&mut self, expected: Tok, what: &str) -> Result<Token, Diag> {
        if self.peek().tok == expected {
            Ok(self.advance())
        } else {
            let t = self.peek();
            Err(Diag::new(
                format!("expected {what}, found {}", t.tok.describe()),
                t.line,
                t.col,
            ))
        }
    }

    fn eat_ident(&mut self, what: &str) -> Result<(String, u32, u32), Diag> {
        let t = self.peek().clone();
        match t.tok {
            Tok::Ident(name) => {
                self.advance();
                Ok((name, t.line, t.col))
            }
            _ => Err(Diag::new(
                format!("expected {what}, found {}", t.tok.describe()),
                t.line,
                t.col,
            )),
        }
    }

    fn parse_type(&mut self) -> Result<Ty, Diag> {
        if self.peek().tok == Tok::LBracket {
            self.advance();
            let (ename, eline, ecol) = self.eat_ident("an element type (`i64` or `f64`)")?;
            let elem = match ename.as_str() {
                "i64" => Elem::I64,
                "f64" => Elem::F64,
                other => {
                    return Err(Diag::new(
                        format!("arrays of `{other}` are not supported"),
                        eline,
                        ecol,
                    )
                    .with_note("array element types are `i64` and `f64`"))
                }
            };
            self.eat(Tok::Semi, "`;` (array types are written `[f64; 256]`)")?;
            let t = self.peek().clone();
            let len = match t.tok {
                Tok::Int(n) if n > 0 && n <= u32::MAX as i64 => {
                    self.advance();
                    n as u32
                }
                _ => {
                    return Err(Diag::new(
                        format!("expected a positive array length, found {}", t.tok.describe()),
                        t.line,
                        t.col,
                    )
                    .with_note("array lengths are compile-time constants — that's how hotlang bounds memory statically"))
                }
            };
            self.eat(Tok::RBracket, "`]`")?;
            return Ok(Ty::Arr(elem, len));
        }
        let (name, line, col) = self.eat_ident("a type (`i64`, `f64`, `bool`, or `[f64; N]`)")?;
        match name.as_str() {
            "i64" => Ok(Ty::I64),
            "f64" => Ok(Ty::F64),
            "bool" => Ok(Ty::Bool),
            other => Err(Diag::new(
                format!("unknown type `{other}`"),
                line,
                col,
            )
            .with_note("hotlang types are `i64`, `f64`, `bool`, and fixed arrays like `[f64; 256]`".to_string())),
        }
    }

    pub fn parse_program(&mut self) -> Result<Vec<FnDef>, Diag> {
        let mut fns = Vec::new();
        while self.peek().tok != Tok::Eof {
            if self.peek().tok == Tok::Let {
                let t = self.peek();
                return Err(Diag::new(
                    "module-level `let` is not supported",
                    t.line,
                    t.col,
                )
                .with_note(
                    "a hotlang module contains only `fn` definitions; \
                     write shared constants as literals or pass them as parameters",
                ));
            }
            fns.push(self.parse_fn()?);
        }
        Ok(fns)
    }

    fn parse_fn(&mut self) -> Result<FnDef, Diag> {
        let kw = self.eat(Tok::Fn, "`fn`")?;
        let (name, _, _) = self.eat_ident("function name")?;
        self.eat(Tok::LParen, "`(`")?;
        let mut params = Vec::new();
        if self.peek().tok != Tok::RParen {
            loop {
                let (pname, pline, pcol) = self.eat_ident("parameter name")?;
                self.eat(Tok::Colon, "`:` after parameter name")?;
                let mutable = if self.peek().tok == Tok::Mut {
                    self.advance();
                    true
                } else {
                    false
                };
                let ty = self.parse_type()?;
                if mutable && !matches!(ty, Ty::Arr(..)) {
                    return Err(Diag::new(
                        format!("parameter `{pname}` cannot be `mut`: only array parameters (output buffers) are mutable"),
                        pline,
                        pcol,
                    ));
                }
                params.push(Param { name: pname, ty, mutable, line: pline, col: pcol });
                if self.peek().tok == Tok::Comma {
                    self.advance();
                } else {
                    break;
                }
            }
        }
        self.eat(Tok::RParen, "`)`")?;
        self.eat(Tok::Arrow, "`->` (every hotlang function returns a value)")?;
        let ret = self.parse_type()?;
        self.eat(Tok::LBrace, "`{`")?;
        let mut body = Vec::new();
        while self.peek().tok != Tok::RBrace {
            body.push(self.parse_stmt()?);
        }
        self.eat(Tok::RBrace, "`}`")?;
        Ok(FnDef { name, params, ret, body, line: kw.line, col: kw.col })
    }

    fn parse_stmt(&mut self) -> Result<Stmt, Diag> {
        let t = self.peek().clone();
        match t.tok {
            Tok::Let => {
                self.advance();
                let mutable = if self.peek().tok == Tok::Mut {
                    self.advance();
                    true
                } else {
                    false
                };
                let (name, _, _) = self.eat_ident("binding name")?;
                self.eat(Tok::Eq, "`=`")?;
                let expr = self.parse_expr()?;
                self.eat(Tok::Semi, "`;`")?;
                Ok(Stmt::Let { name, mutable, expr, line: t.line, col: t.col })
            }
            Tok::For => {
                self.advance();
                let (var, _, _) = self.eat_ident("loop variable")?;
                self.eat(Tok::In, "`in`")?;
                let lo = self.eat_int_literal("loop lower bound")?;
                self.eat(Tok::DotDot, "`..`")?;
                let hi = self.eat_int_literal("loop upper bound")?;
                if lo >= hi {
                    return Err(Diag::new(
                        format!("loop range {lo}..{hi} is empty"),
                        t.line,
                        t.col,
                    ));
                }
                self.eat(Tok::LBrace, "`{`")?;
                let mut body = Vec::new();
                while self.peek().tok != Tok::RBrace {
                    body.push(self.parse_stmt()?);
                }
                self.eat(Tok::RBrace, "`}`")?;
                Ok(Stmt::For { var, lo, hi, body, line: t.line, col: t.col })
            }
            Tok::Return => {
                self.advance();
                let expr = self.parse_expr()?;
                self.eat(Tok::Semi, "`;`")?;
                Ok(Stmt::Return { expr, line: t.line, col: t.col })
            }
            Tok::Ident(name) => {
                if name == "while" {
                    return Err(Diag::new("hotlang has no `while` loop", t.line, t.col)
                        .with_note(
                            "the only loop is `for i in lo..hi` with integer-literal bounds — \
                             a statically known trip count is how hotlang proves bounded execution",
                        ));
                }
                // `x = expr;` or `arr[idx] = expr;`
                self.advance();
                let compound = matches!(
                    self.peek().tok,
                    Tok::Plus | Tok::Minus | Tok::Star | Tok::Slash | Tok::Percent
                ) && self.peek2().map_or(false, |n| n.tok == Tok::Eq);
                if compound {
                    let op = self.peek().tok.clone();
                    return Err(Diag::new(
                        format!("compound assignment `{}=` is not supported", op.describe().trim_matches('`')),
                        t.line,
                        t.col,
                    )
                    .with_note(format!(
                        "write it out: `{name} = {name} {} ...;` (the binding must be `let mut`)",
                        op.describe().trim_matches('`')
                    )));
                }
                match self.peek().tok {
                    Tok::Eq => {
                        self.advance();
                        let expr = self.parse_expr()?;
                        self.eat(Tok::Semi, "`;`")?;
                        Ok(Stmt::Assign { name, expr, line: t.line, col: t.col })
                    }
                    Tok::LBracket => {
                        self.advance();
                        let idx = self.parse_expr()?;
                        self.eat(Tok::RBracket, "`]`")?;
                        self.eat(Tok::Eq, "`=`")?;
                        let expr = self.parse_expr()?;
                        self.eat(Tok::Semi, "`;`")?;
                        Ok(Stmt::Store { arr: name, idx, expr, line: t.line, col: t.col })
                    }
                    Tok::LParen => Err(Diag::new(
                        format!("a bare call `{name}(...)` cannot be a statement"),
                        t.line,
                        t.col,
                    )
                    .with_note(
                        "hotlang functions are pure — an unused result is dead code. \
                         Bind it: `let r = ...;` or use it in the `return` expression",
                    )),
                    ref other => Err(Diag::new(
                        format!("expected `=` or `[` after `{name}`, found {}", other.describe()),
                        t.line,
                        t.col,
                    )),
                }
            }
            Tok::If => Err(Diag::new(
                "`if` is an expression in hotlang, not a statement",
                t.line,
                t.col,
            )
            .with_note(
                "there is no early return or conditional statement — bind the result \
                 (`let x = if cond { a } else { b };`) or fold the condition into the \
                 final `return`. Both arms are always evaluated (branchless select)",
            )),
            other => Err(Diag::new(
                format!(
                    "expected `let`, `for`, an assignment, or `return`, found {}",
                    other.describe()
                ),
                t.line,
                t.col,
            )),
        }
    }

    fn eat_int_literal(&mut self, what: &str) -> Result<i64, Diag> {
        let t = self.peek().clone();
        match t.tok {
            Tok::Int(n) => {
                self.advance();
                Ok(n)
            }
            other => Err(Diag::new(
                format!("expected {what} (an integer literal), found {}", other.describe()),
                t.line,
                t.col,
            )
            .with_note("loop bounds are compile-time constants — that's how hotlang proves bounded execution")),
        }
    }

    fn parse_expr(&mut self) -> Result<Expr, Diag> {
        let e = self.parse_or()?;
        if let Tok::Ident(word) = &self.peek().tok {
            if word == "as" {
                let t = self.peek();
                return Err(Diag::new("hotlang has no `as` casts", t.line, t.col).with_note(
                    "conversions are builtin calls: `f64(x)` for i64 -> f64, `i64(x)` for \
                     f64 -> i64 (truncates toward zero, saturates, NaN -> 0)",
                ));
            }
        }
        Ok(e)
    }

    fn parse_or(&mut self) -> Result<Expr, Diag> {
        let mut lhs = self.parse_and()?;
        while self.peek().tok == Tok::OrOr {
            let t = self.advance();
            let rhs = self.parse_and()?;
            lhs = Expr {
                kind: ExprKind::Binary { op: BinOp::Or, lhs: Box::new(lhs), rhs: Box::new(rhs) },
                line: t.line,
                col: t.col,
            };
        }
        Ok(lhs)
    }

    fn parse_and(&mut self) -> Result<Expr, Diag> {
        let mut lhs = self.parse_cmp()?;
        while self.peek().tok == Tok::AndAnd {
            let t = self.advance();
            let rhs = self.parse_cmp()?;
            lhs = Expr {
                kind: ExprKind::Binary { op: BinOp::And, lhs: Box::new(lhs), rhs: Box::new(rhs) },
                line: t.line,
                col: t.col,
            };
        }
        Ok(lhs)
    }

    fn parse_cmp(&mut self) -> Result<Expr, Diag> {
        let lhs = self.parse_add()?;
        let op = match self.peek().tok {
            Tok::EqEq => Some(BinOp::Eq),
            Tok::NotEq => Some(BinOp::Ne),
            Tok::Lt => Some(BinOp::Lt),
            Tok::Le => Some(BinOp::Le),
            Tok::Gt => Some(BinOp::Gt),
            Tok::Ge => Some(BinOp::Ge),
            _ => None,
        };
        if let Some(op) = op {
            let t = self.advance();
            let rhs = self.parse_add()?;
            Ok(Expr {
                kind: ExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) },
                line: t.line,
                col: t.col,
            })
        } else {
            Ok(lhs)
        }
    }

    fn parse_add(&mut self) -> Result<Expr, Diag> {
        let mut lhs = self.parse_mul()?;
        loop {
            let op = match self.peek().tok {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                _ => break,
            };
            let t = self.advance();
            let rhs = self.parse_mul()?;
            lhs = Expr {
                kind: ExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) },
                line: t.line,
                col: t.col,
            };
        }
        Ok(lhs)
    }

    fn parse_mul(&mut self) -> Result<Expr, Diag> {
        let mut lhs = self.parse_unary()?;
        loop {
            let op = match self.peek().tok {
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                Tok::Percent => BinOp::Rem,
                _ => break,
            };
            let t = self.advance();
            let rhs = self.parse_unary()?;
            lhs = Expr {
                kind: ExprKind::Binary { op, lhs: Box::new(lhs), rhs: Box::new(rhs) },
                line: t.line,
                col: t.col,
            };
        }
        Ok(lhs)
    }

    fn parse_unary(&mut self) -> Result<Expr, Diag> {
        let t = self.peek().clone();
        match t.tok {
            Tok::Minus => {
                self.advance();
                let rhs = self.parse_unary()?;
                Ok(Expr {
                    kind: ExprKind::Unary { op: UnOp::Neg, rhs: Box::new(rhs) },
                    line: t.line,
                    col: t.col,
                })
            }
            Tok::Bang => {
                self.advance();
                let rhs = self.parse_unary()?;
                Ok(Expr {
                    kind: ExprKind::Unary { op: UnOp::Not, rhs: Box::new(rhs) },
                    line: t.line,
                    col: t.col,
                })
            }
            _ => self.parse_primary(),
        }
    }

    fn parse_primary(&mut self) -> Result<Expr, Diag> {
        let t = self.peek().clone();
        match t.tok {
            Tok::Int(n) => {
                self.advance();
                Ok(Expr { kind: ExprKind::Int(n), line: t.line, col: t.col })
            }
            Tok::Float(x) => {
                self.advance();
                Ok(Expr { kind: ExprKind::Float(x), line: t.line, col: t.col })
            }
            Tok::True => {
                self.advance();
                Ok(Expr { kind: ExprKind::Bool(true), line: t.line, col: t.col })
            }
            Tok::False => {
                self.advance();
                Ok(Expr { kind: ExprKind::Bool(false), line: t.line, col: t.col })
            }
            Tok::LParen => {
                self.advance();
                let e = self.parse_expr()?;
                self.eat(Tok::RParen, "`)`")?;
                Ok(e)
            }
            Tok::If => {
                self.advance();
                let cond = self.parse_expr()?;
                self.eat(Tok::LBrace, "`{`")?;
                let then = self.parse_expr()?;
                self.eat(Tok::RBrace, "`}`")?;
                self.eat(Tok::Else, "`else` (hotlang `if` is an expression; both arms are required)")?;
                if self.peek().tok == Tok::If {
                    let t = self.peek();
                    return Err(Diag::new("`else if` is not supported", t.line, t.col)
                        .with_note(
                            "nest the next `if` inside braces: \
                             `else { if cond { a } else { b } }`",
                        ));
                }
                self.eat(Tok::LBrace, "`{`")?;
                let els = self.parse_expr()?;
                self.eat(Tok::RBrace, "`}`")?;
                Ok(Expr {
                    kind: ExprKind::IfElse {
                        cond: Box::new(cond),
                        then: Box::new(then),
                        els: Box::new(els),
                    },
                    line: t.line,
                    col: t.col,
                })
            }
            Tok::Ident(name) => {
                self.advance();
                if self.peek().tok == Tok::LParen {
                    self.advance();
                    let mut args = Vec::new();
                    if self.peek().tok != Tok::RParen {
                        loop {
                            args.push(self.parse_expr()?);
                            if self.peek().tok == Tok::Comma {
                                self.advance();
                            } else {
                                break;
                            }
                        }
                    }
                    self.eat(Tok::RParen, "`)`")?;
                    Ok(Expr { kind: ExprKind::Call { name, args }, line: t.line, col: t.col })
                } else if self.peek().tok == Tok::LBracket {
                    self.advance();
                    let idx = self.parse_expr()?;
                    self.eat(Tok::RBracket, "`]`")?;
                    Ok(Expr {
                        kind: ExprKind::Index { arr: name, idx: Box::new(idx) },
                        line: t.line,
                        col: t.col,
                    })
                } else {
                    Ok(Expr { kind: ExprKind::Var(name), line: t.line, col: t.col })
                }
            }
            other => Err(Diag::new(
                format!("expected an expression, found {}", other.describe()),
                t.line,
                t.col,
            )),
        }
    }
}
