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
2. For autonomous DLL testing, prefer run_iteration. It establishes an event baseline, launches or adopts the game, retries injection while the game becomes ready, observes broadly for crashes, collects artifacts, checks success signals, and returns a classified result.
3. Use launch_game/inject/wait_for_event separately for reverse engineering or when the iteration requires manual control. Always use absolute DLL paths and the PID returned by the MCP.
4. Configure success.file or success.log_pattern when the task has a concrete success marker. Without one, healthy observation is not proof that the DLL achieved its task.
5. On process_crash, process_exit, debug_dump_ready, crash_report_ready, debug_exception, or debug_supervisor_error, call get_run_report and inspect the referenced dump, text report, game logs, and DLL logs before changing code.
6. For a clean manual iteration, call uninject with the DLL module name/path and its cleanup export when the DLL supports one, then rebuild and inject the next artifact. Never force-unload a DLL without a cleanup export.
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
- Automatic recovery is enabled by default only for processes launched by this MCP. Do not terminate a game that was already running before the iteration unless explicitly instructed.
- A second-chance exception emits process_crash before dump capture. Dump capture is bounded; inspect debug_dump_ready or debug_dump_error, then use the terminal result rather than waiting indefinitely.
- Bugsplat is an artifact source, not a supervisor dependency. Never wait for Bugsplat UI activity or attach the supervisor to Bugsplat.
- A minidump is strongest when paired with the event history, exception address/code, module list, and source/PDB files.
- If no exception event is available, treat the exit event and generated report/log artifacts as the diagnostic result; fail-fast and external crash-reporting paths may not provide a stack.
- After a crash, exception, injection failure, or unexpected behavior, call read_log to read the most recently modified regular file in Scrap Mechanic's Logs directory. The tool discovers the Steam library installation when possible; set SCRAP_MECHANIC_ROOT if discovery needs an explicit override.
- After diagnosing a failure, make one focused code change, rebuild, and repeat the same runtime flow."#;

#[derive(Clone, Debug, Serialize)]
struct Event {
    id: u64,
    timestamp: u64,
    session_id: Option<String>,
    source: String,
    kind: String,
    pid: Option<u32>,
    detail: String,
    report: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
struct RunSession {
    id: String,
    pid: Option<u32>,
    dll: Option<String>,
    manager_owned: bool,
    baseline_event_id: u64,
    phase: String,
    terminal: bool,
    outcome: Option<String>,
    report: Option<String>,
}

#[derive(Default)]
struct State {
    launched_pid: Option<u32>,
    last_seen_running: Option<bool>,
    last_seen_pid: Option<u32>,
    next_event_id: u64,
    events: Vec<Event>,
    sessions: HashMap<String, RunSession>,
    active_session: Option<String>,
    next_session_id: u64,
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
        tool("wait_for_event", "Wait for matching lifecycle or debugger events without client-side polling.", json!({"type":"object","properties":{"kind":{"type":"string"},"kinds":{"type":"array","items":{"type":"string"}},"pid":{"type":"integer"},"after_id":{"type":"integer"},"timeout_ms":{"type":"integer","maximum":300000}}})),
        tool("get_event_cursor", "Return the current event cursor and active run session.", json!({"type":"object","properties":{}})),
        tool("run_iteration", "Run one autonomous DLL runtime iteration with readiness retries, crash detection, artifact collection, success signals, and bounded recovery.", json!({"type":"object","required":["dll"],"properties":{"dll":{"type":"string"},"pid":{"type":"integer"},"keep_graphics":{"type":"boolean"},"args":{"type":"array","items":{"type":"string"}},"readiness_timeout_ms":{"type":"integer"},"observation_timeout_ms":{"type":"integer"},"inject_timeout_ms":{"type":"integer"},"recovery":{"type":"boolean"},"max_attempts":{"type":"integer"},"success":{"type":"object","properties":{"file":{"type":"string"},"log_pattern":{"type":"string"},"timeout_ms":{"type":"integer"}}}}})),
        tool("get_run_report", "Return the latest crash report and diagnostic artifact paths.", json!({"type":"object","properties":{"pid":{"type":"integer"}}})),
        tool("read_log", "Read the most recently modified regular file in Scrap Mechanic's Logs directory. Uses Steam library discovery when possible; set SCRAP_MECHANIC_ROOT to override the installation root.", json!({"type":"object","properties":{"max_bytes":{"type":"integer","minimum":1,"maximum":16777216,"description":"Maximum bytes to return; defaults to 4194304."}}}))
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
            "get_event_cursor" => get_event_cursor(state),
            "run_iteration" => run_iteration(state, args),
            "get_run_report" => get_run_report(state, args),
            "read_log" => read_latest_log(args),
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
    event_from(state, "runtime", kind, pid, detail);
}

fn event_from(
    state: &SharedState,
    source: &str,
    kind: &str,
    pid: Option<u32>,
    detail: impl Into<String>,
) {
    event_for_session(state, source, kind, pid, detail, None);
}

fn event_for_session(
    state: &SharedState,
    source: &str,
    kind: &str,
    pid: Option<u32>,
    detail: impl Into<String>,
    session_id: Option<&str>,
) {
    let detail = detail.into();
    let (event, tx, signal) = {
        let mut guard = state.lock().unwrap();
        guard.next_event_id += 1;
        guard.event_generation = guard.event_generation.wrapping_add(1);
        let event = Event {
            id: guard.next_event_id,
            timestamp: now(),
            session_id: session_id
                .map(str::to_owned)
                .or_else(|| guard.active_session.clone()),
            source: source.into(),
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
    let kind_matches = args
        .get("kind")
        .and_then(Value::as_str)
        .map(|kind| event.kind == kind)
        .unwrap_or(true);
    let kinds_matches = args
        .get("kinds")
        .and_then(Value::as_array)
        .map(|kinds| {
            kinds
                .iter()
                .filter_map(Value::as_str)
                .any(|kind| kind == event.kind)
        })
        .unwrap_or(true);
    if !kind_matches || !kinds_matches {
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

fn get_event_cursor(state: &SharedState) -> Result<Value, String> {
    let guard = state.lock().map_err(|_| "state lock poisoned")?;
    Ok(json!({
        "event_id": guard.next_event_id,
        "active_session": guard.active_session.as_ref().and_then(|id| guard.sessions.get(id)),
        "events_retained": guard.events.len()
    }))
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
fn run_iteration(state: &SharedState, args: Value) -> Result<Value, String> {
    let dll = args
        .get("dll")
        .and_then(Value::as_str)
        .ok_or("dll is required")?;
    let dll_path = PathBuf::from(dll);
    if !dll_path.is_absolute() || !dll_path.is_file() {
        return Err(format!(
            "dll must be an existing absolute path: {}",
            dll_path.display()
        ));
    }
    let recovery = args
        .get("recovery")
        .and_then(Value::as_bool)
        .unwrap_or(true);
    let max_attempts = args
        .get("max_attempts")
        .and_then(Value::as_u64)
        .unwrap_or(1)
        .clamp(1, 10) as usize;
    let observation_timeout = args
        .get("observation_timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(30_000)
        .clamp(100, 300_000);
    let readiness_timeout = args
        .get("readiness_timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(60_000)
        .clamp(1_000, 300_000);
    let inject_timeout = args
        .get("inject_timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(15_000);
    let success = args.get("success").cloned().unwrap_or_else(|| json!({}));
    let mut attempts = Vec::new();

    for attempt in 1..=max_attempts {
        let (session_id, baseline, existing_pid) = {
            let mut guard = state.lock().map_err(|_| "state lock poisoned")?;
            guard.next_session_id += 1;
            let id = format!("run-{}-{}", now(), guard.next_session_id);
            let baseline = guard.next_event_id;
            let existing_pid = args
                .get("pid")
                .and_then(Value::as_u64)
                .map(|value| value as u32)
                .or_else(find_pid);
            guard.sessions.insert(
                id.clone(),
                RunSession {
                    id: id.clone(),
                    pid: existing_pid,
                    dll: Some(dll_path.display().to_string()),
                    manager_owned: existing_pid.is_none(),
                    baseline_event_id: baseline,
                    phase: "created".into(),
                    terminal: false,
                    outcome: None,
                    report: None,
                },
            );
            guard.active_session = Some(id.clone());
            (id, baseline, existing_pid)
        };

        let manager_owned = existing_pid.is_none();
        let pid = if let Some(pid) = existing_pid {
            pid
        } else {
            let mut launch_args = args.clone();
            if let Some(object) = launch_args.as_object_mut() {
                object.remove("dll");
                object.remove("pid");
                object.remove("recovery");
                object.remove("max_attempts");
                object.remove("observation_timeout_ms");
                object.remove("inject_timeout_ms");
                object.remove("success");
            }
            let launched = launch_game(state, launch_args)?;
            launched
                .get("pid")
                .and_then(Value::as_u64)
                .ok_or("launch did not return a PID")? as u32
        };
        update_session(state, &session_id, |session| {
            session.pid = Some(pid);
            session.phase = "launched".into();
            session.manager_owned = manager_owned;
        });
        let _ = game_status(state);

        let inject_args = json!({
            "dll": dll_path,
            "pid": pid,
            "timeout_ms": inject_timeout
        });
        let injection = match inject_with_readiness(
            state,
            inject_args,
            pid,
            dll_path.to_str().unwrap_or_default(),
            readiness_timeout,
        ) {
            Ok(value) => value,
            Err(error) => {
                update_session(state, &session_id, |session| {
                    session.phase = "terminal".into();
                    session.terminal = true;
                    session.outcome = Some("inject_failed".into());
                });
                let report = write_session_report(state, &session_id, "inject_failed");
                update_session(state, &session_id, |session| {
                    session.report = report.clone()
                });
                attempts.push(json!({"attempt":attempt,"session_id":session_id,"outcome":"inject_failed","error":error,"report":report}));
                if recovery && manager_owned && attempt < max_attempts {
                    let _ = stop_game(state, json!({"pid":pid}));
                    thread::sleep(std::time::Duration::from_secs(1));
                    continue;
                }
                return Ok(json!({"outcome":"inject_failed","attempts":attempts}));
            }
        };
        update_session(state, &session_id, |session| {
            session.phase = "observing".into()
        });
        let observation = observe_iteration(state, pid, baseline, observation_timeout, &success);
        let outcome = observation
            .get("outcome")
            .and_then(Value::as_str)
            .unwrap_or("uncertain")
            .to_string();
        let terminal = outcome != "healthy_observation" && outcome != "success";
        update_session(state, &session_id, |session| {
            session.phase = "terminal".into();
            session.terminal = true;
            session.outcome = Some(outcome.clone());
        });
        let report = write_session_report(state, &session_id, &outcome);
        update_session(state, &session_id, |session| {
            session.report = report.clone()
        });
        attempts.push(json!({"attempt":attempt,"session_id":session_id,"outcome":outcome,"injection":injection,"observation":observation,"report":report}));
        if !terminal || !recovery || !manager_owned || attempt == max_attempts {
            return Ok(json!({"outcome":outcome,"attempts":attempts}));
        }
        let _ = stop_game(state, json!({"pid":pid}));
        thread::sleep(std::time::Duration::from_secs(1));
        let _ = game_status(state);
    }
    Ok(json!({"outcome":"recovery_failed","attempts":attempts}))
}

#[cfg(target_os = "windows")]
fn inject_with_readiness(
    state: &SharedState,
    args: Value,
    pid: u32,
    dll: &str,
    timeout_ms: u64,
) -> Result<Value, String> {
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(timeout_ms);
    loop {
        match inject_dll(state, args.clone()) {
            Ok(value) => return Ok(value),
            Err(error) => {
                if module_info(pid, dll).is_some() {
                    return Ok(json!({
                        "injected": true,
                        "pid": pid,
                        "dll": dll,
                        "loader_completed": false,
                        "loader_status": "module already loaded",
                        "module_base": module_info(pid, dll).map(|(base, _)| format!("0x{base:X}"))
                    }));
                }
                if std::time::Instant::now() >= deadline {
                    return Err(format!("{error} (readiness timeout {timeout_ms} ms)"));
                }
            }
        }
        if std::time::Instant::now() >= deadline {
            return Err(format!(
                "game did not become injection-ready (readiness timeout {timeout_ms} ms)"
            ));
        }
        thread::sleep(std::time::Duration::from_millis(250));
    }
}

#[cfg(target_os = "windows")]
fn update_session(state: &SharedState, id: &str, update: impl FnOnce(&mut RunSession)) {
    if let Ok(mut guard) = state.lock() {
        if let Some(session) = guard.sessions.get_mut(id) {
            update(session);
        }
    }
}

#[cfg(target_os = "windows")]
fn observe_iteration(
    state: &SharedState,
    pid: u32,
    baseline: u64,
    timeout_ms: u64,
    success: &Value,
) -> Value {
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(timeout_ms);
    let crash_kinds = json!([
        "process_crash",
        "process_exit",
        "debug_exception",
        "debug_dump_ready",
        "crash_report_ready",
        "debug_supervisor_error"
    ]);
    let mut cursor = baseline;
    loop {
        if success_signal_observed(success) {
            event(
                state,
                "iteration_success",
                Some(pid),
                "configured success signal observed",
            );
            return json!({"outcome":"success","event_id":cursor});
        }
        let remaining = deadline.saturating_duration_since(std::time::Instant::now());
        if remaining.is_zero() {
            return json!({"outcome":if success.as_object().is_some_and(|v| !v.is_empty()) { "success_signal_timeout" } else { "healthy_observation" },"event_id":cursor});
        }
        let wait_ms = remaining.as_millis().min(500) as u64;
        let mut wait_args =
            json!({"pid":pid,"after_id":cursor,"kinds":crash_kinds,"timeout_ms":wait_ms});
        if let Some(object) = wait_args.as_object_mut() {
            object.insert("kinds".into(), crash_kinds.clone());
        }
        if let Ok(result) = wait_for_event(state, wait_args) {
            if result.get("matched").and_then(Value::as_bool) == Some(true) {
                if let Some(id) = result.pointer("/event/id").and_then(Value::as_u64) {
                    cursor = id;
                }
                let kind = result
                    .pointer("/event/kind")
                    .and_then(Value::as_str)
                    .unwrap_or("uncertain");
                if kind == "debug_exception"
                    && result
                        .pointer("/event/detail")
                        .and_then(Value::as_str)
                        .is_some_and(|detail| detail.contains("first_chance=1"))
                {
                    continue;
                }
                let artifact_event = if kind == "process_crash" || kind == "debug_exception" {
                    wait_for_event(
                        state,
                        json!({
                            "pid": pid,
                            "after_id": cursor,
                            "kinds": ["debug_dump_ready", "debug_dump_error", "crash_report_ready"],
                            "timeout_ms": 10_000
                        }),
                    )
                    .ok()
                } else {
                    None
                };
                let outcome = if kind == "process_exit" {
                    "process_exit"
                } else if kind == "debug_supervisor_error" {
                    "debugger_attach_failed"
                } else {
                    "process_crash"
                };
                return json!({"outcome":outcome,"event":result.get("event"),"artifact_event":artifact_event,"event_id":cursor});
            }
        }
        if find_pid() != Some(pid) {
            let _ = game_status(state);
        }
    }
}

#[cfg(target_os = "windows")]
fn success_signal_observed(success: &Value) -> bool {
    let Some(object) = success.as_object() else {
        return false;
    };
    if let Some(path) = object.get("file").and_then(Value::as_str) {
        if PathBuf::from(path).is_file() {
            return true;
        }
    }
    if let Some(pattern) = object.get("log_pattern").and_then(Value::as_str) {
        if let Some(log) = latest_game_log() {
            if std::fs::read_to_string(log)
                .map(|text| text.contains(pattern))
                .unwrap_or(false)
            {
                return true;
            }
        }
    }
    false
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
fn latest_game_log() -> Option<PathBuf> {
    let logs = game_root().join("Logs");
    std::fs::read_dir(logs)
        .ok()?
        .filter_map(|entry| entry.ok().map(|value| value.path()))
        .filter(|path| path.is_file())
        .max_by_key(|path| {
            std::fs::metadata(path)
                .and_then(|meta| meta.modified())
                .ok()
        })
}

#[cfg(target_os = "windows")]
fn read_latest_log(args: Value) -> Result<Value, String> {
    use std::io::Read;

    let max_bytes = args
        .get("max_bytes")
        .and_then(Value::as_u64)
        .unwrap_or(4 * 1024 * 1024);
    if !(1..=16 * 1024 * 1024).contains(&max_bytes) {
        return Err("max_bytes must be between 1 and 16777216".into());
    }
    let logs_directory = game_root().join("Logs");
    let path = latest_game_log()
        .ok_or_else(|| format!("No regular files found in {}", logs_directory.display()))?;
    let mut file = std::fs::File::open(&path)
        .map_err(|error| format!("Failed to open {}: {error}", path.display()))?;
    let mut bytes = Vec::with_capacity(max_bytes as usize + 1);
    file.by_ref()
        .take(max_bytes + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| format!("Failed to read {}: {error}", path.display()))?;
    let truncated = bytes.len() > max_bytes as usize;
    bytes.truncate(max_bytes as usize);
    Ok(json!({
        "path": path,
        "logs_directory": logs_directory,
        "bytes": bytes.len(),
        "truncated": truncated,
        "content": String::from_utf8_lossy(&bytes)
    }))
}

#[cfg(target_os = "windows")]
fn write_session_report(state: &SharedState, session_id: &str, outcome: &str) -> Option<String> {
    let session = state.lock().ok()?.sessions.get(session_id).cloned()?;
    let events = state
        .lock()
        .ok()?
        .events
        .iter()
        .filter(|event| event.session_id.as_deref() == Some(session_id))
        .cloned()
        .collect::<Vec<_>>();
    let root = report_root().join(session_id);
    std::fs::create_dir_all(&root).ok()?;
    let pid = session.pid.unwrap_or(0);
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
                    .then(|| entry.path().display().to_string())
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let report = json!({
        "session": session,
        "outcome": outcome,
        "captured_at": now(),
        "events": events,
        "game_log": latest_game_log(),
        "crash_artifacts": temp_artifacts,
        "note": "This report combines MCP lifecycle/debugger evidence with artifacts emitted by the game or injected DLL."
    });
    let path = root.join("report.json");
    std::fs::write(&path, serde_json::to_vec_pretty(&report).ok()?).ok()?;
    Some(path.display().to_string())
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
fn write_debug_minidump_bounded(
    pid: u32,
    exception_code: u32,
    exception_address: usize,
    timeout: std::time::Duration,
) -> Result<Option<PathBuf>, String> {
    let (sender, receiver) = std::sync::mpsc::channel();
    thread::spawn(move || {
        let _ = sender.send(write_debug_minidump(pid, exception_code, exception_address));
    });
    receiver
        .recv_timeout(timeout)
        .map_err(|_| format!("minidump timed out after {} ms", timeout.as_millis()))
}

#[cfg(target_os = "windows")]
fn debug_supervisor(state: SharedState, pid: u32, session_id: Option<String>) {
    use windows::Win32::Foundation::{DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED};
    use windows::Win32::System::Diagnostics::Debug::{
        ContinueDebugEvent, DEBUG_EVENT, DebugActiveProcess, EXCEPTION_DEBUG_EVENT,
        EXIT_PROCESS_DEBUG_EVENT, WaitForDebugEvent,
    };
    unsafe {
        if let Err(error) = DebugActiveProcess(pid) {
            event_for_session(
                &state,
                "debug_supervisor",
                "debug_supervisor_error",
                Some(pid),
                format!("DebugActiveProcess failed: {error}"),
                session_id.as_deref(),
            );
            return;
        }
        event_for_session(
            &state,
            "debug_supervisor",
            "debug_supervisor_attached",
            Some(pid),
            "external debugger attached",
            session_id.as_deref(),
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
                event_for_session(
                    &state,
                    "debug_supervisor",
                    "debug_exception",
                    Some(pid),
                    format!(
                        "code=0x{code:08X} address=0x{address:X} first_chance={}",
                        info.dwFirstChance
                    ),
                    session_id.as_deref(),
                );
                if info.dwFirstChance == 0 {
                    event_for_session(
                        &state,
                        "debug_supervisor",
                        "process_crash",
                        Some(pid),
                        format!("second-chance exception code=0x{code:08X} address=0x{address:X}"),
                        session_id.as_deref(),
                    );
                    match write_debug_minidump_bounded(
                        pid,
                        code,
                        address,
                        std::time::Duration::from_secs(10),
                    ) {
                        Ok(Some(path)) => event_for_session(
                            &state,
                            "debug_supervisor",
                            "debug_dump_ready",
                            Some(pid),
                            path.display().to_string(),
                            session_id.as_deref(),
                        ),
                        Ok(None) => event_for_session(
                            &state,
                            "debug_supervisor",
                            "debug_dump_error",
                            Some(pid),
                            "MiniDumpWriteDump failed",
                            session_id.as_deref(),
                        ),
                        Err(error) => event_for_session(
                            &state,
                            "debug_supervisor",
                            "debug_dump_error",
                            Some(pid),
                            error,
                            session_id.as_deref(),
                        ),
                    }
                    continue_status = DBG_EXCEPTION_NOT_HANDLED;
                }
            } else if debug_event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT {
                event_for_session(
                    &state,
                    "debug_supervisor",
                    "debug_process_exit",
                    Some(pid),
                    "debug supervisor observed process exit",
                    session_id.as_deref(),
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
    let guard = state.lock().map_err(|_| "state lock poisoned")?;
    let sessions = guard
        .sessions
        .values()
        .filter(|session| pid.is_none() || session.pid == pid)
        .cloned()
        .collect::<Vec<_>>();
    let events = guard.events.clone();
    let selected = events.iter().rev().find(|e| pid.is_none() || e.pid == pid);
    Ok(json!({"latest_event":selected,"sessions":sessions,"reports_root":report_root()}))
}

#[cfg(target_os = "windows")]
fn game_root() -> PathBuf {
    if let Some(root) = std::env::var_os("SCRAP_MECHANIC_ROOT") {
        let root = PathBuf::from(root);
        if root.is_dir() {
            return root;
        }
    }

    let mut steam_roots = Vec::new();
    for variable in ["PROGRAMFILES(X86)", "PROGRAMFILES"] {
        if let Some(program_files) = std::env::var_os(variable) {
            steam_roots.push(PathBuf::from(program_files).join("Steam"));
        }
    }
    if let Some(local_app_data) = std::env::var_os("LOCALAPPDATA") {
        steam_roots.push(PathBuf::from(local_app_data).join("Steam"));
    }

    let mut libraries = Vec::new();
    for steam in &steam_roots {
        let library_file = steam.join("steamapps").join("libraryfolders.vdf");
        if let Ok(contents) = std::fs::read_to_string(library_file) {
            for line in contents.lines().filter(|line| line.contains("\"path\"")) {
                if let Some(value) = line.split('"').nth(3) {
                    libraries.push(PathBuf::from(value.replace("\\\\", "\\")));
                }
            }
        }
    }
    libraries.extend(steam_roots);
    libraries
        .into_iter()
        .map(|library| {
            library
                .join("steamapps")
                .join("common")
                .join("Scrap Mechanic")
        })
        .find(|path| path.join("Release").join(EXE_NAME).is_file() || path.join("Logs").is_dir())
        .unwrap_or_else(|| PathBuf::from(DEFAULT_ROOT))
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
        let (start_supervisor, session_id) = {
            let mut guard = state.lock().unwrap();
            let start = guard.supervised.insert(pid);
            (start, guard.active_session.clone())
        };
        if start_supervisor {
            let supervisor_state = state.clone();
            thread::spawn(move || debug_supervisor(supervisor_state, pid, session_id));
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

#[cfg(test)]
mod tests {
    use super::*;

    fn event(kind: &str, id: u64) -> Event {
        Event {
            id,
            timestamp: 0,
            session_id: Some("run-test".into()),
            source: "test".into(),
            kind: kind.into(),
            pid: Some(42),
            detail: String::new(),
            report: None,
        }
    }

    #[test]
    fn event_kind_array_matches_crash_family() {
        let value = json!({"kinds":["process_crash","debug_dump_ready"],"after_id":2,"pid":42});
        assert!(event_matches(&event("debug_dump_ready", 3), &value));
        assert!(!event_matches(&event("process_start", 3), &value));
        assert!(!event_matches(&event("debug_dump_ready", 2), &value));
    }

    #[test]
    fn singular_kind_remains_compatible() {
        let value = json!({"kind":"process_exit","pid":42});
        assert!(event_matches(&event("process_exit", 1), &value));
        assert!(!event_matches(&event("process_crash", 1), &value));
    }
}
