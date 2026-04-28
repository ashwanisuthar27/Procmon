/**
 * ============================================================
 *  PROCMON.EXE  -  Real-Time Process Monitoring Dashboard
 *  Windows 10/11 | C++17 | Win32 + PDH + PSAPI
 * ============================================================
 *
 *  Build (MSVC):
 *      cl /std:c++17 /O2 /EHsc procmon.cpp
 *         /link pdh.lib psapi.lib ntdll.lib user32.lib kernel32.lib
 *
 *  Build (MinGW / MSYS2):
 *      g++ -std=c++17 -O2 -o procmon.exe procmon.cpp
 *          -lpdh -lpsapi -lntdll -luser32 -lkernel32
 *
 *  Features:
 *   - Live CPU / RAM / Virtual-memory system gauges
 *   - Per-process table: PID, name, CPU%, MEM(MB), handles, threads, priority
 *   - Sort: c=CPU  m=MEM  p=PID  n=NAME  h=HANDLES
 *   - Kill selected process            (Del key)
 *   - Live name filter                 (/ key)
 *   - Scroll                           (Up/Down arrows, PgUp/PgDn)
 *   - Pause / Resume                   (Space)
 *   - Quit                             (Q or Esc)
 */

// ── Windows headers (order matters) ─────────────────────────────────────────
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <winternl.h>   // NtQuerySystemInformation prototype
#include <shlwapi.h>

// ── C++ standard headers ─────────────────────────────────────────────────────
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cassert>

// ── Lib pragmas (MSVC convenience) ───────────────────────────────────────────
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")

// ── ANSI / VT helpers ────────────────────────────────────────────────────────
#define ESC "\033["
#define RESET       ESC "0m"
#define BOLD        ESC "1m"
#define DIM         ESC "2m"
#define ITALIC      ESC "3m"

static inline std::string fgRGB(int r,int g,int b){
    char buf[32];
    snprintf(buf,sizeof(buf),"\033[38;2;%d;%d;%dm",r,g,b);
    return buf;
}
static inline std::string bgRGB(int r,int g,int b){
    char buf[32];
    snprintf(buf,sizeof(buf),"\033[48;2;%d;%d;%dm",r,g,b);
    return buf;
}
static inline std::string moveTo(int row,int col){
    char buf[24];
    snprintf(buf,sizeof(buf),"\033[%d;%dH",row,col);
    return buf;
}
static inline void clearScreen(){ printf("\033[2J\033[H"); fflush(stdout); }
static inline void hideCursor()  { printf("\033[?25l"); fflush(stdout); }
static inline void showCursor()  { printf("\033[?25h"); fflush(stdout); }

// ── Console VT enable ────────────────────────────────────────────────────────
static HANDLE g_hStdOut = INVALID_HANDLE_VALUE;
static HANDLE g_hStdIn  = INVALID_HANDLE_VALUE;
static DWORD  g_origOutMode = 0;
static DWORD  g_origInMode  = 0;

bool enableVT(){
    g_hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hStdIn  = GetStdHandle(STD_INPUT_HANDLE);
    if(g_hStdOut==INVALID_HANDLE_VALUE||g_hStdIn==INVALID_HANDLE_VALUE) return false;

    GetConsoleMode(g_hStdOut, &g_origOutMode);
    GetConsoleMode(g_hStdIn,  &g_origInMode);

    DWORD outMode = g_origOutMode
                  | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                  | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(g_hStdOut, outMode);

    DWORD inMode = (g_origInMode & ~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT))
                 | ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(g_hStdIn, inMode);
    return true;
}
void restoreConsole(){
    if(g_hStdOut!=INVALID_HANDLE_VALUE) SetConsoleMode(g_hStdOut, g_origOutMode);
    if(g_hStdIn !=INVALID_HANDLE_VALUE) SetConsoleMode(g_hStdIn,  g_origInMode);
}

// ── Console size ─────────────────────────────────────────────────────────────
struct TermSize { int rows=25, cols=80; };
TermSize getTermSize(){
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if(GetConsoleScreenBufferInfo(g_hStdOut,&csbi)){
        TermSize ts;
        ts.cols = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        ts.rows = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
        return ts;
    }
    return {25,80};
}

// ── String utilities ─────────────────────────────────────────────────────────
static std::string wstrToUtf8(const std::wstring& ws){
    if(ws.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8,0,ws.data(),(int)ws.size(),nullptr,0,nullptr,nullptr);
    std::string s(sz,'\0');
    WideCharToMultiByte(CP_UTF8,0,ws.data(),(int)ws.size(),&s[0],sz,nullptr,nullptr);
    return s;
}
static std::string padLeft(const std::string& s, int w){
    if((int)s.size()>=w) return s.substr(0,w);
    return std::string(w-s.size(),' ')+s;
}
static std::string padRight(const std::string& s, int w){
    if((int)s.size()>=w) return s.substr(0,w);
    return s+std::string(w-s.size(),' ');
}
static std::string truncate(const std::string& s, int w){
    if((int)s.size()<=w) return s;
    return s.substr(0,w-1)+"~";
}
static std::string fmtBytes(SIZE_T bytes){
    char buf[32];
    double mb = bytes/(1024.0*1024.0);
    if(mb>=1024) snprintf(buf,sizeof(buf),"%.1fGB",mb/1024.0);
    else         snprintf(buf,sizeof(buf),"%.1fMB",mb);
    return buf;
}
static std::string fmtBytesK(DWORDLONG kb){
    char buf[32];
    double mb = kb/1024.0;
    if(mb>=1024) snprintf(buf,sizeof(buf),"%.2fGB",mb/1024.0);
    else         snprintf(buf,sizeof(buf),"%.0fMB",mb);
    return buf;
}
static std::string repeat(const std::string& s, int n){
    std::string r; r.reserve(s.size()*n);
    for(int i=0;i<n;i++) r+=s;
    return r;
}

// ── Progress bar ─────────────────────────────────────────────────────────────
static std::string makeBar(double pct, int width){
    int fill = (int)(pct/100.0*width + 0.5);
    fill = std::max(0,std::min(width,fill));

    std::string clr;
    if     (pct<50) clr=fgRGB(60,200,90);
    else if(pct<75) clr=fgRGB(230,195,40);
    else if(pct<90) clr=fgRGB(235,120,30);
    else            clr=fgRGB(215,50,50);

    std::string bar = clr;
    for(int i=0;i<fill;i++)        bar += "\xe2\x96\x88"; // █
    bar += fgRGB(40,55,75);
    for(int i=fill;i<width;i++)    bar += "\xe2\x96\x91"; // ░
    bar += RESET;
    return bar;
}

// ── Priority string ──────────────────────────────────────────────────────────
static std::string priorityStr(DWORD cls){
    switch(cls){
        case IDLE_PRIORITY_CLASS:          return "Idle   ";
        case BELOW_NORMAL_PRIORITY_CLASS:  return "BelowN ";
        case NORMAL_PRIORITY_CLASS:        return "Normal ";
        case ABOVE_NORMAL_PRIORITY_CLASS:  return "AboveN ";
        case HIGH_PRIORITY_CLASS:          return "High   ";
        case REALTIME_PRIORITY_CLASS:      return "RealTim";
        default:                           return "Unknown";
    }
}

// ── Data structures ───────────────────────────────────────────────────────────
struct ProcessInfo {
    DWORD       pid{0};
    std::string name;
    double      cpuPercent{0.0};
    SIZE_T      workingSetBytes{0};
    SIZE_T      privateBytes{0};
    DWORD       handleCount{0};
    DWORD       threadCount{0};
    DWORD       priorityClass{0};
    bool        accessible{true};

    // CPU calc fields
    ULONGLONG   prevKernel{0};
    ULONGLONG   prevUser{0};
    ULONGLONG   prevTimestamp{0};    // 100ns units (FILETIME)
};

struct SysInfo {
    double      cpuPercent{0.0};
    DWORDLONG   memTotalKB{0};
    DWORDLONG   memUsedKB{0};
    DWORDLONG   memAvailKB{0};
    double      memPercent{0.0};
    DWORDLONG   pageTotalKB{0};
    DWORDLONG   pageUsedKB{0};
    double      pagePercent{0.0};
    int         procCount{0};
    int         threadCount{0};
    int         handleCount{0};
    ULONGLONG   uptimeMs{0};
    int         cpuLogicalCount{0};
};

// ── PDH CPU query ─────────────────────────────────────────────────────────────
static PDH_HQUERY   g_pdhQuery   = nullptr;
static PDH_HCOUNTER g_pdhCpu     = nullptr;

void initPDH(){
    PdhOpenQuery(nullptr, 0, &g_pdhQuery);
    PdhAddEnglishCounterW(g_pdhQuery,
        L"\\Processor(_Total)\\% Processor Time", 0, &g_pdhCpu);
    PdhCollectQueryData(g_pdhQuery); // prime first sample
    Sleep(100);
    PdhCollectQueryData(g_pdhQuery);
}
double queryCpuPDH(){
    if(!g_pdhQuery) return 0.0;
    PdhCollectQueryData(g_pdhQuery);
    PDH_FMT_COUNTERVALUE val{};
    PdhGetFormattedCounterValue(g_pdhCpu, PDH_FMT_DOUBLE, nullptr, &val);
    return std::min(100.0, std::max(0.0, val.doubleValue));
}

// ── System info reader ────────────────────────────────────────────────────────
void readSysInfo(SysInfo& si){
    // CPU via PDH
    si.cpuPercent = queryCpuPDH();

    // Memory via GlobalMemoryStatusEx
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    si.memTotalKB  = ms.ullTotalPhys / 1024;
    si.memAvailKB  = ms.ullAvailPhys / 1024;
    si.memUsedKB   = si.memTotalKB - si.memAvailKB;
    si.memPercent  = si.memTotalKB>0 ? 100.0*si.memUsedKB/si.memTotalKB : 0.0;
    si.pageTotalKB = ms.ullTotalPageFile / 1024;
    si.pageUsedKB  = (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1024;
    si.pagePercent = si.pageTotalKB>0 ? 100.0*si.pageUsedKB/si.pageTotalKB : 0.0;

    // Uptime
    si.uptimeMs = GetTickCount64();

    // CPU count
    SYSTEM_INFO sysInf{};
    GetSystemInfo(&sysInf);
    si.cpuLogicalCount = (int)sysInf.dwNumberOfProcessors;
}

// ── Per-process CPU calculation ───────────────────────────────────────────────
// Previous times map: pid -> {kernelTime, userTime, wallTimestamp}
static std::map<DWORD, std::tuple<ULONGLONG,ULONGLONG,ULONGLONG>> g_prevCpu;
static int g_cpuCount = 1;

static ULONGLONG filetimeToULL(const FILETIME& ft){
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return ui.QuadPart;
}

double calcProcessCpu(DWORD pid, HANDLE hProc){
    FILETIME ftCreate,ftExit,ftKernel,ftUser;
    if(!GetProcessTimes(hProc,&ftCreate,&ftExit,&ftKernel,&ftUser))
        return 0.0;

    ULONGLONG kNow = filetimeToULL(ftKernel);
    ULONGLONG uNow = filetimeToULL(ftUser);

    // Wall clock in 100ns units
    FILETIME ftNow; GetSystemTimeAsFileTime(&ftNow);
    ULONGLONG wallNow = filetimeToULL(ftNow);

    auto it = g_prevCpu.find(pid);
    if(it==g_prevCpu.end()){
        g_prevCpu[pid] = {kNow, uNow, wallNow};
        return 0.0;
    }

    auto [kPrev, uPrev, wallPrev] = it->second;
    ULONGLONG dCpu  = (kNow-kPrev) + (uNow-uPrev); // 100ns units
    ULONGLONG dWall = wallNow - wallPrev;
    g_prevCpu[pid] = {kNow, uNow, wallNow};

    if(dWall==0) return 0.0;
    // Divide by logical CPUs to get per-CPU percentage (optional: remove for raw total)
    double pct = 100.0 * dCpu / dWall;
    return std::min(pct, 100.0*(double)g_cpuCount);
}

// ── Process list reader ────────────────────────────────────────────────────────
std::vector<ProcessInfo> readAllProcesses(){
    std::vector<ProcessInfo> result;
    result.reserve(256);

    // Snapshot for names + thread counts
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(hSnap==INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if(!Process32FirstW(hSnap,&pe)){ CloseHandle(hSnap); return result; }

    do {
        ProcessInfo p;
        p.pid         = pe.th32ProcessID;
        p.name        = wstrToUtf8(pe.szExeFile);
        p.threadCount = pe.cntThreads;

        if(p.pid==0){ result.push_back(p); continue; }

        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE, p.pid);

        if(!hProc){
            p.accessible = false;
            result.push_back(p);
            continue;
        }

        // CPU %
        p.cpuPercent = calcProcessCpu(p.pid, hProc);

        // Memory
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if(GetProcessMemoryInfo(hProc,(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc))){
            p.workingSetBytes = pmc.WorkingSetSize;
            p.privateBytes    = pmc.PrivateUsage;
        }

        // Handle count
        GetProcessHandleCount(hProc, &p.handleCount);

        // Priority class
        p.priorityClass = GetPriorityClass(hProc);

        CloseHandle(hProc);
        result.push_back(p);

    } while(Process32NextW(hSnap,&pe));

    CloseHandle(hSnap);
    return result;
}

// ── Dashboard state ────────────────────────────────────────────────────────────
enum SortBy { SORT_CPU, SORT_MEM, SORT_PID, SORT_NAME, SORT_HANDLES };

struct DashState {
    SysInfo                  si;
    std::vector<ProcessInfo> procs;
    std::mutex               mtx;

    SortBy      sortBy   = SORT_CPU;
    bool        sortAsc  = false;
    int         scrollTop= 0;
    int         selIdx   = 0;       // index in current visible/sorted list
    bool        paused   = false;

    std::string filter;
    bool        filtering= false;

    std::string statusMsg;
    std::chrono::steady_clock::time_point statusExpiry{};

    bool running = true;
};

static DashState g_dash;
static std::atomic<bool> g_running{true};

void setStatus(const std::string& msg, int ms=2500){
    g_dash.statusMsg    = msg;
    g_dash.statusExpiry = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(ms);
}

// ── Data thread ────────────────────────────────────────────────────────────────
void dataThread(){
    while(g_running){
        if(!g_dash.paused){
            SysInfo si;
            readSysInfo(si);
            auto procs = readAllProcesses();

            // aggregate counts
            si.procCount   = (int)procs.size();
            si.threadCount = 0;
            si.handleCount = 0;
            for(auto& p: procs){
                si.threadCount += (int)p.threadCount;
                si.handleCount += (int)p.handleCount;
            }

            std::lock_guard<std::mutex> lk(g_dash.mtx);
            g_dash.si    = si;
            g_dash.procs = std::move(procs);
        }
        Sleep(1500);
    }
}

// ── Uptime formatter ───────────────────────────────────────────────────────────
static std::string fmtUptime(ULONGLONG ms){
    ULONGLONG s  = ms/1000;
    ULONGLONG d  = s/86400;
    ULONGLONG h  = (s%86400)/3600;
    ULONGLONG m  = (s%3600)/60;
    ULONGLONG sc = s%60;
    char buf[64];
    if(d>0) snprintf(buf,sizeof(buf),"%llud %02lluh %02llum",d,h,m);
    else    snprintf(buf,sizeof(buf),"%02lluh %02llum %02llus",h,m,sc);
    return buf;
}

// ── Render ─────────────────────────────────────────────────────────────────────
void render(){
    TermSize ts = getTermSize();
    int cols = ts.cols, rows = ts.rows;

    SysInfo si;
    std::vector<ProcessInfo> procs;
    {
        std::lock_guard<std::mutex> lk(g_dash.mtx);
        si    = g_dash.si;
        procs = g_dash.procs;
    }

    // ── Filter ──
    if(!g_dash.filter.empty()){
        std::vector<ProcessInfo> filt;
        std::string fl = g_dash.filter;
        // case-insensitive
        std::transform(fl.begin(),fl.end(),fl.begin(),::tolower);
        for(auto& p: procs){
            std::string nm = p.name;
            std::transform(nm.begin(),nm.end(),nm.begin(),::tolower);
            if(nm.find(fl)!=std::string::npos) filt.push_back(p);
        }
        procs = std::move(filt);
    }

    // ── Sort ──
    std::sort(procs.begin(),procs.end(),[&](const ProcessInfo& a, const ProcessInfo& b){
        bool res;
        switch(g_dash.sortBy){
            case SORT_CPU:     res = a.cpuPercent       > b.cpuPercent;       break;
            case SORT_MEM:     res = a.workingSetBytes  > b.workingSetBytes;  break;
            case SORT_PID:     res = a.pid              < b.pid;              break;
            case SORT_NAME:    res = a.name             < b.name;             break;
            case SORT_HANDLES: res = a.handleCount      > b.handleCount;      break;
            default:           res = a.cpuPercent       > b.cpuPercent;
        }
        return g_dash.sortAsc ? !res : res;
    });

    // Layout rows
    const int HEADER_ROWS = 7;
    const int FOOTER_ROWS = 2;
    const int TABLE_HDR   = HEADER_ROWS + 1;
    const int TABLE_START = TABLE_HDR + 1;
    const int TABLE_ROWS  = rows - TABLE_START - FOOTER_ROWS + 1;

    // Clamp selection + scroll
    int total = (int)procs.size();
    g_dash.selIdx   = std::max(0,std::min(g_dash.selIdx,total-1));
    int maxScroll   = std::max(0, total - TABLE_ROWS);
    if(g_dash.selIdx < g_dash.scrollTop) g_dash.scrollTop = g_dash.selIdx;
    if(g_dash.selIdx >= g_dash.scrollTop+TABLE_ROWS)
        g_dash.scrollTop = g_dash.selIdx - TABLE_ROWS + 1;
    g_dash.scrollTop = std::max(0,std::min(g_dash.scrollTop,maxScroll));

    int barW = std::max(10, cols/2 - 22);

    // ── Build frame into string buffer for one write ──
    std::string frame;
    frame.reserve(65536);

    // ── Title bar ──
    std::string title = " \xe2\x97\x88 PROCMON  Windows Process Dashboard";
    std::string right = "uptime: "+fmtUptime(si.uptimeMs)+" | "+
                        std::to_string(si.procCount)+" procs  ";
    int pad = cols - (int)title.size() - (int)right.size() - 2;

    frame += moveTo(1,1)
          + bgRGB(10,25,50) + fgRGB(80,200,255) + BOLD
          + title
          + std::string(std::max(0,pad),' ')
          + fgRGB(140,165,200) + right + RESET;

    // ── CPU bar ──
    {
        char pf[16]; snprintf(pf,sizeof(pf),"%.1f%%",si.cpuPercent);
        frame += moveTo(2,1) + bgRGB(8,18,38)
              + fgRGB(80,200,255) + BOLD + " CPU " + RESET + bgRGB(8,18,38)
              + " " + makeBar(si.cpuPercent, barW) + " ";
        std::string vc = si.cpuPercent>=90?fgRGB(215,50,50):
                         si.cpuPercent>=75?fgRGB(235,120,30):
                         si.cpuPercent>=50?fgRGB(230,195,40):fgRGB(60,200,90);
        frame += vc + BOLD + padLeft(pf,6) + RESET
              + bgRGB(8,18,38) + fgRGB(140,160,185)
              + "  " + std::to_string(si.cpuLogicalCount) + " logical CPUs"
              + RESET + ESC "K";
    }

    // ── MEM bar ──
    {
        char pf[16]; snprintf(pf,sizeof(pf),"%.1f%%",si.memPercent);
        frame += moveTo(3,1) + bgRGB(8,18,38)
              + fgRGB(150,100,255) + BOLD + " MEM " + RESET + bgRGB(8,18,38)
              + " " + makeBar(si.memPercent, barW) + " ";
        std::string vc = si.memPercent>=90?fgRGB(215,50,50):
                         si.memPercent>=75?fgRGB(235,120,30):fgRGB(150,100,255);
        frame += vc + BOLD + padLeft(pf,6) + RESET
              + bgRGB(8,18,38) + fgRGB(140,160,185)
              + "  used " + fgRGB(200,200,210)+fmtBytesK(si.memUsedKB)
              + fgRGB(100,115,140) + " / " + fgRGB(200,200,210)+fmtBytesK(si.memTotalKB)
              + fgRGB(140,160,185) + "   avail " + fgRGB(200,200,210)+fmtBytesK(si.memAvailKB)
              + RESET + ESC "K";
    }

    // ── Page file bar ──
    if(si.pageTotalKB>0){
        char pf[16]; snprintf(pf,sizeof(pf),"%.1f%%",si.pagePercent);
        frame += moveTo(4,1) + bgRGB(8,18,38)
              + fgRGB(255,140,70) + BOLD + " PGF " + RESET + bgRGB(8,18,38)
              + " " + makeBar(si.pagePercent, barW) + " ";
        std::string vc = si.pagePercent>=90?fgRGB(215,50,50):
                         si.pagePercent>=60?fgRGB(235,120,30):fgRGB(255,140,70);
        frame += vc + BOLD + padLeft(pf,6) + RESET
              + bgRGB(8,18,38) + fgRGB(140,160,185)
              + "  used " + fgRGB(200,200,210)+fmtBytesK(si.pageUsedKB)
              + fgRGB(100,115,140) + " / " + fgRGB(200,200,210)+fmtBytesK(si.pageTotalKB)
              + RESET + ESC "K";
    }

    // ── Stats row ──
    frame += moveTo(5,1) + bgRGB(8,18,38) + fgRGB(80,110,150)
          + " Procs: "   + fgRGB(200,210,230) + std::to_string(si.procCount)
          + fgRGB(80,110,150) + "   Threads: " + fgRGB(200,210,230) + std::to_string(si.threadCount)
          + fgRGB(80,110,150) + "   Handles: " + fgRGB(200,210,230) + std::to_string(si.handleCount)
          + fgRGB(80,110,150) + "   Filter: "
          + (g_dash.filter.empty() ? fgRGB(70,90,120)+"none" : fgRGB(255,220,60)+g_dash.filter)
          + (g_dash.paused ? fgRGB(215,60,60)+BOLD+"   [PAUSED]"+RESET : std::string(""))
          + RESET + ESC "K";

    // ── Separator ──
    frame += moveTo(6,1) + fgRGB(25,45,75)
          + repeat("\xe2\x94\x80", cols) // ─
          + RESET;

    // ── Table header ──
    auto sortMark = [&](SortBy sb, const char* label) -> std::string {
        if(g_dash.sortBy==sb)
            return std::string(bgRGB(25,55,95))+fgRGB(255,215,55)+BOLD+label+RESET
                  +bgRGB(12,22,42)+(g_dash.sortAsc?"\xe2\x86\x91":"\xe2\x86\x93")+" ";
        return std::string(fgRGB(120,145,175))+label+"  ";
    };

    frame += moveTo(7,1)
          + bgRGB(14,26,50) + fgRGB(100,130,170)
          + "  " + padRight("PID",7)
          + sortMark(SORT_NAME," NAME          ")
          + sortMark(SORT_CPU," CPU%  ")
          + sortMark(SORT_MEM," WORKING-SET ")
          + "  PRIVATE     "
          + sortMark(SORT_HANDLES,"HANDLES")
          + "  THR  PRIORITY"
          + RESET + ESC "K";

    // ── Process rows ──
    for(int i=0; i<TABLE_ROWS; i++){
        int idx = g_dash.scrollTop + i;
        int drawRow = TABLE_START + i;
        if(idx >= total){
            frame += moveTo(drawRow,1) + bgRGB(10,18,35) + ESC "K" + RESET;
            continue;
        }
        const ProcessInfo& p = procs[idx];
        bool sel = (idx == g_dash.selIdx);

        std::string rb = sel ? bgRGB(28,52,88) : (i%2==0 ? bgRGB(10,18,35) : bgRGB(12,21,40));

        // PID
        frame += moveTo(drawRow,1) + rb
              + (sel ? fgRGB(255,255,255)+BOLD : fgRGB(130,165,210))
              + "  " + padLeft(std::to_string(p.pid),6) + " ";

        // Name
        int nameW = 16;
        if(!p.accessible) frame += fgRGB(80,90,110) + DIM;
        else              frame += fgRGB(185,205,230);
        frame += padRight(truncate(p.name,nameW),nameW) + "  ";
        frame += RESET + rb;

        // CPU%
        char cpuf[12]; snprintf(cpuf,sizeof(cpuf),"%6.1f",p.cpuPercent);
        std::string cpuClr;
        if(p.cpuPercent>50)     cpuClr=fgRGB(215,55,55);
        else if(p.cpuPercent>20) cpuClr=fgRGB(230,185,45);
        else if(p.cpuPercent>2)  cpuClr=fgRGB(70,200,100);
        else                     cpuClr=fgRGB(65,90,120);
        frame += rb + cpuClr + BOLD + cpuf + RESET + rb + "   ";

        // Working set
        std::string ws = fmtBytes(p.workingSetBytes);
        std::string memClr;
        SIZE_T wsMB = p.workingSetBytes/(1024*1024);
        if(wsMB>1000)      memClr=fgRGB(215,55,55);
        else if(wsMB>200)  memClr=fgRGB(230,185,45);
        else if(wsMB>10)   memClr=fgRGB(140,100,240);
        else               memClr=fgRGB(90,110,140);
        frame += memClr + padLeft(ws,10) + RESET + rb + "   ";

        // Private
        frame += fgRGB(95,110,140) + padLeft(fmtBytes(p.privateBytes),10) + "   ";

        // Handles
        std::string hc = std::to_string(p.handleCount);
        std::string hClr = p.handleCount>5000?fgRGB(215,55,55):
                           p.handleCount>1000?fgRGB(230,185,45):fgRGB(110,145,190);
        frame += rb + hClr + padLeft(hc,6) + RESET + rb + "   ";

        // Threads
        frame += fgRGB(110,145,175) + padLeft(std::to_string(p.threadCount),4) + "   ";

        // Priority
        std::string priClr = p.priorityClass==HIGH_PRIORITY_CLASS    ? fgRGB(215,55,55) :
                             p.priorityClass==REALTIME_PRIORITY_CLASS ? fgRGB(240,80,80) :
                             p.priorityClass==ABOVE_NORMAL_PRIORITY_CLASS ? fgRGB(230,185,45) :
                             fgRGB(100,130,165);
        frame += rb + priClr + priorityStr(p.priorityClass) + RESET + ESC "K";
    }

    // ── Footer separator ──
    frame += moveTo(rows-1,1) + fgRGB(25,45,75) + repeat("\xe2\x94\x80",cols) + RESET;

    // ── Footer hints / filter input ──
    frame += moveTo(rows,1) + bgRGB(8,16,32);
    if(g_dash.filtering){
        frame += fgRGB(255,215,55)+BOLD+" Filter: "+RESET
              + fgRGB(200,220,255) + g_dash.filter
              + fgRGB(255,255,255) + "\xe2\x96\x88" + RESET; // block cursor
    } else if(!g_dash.statusMsg.empty() &&
              std::chrono::steady_clock::now()<g_dash.statusExpiry){
        frame += fgRGB(80,220,130)+BOLD+" + "+RESET+fgRGB(180,220,180)+g_dash.statusMsg+RESET;
    } else {
        frame += fgRGB(60,85,120)
              + " [c]pu [m]em [p]id [n]ame [h]andles  "
              + "[/]filter  [Del]kill  [Esc/q]quit  "
              + "[Space]pause  [\xe2\x86\x91\xe2\x86\x93]select"
              + fgRGB(50,70,100) + "  sort:"
              + fgRGB(255,215,55) + ([&]()->std::string{
                    switch(g_dash.sortBy){
                        case SORT_CPU: return "CPU";
                        case SORT_MEM: return "MEM";
                        case SORT_PID: return "PID";
                        case SORT_HANDLES: return "HDL";
                        default: return "NAME";
                    }}())
              + RESET;
    }
    frame += ESC "K";

    // ── Scrollbar ──
    if(total>TABLE_ROWS && maxScroll>0){
        int sbH  = TABLE_ROWS;
        int sbPos= (int)((double)g_dash.scrollTop/maxScroll*(sbH-1)+0.5);
        for(int i=0;i<sbH;i++){
            frame += moveTo(TABLE_START+i, cols);
            if(i==sbPos) frame += fgRGB(70,140,240)+"\xe2\x96\x88"+RESET; // █
            else         frame += fgRGB(25,40,65)  +"\xe2\x94\x82"+RESET; // │
        }
    }

    // ── Write entire frame at once ──
    fwrite(frame.c_str(), 1, frame.size(), stdout);
    fflush(stdout);
}

// ── Input handler ──────────────────────────────────────────────────────────────
void handleInput(){
    // Non-blocking check for key events
    DWORD numEvents=0;
    GetNumberOfConsoleInputEvents(g_hStdIn, &numEvents);
    if(numEvents==0) return;

    INPUT_RECORD ir{};
    DWORD read=0;
    while(ReadConsoleInputW(g_hStdIn,&ir,1,&read) && read>0){
        if(ir.EventType!=KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk   = ir.Event.KeyEvent.wVirtualKeyCode;
        WCHAR wch = ir.Event.KeyEvent.uChar.UnicodeChar;

        if(g_dash.filtering){
            if(vk==VK_RETURN || vk==VK_ESCAPE){ g_dash.filtering=false; return; }
            if(vk==VK_BACK){
                if(!g_dash.filter.empty()){
                    // remove last UTF-8 char (simple: just pop_back until valid)
                    g_dash.filter.pop_back();
                }
                return;
            }
            if(wch>=32){
                char mb[5]={};
                WideCharToMultiByte(CP_UTF8,0,&wch,1,mb,4,nullptr,nullptr);
                g_dash.filter += mb;
            }
            return;
        }

        // Arrow keys
        if(vk==VK_UP){
            g_dash.selIdx = std::max(0, g_dash.selIdx-1);
            return;
        }
        if(vk==VK_DOWN){
            g_dash.selIdx++;
            return;
        }
        if(vk==VK_PRIOR){ g_dash.selIdx = std::max(0, g_dash.selIdx-10); return; }
        if(vk==VK_NEXT)  { g_dash.selIdx += 10; return; }
        if(vk==VK_HOME)  { g_dash.selIdx=0; g_dash.scrollTop=0; return; }
        if(vk==VK_END)   { g_dash.selIdx=999999; return; }

        if(vk==VK_DELETE){
            // Kill selected process
            std::lock_guard<std::mutex> lk(g_dash.mtx);
            std::vector<ProcessInfo> procs = g_dash.procs;

            // Apply same filter + sort to find correct PID
            if(!g_dash.filter.empty()){
                std::string fl=g_dash.filter;
                std::transform(fl.begin(),fl.end(),fl.begin(),::tolower);
                procs.erase(std::remove_if(procs.begin(),procs.end(),[&](const ProcessInfo& p){
                    std::string nm=p.name;
                    std::transform(nm.begin(),nm.end(),nm.begin(),::tolower);
                    return nm.find(fl)==std::string::npos;
                }),procs.end());
            }
            if(g_dash.selIdx<(int)procs.size()){
                DWORD pid = procs[g_dash.selIdx].pid;
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE,FALSE,pid);
                if(hProc){
                    if(TerminateProcess(hProc,1))
                        setStatus("Killed PID "+std::to_string(pid));
                    else
                        setStatus("Failed to kill PID "+std::to_string(pid)+" (access denied?)");
                    CloseHandle(hProc);
                } else {
                    setStatus("Cannot open PID "+std::to_string(pid)+" (access denied)");
                }
            }
            return;
        }

        if(vk==VK_ESCAPE || wch=='q' || wch=='Q'){ g_running=false; return; }

        switch(wch){
            case 'c': case 'C': g_dash.sortBy=SORT_CPU;     g_dash.sortAsc=false; break;
            case 'm': case 'M': g_dash.sortBy=SORT_MEM;     g_dash.sortAsc=false; break;
            case 'p': case 'P': g_dash.sortBy=SORT_PID;     g_dash.sortAsc=true;  break;
            case 'n': case 'N': g_dash.sortBy=SORT_NAME;    g_dash.sortAsc=true;  break;
            case 'h': case 'H': g_dash.sortBy=SORT_HANDLES; g_dash.sortAsc=false; break;
            case ' ':
                g_dash.paused = !g_dash.paused;
                setStatus(g_dash.paused ? "Refresh paused" : "Refresh resumed");
                break;
            case '/':
                g_dash.filtering=true;
                g_dash.filter.clear();
                break;
        }
    }
}

// ── Ctrl-C handler ─────────────────────────────────────────────────────────────
BOOL WINAPI ctrlHandler(DWORD dwCtrlType){
    if(dwCtrlType==CTRL_C_EVENT||dwCtrlType==CTRL_CLOSE_EVENT){
        g_running=false;
        return TRUE;
    }
    return FALSE;
}

// ── Entry point ────────────────────────────────────────────────────────────────
int main(){
    // Set UTF-8 output
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if(!enableVT()){
        fprintf(stderr, "ERROR: Could not enable Virtual Terminal Processing.\n"
                        "Requires Windows 10 version 1607+ or Windows 11.\n");
        return 1;
    }

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Window title
    SetConsoleTitleW(L"PROCMON - Real-Time Process Dashboard");

    // Init PDH
    initPDH();

    // Get CPU count for % calculation
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_cpuCount = (int)si.dwNumberOfProcessors;

    hideCursor();
    clearScreen();

    // Splash / first data pull
    printf("%s%s  Initializing PROCMON...%s\n",
           bgRGB(10,25,50).c_str(), fgRGB(80,200,255).c_str(), RESET);
    fflush(stdout);

    // Prime CPU delta
    readAllProcesses();
    Sleep(1200);

    // Start data thread
    std::thread dt(dataThread);

    // Main loop
    while(g_running){
        handleInput();
        if(g_running) render();
        Sleep(120);
    }

    g_running=false;
    dt.join();

    // Cleanup
    if(g_pdhQuery) PdhCloseQuery(g_pdhQuery);
    restoreConsole();
    showCursor();
    clearScreen();

    printf("%s%s\n  PROCMON exited cleanly.\n%s",
           bgRGB(10,25,50).c_str(), fgRGB(80,200,255).c_str(), RESET);
    return 0;
}
