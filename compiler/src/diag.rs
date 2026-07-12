//! Compiler diagnostics with source snippets.

#[derive(Debug)]
pub struct Diag {
    pub msg: String,
    pub line: u32,
    pub col: u32,
    pub note: Option<String>,
}

impl Diag {
    pub fn new(msg: impl Into<String>, line: u32, col: u32) -> Self {
        Diag { msg: msg.into(), line, col, note: None }
    }

    pub fn with_note(mut self, note: impl Into<String>) -> Self {
        self.note = Some(note.into());
        self
    }

    /// Render a rustc-style diagnostic against the original source.
    pub fn render(&self, file: &str, src: &str) -> String {
        let mut out = String::new();
        out.push_str(&format!("error: {}\n", self.msg));
        out.push_str(&format!("  --> {}:{}:{}\n", file, self.line, self.col));
        if let Some(text) = src.lines().nth(self.line as usize - 1) {
            let gutter = format!("{}", self.line);
            let pad = " ".repeat(gutter.len());
            out.push_str(&format!("{pad} |\n"));
            out.push_str(&format!("{gutter} | {text}\n"));
            let caret_pad = " ".repeat(self.col.saturating_sub(1) as usize);
            out.push_str(&format!("{pad} | {caret_pad}^\n"));
        }
        if let Some(note) = &self.note {
            out.push_str(&format!("note: {note}\n"));
        }
        out
    }
}
