//! Hand-written lexer. Tracks line/col for diagnostics.

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    // keywords
    Fn,
    Let,
    Mut,
    Return,
    If,
    Else,
    For,
    In,
    True,
    False,
    // literals / identifiers
    Ident(String),
    Int(i64),
    Float(f64),
    // punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semi,
    Colon,
    Arrow,
    DotDot,
    // operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    EqEq,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,
    Bang,
    Eq,
    Eof,
}

impl Tok {
    pub fn describe(&self) -> String {
        match self {
            Tok::Ident(s) => format!("identifier `{s}`"),
            Tok::Int(n) => format!("integer literal `{n}`"),
            Tok::Float(x) => format!("float literal `{x}`"),
            Tok::Eof => "end of file".to_string(),
            other => format!("`{}`", other.lexeme()),
        }
    }

    fn lexeme(&self) -> &'static str {
        match self {
            Tok::Fn => "fn",
            Tok::Let => "let",
            Tok::Mut => "mut",
            Tok::Return => "return",
            Tok::If => "if",
            Tok::Else => "else",
            Tok::For => "for",
            Tok::In => "in",
            Tok::True => "true",
            Tok::False => "false",
            Tok::LParen => "(",
            Tok::RParen => ")",
            Tok::LBrace => "{",
            Tok::RBrace => "}",
            Tok::LBracket => "[",
            Tok::RBracket => "]",
            Tok::Comma => ",",
            Tok::Semi => ";",
            Tok::Colon => ":",
            Tok::Arrow => "->",
            Tok::DotDot => "..",
            Tok::Plus => "+",
            Tok::Minus => "-",
            Tok::Star => "*",
            Tok::Slash => "/",
            Tok::Percent => "%",
            Tok::EqEq => "==",
            Tok::NotEq => "!=",
            Tok::Lt => "<",
            Tok::Le => "<=",
            Tok::Gt => ">",
            Tok::Ge => ">=",
            Tok::AndAnd => "&&",
            Tok::OrOr => "||",
            Tok::Bang => "!",
            Tok::Eq => "=",
            _ => "",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Token {
    pub tok: Tok,
    pub line: u32,
    pub col: u32,
}

pub fn lex(src: &str) -> Result<Vec<Token>, crate::diag::Diag> {
    let mut out = Vec::new();
    let chars: Vec<char> = src.chars().collect();
    let mut i = 0usize;
    let mut line = 1u32;
    let mut col = 1u32;

    macro_rules! push {
        ($tok:expr, $line:expr, $col:expr) => {
            out.push(Token { tok: $tok, line: $line, col: $col })
        };
    }

    while i < chars.len() {
        let c = chars[i];
        let (tl, tc) = (line, col);

        // whitespace
        if c == '\n' {
            i += 1;
            line += 1;
            col = 1;
            continue;
        }
        if c.is_whitespace() {
            i += 1;
            col += 1;
            continue;
        }
        // line comment
        if c == '/' && i + 1 < chars.len() && chars[i + 1] == '/' {
            while i < chars.len() && chars[i] != '\n' {
                i += 1;
            }
            continue;
        }
        // identifier / keyword
        if c.is_ascii_alphabetic() || c == '_' {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_alphanumeric() || chars[i] == '_') {
                i += 1;
                col += 1;
            }
            let word: String = chars[start..i].iter().collect();
            let tok = match word.as_str() {
                "fn" => Tok::Fn,
                "let" => Tok::Let,
                "mut" => Tok::Mut,
                "return" => Tok::Return,
                "if" => Tok::If,
                "else" => Tok::Else,
                "for" => Tok::For,
                "in" => Tok::In,
                "true" => Tok::True,
                "false" => Tok::False,
                _ => Tok::Ident(word),
            };
            push!(tok, tl, tc);
            continue;
        }
        // number (integer or float, `_` separators and scientific notation)
        if c.is_ascii_digit() {
            let start = i;
            let mut is_float = false;
            while i < chars.len()
                && (chars[i].is_ascii_digit()
                    || chars[i] == '_'
                    || (chars[i] == '.' && !is_float && i + 1 < chars.len() && chars[i + 1].is_ascii_digit()))
            {
                if chars[i] == '.' {
                    is_float = true;
                }
                i += 1;
                col += 1;
            }
            // exponent: `e`/`E`, optional sign, at least one digit (1e-12, 2.5E9)
            if i < chars.len() && (chars[i] == 'e' || chars[i] == 'E') {
                let mut j = i + 1;
                if j < chars.len() && (chars[j] == '+' || chars[j] == '-') {
                    j += 1;
                }
                if j < chars.len() && chars[j].is_ascii_digit() {
                    is_float = true;
                    while j < chars.len() && chars[j].is_ascii_digit() {
                        j += 1;
                    }
                    col += (j - i) as u32;
                    i = j;
                }
            }
            let raw: String = chars[start..i].iter().filter(|&&ch| ch != '_').collect();
            if is_float {
                let x: f64 = raw.parse().map_err(|_| {
                    crate::diag::Diag::new(format!("invalid float literal `{raw}`"), tl, tc)
                })?;
                push!(Tok::Float(x), tl, tc);
            } else {
                let n: i64 = raw.parse().map_err(|_| {
                    crate::diag::Diag::new(format!("integer literal `{raw}` overflows i64"), tl, tc)
                })?;
                push!(Tok::Int(n), tl, tc);
            }
            continue;
        }
        // operators / punctuation
        let two = if i + 1 < chars.len() {
            Some((chars[i], chars[i + 1]))
        } else {
            None
        };
        let (tok, len) = match two {
            Some(('-', '>')) => (Tok::Arrow, 2),
            Some(('.', '.')) => (Tok::DotDot, 2),
            Some(('=', '=')) => (Tok::EqEq, 2),
            Some(('!', '=')) => (Tok::NotEq, 2),
            Some(('<', '=')) => (Tok::Le, 2),
            Some(('>', '=')) => (Tok::Ge, 2),
            Some(('&', '&')) => (Tok::AndAnd, 2),
            Some(('|', '|')) => (Tok::OrOr, 2),
            _ => match c {
                '(' => (Tok::LParen, 1),
                ')' => (Tok::RParen, 1),
                '{' => (Tok::LBrace, 1),
                '}' => (Tok::RBrace, 1),
                '[' => (Tok::LBracket, 1),
                ']' => (Tok::RBracket, 1),
                ',' => (Tok::Comma, 1),
                ';' => (Tok::Semi, 1),
                ':' => (Tok::Colon, 1),
                '+' => (Tok::Plus, 1),
                '-' => (Tok::Minus, 1),
                '*' => (Tok::Star, 1),
                '/' => (Tok::Slash, 1),
                '%' => (Tok::Percent, 1),
                '<' => (Tok::Lt, 1),
                '>' => (Tok::Gt, 1),
                '!' => (Tok::Bang, 1),
                '=' => (Tok::Eq, 1),
                other => {
                    return Err(crate::diag::Diag::new(
                        format!("unexpected character `{other}`"),
                        tl,
                        tc,
                    ))
                }
            },
        };
        push!(tok, tl, tc);
        i += len;
        col += len as u32;
    }
    out.push(Token { tok: Tok::Eof, line, col });
    Ok(out)
}
