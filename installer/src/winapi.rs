// winapi.rs - the few Windows API calls the setup makes itself, in place of helper programs it used
// to start (taskkill, a PowerShell one-liner, Wait-Process, cmd, explorer): a job object that holds
// the steps' process tree (so Cancel stops all of it), waiting for a process, the PATH as the
// registry has it, opening a folder. Plain kernel32/advapi32/shell32 functions, no extra crates.
use std::ffi::{OsStr, c_void};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::io::AsRawHandle;
use std::path::Path;

type Handle = *mut c_void;

#[repr(C)]
#[derive(Default)]
struct BasicLimit {
    per_process_user_time_limit: i64,
    per_job_user_time_limit: i64,
    limit_flags: u32,
    minimum_working_set_size: usize,
    maximum_working_set_size: usize,
    active_process_limit: u32,
    affinity: usize,
    priority_class: u32,
    scheduling_class: u32,
}

#[repr(C)]
#[derive(Default)]
struct IoCounters {
    counts: [u64; 6],
}

#[repr(C)]
#[derive(Default)]
struct ExtendedLimit {
    basic: BasicLimit,
    io: IoCounters,
    process_memory_limit: usize,
    job_memory_limit: usize,
    peak_process_memory_used: usize,
    peak_job_memory_used: usize,
}

const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: i32 = 9;
const JOB_OBJECT_LIMIT_BREAKAWAY_OK: u32 = 0x0000_0800;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x0000_2000;
const SYNCHRONIZE: u32 = 0x0010_0000;
const PROCESS_TERMINATE: u32 = 0x0000_0001;
const HKEY_CURRENT_USER: isize = 0x8000_0001u32 as i32 as isize;
const HKEY_LOCAL_MACHINE: isize = 0x8000_0002u32 as i32 as isize;
const RRF_RT_REG_SZ_OR_EXPAND_SZ: u32 = 0x0000_0002 | 0x0000_0004;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateJobObjectW(attributes: *mut c_void, name: *const u16) -> Handle;
    fn SetInformationJobObject(job: Handle, class: i32, info: *mut c_void, length: u32) -> i32;
    fn AssignProcessToJobObject(job: Handle, process: Handle) -> i32;
    fn TerminateJobObject(job: Handle, exit_code: u32) -> i32;
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> Handle;
    fn TerminateProcess(process: Handle, exit_code: u32) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
    fn CloseHandle(handle: Handle) -> i32;
}

#[link(name = "advapi32")]
unsafe extern "system" {
    fn RegGetValueW(key: isize, sub_key: *const u16, value: *const u16, flags: u32, kind: *mut u32, data: *mut c_void, size: *mut u32) -> i32;
}

#[link(name = "user32")]
unsafe extern "system" {
    fn GetAsyncKeyState(key: i32) -> i16;
}

/// The Shift key held down right now (a launcher's start with Shift held: the setup's window instead of the game).
pub fn shift_held() -> bool {
    const VK_SHIFT: i32 = 0x10;
    unsafe { GetAsyncKeyState(VK_SHIFT) < 0 }
}

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteW(hwnd: *mut c_void, operation: *const u16, file: *const u16, parameters: *const u16, directory: *const u16, show: i32) -> isize;
}

fn wide(s: impl AsRef<OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(std::iter::once(0)).collect()
}

/// A process and everything it starts, in one job object. Cancel (`terminate`) ends the whole tree.
/// With `kill_on_close` the tree also ends when the job is dropped (the step finished, or the setup
/// closed); without it what is left keeps running (the game an update starts again at its end).
pub struct ProcessTree {
    job: Handle, // null: the process could not be put in a job (then only it is ended)
    pid: u32,
}

// (the handle is only used through the job functions, which are thread-safe)
unsafe impl Send for ProcessTree {}
unsafe impl Sync for ProcessTree {}

impl ProcessTree {
    pub fn new(child: &std::process::Child, kill_on_close: bool) -> Self {
        let pid = child.id();
        unsafe {
            let job = CreateJobObjectW(std::ptr::null_mut(), std::ptr::null());
            if job.is_null() {
                return Self { job, pid };
            }
            let mut info = ExtendedLimit::default();
            // (BREAKAWAY_OK: a process that asks to leave the job may: the game starts its updater so)
            info.basic.limit_flags = JOB_OBJECT_LIMIT_BREAKAWAY_OK | if kill_on_close { JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE } else { 0 };
            let ok = SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, (&mut info as *mut ExtendedLimit).cast(),
                                             std::mem::size_of::<ExtendedLimit>() as u32) != 0
                && AssignProcessToJobObject(job, child.as_raw_handle()) != 0;
            if !ok {
                CloseHandle(job);
                return Self { job: std::ptr::null_mut(), pid };
            }
            Self { job, pid }
        }
    }

    /// Ends the process and everything it started (or only the process, when it is not in a job).
    pub fn terminate(&self) {
        unsafe {
            if !self.job.is_null() {
                TerminateJobObject(self.job, 1);
                return;
            }
            let process = OpenProcess(PROCESS_TERMINATE, 0, self.pid);
            if !process.is_null() {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        }
    }
}

impl Drop for ProcessTree {
    fn drop(&mut self) {
        if !self.job.is_null() {
            unsafe { CloseHandle(self.job) };
        }
    }
}

/// Waits up to `timeout_ms` for a process to end (at once when it is not there).
pub fn wait_for_process(pid: u32, timeout_ms: u32) {
    unsafe {
        let process = OpenProcess(SYNCHRONIZE, 0, pid);
        if !process.is_null() {
            WaitForSingleObject(process, timeout_ms);
            CloseHandle(process);
        }
    }
}

/// A REG_SZ / REG_EXPAND_SZ value (environment variables expanded), or None.
fn registry_string(root: isize, key: &str, value: &str) -> Option<String> {
    let (key, value) = (wide(key), wide(value));
    let mut size = 0u32;
    unsafe {
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ_OR_EXPAND_SZ, std::ptr::null_mut(), std::ptr::null_mut(), &mut size) != 0 {
            return None;
        }
        // (an expanded value can be longer than the stored one: room for that)
        let mut buffer = vec![0u16; (size as usize / 2).max(1) + 4096];
        let mut bytes = (buffer.len() * 2) as u32;
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ_OR_EXPAND_SZ, std::ptr::null_mut(), buffer.as_mut_ptr().cast(), &mut bytes) != 0 {
            return None;
        }
        let len = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
        Some(String::from_utf16_lossy(&buffer[..len]))
    }
}

/// PATH as the registry has it now (the machine's, then the user's): programs installed since this
/// process started are found without a restart.
pub fn registry_path() -> String {
    let machine = registry_string(HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment", "Path");
    let user = registry_string(HKEY_CURRENT_USER, "Environment", "Path");
    [machine, user].into_iter().flatten().collect::<Vec<_>>().join(";")
}

/// Opens a folder in Explorer.
pub fn open_folder(folder: &Path) {
    let (op, file) = (wide("open"), wide(folder.as_os_str()));
    unsafe {
        ShellExecuteW(std::ptr::null_mut(), op.as_ptr(), file.as_ptr(), std::ptr::null(), std::ptr::null(), 1);
    }
}
