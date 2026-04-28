# PROCMON — Windows Real-Time Process Monitoring Dashboard
### C++17 | Win32 API | PDH | PSAPI | ANSI/VT Console

---

## Requirements

- **Windows 10 version 1607+** or **Windows 11** (Virtual Terminal support)
- **Compiler:** MSVC (Visual Studio 2019/2022) **or** MinGW/MSYS2 g++

---

## Build

### Option A — MSVC (Visual Studio)

Open a **"Developer Command Prompt for VS"**, then:

```bat
build_msvc.bat
```

Or manually:

```bat
cl /std:c++17 /O2 /EHsc procmon.cpp /Fe:procmon.exe ^
   /link pdh.lib psapi.lib ntdll.lib user32.lib kernel32.lib
```

### Option B — MinGW / MSYS2

Install toolchain first (MSYS2 terminal):

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
```

Then in CMD or PowerShell (with MSYS2 ucrt64 bin in PATH):

```bat
build_mingw.bat
```

Or manually:

```bat
g++ -std=c++17 -O2 -o procmon.exe procmon.cpp ^
    -lpdh -lpsapi -lntdll -luser32 -lkernel32
```

---

## Run

```bat
procmon.exe
```

> **Tip:** Run from Windows Terminal for best color rendering.  
> Right-click → "Run as Administrator" for full access to system processes.

---

## Dashboard Layout

```
┌─────────────────────────────────────────────────────────────────────────┐
│ ◈ PROCMON  Windows Process Dashboard        uptime: 02h 14m │ 247 procs │
│ CPU  ████████████░░░░░░░░░░░░░░░  38.4%   8 logical CPUs                │
│ MEM  ███████░░░░░░░░░░░░░░░░░░░░  29.1%   used 4.7GB / 15.9GB           │
│ PGF  ██░░░░░░░░░░░░░░░░░░░░░░░░░   5.2%   used 1.2GB / 8.0GB            │
│ Procs: 247   Threads: 3142   Handles: 91204   Filter: none               │
├─────────────────────────────────────────────────────────────────────────┤
│    PID  NAME              CPU%   WORKING-SET   PRIVATE   HANDLES  THR   │
│   1842  chrome.exe        48.2      341.0MB   280.0MB     3210   42    │
│   1234  svchost.exe        3.4       56.0MB    32.0MB      890   18    │
│      4  System             0.1       10.2MB     0.1MB      210   180   │
└─────────────────────────────────────────────────────────────────────────┘
  [c]pu [m]em [p]id [n]ame [h]andles  [/]filter  [Del]kill  [q]quit
```

---

## Keybindings

| Key         | Action                                    |
|-------------|-------------------------------------------|
| `c`         | Sort by CPU% (default)                    |
| `m`         | Sort by Working Set memory                |
| `p`         | Sort by PID                               |
| `n`         | Sort by process name                      |
| `h`         | Sort by handle count                      |
| `↑` / `↓`  | Move selection up/down                    |
| `PgUp/PgDn` | Scroll 10 rows at a time                  |
| `Home`      | Jump to top                               |
| `End`       | Jump to bottom                            |
| `/`         | Start name filter (case-insensitive)      |
| `Enter/Esc` | Confirm/cancel filter                     |
| `Del`       | Kill selected process (TerminateProcess)  |
| `Space`     | Pause / Resume refresh                    |
| `Q` / `Esc`| Quit                                      |

---

## Metrics Explained

| Column        | Source API                              |
|---------------|-----------------------------------------|
| CPU%          | `GetProcessTimes` (delta kernel+user)   |
| Working Set   | `GetProcessMemoryInfo` → WorkingSetSize |
| Private       | `GetProcessMemoryInfo` → PrivateUsage   |
| Handles       | `GetProcessHandleCount`                 |
| Threads       | `CreateToolhelp32Snapshot` PROCESSENTRY |
| Priority      | `GetPriorityClass`                      |
| System CPU%   | PDH `\Processor(_Total)\% Processor Time` |
| System RAM    | `GlobalMemoryStatusEx`                  |
| Page File     | `GlobalMemoryStatusEx` (PageFile fields)|

---

## Color Coding

**Gauges:**
- Green  `< 50%` — healthy
- Yellow `50–75%` — moderate
- Orange `75–90%` — high
- Red    `> 90%` — critical

**Per-process CPU:**
- Dark  `< 2%` — idle
- Green `2–20%` — light load
- Yellow `20–50%` — moderate
- Red   `> 50%` — heavy

**Working Set:**
- Grey  `< 10 MB`
- Purple `10–200 MB`
- Yellow `200 MB – 1 GB`
- Red   `> 1 GB`

---

## Architecture

```
main()
  ├─ initPDH()               ← Open PDH query for system CPU%
  ├─ dataThread()            ← Background, refreshes every 1.5s
  │    ├─ readSysInfo()      ← PDH + GlobalMemoryStatusEx
  │    └─ readAllProcesses() ← CreateToolhelp32Snapshot
  │         ├─ GetProcessTimes()        → CPU% delta
  │         ├─ GetProcessMemoryInfo()   → WS + Private
  │         ├─ GetProcessHandleCount()  → Handles
  │         └─ GetPriorityClass()       → Priority
  │
  └─ Main loop (120ms tick)
       ├─ handleInput()   ← ReadConsoleInputW (non-blocking)
       └─ render()        ← Build frame string, single fwrite()
```

---

## Notes

- **First render** shows 0% CPU for all processes. Values populate after
  the first 1.5-second sample interval.
- **System processes** (PID 0, 4) may show 0 working set — normal.
- **Access denied** processes are shown dimmed; they cannot be killed
  without elevation. Run as Administrator for full control.
- The single `fwrite()` per frame minimises console flicker.
