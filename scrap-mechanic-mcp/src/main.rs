#![cfg_attr(not(target_os = "windows"), allow(dead_code))]

use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::HashMap,
    io,
    path::PathBuf,
    sync::{Arc, Mutex},
    thread,
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

const APP_ID: &str = "387990";
const EXE_NAME: &str = "ScrapMechanic.exe";
const DEFAULT_ROOT: &str = r"C:\Program Files (x86)\Steam\steamapps\common\Scrap Mechanic";

#[derive(Clone, Debug, Serialize)]
struct Event {
    timestamp: u64,
    kind: String,
    pid: Option<u32>,
    detail: String,
}

#[derive(Default)]
struct State {
    write_authorized: bool,
    launched_pid: Option<u32>,
    last_seen_running: Option<bool>,
    events: Vec<Event>,
    breakpoints: HashMap<u32, Breakpoint>,
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
    let state: SharedState = Arc::new(Mutex::new(State::default()));
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
    let mut stdin = tokio::io::stdin();
    let mut stdout = tokio::io::stdout();
    loop {
        let Some(body) = read_message(&mut stdin).await? else {
            break;
        };
        let request: RpcRequest = match serde_json::from_slice(&body) {
            Ok(value) => value,
            Err(error) => {
                write_response(&mut stdout, json!({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":error.to_string()}})).await?;
                continue;
            }
        };
        let Some(id) = request.id.clone() else {
            continue;
        };
        let response = match dispatch(
            &state,
            &request.method,
            request.params.unwrap_or_else(|| json!({})),
        )
        .await
        {
            Ok(result) => json!({"jsonrpc":"2.0","id":id,"result":result}),
            Err(error) => json!({"jsonrpc":"2.0","id":id,"error":{"code":-32000,"message":error}}),
        };
        write_response(&mut stdout, response).await?;
    }
    Ok(())
}

async fn read_message<R: AsyncReadExt + Unpin>(reader: &mut R) -> io::Result<Option<Vec<u8>>> {
    let mut headers = Vec::new();
    let mut byte = [0u8; 1];
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
    let body = serde_json::to_vec(&value).unwrap();
    writer
        .write_all(format!("Content-Length: {}\r\n\r\n", body.len()).as_bytes())
        .await?;
    writer.write_all(&body).await?;
    writer.flush().await
}

async fn dispatch(state: &SharedState, method: &str, params: Value) -> Result<Value, String> {
    match method {
        "initialize" => Ok(
            json!({"protocolVersion":"2025-06-18","capabilities":{"tools":{}},"serverInfo":{"name":"scrap-mechanic-mcp","version":"0.1.0"}}),
        ),
        "notifications/initialized" => Ok(json!({})),
        "tools/list" => Ok(tool_list()),
        "tools/call" => {
            let name = params
                .get("name")
                .and_then(Value::as_str)
                .ok_or("tools/call requires name")?;
            let args = params
                .get("arguments")
                .cloned()
                .unwrap_or_else(|| json!({}));
            call_tool(state, name, args)
        }
        _ => Err(format!("Unsupported method: {method}")),
    }
}

fn tool_list() -> Value {
    let tool = |name: &str, description: &str, schema: Value| json!({"name":name,"description":description,"inputSchema":schema});
    json!({"tools":[
        tool("game_status", "Find ScrapMechanic.exe, report running/crashed state, PID, and manager events.", json!({"type":"object","properties":{}})),
        tool("launch_game", "Launch Scrap Mechanic directly. Refuses to launch a second copy; uses -use_null_driver unless keep_graphics is true.", json!({"type":"object","properties":{"keep_graphics":{"type":"boolean"},"args":{"type":"array","items":{"type":"string"}}}})),
        tool("stop_game", "Stop the process previously launched or explicitly selected by PID.", json!({"type":"object","properties":{"pid":{"type":"integer"}}})),
        tool("read_memory", "Read bytes from the Scrap Mechanic process. Read-only.", json!({"type":"object","required":["address","length"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"length":{"type":"integer","maximum":1048576}}})),
        tool("authorize_memory_writes", "One-time session authorization for process memory writes. Call with confirmed=true only after the user explicitly approves.", json!({"type":"object","required":["confirmed"],"properties":{"confirmed":{"type":"boolean"}}})),
        tool("write_memory", "Write process memory after one-time session authorization. Requires explicit authorization first.", json!({"type":"object","required":["address","bytes"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"bytes":{"type":"string","description":"Hex bytes, e.g. 90 90"}}})),
        tool("set_hardware_breakpoint", "Set a Windows hardware breakpoint on the target process and record debugger hits.", json!({"type":"object","required":["address"],"properties":{"pid":{"type":"integer"},"address":{"type":"integer"},"access":{"type":"string","enum":["execute","write","readwrite"]},"length":{"type":"integer","enum":[1,2,4,8]}}})),
        tool("clear_hardware_breakpoint", "Remove a previously configured hardware breakpoint.", json!({"type":"object","required":["id"],"properties":{"id":{"type":"integer"}}})),
        tool("get_debug_events", "Return process lifecycle and hardware-breakpoint hit events.", json!({"type":"object","properties":{"clear":{"type":"boolean"}}}))
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
            "stop_game" => stop_game(state, args),
            "read_memory" => read_memory(args),
            "authorize_memory_writes" => authorize_writes(state, args),
            "write_memory" => write_memory(state, args),
            "set_hardware_breakpoint" => set_breakpoint(state, args),
            "clear_hardware_breakpoint" => clear_breakpoint(state, args),
            "get_debug_events" => get_events(state, args),
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
    state.lock().unwrap().events.push(Event {
        timestamp: now(),
        kind: kind.into(),
        pid,
        detail: detail.into(),
    });
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
    let previous = state.lock().unwrap().last_seen_running;
    if previous == Some(true) && !running {
        let launched = state.lock().unwrap().launched_pid;
        let code = launched.and_then(exit_code);
        let kind = if code.unwrap_or(0) == 0 {
            "process_exit"
        } else {
            "process_crash"
        };
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
    }
    if previous == Some(false) && running {
        event(state, "process_start", pid, "ScrapMechanic.exe detected");
    }
    state.lock().unwrap().last_seen_running = Some(running);
    Ok(
        json!({"running":running,"pid":pid,"write_authorized":state.lock().unwrap().write_authorized,"root":game_root()}),
    )
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

fn authorize_writes(state: &SharedState, args: Value) -> Result<Value, String> {
    if !args
        .get("confirmed")
        .and_then(Value::as_bool)
        .unwrap_or(false)
    {
        return Err("Memory writes remain disabled. Call with confirmed=true only after explicit user approval.".into());
    }
    state.lock().unwrap().write_authorized = true;
    Ok(json!({"authorized":true,"scope":"this MCP session until process restart"}))
}

#[cfg(target_os = "windows")]
fn write_memory(state: &SharedState, args: Value) -> Result<Value, String> {
    use windows::Win32::System::Diagnostics::Debug::WriteProcessMemory;
    if !state.lock().unwrap().write_authorized {
        return Err("Memory writes are disabled. Obtain one-time user approval with authorize_memory_writes first.".into());
    }
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
