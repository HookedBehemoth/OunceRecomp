//! What a game run asks of the system, per system: the machine's and a process's memory, a child process tied to
//! ounce's life, and its stop. Linux: /proc, PR_SET_PDEATHSIG and SIGTERM. Windows: GlobalMemoryStatusEx,
//! GetProcessMemoryInfo, a job object that kills its processes when ounce's handle to it closes, and TerminateProcess
//! (a Windows process has no SIGTERM to end it cleanly with).

use std::process::{Child, Command};

/// (the memory available, the shared memory) in MB; 0 when not known (Windows: no shared memory figure, 0)
pub fn meminfo() -> (u64, u64) {
    #[cfg(unix)]
    {
        let text = std::fs::read_to_string("/proc/meminfo").unwrap_or_default();
        let field = |k: &str| text.lines().find_map(|l| l.strip_prefix(k)).and_then(|r| r.split_whitespace().next()?.parse::<u64>().ok()).unwrap_or(0) >> 10;
        (field("MemAvailable:"), field("Shmem:"))
    }
    #[cfg(windows)]
    {
        let (_, avail) = mem_total_avail();
        (avail, 0)
    }
}

/// The machine's memory: (installed, available) in MB; 0 when not known
pub fn mem_total_avail() -> (u64, u64) {
    #[cfg(unix)]
    {
        let text = std::fs::read_to_string("/proc/meminfo").unwrap_or_default();
        let field = |k: &str| text.lines().find_map(|l| l.strip_prefix(k)).and_then(|r| r.split_whitespace().next()?.parse::<u64>().ok()).unwrap_or(0) >> 10;
        (field("MemTotal:"), field("MemAvailable:"))
    }
    #[cfg(windows)]
    {
        use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
        // SAFETY: an out struct with its length set
        let mut m: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
        m.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
        if unsafe { GlobalMemoryStatusEx(&mut m) } == 0 {
            return (0, 0);
        }
        (m.ullTotalPhys >> 20, m.ullAvailPhys >> 20)
    }
}

/// A process's resident memory in MB (Windows: its working set); 0 when it is gone or not known
pub fn rss_mb(pid: u32) -> u64 {
    #[cfg(unix)]
    {
        let text = std::fs::read_to_string(format!("/proc/{pid}/status")).unwrap_or_default();
        text.lines().find_map(|l| l.strip_prefix("VmRSS:")).and_then(|r| r.split_whitespace().next()?.parse::<u64>().ok()).unwrap_or(0) >> 10
    }
    #[cfg(windows)]
    {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
        use windows_sys::Win32::System::Threading::{OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION};
        // SAFETY: a handle opened here and closed below, an out struct with its size
        unsafe {
            let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if h.is_null() {
                return 0;
            }
            let mut c: PROCESS_MEMORY_COUNTERS = std::mem::zeroed();
            c.cb = std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32;
            let ok = GetProcessMemoryInfo(h, &mut c, c.cb) != 0;
            CloseHandle(h);
            if ok { c.WorkingSetSize as u64 >> 20 } else { 0 }
        }
    }
}

/// Ties the child `cmd` will start to ounce: killed or interrupted, ounce takes the child with it (a game would
/// otherwise go on holding the GPU). Call before spawning, and keep `Tie::adopt`'s value while the child runs.
pub fn tie_to_parent(cmd: &mut Command) {
    #[cfg(target_os = "linux")]
    {
        use std::os::unix::process::CommandExt;
        // SAFETY: prctl is async-signal-safe
        unsafe {
            cmd.pre_exec(|| {
                libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL);
                Ok(())
            });
        }
    }
    #[cfg(not(target_os = "linux"))]
    let _ = cmd;
}

/// What keeps a child tied to ounce once it runs (Windows: the job object, whose last handle closing kills it; Linux:
/// nothing, tie_to_parent did it)
pub struct Tie {
    #[cfg(windows)]
    job: windows_sys::Win32::Foundation::HANDLE,
}

impl Tie {
    pub fn adopt(child: &Child) -> Tie {
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawHandle;
            use windows_sys::Win32::System::JobObjects::*;
            // SAFETY: a job made here, its limits struct, the child's own handle
            unsafe {
                let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
                if !job.is_null() {
                    let mut l: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                    l.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &l as *const _ as *const _, std::mem::size_of_val(&l) as u32);
                    AssignProcessToJobObject(job, child.as_raw_handle() as _);
                }
                Tie { job }
            }
        }
        #[cfg(not(windows))]
        {
            let _ = child;
            Tie {}
        }
    }
}

#[cfg(windows)]
impl Drop for Tie {
    fn drop(&mut self) {
        if !self.job.is_null() {
            // SAFETY: the job's handle, made in adopt (closing it ends what is left in it)
            unsafe { windows_sys::Win32::Foundation::CloseHandle(self.job) };
        }
    }
}

/// Asks the child to stop (SIGTERM). Windows has no such request: the child is ended there (TerminateProcess)
pub fn stop(child: &mut Child) {
    #[cfg(unix)]
    // SAFETY: a signal to our own child
    unsafe {
        libc::kill(child.id() as i32, libc::SIGTERM);
    }
    #[cfg(windows)]
    let _ = child.kill();
}

/// Whether a process with this id is alive
pub fn alive(pid: u32) -> bool {
    #[cfg(unix)]
    {
        // SAFETY: signal 0 only checks
        unsafe { libc::kill(pid as i32, 0) == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM) }
    }
    #[cfg(windows)]
    {
        use windows_sys::Win32::Foundation::{CloseHandle, STILL_ACTIVE};
        use windows_sys::Win32::System::Threading::{GetExitCodeProcess, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION};
        // SAFETY: a handle opened here and closed below
        unsafe {
            let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if h.is_null() {
                return false;
            }
            let mut code = 0u32;
            let ok = GetExitCodeProcess(h, &mut code) != 0;
            CloseHandle(h);
            ok && code == STILL_ACTIVE as u32
        }
    }
}
