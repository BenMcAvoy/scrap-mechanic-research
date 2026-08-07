#![cfg_attr(not(target_os = "windows"), allow(dead_code))]

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
#[cfg(target_os = "windows")]
use std::os::windows::ffi::OsStrExt;
use std::{
    collections::{HashMap, HashSet},
    io,
    path::PathBuf,
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::mpsc;

const APP_ID: &str = "387990";
const EXE_NAME: &str = "ScrapMechanic.exe";
const DEFAULT_ROOT: &str = r"C:\Program Files (x86)\Steam\steamapps\common\Scrap Mechanic";
const DEFAULT_REPORT_ROOT: &str = r"C:\Users\Ben\scrap_research\reports\scrap-mechanic";

const SERVER_INSTRUCTIONS: &str = r#"You are controlling a local Scrap Mechanic reverse-engineering and DLL-modification runtime.

Autonomous iteration flow:
1. Inspect the workspace and build the DLL with the project's existing build system. Do not assume the DLL path; verify the produced artifact.
2. Call launch_game when Scrap Mechanic is not running. Use keep_graphics=true only when visual rendering is required; otherwise prefer the default null-driver mode.
3. Call inject with the absolute DLL path. The MCP is configured for local development and has no approval gate for injection, memory writes, or unloading. Still validate the target PID and artifact before acting.
4. After launch or injection, call wait_for_event with an appropriate timeout. Prefer this over client-side polling. The Codex harness may not wake the agent from unsolicited notifications, but it will receive the result of a blocking wait_for_event call.
5. On process_crash, debug_dump_ready, crash_report_ready, or debug_exception, call get_run_report and get_debug_events, then inspect the referenced dump, text report, game logs, and DLL logs before changing code.
6. For a clean iteration, call uninject with the DLL module name/path and its cleanup export when the DLL supports one, then rebuild and inject the next artifact. Never force-unload a DLL without a cleanup export.
7. Use read_memory, write_memory, and hardware breakpoints only when the current investigation requires them. Record addresses, module bases, and observed events in the iteration reasoning.

Operational rules:
- Always use absolute DLL paths and the PID returned by the MCP.
- Treat a normal process exit, a crash, a debugger attach failure, and a timeout as different outcomes.
- wait_for_event is an intentional blocking call. Do not repeatedly poll game_status or get_debug_events while it is pending; use its after_id and let it complete.
- This server handles requests concurrently. After wait_for_event returns, get_debug_events, get_run_report, game_status, and stop_game may be called without waiting for another long-poll request to finish.
- A wait timeout means no matching event was observed during that interval; it is not evidence that the game crashed. Inspect status and logs, then start one new wait with the latest event id.
- Always pass after_id equal to the newest event id already observed. Do not reuse an old id, or the same historical event may be returned repeatedly.
- If any MCP tool call remains silent for more than 60 seconds, do not start more polling calls. Treat the server as stale, restart Codex/MCP, and retry with the same PID only after confirming that the game is still running.
- If the game is intentionally stopped by the debugger after an unhandled exception, use stop_game only to clear that known stale session; then relaunch and collect the new run's events and reports.
- A minidump is strongest when paired with the event history, exception address/code, module list, and source/PDB files.
- If no exception event is available, treat the exit event and generated report/log artifacts as the diagnostic result; fail-fast and external crash-reporting paths may not provide a stack.
- After diagnosing a failure, make one focused code change, rebuild, and repeat the same runtime flow."#;

#[derive(Clone, Debug, Serialize)]
struct Event {
    id: u64,
    timestamp: u64,
    kind: String,
    pid: Option<u32>,
    detail: String,
    report: Option<String>,
}

#[derive(Default)]
struct State {
    launched_pid: Option<u32>,
    last_seen_running: Option<bool>,
    last_seen_pid: Option<u32>,
    next_event_id: u64,
    events: Vec<Event>,
    event_generation: u64,
    event_signal: Arc<Condvar>,
    breakpoints: HashMap<u32, Breakpoint>,
    supervised: HashSet<u32>,
    notification_tx: Option<mpsc::UnboundedSender<Value>>,
    client_initialized: bool,
}

#[derive(Clone, Debug, Serialize)]
struct Breakpoint {
    id: u32,
    pid: u32,
    address: u64,
    access: String,
    length: u8,
    slot: u8,
    hits: u64,
    last_hit: Option<u64>,
}

type SharedState = Arc<Mutex<State>>;

#[derive(Deserialize)]
struct RpcRequest {
    #[serde(rename = "jsonrpc")]
    _jsonrpc: Option<String>,
    id: Option<Value>,
    method: String,
    params: Option<Value>,
}

#[tokio::main]
async fn main() -> io::Result<()> {
    let (output_tx, mut output_rx) = mpsc::unbounded_channel();
    let state: SharedState = Arc::new(Mutex::new(State {
        notification_tx: Some(output_tx.clone()),
        ..State::default()
    }));
    let (request_tx, mut request_rx) = mpsc::unbounded_channel();
    tokio::spawn(async move {
        let mut stdin = tokio::io::stdin();
        loop {
            match read_message(&mut stdin).await {
                Ok(Some(body)) => {
                    if request_tx.send(body).is_err() {
                        break;
                    }
                }
                Ok(None) | Err(_) => break,
            }
        }
    });
    #[cfg(target_os = "windows")]
    {
        let monitor_state = state.clone();
        thread::spawn(move || {
            loop {
                let _ = game_status(&monitor_state);
                thread::sleep(std::time::Duration::from_millis(500));
            }
        });
    }
    let mut stdout = tokio::io::stdout();
    loop {
        tokio::select! {
            output = output_rx.recv() => {
                if let Some(value) = output {
                    write_response(&mut stdout, value).await?;
                    continue;
                }
            }
            body = request_rx.recv() => {
                let Some(body) = body else { break; };
                let request: RpcRequest = match serde_json::from_slice(&body) {
                    Ok(value) => value,
                    Err(error) => {
                        write_response(&mut stdout, json!({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":error.to_string()}})).await?;
                        continue;
                    }
                };
                let Some(id) = request.id else {
                    continue;
                };
                let request_state = state.clone();
                let request_tx = output_tx.clone();
                tokio::spawn(async move {
                    let response = match dispatch(
                        &request_state,
                        &request.method,
                        request.params.unwrap_or_else(|| json!({})),
                    )
                    .await
                    {
                        Ok(result) => json!({"jsonrpc":"2.0","id":id,"result":result}),
                        Err(error) => json!({"jsonrpc":"2.0","id":id,"error":{"code":-32000,"message":error}}),
                    };
                    let _ = request_tx.send(response);
                });
            }
        }
    }
    Ok(())
}

async fn read_message<R: AsyncReadExt + Unpin>(reader: &mut R) -> io::Result<Option<Vec<u8>>> {
    let mut byte = [0u8; 1];
    if reader.read_exact(&mut byte).await.is_err() {
        return Ok(None);
    }

    // Current MCP stdio uses one JSON-RPC object per newline-delimited line.
    // Accept this in addition to the older Content-Length framing below.
    if byte[0] == b'{' || byte[0] == b'[' {
        let mut body = vec![byte[0]];
        loop {
            if reader.read_exact(&mut byte).await.is_err() {
                break;
            }
            if byte[0] == b'\n' {
                break;
            }
            body.push(byte[0]);
            if body.len() > 16 * 1024 * 1024 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "MCP message too large",
                ));
            }
        }
        return Ok(Some(body));
    }

    let mut headers = vec![byte[0]];
    loop {
        if reader.read_exact(&mut byte).await.is_err() {
            return Ok(None);
        }
        headers.push(byte[0]);
        if headers.ends_with(b"\r\n\r\n") || headers.ends_with(b"\n\n") {
            break;
        }
        if headers.len() > 8192 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "MCP headers too large",
            ));
        }
    }
    let text = String::from_utf8_lossy(&headers);
    let length = text.lines().find_map(|line| {
        line.strip_prefix("Content-Length:")
            .and_then(|v| v.trim().parse::<usize>().ok())
    });
    if let Some(length) = length {
        let mut body = vec![0; length];
        reader.read_exact(&mut body).await?;
        return Ok(Some(body));
    }
    let mut body = Vec::new();
    if let Some(pos) = headers.iter().position(|b| *b == b'\n') {
        body.extend_from_slice(&headers[pos + 1..]);
    }
    Ok(Some(body))
}

async fn write_response<W: AsyncWriteExt + Unpin>(writer: &mut W, value: Value) -> io::Result<()> {
    let body = serde_json::to_vec(&value)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    writer.write_all(&body).await?;
    writer.write_all(b"\n").await?;
    writer.flush().await
}

async fn dispatch(state: &SharedState, method: &str, params: Value) -> Result<Value, String> {
    match method {
        "initialize" => {
            state.lock().unwrap().client_initialized = true;
            Ok(
                json!({"protocolVersion":"2025-06-18","capabilities":{"tools":{},"logging":{}},"serverInfo":{"name":"scrap-mechanic-mcp","version":"0.2.0"},"instructions":SERVER_INSTRUCTIONS}),
            )
        }
        "notifications/initialized" => Ok(json!({})),
        "tools/list" => Ok(tool_list()),
        "tools/call" => {
            let name = params
                .get("name")
                .and_then(Value::as_str)
                .ok_or("tools/call requires name")?
                .to_owned();
            let args = params
                .get("arguments")
                .cloned()
                .unwrap_or_else(|| json!({}));
            let tool_state = state.clone();
            let result = tokio::task::spawn_blocking(move || call_tool(&tool_state, &name, args))
                .await
                .map_err(|error| format!("tool task failed: {error}"))?;
            match result {
                Ok(value) => Ok(call_tool_result(value, false)),
                Err(error) => Ok(call_tool_result(
                    json!({"error":{"category":"operation_failed","message":error,"retryable":false}}),
                    true,
                )),
            }
        }
        _ => Err(format!("Unsupported method: {method}")),
    }
}

fn call_tool_result(value: Value, is_error: bool) -> Value {
    let text = serde_json::to_string(&value).unwrap_or_else(|_| "{}".into());
    json!({
        "content": [{"type": "text", "text": text}],
        "structuredContent": value,
        "isError": is_error
    })
}

fn tool_list() -> Value {
    let tool = |name: &str, description: &str, schema: Value| json!({"name":name,"description":description,"inputSchema":schema});
    json!({"tools":[
        tool("game_status", "Find ScrapMechanic.exe, report running/crashed state, PID, and manager events.", json!({"type":"object","properties":{}})),
        tool("launch_game", "Launch Scrap Mechanic directly. Refuses to launch a second copy; uses -use_null_driver unless keep_graphics is true.", json!({"type":"object","properties":{"keep_graphics":{"type":"boolean"},"args":{"type":"array","items":{"type":"string"}}}})),
        tool("inject", "Inject an x64 DLL into Scrap Mechanic.", json!({"type":"object","required":["dll"],"properties":{"pid":{"type":"integer"},"dll":{"type":"string"},"timeout_ms":{"type":"integer"}}})),
        tool("uninject", "Cleanly unload a DLL using an explicit exported cleanup function.", json!({"type":"object","required":["module"],"properties":{"pid":{"type":"integer"},"module":{"type":"string"},"unload_export":{"type":"string"},"timeout_ms":{"type":"integer"}}})),
        tool("stop_game", "Stop the process previously launched or explicitly selected by PID.", json!({"type":"object","properties":{"pid":{"type":"integer"}}})),
        tool("read_memory", "Read bytes from the Scrap Mechanic process. Read-only.", json!({"type":"object","required":["address","length"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"length":{"type":"integer","maximum":1048576}}})),
        tool("write_memory", "Write bytes to process memory.", json!({"type":"object","required":["address","bytes"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"bytes":{"type":"string","description":"Hex bytes, e.g. 90 90"}}})),
        tool("set_hardware_breakpoint", "Set a Windows hardware breakpoint on the target process and record debugger hits.", json!({"type":"object","required":["address"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"access":{"type":"string","enum":["execute","write","readwrite"]},"length":{"type":"integer","enum":[1,2,4,8]}}})),
        tool("clear_hardware_breakpoint", "Remove a previously configured hardware breakpoint.", json!({"type":"object","required":["id"],"properties":{"id":{"type":"integer"}}})),
        tool("get_debug_events", "Return process lifecycle and hardware-breakpoint hit events.", json!({"type":"object","properties":{"clear":{"type":"boolean"}}})),
        tool("wait_for_event", "Wait for a matching lifecycle or debugger event without client-side polling.", json!({"type":"object","properties":{"kind":{"type":"string"},"pid":{"type":"integer"},"after_id":{"type":"integer"},"timeout_ms":{"type":"integer","maximum":300000}}})),
        tool("get_run_report", "Return the latest crash report and diagnostic artifact paths.", json!({"type":"object","properties":{"pid":{"type":"integer"}}}))
    ]})
}

fn call_tool(state: &SharedState, name: &str, args: Value) -> Result<Value, String> {
    #[cfg(not(target_os = "windows"))]
    {
        let _ = (state, name, args);
        return Err("This manager currently supports Windows only".into());
    }
    #[cfg(target_os = "windows")]
    {
        match name {
            "game_status" => game_status(state),
            "launch_game" => launch_game(state, args),
            "inject" => inject_dll(state, args),
            "uninject" => uninject_dll(state, args),
            "stop_game" => stop_game(state, args),
            "read_memory" => read_memory(args),
            "write_memory" => write_memory(state, args),
            "set_hardware_breakpoint" => set_breakpoint(state, args),
            "clear_hardware_breakpoint" => clear_breakpoint(state, args),
            "get_debug_events" => get_events(state, args),
            "wait_for_event" => wait_for_event(state, args),
            "get_run_report" => get_run_report(state, args),
            _ => Err(format!("Unknown tool: {name}")),
        }
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
fn event(state: &SharedState, kind: &str, pid: Option<u32>, detail: impl Into<String>) {
    let detail = detail.into();
    let (event, tx, signal) = {
        let mut guard = state.lock().unwrap();
        guard.next_event_id += 1;
        guard.event_generation = guard.event_generation.wrapping_add(1);
        let event = Event {
            id: guard.next_event_id,
            timestamp: now(),
            kind: kind.into(),
            pid,
            detail,
            report: None,
        };
        guard.events.push(event.clone());
        if guard.events.len() > 512 {
            let excess = guard.events.len() - 512;
            guard.events.drain(0..excess);
        }
        (
            event,
            guard.notification_tx.clone(),
            guard.event_signal.clone(),
        )
    };
    signal.notify_all();
    let initialized = state
        .lock()
        .map(|guard| guard.client_initialized)
        .unwrap_or(false);
    if initialized {
        if let Some(tx) = tx {
            let _ = tx.send(json!({
                "jsonrpc":"2.0",
                "method":"notifications/message",
                "params":{"level":"info","logger":"scrap-mechanic-mcp","data":event}
            }));
        }
    }
}

fn event_matches(event: &Event, args: &Value) -> bool {
    if let Some(kind) = args.get("kind").and_then(Value::as_str)
        && event.kind != kind
    {
        return false;
    }
    if let Some(pid) = args.get("pid").and_then(Value::as_u64)
        && event.pid != Some(pid as u32)
    {
        return false;
    }
    if let Some(after) = args.get("after_id").and_then(Value::as_u64)
        && event.id <= after
    {
        return false;
    }
    true
}

fn wait_for_event(state: &SharedState, args: Value) -> Result<Value, String> {
    let timeout = args
        .get("timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(30_000)
        .min(300_000);
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(timeout);
    loop {
        let signal = state.lock().unwrap().event_signal.clone();
        let guard = state.lock().unwrap();
        if let Some(event) = guard
            .events
            .iter()
            .find(|e| event_matches(e, &args))
            .cloned()
        {
            return Ok(json!({"matched":true,"event":event}));
        }
        let remaining = deadline.saturating_duration_since(std::time::Instant::now());
        if remaining.is_zero() {
            return Ok(json!({"matched":false,"timeout_ms":timeout}));
        }
        let generation = guard.event_generation;
        let (_guard, result) = signal
            .wait_timeout_while(guard, remaining, |current| {
                current.event_generation == generation
            })
            .map_err(|_| "event wait lock poisoned")?;
        if result.timed_out() {
            return Ok(json!({"matched":false,"timeout_ms":timeout}));
        }
    }
}

#[cfg(target_os = "windows")]
fn report_root() -> PathBuf {
    std::env::var_os("SCRAP_MECHANIC_MCP_REPORTS")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_REPORT_ROOT))
}

#[cfg(target_os = "windows")]
fn write_crash_report(state: &SharedState, pid: u32, code: Option<u32>) -> Option<PathBuf> {
    let root = report_root().join(format!("run-{pid}-{}", now()));
    if std::fs::create_dir_all(&root).is_err() {
        return None;
    }
    let events = state.lock().ok()?.events.clone();
    let temp_artifacts = std::env::var_os("TEMP")
        .map(PathBuf::from)
        .map(|temp| {
            std::fs::read_dir(temp)
                .ok()
                .into_iter()
                .flatten()
                .filter_map(|entry| entry.ok())
                .filter_map(|entry| {
                    let name = entry.file_name().to_string_lossy().to_string();
                    (name.contains(&format!("-{pid}-"))
                        && (name.ends_with(".dmp") || name.ends_with(".txt")))
                    .then(|| entry.path())
                })
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let report = json!({
        "pid": pid,
        "exit_code": code.map(|v| format!("0x{v:08X}")),
        "captured_at": now(),
        "kind": if code.unwrap_or(0) == 0 { "process_exit" } else { "process_crash" },
        "events": events,
        "in_process_crash_artifacts": temp_artifacts,
        "note": "The MCP records lifecycle evidence and discovers crash artifacts emitted by an injected reporter."
    });
    let path = root.join("report.json");
    if std::fs::write(&path, serde_json::to_vec_pretty(&report).ok()?).is_err() {
        return None;
    }
    Some(path)
}

#[cfg(target_os = "windows")]
fn write_debug_minidump(
    pid: u32,
    exception_code: u32,
    exception_address: usize,
) -> Option<PathBuf> {
    use std::os::windows::io::AsRawHandle;
    use windows::Win32::System::Diagnostics::Debug::{
        MiniDumpWithFullMemoryInfo, MiniDumpWithIndirectlyReferencedMemory, MiniDumpWithThreadInfo,
        MiniDumpWithUnloadedModules, MiniDumpWriteDump,
    };
    let root = report_root().join(format!("run-{pid}-{}", now()));
    std::fs::create_dir_all(&root).ok()?;
    let path = root.join(format!(
        "crash-{exception_code:08X}-{exception_address:X}.dmp"
    ));
    let file = std::fs::File::create(&path).ok()?;
    let process = process_handle(pid, 0x0010 | 0x0400 | 0x0800).ok()?;
    let dump_type = MiniDumpWithThreadInfo
        | MiniDumpWithUnloadedModules
        | MiniDumpWithIndirectlyReferencedMemory
        | MiniDumpWithFullMemoryInfo;
    unsafe {
        MiniDumpWriteDump(
            process,
            pid,
            windows::Win32::Foundation::HANDLE(file.as_raw_handle() as *mut std::ffi::c_void),
            dump_type,
            None,
            None,
            None,
        )
        .ok()?;
        let _ = windows::Win32::Foundation::CloseHandle(process);
    }
    Some(path)
}

#[cfg(target_os = "windows")]
fn debug_supervisor(state: SharedState, pid: u32) {
    use windows::Win32::Foundation::{DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED};
    use windows::Win32::System::Diagnostics::Debug::{
        ContinueDebugEvent, DEBUG_EVENT, DebugActiveProcess, EXCEPTION_DEBUG_EVENT,
        EXIT_PROCESS_DEBUG_EVENT, WaitForDebugEvent,
    };
    unsafe {
        if let Err(error) = DebugActiveProcess(pid) {
            event(
                &state,
                "debug_supervisor_error",
                Some(pid),
                format!("DebugActiveProcess failed: {error}"),
            );
            return;
        }
        event(
            &state,
            "debug_supervisor_attached",
            Some(pid),
            "external debugger attached",
        );
        loop {
            let mut debug_event = DEBUG_EVENT::default();
            if WaitForDebugEvent(&mut debug_event, 500).is_err() {
                if find_pid() != Some(pid) {
                    break;
                }
                continue;
            }
            let mut continue_status = DBG_CONTINUE;
            if debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT {
                let info = debug_event.u.Exception;
                let record = info.ExceptionRecord;
                let code = record.ExceptionCode.0 as u32;
                let address = record.ExceptionAddress as usize;
                event(
                    &state,
                    "debug_exception",
                    Some(pid),
                    format!(
                        "code=0x{code:08X} address=0x{address:X} first_chance={}",
                        info.dwFirstChance
                    ),
                );
                if info.dwFirstChance == 0 {
                    if let Some(path) = write_debug_minidump(pid, code, address) {
                        event(
                            &state,
                            "debug_dump_ready",
                            Some(pid),
                            path.display().to_string(),
                        );
                    } else {
                        event(
                            &state,
                            "debug_dump_error",
                            Some(pid),
                            "MiniDumpWriteDump failed",
                        );
                    }
                    continue_status = DBG_EXCEPTION_NOT_HANDLED;
                }
            } else if debug_event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT {
                event(
                    &state,
                    "debug_process_exit",
                    Some(pid),
                    "debug supervisor observed process exit",
                );
            }
            let _ = ContinueDebugEvent(
                debug_event.dwProcessId,
                debug_event.dwThreadId,
                continue_status,
            );
            if debug_event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT {
                break;
            }
        }
        let _ = windows::Win32::System::Diagnostics::Debug::DebugActiveProcessStop(pid);
    }
}

#[cfg(target_os = "windows")]
fn get_run_report(state: &SharedState, args: Value) -> Result<Value, String> {
    let pid = args.get("pid").and_then(Value::as_u64).map(|v| v as u32);
    let events = state
        .lock()
        .map_err(|_| "state lock poisoned")?
        .events
        .clone();
    let selected = events.iter().rev().find(|e| pid.is_none() || e.pid == pid);
    Ok(json!({"latest_event":selected,"reports_root":report_root()}))
}

#[cfg(target_os = "windows")]
fn game_root() -> PathBuf {
    PathBuf::from(DEFAULT_ROOT)
}

#[cfg(target_os = "windows")]
fn find_pid() -> Option<u32> {
    use windows::Win32::System::Diagnostics::ToolHelp::*;
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0).ok()?;
        let mut entry = PROCESSENTRY32W {
            dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
            ..Default::default()
        };
        let mut found = None;
        if Process32FirstW(snapshot, &mut entry).is_ok() {
            loop {
                let name = String::from_utf16_lossy(&entry.szExeFile);
                if name.trim_end_matches('\0').eq_ignore_ascii_case(EXE_NAME) {
                    found = Some(entry.th32ProcessID);
                    break;
                }
                if Process32NextW(snapshot, &mut entry).is_err() {
                    break;
                }
            }
        }
        let _ = windows::Win32::Foundation::CloseHandle(snapshot);
        found
    }
}

#[cfg(target_os = "windows")]
fn process_handle(pid: u32, access: u32) -> Result<windows::Win32::Foundation::HANDLE, String> {
    use windows::Win32::System::Threading::OpenProcess;
    unsafe {
        OpenProcess(
            windows::Win32::System::Threading::PROCESS_ACCESS_RIGHTS(access),
            false,
            pid,
        )
        .map_err(|e| format!("OpenProcess failed: {e}"))
    }
}

#[cfg(target_os = "windows")]
fn game_status(state: &SharedState) -> Result<Value, String> {
    let pid = find_pid();
    let running = pid.is_some();
    let (previous, previous_pid) = {
        let guard = state.lock().unwrap();
        (guard.last_seen_running, guard.last_seen_pid)
    };
    if previous == Some(true) && !running {
        let launched = previous_pid.or_else(|| state.lock().unwrap().launched_pid);
        let code = launched.and_then(exit_code);
        let kind = if code.unwrap_or(0) == 0 {
            "process_exit"
        } else {
            "process_crash"
        };
        let report = write_crash_report(state, launched.unwrap_or(0), code);
        event(
            state,
            kind,
            launched,
            format!(
                "ScrapMechanic.exe exited with code {}",
                code.map(|v| format!("0x{v:X}"))
                    .unwrap_or_else(|| "unknown".into())
            ),
        );
        if let Some(report) = report {
            event(
                state,
                "crash_report_ready",
                launched,
                report.display().to_string(),
            );
        }
    }
    if previous == Some(false) && running {
        event(state, "process_start", pid, "ScrapMechanic.exe detected");
    }
    if let Some(pid) = pid {
        let start_supervisor = {
            let mut guard = state.lock().unwrap();
            guard.supervised.insert(pid)
        };
        if start_supervisor {
            let supervisor_state = state.clone();
            thread::spawn(move || debug_supervisor(supervisor_state, pid));
        }
    }
    {
        let mut guard = state.lock().unwrap();
        guard.last_seen_running = Some(running);
        guard.last_seen_pid = pid;
    }
    Ok(json!({"running":running,"pid":pid,"root":game_root()}))
}

#[cfg(target_os = "windows")]
fn exit_code(pid: u32) -> Option<u32> {
    use windows::Win32::System::Threading::{
        GetExitCodeProcess, PROCESS_QUERY_LIMITED_INFORMATION,
    };
    let handle = process_handle(pid, PROCESS_QUERY_LIMITED_INFORMATION.0).ok()?;
    let mut code = 0;
    unsafe {
        GetExitCodeProcess(handle, &mut code).ok()?;
    }
    Some(code)
}

#[cfg(target_os = "windows")]
fn launch_game(state: &SharedState, args: Value) -> Result<Value, String> {
    use std::process::Command;
    if let Some(pid) = find_pid() {
        return Ok(json!({"launched":false,"already_running":true,"pid":pid}));
    }
    let root = game_root();
    let release = root.join("Release");
    let exe = release.join(EXE_NAME);
    if !exe.exists() {
        return Err(format!("Executable not found: {}", exe.display()));
    }
    std::fs::write(release.join("steam_appid.txt"), format!("{APP_ID}\n"))
        .map_err(|e| format!("Failed to write steam_appid.txt: {e}"))?;
    let keep_graphics = args
        .get("keep_graphics")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let mut command = Command::new(&exe);
    if !keep_graphics {
        command.arg("-use_null_driver");
    }
    if let Some(extra) = args.get("args").and_then(Value::as_array) {
        for value in extra {
            if let Some(arg) = value.as_str() {
                command.arg(arg);
            }
        }
    }
    let child = command
        .current_dir(&release)
        .spawn()
        .map_err(|e| format!("Launch failed: {e}"))?;
    let pid = child.id();
    state.lock().unwrap().launched_pid = Some(pid);
    event(
        state,
        "process_start",
        Some(pid),
        format!("launched with null_driver={}", !keep_graphics),
    );
    Ok(
        json!({"launched":true,"pid":pid,"keep_graphics":keep_graphics,"arguments":if keep_graphics { json!(args.get("args")) } else { json!(["-use_null_driver", args.get("args")]) }}),
    )
}

#[cfg(target_os = "windows")]
fn module_info(pid: u32, wanted: &str) -> Option<(u64, String)> {
    use windows::Win32::System::Diagnostics::ToolHelp::*;
    unsafe {
        let snapshot =
            CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid).ok()?;
        let mut entry = MODULEENTRY32W {
            dwSize: std::mem::size_of::<MODULEENTRY32W>() as u32,
            ..Default::default()
        };
        let mut found = None;
        if Module32FirstW(snapshot, &mut entry).is_ok() {
            loop {
                let name = String::from_utf16_lossy(&entry.szModule)
                    .trim_end_matches('\0')
                    .to_string();
                let path = String::from_utf16_lossy(&entry.szExePath)
                    .trim_end_matches('\0')
                    .to_string();
                if name.eq_ignore_ascii_case(wanted)
                    || path.eq_ignore_ascii_case(wanted)
                    || path.eq_ignore_ascii_case(
                        &std::fs::canonicalize(wanted).ok()?.display().to_string(),
                    )
                {
                    found = Some((entry.modBaseAddr as usize as u64, path));
                    break;
                }
                if Module32NextW(snapshot, &mut entry).is_err() {
                    break;
                }
            }
        }
        let _ = windows::Win32::Foundation::CloseHandle(snapshot);
        found
    }
}

#[cfg(target_os = "windows")]
fn inject_dll(state: &SharedState, args: Value) -> Result<Value, String> {
    use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
    use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
    use windows::Win32::System::Memory::{
        MEM_COMMIT, MEM_RELEASE, MEM_RESERVE, PAGE_READWRITE, VirtualAllocEx, VirtualFreeEx,
    };
    use windows::Win32::System::Threading::{
        CreateRemoteThread, GetExitCodeThread, WaitForSingleObject,
    };
    use windows::core::PCWSTR;

    let dll = args
        .get("dll")
        .and_then(Value::as_str)
        .ok_or("dll is required")?;
    let path = PathBuf::from(dll);
    if !path.is_absolute() {
        return Err("dll must be an absolute Windows path".into());
    }
    if !path.is_file() {
        return Err(format!("dll does not exist: {}", path.display()));
    }
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let timeout = args
        .get("timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(15_000)
        .min(120_000) as u32;
    if module_info(pid, path.to_str().unwrap_or_default()).is_some() {
        return Err("DLL is already loaded in the target process".into());
    }
    let target = process_handle(pid, 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400)?;
    let wide: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let byte_len = wide.len() * std::mem::size_of::<u16>();
    unsafe {
        let remote = VirtualAllocEx(
            target,
            None,
            byte_len,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE,
        );
        if remote.is_null() {
            return Err("VirtualAllocEx failed".into());
        }
        let mut keep_remote_allocation = false;
        let result = (|| {
            let mut written = 0;
            WriteProcessMemory(
                target,
                remote,
                wide.as_ptr() as *const _,
                byte_len,
                Some(&mut written),
            )
            .map_err(|e| format!("WriteProcessMemory failed: {e}"))?;
            if written != byte_len {
                return Err(format!(
                    "WriteProcessMemory wrote {written} of {byte_len} bytes"
                ));
            }
            let kernel_name: Vec<u16> = "kernel32.dll"
                .encode_utf16()
                .chain(std::iter::once(0))
                .collect();
            let kernel = GetModuleHandleW(PCWSTR(kernel_name.as_ptr()))
                .map_err(|e| format!("GetModuleHandleW failed: {e}"))?;
            let load = GetProcAddress(kernel, windows::core::PCSTR(b"LoadLibraryW\0".as_ptr()))
                .ok_or("LoadLibraryW export not found")?;
            let start: unsafe extern "system" fn(*mut std::ffi::c_void) -> u32 =
                std::mem::transmute(load);
            let thread = CreateRemoteThread(target, None, 0, Some(start), Some(remote), 0, None)
                .map_err(|e| format!("CreateRemoteThread failed: {e}"))?;
            let wait = WaitForSingleObject(thread, timeout);
            if wait != windows::Win32::Foundation::WAIT_OBJECT_0 {
                keep_remote_allocation = true;
                let loaded = module_info(pid, path.to_str().unwrap_or_default());
                let _ = windows::Win32::Foundation::CloseHandle(thread);
                if let Some((base, _)) = loaded {
                    return Ok((
                        base,
                        false,
                        format!("loader did not finish: 0x{:08X}", wait.0),
                    ));
                }
                return Err(format!(
                    "remote loader timed out or failed: 0x{:08X}",
                    wait.0
                ));
            }
            let mut module = 0;
            GetExitCodeThread(thread, &mut module)
                .map_err(|e| format!("GetExitCodeThread failed: {e}"))?;
            let _ = windows::Win32::Foundation::CloseHandle(thread);
            if module == 0 {
                return Err("LoadLibraryW returned failure".into());
            }
            Ok((module as u64, true, "loader completed".to_string()))
        })();
        if !keep_remote_allocation {
            let _ = VirtualFreeEx(target, remote, 0, MEM_RELEASE);
        }
        let _ = windows::Win32::Foundation::CloseHandle(target);
        let (module, loader_completed, loader_status) = result?;
        event(
            state,
            "dll_injected",
            Some(pid),
            format!(
                "{} base=0x{module:X} loader={loader_status}",
                path.display()
            ),
        );
        Ok(
            json!({"injected":true,"pid":pid,"dll":path,"module_base":format!("0x{module:X}"),"timeout_ms":timeout,"loader_completed":loader_completed,"loader_status":loader_status,"remote_path_retained":keep_remote_allocation}),
        )
    }
}

#[cfg(target_os = "windows")]
fn uninject_dll(state: &SharedState, args: Value) -> Result<Value, String> {
    use windows::Win32::Foundation::FreeLibrary;
    use windows::Win32::System::LibraryLoader::{
        DONT_RESOLVE_DLL_REFERENCES, GetProcAddress, LoadLibraryExW,
    };
    use windows::Win32::System::Threading::{
        CreateRemoteThread, GetExitCodeThread, WaitForSingleObject,
    };
    use windows::core::{PCSTR, PCWSTR};
    let module = args
        .get("module")
        .and_then(Value::as_str)
        .ok_or("module is required")?;
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let (base, path) = module_info(pid, module).ok_or("module is not loaded in target process")?;
    let export = args
        .get("unload_export")
        .and_then(Value::as_str)
        .unwrap_or("ScrapMechanicMod_Unload");
    let local_path: Vec<u16> = std::path::Path::new(&path)
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let local = LoadLibraryExW(
            PCWSTR(local_path.as_ptr()),
            None,
            DONT_RESOLVE_DLL_REFERENCES,
        )
        .map_err(|e| format!("LoadLibraryExW failed: {e}"))?;
        let export_name = format!("{export}\0");
        let symbol = GetProcAddress(local, PCSTR(export_name.as_bytes().as_ptr()))
            .ok_or_else(|| format!("module does not export {export}"))?;
        let local_base = local.0 as usize as u64;
        let rva = (symbol as usize as u64)
            .checked_sub(local_base)
            .ok_or("invalid unload export RVA")?;
        let remote_fn = (base + rva) as *const std::ffi::c_void;
        let target = process_handle(pid, 0x0002 | 0x0400)?;
        let start: unsafe extern "system" fn(*mut std::ffi::c_void) -> u32 =
            std::mem::transmute(remote_fn);
        let thread =
            CreateRemoteThread(target, None, 0, Some(start), Some(base as *mut _), 0, None)
                .map_err(|e| format!("CreateRemoteThread(unload) failed: {e}"))?;
        let timeout = args
            .get("timeout_ms")
            .and_then(Value::as_u64)
            .unwrap_or(15_000)
            .min(120_000) as u32;
        let wait = WaitForSingleObject(thread, timeout);
        let mut result = 0;
        let _ = GetExitCodeThread(thread, &mut result);
        let _ = windows::Win32::Foundation::CloseHandle(thread);
        let _ = windows::Win32::Foundation::CloseHandle(target);
        let _ = FreeLibrary(local);
        if wait != windows::Win32::Foundation::WAIT_OBJECT_0 {
            return Err(format!(
                "remote unload timed out or failed: 0x{:08X}",
                wait.0
            ));
        }
        if module_info(pid, module).is_some() {
            return Err("unload returned but module is still loaded".into());
        }
        event(
            state,
            "dll_uninjected",
            Some(pid),
            format!("{module} export={export} result={result}"),
        );
        Ok(
            json!({"uninjected":true,"pid":pid,"module":module,"path":path,"unload_export":export,"return_code":result}),
        )
    }
}

#[cfg(target_os = "windows")]
fn stop_game(state: &SharedState, args: Value) -> Result<Value, String> {
    use windows::Win32::System::Threading::TerminateProcess;
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(|| state.lock().unwrap().launched_pid)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let handle = process_handle(pid, 1)?;
    unsafe {
        TerminateProcess(handle, 0).map_err(|e| format!("TerminateProcess failed: {e}"))?;
    }
    event(
        state,
        "process_terminate",
        Some(pid),
        "termination requested",
    );
    Ok(json!({"stopped":true,"pid":pid}))
}

#[cfg(target_os = "windows")]
fn read_memory(args: Value) -> Result<Value, String> {
    use windows::Win32::System::Diagnostics::Debug::ReadProcessMemory;
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let address = args
        .get("address")
        .and_then(Value::as_u64)
        .ok_or("address is required")?;
    let length = args
        .get("length")
        .and_then(Value::as_u64)
        .ok_or("length is required")? as usize;
    if length == 0 || length > 1024 * 1024 {
        return Err("length must be between 1 and 1048576".into());
    }
    let handle = process_handle(pid, 0x0010 | 0x0400)?;
    let mut bytes = vec![0u8; length];
    let mut read = 0;
    unsafe {
        ReadProcessMemory(
            handle,
            address as *const _,
            bytes.as_mut_ptr() as *mut _,
            length,
            Some(&mut read),
        )
        .map_err(|e| format!("ReadProcessMemory failed: {e}"))?;
    }
    bytes.truncate(read);
    Ok(
        json!({"pid":pid,"address":format!("0x{address:X}"),"length":read,"bytes":bytes.iter().map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" ")}),
    )
}

#[cfg(target_os = "windows")]
fn write_memory(state: &SharedState, args: Value) -> Result<Value, String> {
    use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let address = args
        .get("address")
        .and_then(Value::as_u64)
        .ok_or("address is required")?;
    let text = args
        .get("bytes")
        .and_then(Value::as_str)
        .ok_or("bytes must be a hex string")?;
    let bytes = parse_hex(text)?;
    let handle = process_handle(pid, 0x0020 | 0x0008 | 0x0400)?;
    let mut written = 0;
    unsafe {
        WriteProcessMemory(
            handle,
            address as *const _,
            bytes.as_ptr() as *const _,
            bytes.len(),
            Some(&mut written),
        )
        .map_err(|e| format!("WriteProcessMemory failed: {e}"))?;
    }
    event(
        state,
        "memory_write",
        Some(pid),
        format!("{} bytes at 0x{address:X}", written),
    );
    Ok(json!({"pid":pid,"address":format!("0x{address:X}"),"written":written}))
}

fn parse_hex(text: &str) -> Result<Vec<u8>, String> {
    let compact = text.replace([' ', '\n', '\r', '\t'], "");
    if compact.len() % 2 != 0 {
        return Err("Hex byte string must have an even number of digits".into());
    }
    (0..compact.len())
        .step_by(2)
        .map(|i| {
            u8::from_str_radix(&compact[i..i + 2], 16)
                .map_err(|_| format!("Invalid hex at byte {i}"))
        })
        .collect()
}

#[cfg(target_os = "windows")]
fn thread_ids(pid: u32) -> Vec<u32> {
    use windows::Win32::System::Diagnostics::ToolHelp::*;
    unsafe {
        let snapshot = match CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) {
            Ok(v) => v,
            Err(_) => return Vec::new(),
        };
        let mut entry = THREADENTRY32 {
            dwSize: std::mem::size_of::<THREADENTRY32>() as u32,
            ..Default::default()
        };
        let mut ids = Vec::new();
        if Thread32First(snapshot, &mut entry).is_ok() {
            loop {
                if entry.th32OwnerProcessID == pid {
                    ids.push(entry.th32ThreadID);
                }
                if Thread32Next(snapshot, &mut entry).is_err() {
                    break;
                }
            }
        }
        let _ = windows::Win32::Foundation::CloseHandle(snapshot);
        ids
    }
}

#[cfg(target_os = "windows")]
fn program_breakpoint(bp: &Breakpoint, clear: bool) -> Result<usize, String> {
    use windows::Win32::System::Diagnostics::Debug::{
        CONTEXT, CONTEXT_DEBUG_REGISTERS_AMD64, GetThreadContext, SetThreadContext,
    };
    use windows::Win32::System::Threading::{
        OpenThread, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION, THREAD_SET_CONTEXT,
    };
    let mut changed = 0;
    for tid in thread_ids(bp.pid) {
        unsafe {
            let handle = match OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                false,
                tid,
            ) {
                Ok(h) => h,
                Err(_) => continue,
            };
            let mut context = CONTEXT {
                ContextFlags: CONTEXT_DEBUG_REGISTERS_AMD64,
                ..Default::default()
            };
            if GetThreadContext(handle, &mut context).is_err() {
                let _ = windows::Win32::Foundation::CloseHandle(handle);
                continue;
            }
            let enable_bit = 1u64 << (bp.slot as u64 * 2);
            if clear {
                context.Dr7 &= !enable_bit;
                context.Dr6 = 0;
            } else {
                match bp.slot {
                    0 => context.Dr0 = bp.address,
                    1 => context.Dr1 = bp.address,
                    2 => context.Dr2 = bp.address,
                    3 => context.Dr3 = bp.address,
                    _ => unreachable!(),
                }
                let rw = match bp.access.as_str() {
                    "execute" => 0,
                    "write" => 1,
                    _ => 3,
                };
                let len_code = match bp.length {
                    1 => 0,
                    2 => 1,
                    4 => 3,
                    8 => 2,
                    _ => return Err("invalid hardware breakpoint length".into()),
                };
                let shift = 16 + bp.slot as u64 * 4;
                context.Dr7 &= !(0xFu64 << shift);
                context.Dr7 |= ((rw | (len_code << 2)) as u64) << shift;
                context.Dr7 |= enable_bit;
            }
            if SetThreadContext(handle, &context).is_ok() {
                changed += 1;
            }
            let _ = windows::Win32::Foundation::CloseHandle(handle);
        }
    }
    Ok(changed)
}

#[cfg(target_os = "windows")]
fn set_breakpoint(state: &SharedState, args: Value) -> Result<Value, String> {
    let pid = args
        .get("pid")
        .and_then(Value::as_u64)
        .map(|v| v as u32)
        .or_else(find_pid)
        .ok_or("Game is not running")?;
    let address = args
        .get("address")
        .and_then(Value::as_u64)
        .ok_or("address is required")?;
    let access = args
        .get("access")
        .and_then(Value::as_str)
        .unwrap_or("execute")
        .to_string();
    let length = args.get("length").and_then(Value::as_u64).unwrap_or(1) as u8;
    if ![1, 2, 4, 8].contains(&length) || (access == "execute" && length != 1) {
        return Err("Invalid breakpoint length; execute breakpoints require length 1".into());
    }
    let used: Vec<u8> = state
        .lock()
        .unwrap()
        .breakpoints
        .values()
        .filter(|b| b.pid == pid)
        .map(|b| b.slot)
        .collect();
    let slot = (0..4)
        .find(|s| !used.contains(s))
        .ok_or("All four hardware breakpoint slots are already in use for this process")?;
    let id = state
        .lock()
        .unwrap()
        .breakpoints
        .keys()
        .max()
        .copied()
        .unwrap_or(0)
        + 1;
    let bp = Breakpoint {
        id,
        pid,
        address,
        access: access.clone(),
        length,
        slot,
        hits: 0,
        last_hit: None,
    };
    let changed = program_breakpoint(&bp, false)?;
    if changed == 0 {
        return Err(
            "Could not program any game threads; process may be exiting or protected".into(),
        );
    }
    state.lock().unwrap().breakpoints.insert(id, bp.clone());
    event(
        state,
        "hardware_breakpoint",
        Some(pid),
        format!(
            "configured id={id} address=0x{address:X} access={access} length={length} threads={changed}"
        ),
    );
    let monitor_state = state.clone();
    thread::spawn(move || monitor_breakpoint(monitor_state, id));
    Ok(
        json!({"configured":true,"breakpoint":bp,"threads_configured":changed,"hit_detection":"DR6 polling"}),
    )
}

#[cfg(target_os = "windows")]
fn monitor_breakpoint(state: SharedState, id: u32) {
    use windows::Win32::System::Diagnostics::Debug::{
        CONTEXT, CONTEXT_DEBUG_REGISTERS_AMD64, GetThreadContext, SetThreadContext,
    };
    use windows::Win32::System::Threading::{
        OpenThread, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION, THREAD_SET_CONTEXT,
    };
    loop {
        let bp = { state.lock().unwrap().breakpoints.get(&id).cloned() };
        let Some(bp) = bp else { break };
        for tid in thread_ids(bp.pid) {
            unsafe {
                let Ok(handle) = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    false,
                    tid,
                ) else {
                    continue;
                };
                let mut context = CONTEXT {
                    ContextFlags: CONTEXT_DEBUG_REGISTERS_AMD64,
                    ..Default::default()
                };
                if GetThreadContext(handle, &mut context).is_ok()
                    && context.Dr6 & (1u64 << bp.slot) != 0
                {
                    let hit_address = match bp.slot {
                        0 => context.Dr0,
                        1 => context.Dr1,
                        2 => context.Dr2,
                        3 => context.Dr3,
                        _ => 0,
                    };
                    context.Dr6 = 0;
                    let _ = SetThreadContext(handle, &context);
                    if let Ok(mut guard) = state.lock() {
                        if let Some(current) = guard.breakpoints.get_mut(&id) {
                            current.hits += 1;
                            current.last_hit = Some(now());
                        }
                    }
                    event(
                        &state,
                        "hardware_breakpoint_hit",
                        Some(bp.pid),
                        format!("id={id} thread={tid} address=0x{hit_address:X}"),
                    );
                }
                let _ = windows::Win32::Foundation::CloseHandle(handle);
            }
        }
        thread::sleep(std::time::Duration::from_millis(25));
    }
}

#[cfg(target_os = "windows")]
fn clear_breakpoint(state: &SharedState, args: Value) -> Result<Value, String> {
    let id = args
        .get("id")
        .and_then(Value::as_u64)
        .ok_or("id is required")? as u32;
    let bp = state
        .lock()
        .unwrap()
        .breakpoints
        .remove(&id)
        .ok_or("Breakpoint id not found")?;
    let threads = program_breakpoint(&bp, true)?;
    event(
        state,
        "hardware_breakpoint_clear",
        Some(bp.pid),
        format!("cleared id={id} threads={threads}"),
    );
    Ok(json!({"cleared":true,"id":id,"threads_cleared":threads}))
}

fn get_events(state: &SharedState, args: Value) -> Result<Value, String> {
    let clear = args.get("clear").and_then(Value::as_bool).unwrap_or(false);
    let mut guard = state.lock().unwrap();
    let events = guard.events.clone();
    if clear {
        guard.events.clear();
    }
    Ok(json!({"events":events,"breakpoints":guard.breakpoints.values().collect::<Vec<_>>() }))
}
