/**
 * ============================================================
 *  PROCMON GUI v2  -  Real-Time Process Monitor (Win32 GUI)
 *  Windows 10/11 | C++17 | Win32 + GDI + PDH + PSAPI
 * ============================================================
 *
 *  BUILD (MinGW / MSYS2):
 *    g++ -std=c++17 -O2 -static -mwindows -o procmon_gui.exe procmon_gui.cpp
 *        -lpdh -lpsapi -lntdll -luser32 -lkernel32 -lgdi32
 *        -lcomctl32 -lcomdlg32 -lshell32 -luxtheme -lshlwapi
 *
 *  KEYBOARD SHORTCUTS:
 *    Del      – Kill selected process
 *    Space    – Pause / Resume refresh
 *    C        – Sort by CPU
 *    M        – Sort by Memory
 *    P        – Sort by PID
 *    N        – Sort by Name
 *    H        – Sort by Handles
 *    T        – Sort by Threads
 *    Ctrl+F   – Focus search box
 *    Ctrl+E   – Export to CSV
 *    Ctrl+T   – Toggle Always on Top
 *    F5       – Force refresh
 *    Escape   – Clear filter / deselect
 *    Q        – Quit
 * ============================================================
 */

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#pragma comment(lib,"pdh.lib")
#pragma comment(lib,"psapi.lib")
#pragma comment(lib,"ntdll.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"kernel32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"shlwapi.lib")

#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <winternl.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <cstdio>
#include <cmath>

// ── Resource / Control IDs ────────────────────────────────────────────────────
#define IDC_LISTVIEW     1001
#define IDC_SEARCH       1002
#define IDC_BTN_KILL     1003
#define IDC_BTN_PAUSE    1004
#define IDC_BTN_EXPORT   1005
#define IDC_STATUS_BAR   1007
#define IDC_GRAPH_PANEL  1008
#define IDC_COMBO_RATE   1009
#define IDC_BTN_ONTOP    1010

#define IDM_KILL              2001
#define IDM_COPY_NAME         2002
#define IDM_COPY_PID          2003
#define IDM_COPY_PATH         2004
#define IDM_OPEN_LOCATION     2005
#define IDM_PRIORITY_REALTIME 2009
#define IDM_PRIORITY_HIGH     2010
#define IDM_PRIORITY_ABOVE    2011
#define IDM_PRIORITY_NORMAL   2012
#define IDM_PRIORITY_BELOW    2013
#define IDM_PRIORITY_IDLE     2014
#define IDM_AFFINITY          2015
#define IDM_SUSPEND           2016

#define IDM_TRAY_SHOW         3001
#define IDM_TRAY_EXIT         3002

#define TIMER_REFRESH   100
#define TIMER_GRAPH     101
#define WM_TRAYICON     (WM_USER+10)
#define WM_NEWDATA      (WM_USER+1)
#define TRAY_UID        1

// ── History/sparkline settings ────────────────────────────────────────────────
#define HISTORY_LEN  90   // data points kept for the graph

// ── Color palette ─────────────────────────────────────────────────────────────
static const COLORREF C_BG_DARK     = RGB(9,   14,  26 );
static const COLORREF C_BG_MID      = RGB(13,  20,  38 );
static const COLORREF C_BG_PANEL    = RGB(11,  17,  33 );
static const COLORREF C_BG_GRAPH    = RGB(7,   11,  22 );
static const COLORREF C_BG_GAUGE    = RGB(18,  28,  52 );
static const COLORREF C_BG_ROW_E    = RGB(11,  18,  34 );
static const COLORREF C_BG_ROW_O    = RGB(14,  22,  40 );
static const COLORREF C_BG_SEL      = RGB(25,  52,  100);
static const COLORREF C_BG_NEW      = RGB(15,  48,  25 );   // newly appeared process
static const COLORREF C_BG_GONE     = RGB(50,  15,  15 );   // about-to-vanish
static const COLORREF C_TXT_MAIN    = RGB(195, 215, 240);
static const COLORREF C_TXT_DIM     = RGB(80,  105, 145);
static const COLORREF C_TXT_BRIGHT  = RGB(230, 245, 255);
static const COLORREF C_ACCENT_BLUE = RGB(55,  155, 255);
static const COLORREF C_ACCENT_PURP = RGB(135, 85,  255);
static const COLORREF C_ACCENT_CYAN = RGB(40,  210, 200);
static const COLORREF C_GREEN       = RGB(45,  205, 95 );
static const COLORREF C_YELLOW      = RGB(228, 192, 42 );
static const COLORREF C_RED         = RGB(212, 52,  52 );
static const COLORREF C_ORANGE      = RGB(232, 118, 28 );
static const COLORREF C_SEP         = RGB(22,  38,  68 );
static const COLORREF C_BTN_BG      = RGB(20,  34,  64 );
static const COLORREF C_BTN_HOT     = RGB(30,  55,  100);
static const COLORREF C_BTN_ACT     = RGB(18,  42,  82 );
static const COLORREF C_GRAPH_CPU   = RGB(55,  155, 255);
static const COLORREF C_GRAPH_MEM   = RGB(135, 85,  255);
static const COLORREF C_GRAPH_PGF   = RGB(255, 135, 50 );
static const COLORREF C_GRID        = RGB(20,  32,  58 );

// ── Data structures ───────────────────────────────────────────────────────────
struct ProcessInfo {
    DWORD        pid{0};
    DWORD        parentPid{0};
    std::wstring name;
    std::wstring path;          // full exe path (lazy-loaded)
    double       cpuPercent{0.0};
    SIZE_T       workingSetBytes{0};
    SIZE_T       privateBytes{0};
    DWORD        handleCount{0};
    DWORD        threadCount{0};
    DWORD        priorityClass{0};
    ULONGLONG    createTime{0};  // FILETIME as ULL
    bool         accessible{true};
    bool         isNew{false};   // appeared this cycle
};

struct SysInfo {
    double    cpuPercent{0.0};
    DWORDLONG memTotalKB{0};
    DWORDLONG memUsedKB{0};
    DWORDLONG memAvailKB{0};
    double    memPercent{0.0};
    DWORDLONG pageTotalKB{0};
    DWORDLONG pageUsedKB{0};
    double    pagePercent{0.0};
    int       procCount{0};
    int       threadCount{0};
    int       handleCount{0};
    ULONGLONG uptimeMs{0};
    int       cpuLogicalCount{1};
};

enum SortBy {
    SORT_CPU=0, SORT_MEM, SORT_PID, SORT_NAME,
    SORT_HANDLES, SORT_THREADS, SORT_PRIVATE, SORT_COUNT
};

// ── Globals ───────────────────────────────────────────────────────────────────
static HWND  g_hWnd      = nullptr;
static HWND  g_hList     = nullptr;
static HWND  g_hSearch   = nullptr;
static HWND  g_hStatus   = nullptr;
static HWND  g_hBtnKill  = nullptr;
static HWND  g_hBtnPause = nullptr;
static HWND  g_hBtnExport= nullptr;
static HWND  g_hBtnOnTop = nullptr;
static HWND  g_hComboRate= nullptr;
static HINSTANCE g_hInst = nullptr;

static std::mutex              g_mtx;
static std::vector<ProcessInfo> g_procs;
static SysInfo                 g_si;
static std::atomic<bool>       g_running{true};
static std::atomic<bool>       g_paused{false};
static std::atomic<bool>       g_dataReady{false};

static SortBy g_sortBy  = SORT_CPU;
static bool   g_sortAsc = false;
static bool   g_alwaysOnTop = false;

// History (ring-buffer style)
static std::deque<double> g_cpuHist, g_memHist, g_pgfHist;

// New-process flash tracking
static std::set<DWORD>    g_knownPids;
static std::map<DWORD,int> g_newFlash; // pid -> remaining flash ticks

// PDH
static PDH_HQUERY   g_pdhQuery = nullptr;
static PDH_HCOUNTER g_pdhCpu   = nullptr;

// Per-process CPU tracking
static std::map<DWORD, std::tuple<ULONGLONG,ULONGLONG,ULONGLONG>> g_prevCpu;
static int g_cpuCount = 1;

// Refresh interval (ms)
static int g_refreshMs = 1500;

// GDI cache
static HFONT  g_fMain=nullptr, g_fBold=nullptr, g_fSmall=nullptr,
              g_fTitle=nullptr, g_fMono=nullptr;
static HBRUSH g_brDark=nullptr, g_brMid=nullptr,
              g_brPanel=nullptr, g_brSel=nullptr, g_brGraph=nullptr;

// Tray icon
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded=false;

// ── Utilities ─────────────────────────────────────────────────────────────────
static ULONGLONG filetimeToULL(const FILETIME& ft){
    ULARGE_INTEGER ui; ui.LowPart=ft.dwLowDateTime; ui.HighPart=ft.dwHighDateTime;
    return ui.QuadPart;
}
static std::wstring fmtBytes(SIZE_T bytes){
    wchar_t buf[32]; double mb=bytes/1048576.0;
    if(mb>=1024) swprintf(buf,32,L"%.2f GB",mb/1024.0);
    else         swprintf(buf,32,L"%.1f MB",mb);
    return buf;
}
static std::wstring fmtKB(DWORDLONG kb){
    wchar_t buf[32]; double mb=kb/1024.0;
    if(mb>=1024) swprintf(buf,32,L"%.2f GB",mb/1024.0);
    else         swprintf(buf,32,L"%.0f MB",mb);
    return buf;
}
static std::wstring fmtUptime(ULONGLONG ms){
    ULONGLONG s=ms/1000,d=s/86400,h=(s%86400)/3600,m=(s%3600)/60,sc=s%60;
    wchar_t buf[64];
    if(d>0) swprintf(buf,64,L"%llud %02lluh %02llum",d,h,m);
    else    swprintf(buf,64,L"%02lluh %02llum %02llus",h,m,sc);
    return buf;
}
static std::wstring priorityStr(DWORD cls){
    switch(cls){
        case IDLE_PRIORITY_CLASS:          return L"Idle";
        case BELOW_NORMAL_PRIORITY_CLASS:  return L"Below Normal";
        case NORMAL_PRIORITY_CLASS:        return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS:  return L"Above Normal";
        case HIGH_PRIORITY_CLASS:          return L"High";
        case REALTIME_PRIORITY_CLASS:      return L"Realtime";
        default:                           return L"Unknown";
    }
}
static std::wstring getProcessPath(DWORD pid){
    HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!h) return L"";
    wchar_t buf[MAX_PATH]={};
    DWORD sz=MAX_PATH;
    QueryFullProcessImageNameW(h,0,buf,&sz);
    CloseHandle(h);
    return buf;
}
static COLORREF gaugeClr(double pct){
    if(pct<50) return C_GREEN; if(pct<75) return C_YELLOW;
    if(pct<90) return C_ORANGE; return C_RED;
}

// ── PDH / CPU ─────────────────────────────────────────────────────────────────
static void initPDH(){
    PdhOpenQuery(nullptr,0,&g_pdhQuery);
    PdhAddEnglishCounterW(g_pdhQuery,L"\\Processor(_Total)\\% Processor Time",0,&g_pdhCpu);
    PdhCollectQueryData(g_pdhQuery); Sleep(80); PdhCollectQueryData(g_pdhQuery);
}
static double queryCpuPDH(){
    if(!g_pdhQuery) return 0.0;
    PdhCollectQueryData(g_pdhQuery);
    PDH_FMT_COUNTERVALUE v{};
    PdhGetFormattedCounterValue(g_pdhCpu,PDH_FMT_DOUBLE,nullptr,&v);
    return std::min(100.0,std::max(0.0,v.doubleValue));
}
static double calcProcessCpu(DWORD pid,HANDLE hProc){
    FILETIME c,e,k,u; if(!GetProcessTimes(hProc,&c,&e,&k,&u)) return 0.0;
    ULONGLONG kN=filetimeToULL(k),uN=filetimeToULL(u);
    FILETIME fn; GetSystemTimeAsFileTime(&fn);
    ULONGLONG wN=filetimeToULL(fn);
    auto it=g_prevCpu.find(pid);
    if(it==g_prevCpu.end()){ g_prevCpu[pid]={kN,uN,wN}; return 0.0; }
    auto [kP,uP,wP]=it->second;
    g_prevCpu[pid]={kN,uN,wN};
    ULONGLONG dC=(kN-kP)+(uN-uP), dW=wN-wP;
    if(dW==0) return 0.0;
    return std::min(100.0*(double)g_cpuCount, 100.0*dC/dW);
}

// ── Data collection ────────────────────────────────────────────────────────────
static void readSysInfo(SysInfo& si){
    si.cpuPercent=queryCpuPDH();
    MEMORYSTATUSEX ms{}; ms.dwLength=sizeof(ms); GlobalMemoryStatusEx(&ms);
    si.memTotalKB=ms.ullTotalPhys/1024; si.memAvailKB=ms.ullAvailPhys/1024;
    si.memUsedKB=si.memTotalKB-si.memAvailKB;
    si.memPercent=si.memTotalKB>0?100.0*si.memUsedKB/si.memTotalKB:0;
    si.pageTotalKB=ms.ullTotalPageFile/1024;
    si.pageUsedKB=(ms.ullTotalPageFile-ms.ullAvailPageFile)/1024;
    si.pagePercent=si.pageTotalKB>0?100.0*si.pageUsedKB/si.pageTotalKB:0;
    si.uptimeMs=GetTickCount64();
    SYSTEM_INFO s2{}; GetSystemInfo(&s2); si.cpuLogicalCount=(int)s2.dwNumberOfProcessors;
}

static std::vector<ProcessInfo> readAllProcesses(){
    std::vector<ProcessInfo> result; result.reserve(300);
    HANDLE hSnap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(hSnap==INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if(!Process32FirstW(hSnap,&pe)){ CloseHandle(hSnap); return result; }
    do {
        ProcessInfo p; p.pid=pe.th32ProcessID; p.parentPid=pe.th32ParentProcessID;
        p.name=pe.szExeFile; p.threadCount=pe.cntThreads;
        if(p.pid==0){ result.push_back(p); continue; }
        HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,p.pid);
        if(!h){ p.accessible=false; result.push_back(p); continue; }
        p.cpuPercent=calcProcessCpu(p.pid,h);
        PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb=sizeof(pmc);
        if(GetProcessMemoryInfo(h,(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc))){
            p.workingSetBytes=pmc.WorkingSetSize; p.privateBytes=pmc.PrivateUsage;
        }
        GetProcessHandleCount(h,&p.handleCount);
        p.priorityClass=GetPriorityClass(h);
        FILETIME c,e,k,u;
        if(GetProcessTimes(h,&c,&e,&k,&u)) p.createTime=filetimeToULL(c);
        CloseHandle(h);
        result.push_back(p);
    } while(Process32NextW(hSnap,&pe));
    CloseHandle(hSnap);
    return result;
}

// ── Background thread ─────────────────────────────────────────────────────────
static void dataThread(){
    while(g_running){
        if(!g_paused){
            SysInfo si; readSysInfo(si);
            auto procs=readAllProcesses();
            si.procCount=(int)procs.size();
            for(auto& p:procs){ si.threadCount+=(int)p.threadCount; si.handleCount+=(int)p.handleCount; }

            // Detect new PIDs
            std::set<DWORD> curPids;
            for(auto& p:procs) curPids.insert(p.pid);
            for(auto& p:procs){
                if(g_knownPids.find(p.pid)==g_knownPids.end()) p.isNew=true;
            }
            g_knownPids=curPids;

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_si=si; g_procs=std::move(procs);

                // Update history
                g_cpuHist.push_back(si.cpuPercent);
                g_memHist.push_back(si.memPercent);
                g_pgfHist.push_back(si.pagePercent);
                while((int)g_cpuHist.size()>HISTORY_LEN) g_cpuHist.pop_front();
                while((int)g_memHist.size()>HISTORY_LEN) g_memHist.pop_front();
                while((int)g_pgfHist.size()>HISTORY_LEN) g_pgfHist.pop_front();

                g_dataReady=true;
            }
            if(g_hWnd) PostMessage(g_hWnd,WM_NEWDATA,0,0);
        }
        Sleep(g_refreshMs);
    }
}

// ── System tray ───────────────────────────────────────────────────────────────
static void addTrayIcon(){
    ZeroMemory(&g_nid,sizeof(g_nid));
    g_nid.cbSize=sizeof(g_nid);
    g_nid.hWnd=g_hWnd;
    g_nid.uID=TRAY_UID;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    g_nid.uCallbackMessage=WM_TRAYICON;
    g_nid.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(g_nid.szTip,L"PROCMON – Process Monitor");
    Shell_NotifyIconW(NIM_ADD,&g_nid);
    g_trayAdded=true;
}
static void removeTrayIcon(){
    if(g_trayAdded){ Shell_NotifyIconW(NIM_DELETE,&g_nid); g_trayAdded=false; }
}
static void updateTrayTip(const SysInfo& si){
    swprintf(g_nid.szTip,64,L"PROCMON  CPU:%.0f%%  MEM:%.0f%%",si.cpuPercent,si.memPercent);
    g_nid.uFlags=NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY,&g_nid);
}

// ── GDI drawing helpers ───────────────────────────────────────────────────────
static void FillRoundRect(HDC hdc, RECT r, int rx, COLORREF clr){
    HBRUSH br=CreateSolidBrush(clr);
    HPEN   pn=(HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc,pn); SelectObject(hdc,br);
    RoundRect(hdc,r.left,r.top,r.right,r.bottom,rx,rx);
    DeleteObject(br);
}

// Draw a labelled gauge bar
static void DrawGauge(HDC hdc,RECT r,double pct,COLORREF fill,
                      const wchar_t* label,const wchar_t* val,HFONT hf){
    int w=r.right-r.left;
    FillRoundRect(hdc,r,8,C_BG_GAUGE);
    if(pct>0){
        int fw=std::max(8,(int)(pct/100.0*(w-4)));
        fw=std::min(w-4,fw);
        RECT fr={r.left+2,r.top+2,r.left+2+fw,r.bottom-2};
        FillRoundRect(hdc,fr,6,fill);
    }
    SetBkMode(hdc,TRANSPARENT);
    SelectObject(hdc,hf);
    // label (left)
    SetTextColor(hdc,C_TXT_BRIGHT);
    RECT tl={r.left+8,r.top,r.left+w*2/5,r.bottom};
    DrawTextW(hdc,label,-1,&tl,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    // value (right)
    RECT tr2={r.left+w*2/5,r.top,r.right-8,r.bottom};
    DrawTextW(hdc,val,-1,&tr2,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
}

// Draw graph from deque
static void DrawHistoryGraph(HDC hdc,RECT r,const std::deque<double>& data,
                             COLORREF lineClr,COLORREF fillClr,const wchar_t* label){
    int W=r.right-r.left, H=r.bottom-r.top;
    if(data.empty()||W<4||H<4) return;

    int n=(int)data.size();
    int N=std::min(n,HISTORY_LEN);

    // Build poly points
    std::vector<POINT> pts; pts.reserve(N+2);
    pts.push_back({r.right, r.bottom});
    for(int i=0;i<N;i++){
        double v=data[n-N+i];
        int x=r.left + (int)((double)i/(N-1)*(W-1));
        int y=r.bottom-2-(int)(v/100.0*(H-4));
        pts.push_back({x,y});
    }
    pts.push_back({r.left, r.bottom});

    // Fill area (semi-transparent simulate via blending not available in plain GDI, just draw solid dim)
    HBRUSH brFill=CreateSolidBrush(fillClr);
    HPEN penNull=(HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc,penNull); SelectObject(hdc,brFill);
    Polygon(hdc,pts.data(),(int)pts.size());
    DeleteObject(brFill);

    // Line
    HPEN penLine=CreatePen(PS_SOLID,2,lineClr);
    SelectObject(hdc,penLine);
    MoveToEx(hdc,pts[1].x,pts[1].y,nullptr);
    for(int i=2;i<(int)pts.size()-1;i++) LineTo(hdc,pts[i].x,pts[i].y);
    DeleteObject(penLine);

    // Label
    SetBkMode(hdc,TRANSPARENT);
    SetTextColor(hdc,lineClr);
    SelectObject(hdc,g_fBold);
    RECT lr={r.left+4,r.top+2,r.right-4,r.top+18};
    DrawTextW(hdc,label,-1,&lr,DT_LEFT|DT_SINGLELINE);

    // Current value
    if(!data.empty()){
        wchar_t vbuf[16]; swprintf(vbuf,16,L"%.1f%%",data.back());
        RECT vr={r.left+4,r.top+2,r.right-4,r.top+18};
        DrawTextW(hdc,vbuf,-1,&vr,DT_RIGHT|DT_SINGLELINE);
    }
}

// Draw mini sparkline for an individual process (used in custom draw)
static void DrawMiniBar(HDC hdc, RECT r, double pct, COLORREF clr){
    int w=r.right-r.left, h=r.bottom-r.top;
    HBRUSH brBg=CreateSolidBrush(C_BG_GAUGE);
    HPEN pn=(HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc,pn); SelectObject(hdc,brBg);
    RoundRect(hdc,r.left,r.top,r.right,r.bottom,3,3);
    DeleteObject(brBg);
    if(pct>0){
        int fw=std::max(1,(int)(pct/100.0*(w-2)));
        fw=std::min(w-2,fw);
        RECT fr={r.left+1,r.top+1,r.left+1+fw,r.bottom-1};
        HBRUSH brFill=CreateSolidBrush(clr);
        SelectObject(hdc,brFill);
        RoundRect(hdc,fr.left,fr.top,fr.right,fr.bottom,2,2);
        DeleteObject(brFill);
    }
}

// ── Layout constants ──────────────────────────────────────────────────────────
#define PANEL_H   130    // top panel (title + gauges + graph)
#define TOOLBAR_H  42
#define STATUS_H   24

// ── Sorted list ───────────────────────────────────────────────────────────────
static std::vector<ProcessInfo> g_sorted;

static void sortAndFilter(const std::wstring& filt){
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sorted=g_procs;
    if(!filt.empty()){
        std::wstring fl=filt; for(auto& c:fl) c=(wchar_t)towlower(c);
        g_sorted.erase(std::remove_if(g_sorted.begin(),g_sorted.end(),[&](const ProcessInfo& p){
            std::wstring nm=p.name; for(auto& c:nm) c=(wchar_t)towlower(c);
            return nm.find(fl)==std::wstring::npos;
        }),g_sorted.end());
    }
    bool asc=g_sortAsc;
    switch(g_sortBy){
        case SORT_CPU:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.cpuPercent<b.cpuPercent:a.cpuPercent>b.cpuPercent; }); break;
        case SORT_MEM:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.workingSetBytes<b.workingSetBytes:a.workingSetBytes>b.workingSetBytes; }); break;
        case SORT_PID:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.pid<b.pid:a.pid>b.pid; }); break;
        case SORT_NAME:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.name<b.name:a.name>b.name; }); break;
        case SORT_HANDLES:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.handleCount<b.handleCount:a.handleCount>b.handleCount; }); break;
        case SORT_THREADS:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.threadCount<b.threadCount:a.threadCount>b.threadCount; }); break;
        case SORT_PRIVATE:
            std::sort(g_sorted.begin(),g_sorted.end(),[asc](const ProcessInfo& a,const ProcessInfo& b){
                return asc?a.privateBytes<b.privateBytes:a.privateBytes>b.privateBytes; }); break;
        default: break;
    }
}

static void populateListView(){
    wchar_t sb[256]={}; GetWindowTextW(g_hSearch,sb,255);
    sortAndFilter(sb);
    int prevSel=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
    DWORD selPid=0;
    if(prevSel>=0&&prevSel<(int)g_sorted.size()) selPid=g_sorted[prevSel].pid;
    ListView_SetItemCountEx(g_hList,(int)g_sorted.size(),LVSICF_NOINVALIDATEALL);
    ListView_RedrawItems(g_hList,0,(int)g_sorted.size()-1);
    UpdateWindow(g_hList);
    if(selPid){
        for(int i=0;i<(int)g_sorted.size();i++){
            if(g_sorted[i].pid==selPid){
                ListView_SetItemState(g_hList,i,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                ListView_EnsureVisible(g_hList,i,FALSE);
                break;
            }
        }
    }
}

// ── Status bar ────────────────────────────────────────────────────────────────
static void updateStatus(){
    SysInfo si; { std::lock_guard<std::mutex> lk(g_mtx); si=g_si; }
    wchar_t buf[512];
    swprintf(buf,512,
        L"  Procs: %d   Threads: %d   Handles: %d   Uptime: %s   CPU: %.1f%%   MEM: %.1f%%%s",
        si.procCount,si.threadCount,si.handleCount,
        fmtUptime(si.uptimeMs).c_str(),
        si.cpuPercent, si.memPercent,
        g_paused.load()?L"   ⏸ PAUSED":L"");
    SetWindowTextW(g_hStatus,buf);
    updateTrayTip(si);
}

// ── Column header with sort arrows ────────────────────────────────────────────
static void updateColumnHeaders(){
    const wchar_t* names[]=
        {L"PID",L"Name",L"CPU %",L"Working Set",L"Private",L"Handles",L"Threads",L"Priority"};
    SortBy colSort[]={
        SORT_PID,(SortBy)-1,SORT_CPU,SORT_MEM,SORT_PRIVATE,SORT_HANDLES,SORT_THREADS,(SortBy)-1};

    HWND hHdr=ListView_GetHeader(g_hList);
    for(int i=0;i<8;i++){
        wchar_t buf[64];
        if(colSort[i]!=(SortBy)-1 && g_sortBy==colSort[i])
            swprintf(buf,64,L"%s %s",names[i],g_sortAsc?L"▲":L"▼");
        else
            wcscpy_s(buf,names[i]);
        HDITEMW hdi={HDI_TEXT,0,buf};
        Header_SetItem(hHdr,i,&hdi);
    }
}

// ── ListView virtual GetDispInfo ──────────────────────────────────────────────
static LRESULT handleGetDispInfo(NMLVDISPINFOW* pdi){
    int i=pdi->item.iItem;
    if(i<0||i>=(int)g_sorted.size()) return 0;
    const ProcessInfo& p=g_sorted[i];
    static wchar_t buf[MAX_PATH];
    if(pdi->item.mask & LVIF_TEXT){
        switch(pdi->item.iSubItem){
            case 0: swprintf(buf,32,L"%lu",p.pid); break;
            case 1: wcsncpy_s(buf,MAX_PATH,p.name.c_str(),_TRUNCATE); break;
            case 2: swprintf(buf,32,L"%.1f%%",p.cpuPercent); break;
            case 3: wcsncpy_s(buf,32,fmtBytes(p.workingSetBytes).c_str(),_TRUNCATE); break;
            case 4: wcsncpy_s(buf,32,fmtBytes(p.privateBytes).c_str(),_TRUNCATE); break;
            case 5: swprintf(buf,32,L"%lu",p.handleCount); break;
            case 6: swprintf(buf,32,L"%lu",p.threadCount); break;
            case 7: wcsncpy_s(buf,32,priorityStr(p.priorityClass).c_str(),_TRUNCATE); break;
            default: buf[0]=0; break;
        }
        pdi->item.pszText=buf;
    }
    return 0;
}

// ── ListView custom draw ──────────────────────────────────────────────────────
static LRESULT handleCustomDraw(NMLVCUSTOMDRAW* cd){
    switch(cd->nmcd.dwDrawStage){
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:{
            int idx=(int)cd->nmcd.dwItemSpec;
            bool sel=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            COLORREF bg;
            if(sel) bg=C_BG_SEL;
            else if(idx<(int)g_sorted.size()&&g_sorted[idx].isNew) bg=C_BG_NEW;
            else bg=(idx%2==0)?C_BG_ROW_E:C_BG_ROW_O;
            cd->clrTextBk=bg; cd->clrText=C_TXT_MAIN;
            if(idx<(int)g_sorted.size()&&!g_sorted[idx].accessible) cd->clrText=C_TXT_DIM;
            return CDRF_NOTIFYSUBITEMDRAW|CDRF_NEWFONT;
        }
        case CDDS_ITEMPREPAINT|CDDS_SUBITEM:{
            int idx=(int)cd->nmcd.dwItemSpec;
            if(idx>=(int)g_sorted.size()) return CDRF_DODEFAULT;
            const ProcessInfo& p=g_sorted[idx];
            bool sel=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            COLORREF bg=sel?C_BG_SEL:(p.isNew?C_BG_NEW:(idx%2==0?C_BG_ROW_E:C_BG_ROW_O));
            cd->clrTextBk=bg;
            switch(cd->iSubItem){
                case 0: cd->clrText=C_TXT_DIM; break;
                case 2:
                    cd->clrText=p.cpuPercent>50?C_RED:p.cpuPercent>20?C_YELLOW:
                                p.cpuPercent>2?C_GREEN:C_TXT_DIM; break;
                case 3:{
                    SIZE_T mb=p.workingSetBytes/1048576;
                    cd->clrText=mb>1000?C_RED:mb>200?C_YELLOW:mb>10?C_ACCENT_PURP:C_TXT_DIM;
                } break;
                case 4:{
                    SIZE_T mb=p.privateBytes/1048576;
                    cd->clrText=mb>1000?C_RED:mb>200?C_YELLOW:mb>10?C_ACCENT_CYAN:C_TXT_DIM;
                } break;
                case 5:
                    cd->clrText=p.handleCount>5000?C_RED:p.handleCount>1000?C_YELLOW:C_TXT_MAIN; break;
                case 6:
                    cd->clrText=p.threadCount>50?C_ORANGE:p.threadCount>20?C_YELLOW:C_TXT_MAIN; break;
                case 7:
                    cd->clrText=p.priorityClass==REALTIME_PRIORITY_CLASS?C_RED:
                                p.priorityClass==HIGH_PRIORITY_CLASS?C_ORANGE:
                                p.priorityClass==ABOVE_NORMAL_PRIORITY_CLASS?C_YELLOW:C_TXT_MAIN; break;
                default: cd->clrText=C_TXT_MAIN; break;
            }
            return CDRF_NEWFONT;
        }
    }
    return CDRF_DODEFAULT;
}

// ── Kill process ──────────────────────────────────────────────────────────────
static void killSelected(bool confirm=true){
    int idx=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
    if(idx<0||idx>=(int)g_sorted.size()){ MessageBeep(MB_ICONWARNING); return; }
    DWORD pid=g_sorted[idx].pid; if(pid==0||pid==4) return;
    if(confirm){
        wchar_t msg[256];
        swprintf(msg,256,L"Kill process '%s' (PID %lu)?",g_sorted[idx].name.c_str(),pid);
        if(MessageBoxW(g_hWnd,msg,L"Confirm Kill",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
    }
    HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pid);
    if(h){
        if(TerminateProcess(h,1)){
            wchar_t sb[80]; swprintf(sb,80,L"  ✓ Killed PID %lu (%s)",pid,g_sorted[idx].name.c_str());
            SetWindowTextW(g_hStatus,sb);
        } else SetWindowTextW(g_hStatus,L"  ✗ Failed to kill (access denied)");
        CloseHandle(h);
    } else SetWindowTextW(g_hStatus,L"  ✗ Cannot open process (access denied)");
}

// ── Export CSV ────────────────────────────────────────────────────────────────
static void exportToCSV(){
    wchar_t path[MAX_PATH]=L"procmon_export.csv";
    OPENFILENAMEW ofn={};
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hWnd;
    ofn.lpstrFilter=L"CSV Files\0*.csv\0All Files\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
    ofn.lpstrDefExt=L"csv";
    ofn.Flags=OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameW(&ofn)) return;

    FILE* f=nullptr; _wfopen_s(&f,path,L"w,ccs=UTF-8");
    if(!f){ MessageBoxW(g_hWnd,L"Cannot open file for writing.",L"Export Error",MB_ICONERROR); return; }
    fwprintf(f,L"PID,Name,CPU%%,WorkingSet(MB),Private(MB),Handles,Threads,Priority,Path\n");
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for(auto& p:g_procs){
            std::wstring path2=getProcessPath(p.pid);
            fwprintf(f,L"%lu,\"%s\",%.2f,%.2f,%.2f,%lu,%lu,\"%s\",\"%s\"\n",
                p.pid,p.name.c_str(),p.cpuPercent,
                p.workingSetBytes/1048576.0,p.privateBytes/1048576.0,
                p.handleCount,p.threadCount,
                priorityStr(p.priorityClass).c_str(),
                path2.c_str());
        }
    }
    fclose(f);
    wchar_t msg[256]; swprintf(msg,256,L"Exported %zu processes to:\n%s",(size_t)g_procs.size(),path);
    MessageBoxW(g_hWnd,msg,L"Export Successful",MB_ICONINFORMATION);
}

// ── Context menu ─────────────────────────────────────────────────────────────
static void showContextMenu(int x,int y){
    int idx=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
    if(idx<0||idx>=(int)g_sorted.size()) return;
    const ProcessInfo& p=g_sorted[idx];

    HMENU hM=CreatePopupMenu();
    wchar_t hdr[80]; swprintf(hdr,80,L"PID %lu — %s",p.pid,p.name.c_str());
    AppendMenuW(hM,MF_STRING|MF_GRAYED,0,hdr);
    AppendMenuW(hM,MF_SEPARATOR,0,nullptr);
    AppendMenuW(hM,MF_STRING,IDM_KILL,         L"⊗  Kill Process\tDel");
    AppendMenuW(hM,MF_SEPARATOR,0,nullptr);
    AppendMenuW(hM,MF_STRING,IDM_COPY_NAME,    L"📋  Copy Name");
    AppendMenuW(hM,MF_STRING,IDM_COPY_PID,     L"📋  Copy PID");
    AppendMenuW(hM,MF_STRING,IDM_COPY_PATH,    L"📋  Copy Full Path");
    AppendMenuW(hM,MF_STRING,IDM_OPEN_LOCATION,L"📂  Open File Location");
    AppendMenuW(hM,MF_SEPARATOR,0,nullptr);

    HMENU hPri=CreatePopupMenu();
    DWORD pc=p.priorityClass;
    AppendMenuW(hPri,MF_STRING|(pc==REALTIME_PRIORITY_CLASS?MF_CHECKED:0), IDM_PRIORITY_REALTIME,L"Realtime");
    AppendMenuW(hPri,MF_STRING|(pc==HIGH_PRIORITY_CLASS?MF_CHECKED:0),     IDM_PRIORITY_HIGH,    L"High");
    AppendMenuW(hPri,MF_STRING|(pc==ABOVE_NORMAL_PRIORITY_CLASS?MF_CHECKED:0),IDM_PRIORITY_ABOVE,L"Above Normal");
    AppendMenuW(hPri,MF_STRING|(pc==NORMAL_PRIORITY_CLASS?MF_CHECKED:0),   IDM_PRIORITY_NORMAL,  L"Normal");
    AppendMenuW(hPri,MF_STRING|(pc==BELOW_NORMAL_PRIORITY_CLASS?MF_CHECKED:0),IDM_PRIORITY_BELOW,L"Below Normal");
    AppendMenuW(hPri,MF_STRING|(pc==IDLE_PRIORITY_CLASS?MF_CHECKED:0),     IDM_PRIORITY_IDLE,    L"Idle");
    AppendMenuW(hM,MF_POPUP,(UINT_PTR)hPri,L"🔧  Set Priority");

    TrackPopupMenu(hM,TPM_RIGHTBUTTON,x,y,0,g_hWnd,nullptr);
    DestroyMenu(hM);
}

// ── set priority helper ───────────────────────────────────────────────────────
static void setPriority(DWORD newCls){
    int idx=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
    if(idx<0||idx>=(int)g_sorted.size()) return;
    HANDLE h=OpenProcess(PROCESS_SET_INFORMATION,FALSE,g_sorted[idx].pid);
    if(h){ SetPriorityClass(h,newCls); CloseHandle(h); }
}

static void copyToClipboard(const std::wstring& text){
    if(!OpenClipboard(g_hWnd)) return;
    EmptyClipboard();
    HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(hg){ wcscpy_s((wchar_t*)GlobalLock(hg),text.size()+1,text.c_str()); GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT,hg); }
    CloseClipboard();
}

// ── Column click → sort ───────────────────────────────────────────────────────
static void onColClick(int col){
    SortBy sb=g_sortBy;
    switch(col){
        case 0: sb=SORT_PID;     break;
        case 1: sb=SORT_NAME;    break;
        case 2: sb=SORT_CPU;     break;
        case 3: sb=SORT_MEM;     break;
        case 4: sb=SORT_PRIVATE; break;
        case 5: sb=SORT_HANDLES; break;
        case 6: sb=SORT_THREADS; break;
        default: return;
    }
    if(g_sortBy==sb) g_sortAsc=!g_sortAsc;
    else{ g_sortBy=sb; g_sortAsc=(sb==SORT_PID||sb==SORT_NAME); }
    updateColumnHeaders();
    populateListView();
}

// ── Paint header panel ────────────────────────────────────────────────────────
static void PaintPanel(HDC hdc, int W){
    RECT pr={0,0,W,PANEL_H};
    FillRect(hdc,&pr,g_brPanel);

    // Title
    SelectObject(hdc,g_fTitle);
    SetBkMode(hdc,TRANSPARENT);
    SetTextColor(hdc,C_ACCENT_BLUE);
    RECT tr={12,5,460,26};
    DrawTextW(hdc,L"▸ PROCMON  —  Real-Time Process Monitor",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    SysInfo si; { std::lock_guard<std::mutex> lk(g_mtx); si=g_si; }

    // Keyboard hints (right side of title bar)
    SetTextColor(hdc,C_TXT_DIM);
    SelectObject(hdc,g_fSmall);
    RECT hr={460,5,W-8,26};
    DrawTextW(hdc,L"C=CPU  M=Mem  P=PID  N=Name  H=Handles  T=Threads  |  Del=Kill  Space=Pause  Ctrl+E=Export",
              -1,&hr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

    // --- Gauges row ---
    int gW=(W-20)/3-5, gH=26, gT=30, mg=10;
    wchar_t v[64];
    swprintf(v,64,L"%.1f%%  |  %d cores",si.cpuPercent,si.cpuLogicalCount);
    RECT gc={mg,gT,mg+gW,gT+gH};
    DrawGauge(hdc,gc,si.cpuPercent,gaugeClr(si.cpuPercent),L"⬡ CPU",v,g_fBold);

    swprintf(v,64,L"%.1f%%  %s / %s",si.memPercent,fmtKB(si.memUsedKB).c_str(),fmtKB(si.memTotalKB).c_str());
    RECT gm={mg+gW+5,gT,mg+gW*2+5,gT+gH};
    DrawGauge(hdc,gm,si.memPercent,gaugeClr(si.memPercent),L"⬡ MEM",v,g_fBold);

    swprintf(v,64,L"%.1f%%  %s / %s",si.pagePercent,fmtKB(si.pageUsedKB).c_str(),fmtKB(si.pageTotalKB).c_str());
    RECT gp={mg+gW*2+10,gT,W-mg,gT+gH};
    DrawGauge(hdc,gp,si.pagePercent,gaugeClr(si.pagePercent),L"⬡ PGF",v,g_fBold);

    // --- History graphs (lower part of panel) ---
    int graphTop=gT+gH+6, graphH=PANEL_H-graphTop-6;
    int gGW=(W-20)/3-5;

    std::deque<double> cpuH,memH,pgfH;
    { std::lock_guard<std::mutex> lk(g_mtx); cpuH=g_cpuHist; memH=g_memHist; pgfH=g_pgfHist; }

    // Graph backgrounds
    RECT bg1={mg,graphTop,mg+gGW,graphTop+graphH};
    RECT bg2={mg+gGW+5,graphTop,mg+gGW*2+5,graphTop+graphH};
    RECT bg3={mg+gGW*2+10,graphTop,W-mg,graphTop+graphH};
    FillRect(hdc,&bg1,g_brGraph); FillRect(hdc,&bg2,g_brGraph); FillRect(hdc,&bg3,g_brGraph);

    // Grid lines (horizontal at 25%, 50%, 75%)
    HPEN penGrid=CreatePen(PS_SOLID,1,C_GRID);
    SelectObject(hdc,penGrid);
    for(const RECT& gr:{bg1,bg2,bg3}){
        for(int pct:{25,50,75}){
            int gy=gr.bottom-(int)(pct/100.0*(gr.bottom-gr.top));
            MoveToEx(hdc,gr.left,gy,nullptr); LineTo(hdc,gr.right,gy);
        }
    }
    DeleteObject(penGrid);

    // Draw graphs (fill color is very dim version of line color)
    DrawHistoryGraph(hdc,bg1,cpuH, C_GRAPH_CPU, RGB(20,50,90),  L"CPU");
    DrawHistoryGraph(hdc,bg2,memH, C_GRAPH_MEM, RGB(45,25,90),  L"MEM");
    DrawHistoryGraph(hdc,bg3,pgfH, C_GRAPH_PGF, RGB(80,40,10),  L"PGF");

    // Separator
    HPEN penSep=CreatePen(PS_SOLID,1,C_SEP);
    HPEN pOld=(HPEN)SelectObject(hdc,penSep);
    MoveToEx(hdc,0,PANEL_H-1,nullptr); LineTo(hdc,W,PANEL_H-1);
    SelectObject(hdc,pOld); DeleteObject(penSep);
}

// ── Layout ────────────────────────────────────────────────────────────────────
static void DoLayout(HWND hWnd){
    RECT rc; GetClientRect(hWnd,&rc);
    int W=rc.right, H=rc.bottom;
    int tbTop=PANEL_H+4;
    // toolbar
    SetWindowPos(g_hSearch,   nullptr, 10,tbTop+7,  220,26, SWP_NOZORDER);
    SetWindowPos(g_hBtnKill,  nullptr,240,tbTop+5,   90,30, SWP_NOZORDER);
    SetWindowPos(g_hBtnPause, nullptr,338,tbTop+5,  105,30, SWP_NOZORDER);
    SetWindowPos(g_hBtnExport,nullptr,451,tbTop+5,  100,30, SWP_NOZORDER);
    SetWindowPos(g_hBtnOnTop, nullptr,559,tbTop+5,  100,30, SWP_NOZORDER);
    // label "Refresh:"
    int cbX=670;
    SetWindowPos(g_hComboRate,nullptr,cbX,tbTop+7,  110,120, SWP_NOZORDER);
    // list
    int listTop=tbTop+TOOLBAR_H;
    SetWindowPos(g_hList,nullptr,0,listTop,W,H-listTop-STATUS_H,SWP_NOZORDER);
    SetWindowPos(g_hStatus,nullptr,0,H-STATUS_H,W,STATUS_H,SWP_NOZORDER);
}

// ── Custom button painting ─────────────────────────────────────────────────────
struct BtnState{ bool hot=false,press=false; };
enum BtnRole{ BR_KILL, BR_PAUSE, BR_EXPORT, BR_ONTOP, BR__COUNT };
static BtnState g_bst[BR__COUNT];

static void PaintCustomBtn(HWND hWnd,BtnState& st,const wchar_t* text,COLORREF tclr){
    PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
    RECT rc; GetClientRect(hWnd,&rc);
    COLORREF bg=st.press?C_BTN_ACT:st.hot?C_BTN_HOT:C_BTN_BG;
    FillRoundRect(hdc,rc,8,bg);
    HPEN pen=CreatePen(PS_SOLID,1,C_SEP);
    HPEN po=(HPEN)SelectObject(hdc,pen);
    HBRUSH bro=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
    RoundRect(hdc,rc.left,rc.top,rc.right-1,rc.bottom-1,8,8);
    SelectObject(hdc,po); SelectObject(hdc,bro); DeleteObject(pen);
    SetBkMode(hdc,TRANSPARENT); SelectObject(hdc,g_fBold);
    SetTextColor(hdc,tclr);
    DrawTextW(hdc,text,-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    EndPaint(hWnd,&ps);
}

static LRESULT CALLBACK GenBtnProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    int role=(int)(INT_PTR)GetWindowLongPtrW(hWnd,GWLP_USERDATA);
    BtnState& st=g_bst[role];
    switch(msg){
        case WM_PAINT:{
            const wchar_t* texts[BR__COUNT]={L"⊗ Kill",nullptr,L"⬇ Export CSV",nullptr};
            COLORREF tclrs[BR__COUNT]={C_RED,C_YELLOW,C_ACCENT_CYAN,C_ACCENT_BLUE};
            if(role==BR_PAUSE){
                bool p=g_paused.load();
                PaintCustomBtn(hWnd,st,p?L"▶ Resume":L"⏸ Pause",p?C_GREEN:C_YELLOW);
            } else if(role==BR_ONTOP){
                PaintCustomBtn(hWnd,st,g_alwaysOnTop?L"📌 On Top ✓":L"📌 On Top",C_ACCENT_BLUE);
            } else {
                PaintCustomBtn(hWnd,st,texts[role],tclrs[role]);
            }
        } return 0;
        case WM_MOUSEMOVE:
            if(!st.hot){ st.hot=true; InvalidateRect(hWnd,nullptr,FALSE);
                TRACKMOUSEEVENT t={sizeof(t),TME_LEAVE,hWnd,0}; TrackMouseEvent(&t); }
            return 0;
        case WM_MOUSELEAVE: st.hot=false; st.press=false; InvalidateRect(hWnd,nullptr,FALSE); return 0;
        case WM_LBUTTONDOWN: st.press=true; InvalidateRect(hWnd,nullptr,FALSE); SetCapture(hWnd); return 0;
        case WM_LBUTTONUP:{
            st.press=false; InvalidateRect(hWnd,nullptr,FALSE); ReleaseCapture();
            if(st.hot){
                switch(role){
                    case BR_KILL:   SendMessage(GetParent(hWnd),WM_COMMAND,IDM_KILL,0); break;
                    case BR_PAUSE:  SendMessage(GetParent(hWnd),WM_COMMAND,IDC_BTN_PAUSE,0); break;
                    case BR_EXPORT: SendMessage(GetParent(hWnd),WM_COMMAND,IDC_BTN_EXPORT,0); break;
                    case BR_ONTOP:  SendMessage(GetParent(hWnd),WM_COMMAND,IDC_BTN_ONTOP,0); break;
                }
            }
        } return 0;
        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ── Search subclass ───────────────────────────────────────────────────────────
static WNDPROC g_origEdit=nullptr;
static LRESULT CALLBACK EditProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){ SetWindowTextW(hWnd,L""); return 0; }
    if(msg==WM_KEYDOWN&&wp==VK_RETURN){ SetFocus(g_hList); return 0; }
    return CallWindowProcW(g_origEdit,hWnd,msg,wp,lp);
}

// ── Tooltip for process path ──────────────────────────────────────────────────
static HWND g_hTip=nullptr;
static void initTooltip(HWND hWnd){
    g_hTip=CreateWindowExW(0,TOOLTIPS_CLASSW,nullptr,
        WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        0,0,0,0,hWnd,nullptr,g_hInst,nullptr);
    TOOLINFOW ti={sizeof(ti),TTF_SUBCLASS,hWnd,1,{0,0,0,0},nullptr,nullptr,(LPARAM)L""};
    GetClientRect(hWnd,&ti.rect);
    SendMessage(g_hTip,TTM_ADDTOOLW,0,(LPARAM)&ti);
    SendMessage(g_hTip,TTM_SETMAXTIPWIDTH,0,600);
}

// ── Main WndProc ──────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){

    // ── WM_CREATE ──────────────────────────────────────────────────────────────
    case WM_CREATE:{
        // Fonts
        LOGFONTW lf={}; lf.lfCharSet=DEFAULT_CHARSET; wcscpy_s(lf.lfFaceName,L"Segoe UI");
        lf.lfHeight=-13; lf.lfWeight=FW_NORMAL; g_fMain =CreateFontIndirectW(&lf);
        lf.lfWeight=FW_BOLD;                     g_fBold =CreateFontIndirectW(&lf);
        lf.lfHeight=-11; lf.lfWeight=FW_NORMAL;  g_fSmall=CreateFontIndirectW(&lf);
        lf.lfHeight=-16; lf.lfWeight=FW_BOLD;    g_fTitle=CreateFontIndirectW(&lf);
        lf.lfHeight=-12; lf.lfWeight=FW_NORMAL;
        wcscpy_s(lf.lfFaceName,L"Consolas");     g_fMono =CreateFontIndirectW(&lf);

        // Brushes
        g_brDark =CreateSolidBrush(C_BG_DARK);
        g_brMid  =CreateSolidBrush(C_BG_MID);
        g_brPanel=CreateSolidBrush(C_BG_PANEL);
        g_brSel  =CreateSolidBrush(C_BG_SEL);
        g_brGraph=CreateSolidBrush(C_BG_GRAPH);

        // ListView
        g_hList=CreateWindowExW(0,WC_LISTVIEWW,L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
            0,0,0,0,hWnd,(HMENU)IDC_LISTVIEW,g_hInst,nullptr);
        SendMessage(g_hList,WM_SETFONT,(WPARAM)g_fMono,TRUE);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_HEADERDRAGDROP|LVS_EX_GRIDLINES);
        ListView_SetBkColor(g_hList,C_BG_ROW_E);
        ListView_SetTextBkColor(g_hList,C_BG_ROW_E);
        ListView_SetTextColor(g_hList,C_TXT_MAIN);

        // Columns
        struct ColDef{ const wchar_t* name; int w; int fmt; };
        ColDef cols[]={
            {L"PID",      75, LVCFMT_RIGHT},
            {L"Name",    180, LVCFMT_LEFT},
            {L"CPU %",    78, LVCFMT_RIGHT},
            {L"Working Set",105, LVCFMT_RIGHT},
            {L"Private",  100, LVCFMT_RIGHT},
            {L"Handles",   75, LVCFMT_RIGHT},
            {L"Threads",   70, LVCFMT_RIGHT},
            {L"Priority", 110, LVCFMT_LEFT},
        };
        for(int i=0;i<8;i++){
            LVCOLUMNW lvc={LVCF_TEXT|LVCF_WIDTH|LVCF_FMT, cols[i].fmt, cols[i].w,(LPWSTR)cols[i].name,0,i};
            ListView_InsertColumn(g_hList,i,&lvc);
        }

        // Search
        g_hSearch=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,0,0,hWnd,(HMENU)IDC_SEARCH,g_hInst,nullptr);
        SendMessage(g_hSearch,WM_SETFONT,(WPARAM)g_fMain,TRUE);
        SendMessage(g_hSearch,EM_SETCUEBANNER,TRUE,(LPARAM)L"🔍 Filter processes...");
        g_origEdit=(WNDPROC)SetWindowLongPtrW(g_hSearch,GWLP_WNDPROC,(LONG_PTR)EditProc);

        // Custom buttons
        WNDCLASSW wc={CS_HREDRAW|CS_VREDRAW,GenBtnProc,0,0,g_hInst,nullptr,
                      LoadCursorW(nullptr,IDC_HAND),nullptr,nullptr,L"ProcBtn"};
        RegisterClassW(&wc);
        auto makeBtn=[&](BtnRole r,HMENU id)->HWND{
            HWND h=CreateWindowW(L"ProcBtn",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,hWnd,id,g_hInst,nullptr);
            SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)r);
            return h;
        };
        g_hBtnKill  =makeBtn(BR_KILL,  (HMENU)IDC_BTN_KILL);
        g_hBtnPause =makeBtn(BR_PAUSE, (HMENU)IDC_BTN_PAUSE);
        g_hBtnExport=makeBtn(BR_EXPORT,(HMENU)IDC_BTN_EXPORT);
        g_hBtnOnTop =makeBtn(BR_ONTOP, (HMENU)IDC_BTN_ONTOP);

        // Refresh rate combo
        g_hComboRate=CreateWindowW(WC_COMBOBOXW,L"",
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            0,0,0,0,hWnd,(HMENU)IDC_COMBO_RATE,g_hInst,nullptr);
        SendMessage(g_hComboRate,WM_SETFONT,(WPARAM)g_fSmall,TRUE);
        const wchar_t* rates[]={L"0.5s",L"1s",L"1.5s",L"2s",L"3s",L"5s"};
        for(auto r:rates) SendMessage(g_hComboRate,CB_ADDSTRING,0,(LPARAM)r);
        SendMessage(g_hComboRate,CB_SETCURSEL,2,0); // default 1.5s

        // Status bar
        g_hStatus=CreateWindowExW(0,STATUSCLASSNAMEW,L"  Initializing...",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,0,0,0,0,
            hWnd,(HMENU)IDC_STATUS_BAR,g_hInst,nullptr);
        SendMessage(g_hStatus,WM_SETFONT,(WPARAM)g_fSmall,TRUE);

        initTooltip(hWnd);
        addTrayIcon();
        SetTimer(hWnd,TIMER_REFRESH,400,nullptr);
        return 0;
    }

    case WM_SIZE:
        DoLayout(hWnd);
        SendMessage(g_hStatus,WM_SIZE,wp,lp);
        return 0;

    case WM_TIMER:
        if(wp==TIMER_REFRESH){
            RECT rc; GetClientRect(hWnd,&rc); rc.bottom=PANEL_H;
            InvalidateRect(hWnd,&rc,FALSE);
            updateStatus();
            InvalidateRect(g_hBtnPause,nullptr,FALSE);
            InvalidateRect(g_hBtnOnTop,nullptr,FALSE);
        }
        return 0;

    case WM_NEWDATA:
        if(g_dataReady){ populateListView(); updateStatus(); updateColumnHeaders(); g_dataReady=false; }
        return 0;

    case WM_COMMAND:
        switch(LOWORD(wp)){
            case IDC_SEARCH:
                if(HIWORD(wp)==EN_CHANGE) populateListView();
                break;
            case IDM_KILL: killSelected(); break;
            case IDC_BTN_PAUSE:
                g_paused=!g_paused.load();
                InvalidateRect(g_hBtnPause,nullptr,FALSE); updateStatus(); break;
            case IDC_BTN_EXPORT: exportToCSV(); break;
            case IDC_BTN_ONTOP:
                g_alwaysOnTop=!g_alwaysOnTop;
                SetWindowPos(hWnd,g_alwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,
                    0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
                InvalidateRect(g_hBtnOnTop,nullptr,FALSE); break;
            case IDC_COMBO_RATE:
                if(HIWORD(wp)==CBN_SELCHANGE){
                    int sel=(int)SendMessage(g_hComboRate,CB_GETCURSEL,0,0);
                    int ms[]={500,1000,1500,2000,3000,5000};
                    if(sel>=0&&sel<6) g_refreshMs=ms[sel];
                }
                break;
            case IDM_COPY_NAME:{
                int i=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
                if(i>=0&&i<(int)g_sorted.size()) copyToClipboard(g_sorted[i].name);
            } break;
            case IDM_COPY_PID:{
                int i=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
                if(i>=0&&i<(int)g_sorted.size()) copyToClipboard(std::to_wstring(g_sorted[i].pid));
            } break;
            case IDM_COPY_PATH:{
                int i=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
                if(i>=0&&i<(int)g_sorted.size()) copyToClipboard(getProcessPath(g_sorted[i].pid));
            } break;
            case IDM_OPEN_LOCATION:{
                int i=ListView_GetNextItem(g_hList,-1,LVNI_SELECTED);
                if(i>=0&&i<(int)g_sorted.size()){
                    std::wstring path=getProcessPath(g_sorted[i].pid);
                    if(!path.empty()){
                        std::wstring arg=L"/select,\""+path+L"\"";
                        ShellExecuteW(hWnd,L"open",L"explorer.exe",arg.c_str(),nullptr,SW_SHOW);
                    }
                }
            } break;
            case IDM_PRIORITY_REALTIME: setPriority(REALTIME_PRIORITY_CLASS); break;
            case IDM_PRIORITY_HIGH:     setPriority(HIGH_PRIORITY_CLASS); break;
            case IDM_PRIORITY_ABOVE:    setPriority(ABOVE_NORMAL_PRIORITY_CLASS); break;
            case IDM_PRIORITY_NORMAL:   setPriority(NORMAL_PRIORITY_CLASS); break;
            case IDM_PRIORITY_BELOW:    setPriority(BELOW_NORMAL_PRIORITY_CLASS); break;
            case IDM_PRIORITY_IDLE:     setPriority(IDLE_PRIORITY_CLASS); break;
            case IDM_TRAY_SHOW:
                ShowWindow(hWnd,SW_RESTORE); SetForegroundWindow(hWnd); break;
            case IDM_TRAY_EXIT: DestroyWindow(hWnd); break;
        }
        return 0;

    case WM_KEYDOWN:
        switch(wp){
            case VK_DELETE: killSelected(); break;
            case 'C': g_sortBy=SORT_CPU;     g_sortAsc=false; updateColumnHeaders(); populateListView(); break;
            case 'M': g_sortBy=SORT_MEM;     g_sortAsc=false; updateColumnHeaders(); populateListView(); break;
            case 'P': g_sortBy=SORT_PID;     g_sortAsc=true;  updateColumnHeaders(); populateListView(); break;
            case 'N': g_sortBy=SORT_NAME;    g_sortAsc=true;  updateColumnHeaders(); populateListView(); break;
            case 'H': g_sortBy=SORT_HANDLES; g_sortAsc=false; updateColumnHeaders(); populateListView(); break;
            case 'T': g_sortBy=SORT_THREADS; g_sortAsc=false; updateColumnHeaders(); populateListView(); break;
            case 'Q': DestroyWindow(hWnd); break;
            case VK_SPACE: SendMessage(hWnd,WM_COMMAND,IDC_BTN_PAUSE,0); break;
            case VK_F5: if(!g_paused){ g_dataReady=false; PostMessage(hWnd,WM_NEWDATA,0,0); } break;
            case VK_ESCAPE:{ // clear filter
                if(GetWindowTextLengthW(g_hSearch)>0) SetWindowTextW(g_hSearch,L"");
                else{ ListView_SetItemState(g_hList,-1,0,LVIS_SELECTED); }
            } break;
            case 'E': if(GetKeyState(VK_CONTROL)&0x8000) exportToCSV(); break;
            case 'F': if(GetKeyState(VK_CONTROL)&0x8000){ SetFocus(g_hSearch); } break;
            case VK_OEM_2: SetFocus(g_hSearch); break; // '/' key focuses search
        }
        break;

    case WM_NOTIFY:{
        NMHDR* h=(NMHDR*)lp;
        if(h->idFrom==IDC_LISTVIEW){
            switch(h->code){
                case LVN_GETDISPINFOW: handleGetDispInfo((NMLVDISPINFOW*)lp); break;
                case NM_CUSTOMDRAW:    return handleCustomDraw((NMLVCUSTOMDRAW*)lp);
                case LVN_COLUMNCLICK:  onColClick(((NMLISTVIEW*)lp)->iSubItem); break;
                case NM_DBLCLK:        killSelected(); break;
                case NM_RCLICK:{
                    NMITEMACTIVATE* na=(NMITEMACTIVATE*)lp;
                    POINT pt; GetCursorPos(&pt);
                    if(na->iItem>=0){
                        ListView_SetItemState(g_hList,na->iItem,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                        showContextMenu(pt.x,pt.y);
                    }
                } break;
                case LVN_KEYDOWN:{
                    NMLVKEYDOWN* kd=(NMLVKEYDOWN*)lp;
                    // Pass key events to parent for sort shortcuts
                    SendMessage(hWnd,WM_KEYDOWN,kd->wVKey,0);
                } break;
            }
        }
        break;
    }

    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc);
        // Double-buffer the header panel
        HDC mem=CreateCompatibleDC(hdc);
        HBITMAP bmp=CreateCompatibleBitmap(hdc,rc.right,PANEL_H);
        HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
        PaintPanel(mem,rc.right);
        BitBlt(hdc,0,0,rc.right,PANEL_H,mem,0,0,SRCCOPY);
        SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
        // Toolbar bg
        RECT tb={0,PANEL_H,rc.right,PANEL_H+TOOLBAR_H+6};
        FillRect(hdc,&tb,g_brMid);
        // Toolbar hint label
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,C_TXT_DIM); SelectObject(hdc,g_fSmall);
        RECT rl={670-50,PANEL_H+12,670,PANEL_H+28};
        DrawTextW(hdc,L"Rate:",-1,&rl,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        EndPaint(hWnd,&ps);
        return 0;
    }

    case WM_ERASEBKGND:{
        HDC hdc=(HDC)wp; RECT rc; GetClientRect(hWnd,&rc);
        FillRect(hdc,&rc,g_brDark); return 1;
    }

    case WM_CTLCOLOREDIT:{
        HDC hdc=(HDC)wp;
        SetBkColor(hdc,C_BG_MID); SetTextColor(hdc,C_TXT_MAIN);
        return (LRESULT)g_brMid;
    }
    case WM_CTLCOLORLISTBOX:{
        HDC hdc=(HDC)wp;
        SetBkColor(hdc,C_BG_MID); SetTextColor(hdc,C_TXT_MAIN);
        return (LRESULT)g_brMid;
    }

    case WM_TRAYICON:
        if(lp==WM_LBUTTONDBLCLK){
            ShowWindow(hWnd,SW_RESTORE); SetForegroundWindow(hWnd);
        } else if(lp==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt);
            HMENU hM=CreatePopupMenu();
            AppendMenuW(hM,MF_STRING,IDM_TRAY_SHOW,L"Show PROCMON");
            AppendMenuW(hM,MF_SEPARATOR,0,nullptr);
            AppendMenuW(hM,MF_STRING,IDM_TRAY_EXIT,L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hM,TPM_RIGHTBUTTON,pt.x,pt.y,0,hWnd,nullptr);
            DestroyMenu(hM);
        }
        return 0;

    case WM_SYSCOMMAND:
        if((wp&0xFFF0)==SC_MINIMIZE){
            ShowWindow(hWnd,SW_HIDE);
            return 0; // minimize to tray
        }
        break;

    case WM_DESTROY:
        g_running=false;
        KillTimer(hWnd,TIMER_REFRESH);
        removeTrayIcon();
        if(g_pdhQuery) PdhCloseQuery(g_pdhQuery);
        DeleteObject(g_fMain); DeleteObject(g_fBold); DeleteObject(g_fSmall);
        DeleteObject(g_fTitle); DeleteObject(g_fMono);
        DeleteObject(g_brDark); DeleteObject(g_brMid); DeleteObject(g_brPanel);
        DeleteObject(g_brSel); DeleteObject(g_brGraph);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ── WinMain ───────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int nCmdShow){
    g_hInst=hInst;
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    initPDH();
    SYSTEM_INFO si{}; GetSystemInfo(&si); g_cpuCount=(int)si.dwNumberOfProcessors;
    readAllProcesses(); Sleep(1000); // prime CPU delta

    WNDCLASSEXW wc={sizeof(wc),CS_HREDRAW|CS_VREDRAW,WndProc,0,0,hInst,
                    LoadIconW(nullptr,IDI_APPLICATION),
                    LoadCursorW(nullptr,IDC_ARROW),
                    (HBRUSH)(COLOR_WINDOW+1),nullptr,L"ProcmonGUI2",
                    LoadIconW(nullptr,IDI_APPLICATION)};
    RegisterClassExW(&wc);

    g_hWnd=CreateWindowExW(0,L"ProcmonGUI2",
        L"PROCMON v2 — Real-Time Process Monitor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1180,800,
        nullptr,nullptr,hInst,nullptr);

    ShowWindow(g_hWnd,nCmdShow);
    UpdateWindow(g_hWnd);
    updateColumnHeaders();

    std::thread dt(dataThread);

    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){
        if(!IsDialogMessageW(g_hWnd,&msg)){
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    g_running=false; dt.join();
    return (int)msg.wParam;
}
