//! Emerald IDE — Tauri backend.
//!
//! Three responsibilities, mirroring SPEC.md §3:
//!   * tree-sitter analysis of the buffer (emerald.rs),
//!   * the proof channel: spawn `emeraldc --check --json` per query,
//!   * trivial fs helpers so the webview never touches the disk directly.

mod emerald;

use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use serde::Serialize;

// ---------------------------------------------------------------- diags

#[derive(Serialize)]
pub struct Diag {
    pub kind: String,
    pub severity: String,
    pub code: String,
    pub file: String,
    pub message: String,
    pub expected: String,
    pub actual: String,
    pub source_line: String,
    /// 1-based, as emitted by emeraldc
    pub line: u32,
    pub column: u32,
}

#[derive(Serialize)]
pub struct CheckOutcome {
    pub exit_code: i32,
    pub timed_out: bool,
    pub ms: u64,
    pub diags: Vec<Diag>,
    /// set when the compiler couldn't be run, hung, or produced no usable JSON
    pub error: Option<String>,
}

fn jstr(o: &serde_json::Value, key: &str) -> String {
    o.get(key)
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .to_string()
}

fn jnum(o: &serde_json::Value, keys: &[&str]) -> u32 {
    for k in keys {
        if let Some(n) = o.get(*k).and_then(|v| v.as_u64()) {
            return n as u32;
        }
    }
    0
}

fn parse_diags(stdout: &str) -> Option<Vec<Diag>> {
    let trimmed = stdout.trim();
    if trimmed.is_empty() {
        return None;
    }
    let parsed: serde_json::Value = serde_json::from_str(trimmed).ok()?;
    let items = parsed.as_array()?;
    Some(
        items
            .iter()
            .filter_map(|item| {
                if item.as_object().is_none() {
                    return None;
                }
                Some(Diag {
                    kind: jstr(item, "kind"),
                    severity: jstr(item, "severity"),
                    code: jstr(item, "code"),
                    file: jstr(item, "file"),
                    message: jstr(item, "message"),
                    expected: jstr(item, "expected"),
                    actual: jstr(item, "actual"),
                    source_line: jstr(item, "source_line"),
                    // emeraldc speaks 1-based lines and byte columns; older
                    // builds said "column" where newer say "col".
                    line: jnum(item, &["line"]),
                    column: jnum(item, &["column", "col"]),
                })
            })
            .collect(),
    )
}

// ---------------------------------------------------------------- subprocess

struct RunResult {
    exit_code: i32,
    timed_out: bool,
    stdout: String,
    stderr: String,
}

/// Run a command capturing both pipes, killing it after `timeout`.
fn run_with_timeout(mut cmd: Command, timeout: Duration) -> Result<RunResult, String> {
    cmd.stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let mut out_pipe = child.stdout.take();
    let mut err_pipe = child.stderr.take();

    let out_t = thread::spawn(move || {
        let mut buf = Vec::new();
        if let Some(p) = out_pipe.as_mut() {
            let _ = p.read_to_end(&mut buf);
        }
        buf
    });
    let err_t = thread::spawn(move || {
        let mut buf = Vec::new();
        if let Some(p) = err_pipe.as_mut() {
            let _ = p.read_to_end(&mut buf);
        }
        buf
    });

    let deadline = Instant::now() + timeout;
    let mut exit_code: i32 = -1;
    let mut timed_out = false;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                exit_code = status.code().unwrap_or(-1);
                break;
            }
            Ok(None) => {
                if Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    timed_out = true;
                    break;
                }
                thread::sleep(Duration::from_millis(10));
            }
            Err(_) => break,
        }
    }

    let stdout = String::from_utf8_lossy(&out_t.join().unwrap_or_default()).into_owned();
    let stderr = String::from_utf8_lossy(&err_t.join().unwrap_or_default()).into_owned();

    Ok(RunResult {
        exit_code,
        timed_out,
        stdout,
        stderr,
    })
}

// ---------------------------------------------------------------- compiler discovery

fn is_executable(p: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        p.is_file()
            && fs::metadata(p)
                .map(|m| m.permissions().mode() & 0o111 != 0)
                .unwrap_or(false)
    }
    #[cfg(not(unix))]
    {
        p.is_file()
    }
}

fn bundled_resources() -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    {
        let exe = std::env::current_exe().ok()?;
        let macos = exe.parent()?; // …/Contents/MacOS
        let contents = macos.parent()?; // …/Contents
        if contents.file_name()?.to_str()? != "Contents" {
            return None;
        }
        Some(contents.join("Resources"))
    }
    #[cfg(not(target_os = "macos"))]
    {
        None
    }
}

/// Resolve emeraldc the same way the original IDE did:
/// $EMERALDC → .app bundle → $PATH → sibling checkout.
pub fn resolve_compiler_path() -> Option<(PathBuf, &'static str)> {
    if let Ok(env) = std::env::var("EMERALDC") {
        let p = PathBuf::from(env);
        if is_executable(&p) {
            return Some((p, "EMERALDC"));
        }
    }

    if let Some(res) = bundled_resources() {
        let cand = res.parent().unwrap().join("MacOS/emeraldc");
        if is_executable(&cand) {
            // the bundled compiler needs its stdlib pointed at Resources
            let stdlib = res.join("stdlib");
            if stdlib.is_dir() && std::env::var_os("EMERALD_STDLIB").is_none() {
                std::env::set_var("EMERALD_STDLIB", &stdlib);
            }
            return Some((cand, "bundled"));
        }
    }

    if let Ok(path_var) = std::env::var("PATH") {
        for dir in std::env::split_paths(&path_var) {
            let cand = dir.join("emeraldc");
            if is_executable(&cand) {
                return Some((cand, "PATH"));
            }
        }
    }

    let sibling = PathBuf::from("../emerald/bin/emeraldc");
    if is_executable(&sibling) {
        return Some((sibling, "sibling"));
    }
    None
}

// ---------------------------------------------------------------- temp files

/// Where the checked prefix is materialised. Living next to the real source
/// makes relative imports resolve identically (SPEC.md §3b).
fn temp_path(dir: Option<&str>, base_name: &str) -> PathBuf {
    match dir.filter(|d| !d.is_empty()) {
        Some(d) => Path::new(d).join(format!(".{base_name}.eide.rald")),
        None => {
            let tmp = std::env::var("TMPDIR").unwrap_or_else(|_| "/tmp".to_string());
            Path::new(&tmp).join("eide-unsaved.rald")
        }
    }
}

// ---------------------------------------------------------------- commands

/// Async: tree-sitter work must not run on Tauri's main thread.
#[tauri::command]
async fn analyze(text: String) -> emerald::Analysis {
    emerald::analyze(&text)
}

#[derive(Serialize)]
pub struct CompilerInfo {
    pub path: Option<String>,
    pub source: Option<String>,
}

#[tauri::command]
fn resolve_compiler() -> CompilerInfo {
    match resolve_compiler_path() {
        Some((p, src)) => CompilerInfo {
            path: Some(p.to_string_lossy().into_owned()),
            source: Some(src.to_string()),
        },
        None => CompilerInfo {
            path: None,
            source: None,
        },
    }
}

/// Async: subprocess + IO must not run on Tauri's main thread.
///
/// Run `emeraldc --check --json` over `text`, optionally truncated at
/// `upto` bytes (the accepted-prefix cut) and extended with a REPL suffix.
#[tauri::command]
async fn check(
    text: String,
    upto: Option<u32>,
    suffix: Option<String>,
    dir: Option<String>,
    base_name: String,
    timeout_ms: Option<u64>,
) -> CheckOutcome {
    let compiler = match resolve_compiler_path() {
        Some((p, _)) => p,
        None => {
            return CheckOutcome {
                exit_code: -1,
                timed_out: false,
                ms: 0,
                diags: vec![],
                error: Some("no emeraldc found".into()),
            }
        }
    };

    let mut body = match upto {
        Some(n) if (n as usize) <= text.len() => text[..n as usize].to_string(),
        _ => text.clone(),
    };
    if let Some(sfx) = suffix {
        body.push('\n');
        body.push_str(&sfx);
        body.push('\n');
    }

    let tmp = temp_path(dir.as_deref(), &base_name);
    if let Err(e) = fs::write(&tmp, body.as_bytes()) {
        return CheckOutcome {
            exit_code: -1,
            timed_out: false,
            ms: 0,
            diags: vec![],
            error: Some(format!("cannot write {}: {}", tmp.display(), e)),
        };
    }

    let mut cmd = Command::new(&compiler);
    cmd.arg("--check").arg("--json").arg(&tmp);

    let started = Instant::now();
    let result = run_with_timeout(cmd, Duration::from_millis(timeout_ms.unwrap_or(10_000)));
    let ms = started.elapsed().as_millis() as u64;
    let _ = fs::remove_file(&tmp);

    let rr = match result {
        Ok(rr) => rr,
        Err(e) => {
            return CheckOutcome {
                exit_code: -1,
                timed_out: false,
                ms,
                diags: vec![],
                error: Some(format!("could not run {}: {}", compiler.display(), e)),
            }
        }
    };

    if rr.timed_out {
        return CheckOutcome {
            exit_code: rr.exit_code,
            timed_out: true,
            ms,
            diags: vec![],
            error: Some(format!(
                "emeraldc timed out after {} ms",
                timeout_ms.unwrap_or(10_000)
            )),
        };
    }
    if rr.exit_code == 127 {
        return CheckOutcome {
            exit_code: 127,
            timed_out: false,
            ms,
            diags: vec![],
            error: Some("compiler vanished mid-run".into()),
        };
    }

    match parse_diags(&rr.stdout) {
        Some(diags) => CheckOutcome {
            exit_code: rr.exit_code,
            timed_out: false,
            ms,
            diags,
            error: None,
        },
        None => {
            let msg = if rr.exit_code == 0 {
                None // clean accept, no diagnostics section
            } else {
                Some(format!(
                    "emeraldc exited {} with no JSON diagnostics{}",
                    rr.exit_code,
                    if rr.stderr.trim().is_empty() {
                        String::new()
                    } else {
                        format!(": {}", rr.stderr.lines().next().unwrap_or(""))
                    }
                ))
            };
            CheckOutcome {
                exit_code: rr.exit_code,
                timed_out: false,
                ms,
                diags: vec![],
                error: msg,
            }
        }
    }
}

#[tauri::command]
async fn read_file(path: String) -> Result<String, String> {
    fs::read_to_string(&path).map_err(|e| e.to_string())
}

#[tauri::command]
async fn write_file(path: String, contents: String) -> Result<(), String> {
    fs::write(&path, contents).map_err(|e| e.to_string())
}

// ---------------------------------------------------------------- app

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            analyze,
            resolve_compiler,
            check,
            read_file,
            write_file
        ])
        .run(tauri::generate_context!())
        .expect("error while running emerald-ide");
}
