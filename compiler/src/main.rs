//! hotc — the hotlang compiler.
//!
//! ```text
//! hotc check <file.hot>            verify only (types + bounded-execution)
//! hotc emit  <file.hot>            print LLVM IR to stdout
//! hotc build <file.hot> [-o dir]   emit .ll, then clang -O3 → .s + .o + .dylib/.so
//! ```

mod ast;
mod codegen;
mod diag;
mod lexer;
mod parser;
mod sema;

use std::path::{Path, PathBuf};
use std::process::{exit, Command};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: hotc <check|emit|build> <file.hot> [-o outdir]");
        exit(2);
    }
    let cmd = args[1].as_str();
    let file = args[2].as_str();
    let outdir = args
        .iter()
        .position(|a| a == "-o")
        .and_then(|i| args.get(i + 1))
        .map(PathBuf::from);

    let src = match std::fs::read_to_string(file) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: cannot read `{file}`: {e}");
            exit(2);
        }
    };

    let ir = match compile(&src, file) {
        Ok(ir) => ir,
        Err(d) => {
            eprint!("{}", d.render(file, &src));
            exit(1);
        }
    };

    match cmd {
        "check" => {
            println!(
                "ok: `{file}` verified — zero allocation, loops statically bounded, \
                 recursion-free, all array accesses proven in-bounds"
            );
        }
        "emit" => {
            print!("{ir}");
        }
        "build" => {
            if let Err(msg) = build_native(&ir, file, outdir.as_deref()) {
                eprintln!("error: {msg}");
                exit(1);
            }
        }
        other => {
            eprintln!("error: unknown command `{other}` (expected check, emit, or build)");
            exit(2);
        }
    }
}

fn compile(src: &str, file: &str) -> Result<String, diag::Diag> {
    let toks = lexer::lex(src)?;
    let fns = parser::Parser::new(toks).parse_program()?;
    let checked = sema::check(&fns)?;
    // The module name is host-controlled (derived from the source filename)
    // and is written into the IR text. Sanitize to a safe charset so a
    // crafted filename cannot inject LLVM IR (e.g. a newline + a global).
    let raw = Path::new(file)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "module".to_string());
    let module: String = raw
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' })
        .collect();
    let module = if module.is_empty() { "module".to_string() } else { module };
    codegen::generate(&fns, &checked, &module)
}

fn build_native(ir: &str, file: &str, outdir: Option<&Path>) -> Result<(), String> {
    let stem = Path::new(file)
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "out".to_string());
    let dir = outdir.map(PathBuf::from).unwrap_or_else(|| PathBuf::from("hotout"));
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create `{}`: {e}", dir.display()))?;

    let ll = dir.join(format!("{stem}.ll"));
    std::fs::write(&ll, ir).map_err(|e| format!("cannot write `{}`: {e}", ll.display()))?;

    let asm = dir.join(format!("{stem}.s"));
    let obj = dir.join(format!("{stem}.o"));
    let dylib = dir.join(if cfg!(target_os = "macos") {
        format!("lib{stem}.dylib")
    } else {
        format!("lib{stem}.so")
    });

    run_clang(&["-O3", "-S", ll.to_str().unwrap(), "-o", asm.to_str().unwrap()])?;
    run_clang(&["-O3", "-c", ll.to_str().unwrap(), "-o", obj.to_str().unwrap()])?;
    run_clang(&["-O3", "-shared", ll.to_str().unwrap(), "-o", dylib.to_str().unwrap()])?;

    println!("built:");
    println!("  {}   (LLVM IR)", ll.display());
    println!("  {}    (native assembly — read it, it's short)", asm.display());
    println!("  {}    (object file, link into any C/C++/Rust host)", obj.display());
    println!("  {} (shared library, dlopen/FFI from any host incl. Java Panama)", dylib.display());
    Ok(())
}

fn run_clang(args: &[&str]) -> Result<(), String> {
    let out = Command::new("clang")
        .args(args)
        .output()
        .map_err(|e| format!("failed to run clang: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "clang failed:\n{}",
            String::from_utf8_lossy(&out.stderr)
        ));
    }
    Ok(())
}
