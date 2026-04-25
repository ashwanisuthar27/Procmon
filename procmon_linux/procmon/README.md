# PROCMON — Real-Time Process Monitoring Dashboard
### C++17 | Linux | Zero dependencies | ANSI Terminal

---

## Screenshot (ASCII preview)

```
╔══════════════════════════════════════════════════════════════════════════════╗
║ ◈ PROCMON  Real-Time Process Monitor              uptime: 02h 14m 33s │ 247 procs ║
║ CPU  ████████████████░░░░░░░░░░░░░░░░  38.4%  8 cores  load: 1.42 0.98 0.85      ║
║ MEM  ████████████░░░░░░░░░░░░░░░░░░░░  29.1%  used: 4.7G / 15.9G  avail: 11.2G  ║
║ SWP  ██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░   5.2%  used: 213M / 4.0G                 ║
║ CORES  38%│ 42%│ 29%│ 55%│ 18%│ 22%│ 46%│ 31%│                                   ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  Tasks: 183  running: 3  filter: none                                       ║
║    PID  USER       ST   CPU%   MEM%     RSS      VSZ  THR  NI  COMMAND      ║
║   1842  user       RUN  48.2    2.1    341M    1.2G    12  +0  /usr/bin/...  ║
║   1234  root       SLP   3.4    0.8     56M    412M     4  +0  /sbin/...     ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

---

## Build

```bash
make
# or manually:
g++ -std=c++17 -O2 -pthread -o procmon procmon.cpp
```

**Requirements:** GCC 8+, Linux, any modern terminal (256-color or true-color).  
No external libraries needed — reads directly from `/proc`.

---

## Run

```bash
./procmon
```

---

## Keybindings

| Key         | Action                                      |
|-------------|---------------------------------------------|
| `c`         | Sort by CPU% (default)                      |
| `m`         | Sort by MEM%                                |
| `p`         | Sort by PID                                 |
| `n`         | Sort by name                                |
| `r`         | Sort by RSS memory                          |
| `/`         | Enter filter mode (type substring, Enter to confirm, ESC to clear) |
| `j` / `↓`  | Scroll down                                 |
| `k` / `↑`  | Scroll up                                   |
| `g`         | Jump to top                                 |
| `G`         | Jump to bottom                              |
| `K`         | Toggle kernel thread visibility             |
| `Space`     | Pause / Resume refresh                      |
| `q`         | Quit                                        |

---

## Features

- **Live system gauges** — CPU%, memory, swap, per-core utilisation bars
- **Process table** with: PID · USER · STATE · CPU% · MEM% · RSS · VSZ · THREADS · NICE · COMMAND
- **Multi-sort** — CPU, MEM, PID, NAME, RSS
- **Live filter** — substring search across process name + cmdline
- **Kernel thread toggle** — hide/show `[kworker]` etc.
- **Color coding**:
  - Process state: green=RUN, blue=SLP, yellow=DSK, red=ZOM, purple=STP
  - CPU/MEM values: green → yellow → orange → red by load
  - Bar gradients match threshold levels
- **Scrollbar** — proportional position indicator
- **Pause mode** — freeze display without stopping data
- **Zero dependencies** — pure `/proc` filesystem + ANSI escape codes

---

## Architecture

```
main()
  ├─ dataThread()          ← background thread, refreshes every 1.5s
  │    ├─ readSysInfo()    ← /proc/stat, /proc/meminfo, /proc/loadavg
  │    └─ readAllProcesses() ← /proc/[pid]/stat + /proc/[pid]/cmdline
  │
  └─ render loop (150ms)
       ├─ handleInput()    ← raw-mode keyboard, non-blocking
       └─ render()
            ├─ renderHeader()      ← CPU/MEM/SWAP/per-core bars
            ├─ renderTableHeader() ← column headers with sort indicator
            ├─ renderProcessRow()  ← per-process colored row
            └─ renderFooter()      ← keybind hints / filter input / status
```

---

## Notes

- CPU% per process is calculated as delta ticks / delta wall-clock seconds,
  so it correctly reflects multi-core usage (can exceed 100% on multi-core).
- First render shows 0% CPU for all processes; values populate after the
  first 1.5-second sample interval.
- Tested on Ubuntu 22.04 / 24.04, Debian 12, Arch Linux.
