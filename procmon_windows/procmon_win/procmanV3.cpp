/**
 * ====================================================
 *  PROCMON v3  —  Windows 11 Task Manager Style
 *  Win32 | C++17 | GDI | PDH | PSAPI
 * ====================================================
 *  Build (MinGW):
 *    g++ -std=c++17 -O2 -static -mwindows -o procmon_gui.exe procmon_gui.cpp
 *        -lpdh -lpsapi -lntdll -luser32 -lkernel32 -lgdi32
 *        -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi
 *
 *  Navigation:
 *    Processes   – real-time process list (CPU / Memory / Disk / Network)
 *    Performance – CPU, Memory, Disk history graphs + stats
 *    Details     – full per-process info table
 *
 *  Keys:  Del=End task  Space=Pause  Ctrl+F=Find  Q=Quit
 * ====================================================
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
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cmath>

// ─── IDs ──────────────────────────────────────────────────────────────────────
#define IDC_PROC_LIST    1001
#define IDC_PROC_SEARCH  1002
#define IDC_PROC_ENDTASK 1003
#define IDC_DET_LIST     1004
#define TIMER_UI         100
#define WM_NEWDATA       (WM_USER+1)

// ─── Layout ───────────────────────────────────────────────────────────────────
#define SIDEBAR_W   220
#define NAV_TOP      56
#define NAV_H        48
#define HIST_LEN     64
#define PERF_LEFT_W 210
#define CARD_H       78

// ─── Tabs / resources ─────────────────────────────────────────────────────────
enum TabPage  { TAB_PROCESSES=0, TAB_PERFORMANCE=1, TAB_DETAILS=2, TAB_COUNT=3 };
enum PerfRes  { PERF_CPU=0, PERF_MEMORY=1, PERF_DISK=2, PERF_COUNT=3 };
enum SortCol  { SCOL_NAME=0, SCOL_CPU, SCOL_MEM, SCOL_HANDLES, SCOL_THREADS };

// ─── Windows-11 dark color palette ────────────────────────────────────────────
static const COLORREF C_BG        = RGB(28,  28,  28 );  // content bg
static const COLORREF C_SIDEBAR   = RGB(18,  18,  18 );  // sidebar
static const COLORREF C_PANEL     = RGB(36,  36,  36 );  // perf-card
static const COLORREF C_HOVER     = RGB(46,  46,  46 );
static const COLORREF C_SEL_NAV   = RGB(38,  38,  38 );  // selected sidebar item
static const COLORREF C_ACCENT    = RGB(0,  120, 212 );  // Windows blue
static const COLORREF C_SEP       = RGB(55,  55,  55 );
static const COLORREF C_TEXT      = RGB(255, 255, 255 );
static const COLORREF C_TEXT_SEC  = RGB(155, 155, 155 );
static const COLORREF C_TEXT_DIM  = RGB(90,   90,  90 );
static const COLORREF C_ROW_EVEN  = RGB(28,  28,  28 );
static const COLORREF C_ROW_ODD   = RGB(32,  32,  32 );
static const COLORREF C_ROW_SEL   = RGB(0,   62, 120 );
static const COLORREF C_ROW_NEW   = RGB(16,  48,  22 );
static const COLORREF C_GRAPH_BG  = RGB(14,  14,  14 );
static const COLORREF C_GRID      = RGB(42,  42,  42 );
static const COLORREF C_GCPU      = RGB(17, 125, 187 );  // blue
static const COLORREF C_GMEM      = RGB(139,  18, 174 );  // purple
static const COLORREF C_GDISK     = RGB(0,  163, 185 );  // teal

// ─── Data structures ──────────────────────────────────────────────────────────
struct ProcInfo {
    DWORD        pid{0}, parentPid{0};
    std::wstring name;
    double       cpu{0};
    SIZE_T       ws{0}, priv{0};
    DWORD        handles{0}, threads{0}, priClass{0};
    ULONGLONG    createTime{0};
    bool         accessible{true}, isNew{false};
};
struct SysInfo {
    double    cpuPct{0}, memPct{0}, diskPct{0};
    DWORDLONG memTotal{0}, memUsed{0}, memFree{0};
    DWORDLONG pageTotal{0}, pageUsed{0};
    double    diskRd{0}, diskWr{0};
    ULONGLONG uptime{0};
    int       procs{0}, threads{0}, handles{0}, logCpus{1};
};

// ─── Globals ──────────────────────────────────────────────────────────────────
static HINSTANCE g_inst = nullptr;
static HWND g_hMain=nullptr, g_hProcPage=nullptr, g_hPerfPage=nullptr, g_hDetPage=nullptr;
static HWND g_hProcList=nullptr, g_hProcSearch=nullptr, g_hEndTask=nullptr;
static HWND g_hDetList=nullptr;

static std::mutex           g_mtx;
static std::vector<ProcInfo> g_procs;
static SysInfo              g_si;
static std::atomic<bool>    g_running{true}, g_paused{false}, g_dataReady{false};

static TabPage  g_curTab   = TAB_PROCESSES;
static PerfRes  g_perfRes  = PERF_CPU;
static SortCol  g_sortCol  = SCOL_CPU;
static bool     g_sortAsc  = false;
static int      g_hoverNav = -1;

static std::deque<double> g_cpuH, g_memH, g_dskH;
static std::set<DWORD>    g_knownPids;

static PDH_HQUERY   g_pdhQ   = nullptr;
static PDH_HCOUNTER g_pdhCpu = nullptr, g_pdhDR = nullptr,
                    g_pdhDW  = nullptr, g_pdhDP = nullptr;
static int g_cpuCount = 1;
static std::map<DWORD,std::tuple<ULONGLONG,ULONGLONG,ULONGLONG>> g_pcpu;

static HFONT  g_fUI=nullptr, g_fBold=nullptr, g_fSm=nullptr,
              g_fTitle=nullptr, g_fGraph=nullptr;
static HBRUSH g_brBG=nullptr, g_brSide=nullptr,
              g_brPanel=nullptr, g_brGBG=nullptr;

static std::vector<ProcInfo> g_sorted, g_detSorted;

// ─── Utilities ────────────────────────────────────────────────────────────────
static ULONGLONG Ft2Ull(const FILETIME& f){
    ULARGE_INTEGER u; u.LowPart=f.dwLowDateTime; u.HighPart=f.dwHighDateTime; return u.QuadPart;
}
static std::wstring FmtMB(SIZE_T b){
    wchar_t s[32]; double mb=b/1048576.0;
    if(mb>=1024) swprintf(s,32,L"%.1f GB",mb/1024); else swprintf(s,32,L"%.1f MB",mb);
    return s;
}
static std::wstring FmtKB(DWORDLONG kb){
    wchar_t s[32]; double mb=kb/1024.0;
    if(mb>=1024) swprintf(s,32,L"%.1f GB",mb/1024); else swprintf(s,32,L"%.0f MB",mb);
    return s;
}
static std::wstring FmtBps(double bps){
    wchar_t s[32];
    if(bps>=1073741824) swprintf(s,32,L"%.1f GB/s",bps/1073741824);
    else if(bps>=1048576) swprintf(s,32,L"%.1f MB/s",bps/1048576);
    else if(bps>=1024) swprintf(s,32,L"%.0f KB/s",bps/1024);
    else swprintf(s,32,L"%.0f B/s",bps);
    return s;
}
static std::wstring FmtUp(ULONGLONG ms){
    ULONGLONG s=ms/1000,d=s/86400,h=(s%86400)/3600,m=(s%3600)/60,sc=s%60;
    wchar_t b[64];
    if(d>0) swprintf(b,64,L"%llud %02lluh %02llum",d,h,m);
    else    swprintf(b,64,L"%02lluh %02llum %02llus",h,m,sc);
    return b;
}
static std::wstring PriStr(DWORD c){
    switch(c){
        case IDLE_PRIORITY_CLASS:         return L"Idle";
        case BELOW_NORMAL_PRIORITY_CLASS: return L"Below Normal";
        case NORMAL_PRIORITY_CLASS:       return L"Normal";
        case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above Normal";
        case HIGH_PRIORITY_CLASS:         return L"High";
        case REALTIME_PRIORITY_CLASS:     return L"Realtime";
        default:                          return L"—";
    }
}
static std::wstring GetExePath(DWORD pid){
    HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!h) return {};
    wchar_t buf[MAX_PATH]={}; DWORD sz=MAX_PATH;
    QueryFullProcessImageNameW(h,0,buf,&sz);
    CloseHandle(h); return buf;
}
static COLORREF CpuClr(double p){
    if(p<5)  return C_TEXT_DIM;
    if(p<25) return RGB(80,200,120);
    if(p<60) return RGB(228,192,42);
    if(p<85) return RGB(235,118,26);
    return RGB(214,50,50);
}
static COLORREF MemClr(SIZE_T b){
    SIZE_T mb=b/1048576;
    if(mb<50)   return C_TEXT_DIM;
    if(mb<200)  return RGB(80,200,120);
    if(mb<1000) return RGB(228,192,42);
    return RGB(214,50,50);
}
static void ClipCopy(const std::wstring& t){
    if(!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,(t.size()+1)*sizeof(wchar_t));
    if(h){ wcscpy_s((wchar_t*)GlobalLock(h),t.size()+1,t.c_str()); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT,h); }
    CloseClipboard();
}

// ─── PDH ──────────────────────────────────────────────────────────────────────
static void InitPDH(){
    PdhOpenQuery(nullptr,0,&g_pdhQ);
    PdhAddEnglishCounterW(g_pdhQ,L"\\Processor(_Total)\\% Processor Time",0,&g_pdhCpu);
    PdhAddEnglishCounterW(g_pdhQ,L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",0,&g_pdhDR);
    PdhAddEnglishCounterW(g_pdhQ,L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",0,&g_pdhDW);
    PdhAddEnglishCounterW(g_pdhQ,L"\\PhysicalDisk(_Total)\\% Disk Time",0,&g_pdhDP);
    PdhCollectQueryData(g_pdhQ); Sleep(100); PdhCollectQueryData(g_pdhQ);
}
static double PdhGetD(PDH_HCOUNTER c){
    PDH_FMT_COUNTERVALUE v{}; PdhGetFormattedCounterValue(c,PDH_FMT_DOUBLE,nullptr,&v);
    return std::max(0.0,v.doubleValue);
}
static double CalcPCpu(DWORD pid,HANDLE h){
    FILETIME c,e,k,u; if(!GetProcessTimes(h,&c,&e,&k,&u)) return 0;
    ULONGLONG kN=Ft2Ull(k),uN=Ft2Ull(u);
    FILETIME fn; GetSystemTimeAsFileTime(&fn); ULONGLONG wN=Ft2Ull(fn);
    auto it=g_pcpu.find(pid);
    if(it==g_pcpu.end()){ g_pcpu[pid]={kN,uN,wN}; return 0; }
    auto [kP,uP,wP]=it->second; g_pcpu[pid]={kN,uN,wN};
    ULONGLONG dC=(kN-kP)+(uN-uP),dW=wN-wP;
    return dW?std::min(100.0*(double)g_cpuCount,100.0*dC/dW):0;
}

// ─── Data collection ──────────────────────────────────────────────────────────
static void ReadSys(SysInfo& si){
    PdhCollectQueryData(g_pdhQ);
    si.cpuPct  = std::min(100.0,PdhGetD(g_pdhCpu));
    si.diskRd  = PdhGetD(g_pdhDR);
    si.diskWr  = PdhGetD(g_pdhDW);
    si.diskPct = std::min(100.0,PdhGetD(g_pdhDP));
    MEMORYSTATUSEX ms{}; ms.dwLength=sizeof(ms); GlobalMemoryStatusEx(&ms);
    si.memTotal = ms.ullTotalPhys/1024;
    si.memFree  = ms.ullAvailPhys/1024;
    si.memUsed  = si.memTotal-si.memFree;
    si.memPct   = si.memTotal?100.0*si.memUsed/si.memTotal:0;
    si.pageTotal= ms.ullTotalPageFile/1024;
    si.pageUsed = (ms.ullTotalPageFile-ms.ullAvailPageFile)/1024;
    si.uptime   = GetTickCount64();
    SYSTEM_INFO s2{}; GetSystemInfo(&s2); si.logCpus=(int)s2.dwNumberOfProcessors;
}
static std::vector<ProcInfo> ReadProcs(){
    std::vector<ProcInfo> out; out.reserve(300);
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize=sizeof(pe);
    if(!Process32FirstW(snap,&pe)){ CloseHandle(snap); return out; }
    do {
        ProcInfo p; p.pid=pe.th32ProcessID; p.parentPid=pe.th32ParentProcessID;
        p.name=pe.szExeFile; p.threads=(DWORD)pe.cntThreads;
        if(!p.pid){ out.push_back(p); continue; }
        HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,p.pid);
        if(!h){ p.accessible=false; out.push_back(p); continue; }
        p.cpu=CalcPCpu(p.pid,h);
        PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb=sizeof(pmc);
        if(GetProcessMemoryInfo(h,(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc))){
            p.ws=pmc.WorkingSetSize; p.priv=pmc.PrivateUsage;
        }
        GetProcessHandleCount(h,&p.handles);
        p.priClass=GetPriorityClass(h);
        FILETIME c,e,k,u; if(GetProcessTimes(h,&c,&e,&k,&u)) p.createTime=Ft2Ull(c);
        CloseHandle(h); out.push_back(p);
    } while(Process32NextW(snap,&pe));
    CloseHandle(snap); return out;
}

// ─── Background thread ────────────────────────────────────────────────────────
static void DataThread(){
    while(g_running){
        if(!g_paused){
            SysInfo si; ReadSys(si);
            auto procs=ReadProcs();
            si.procs=(int)procs.size();
            for(auto& p:procs){ si.threads+=p.threads; si.handles+=p.handles; }
            std::set<DWORD> cur;
            for(auto& p:procs){ cur.insert(p.pid); if(!g_knownPids.count(p.pid)) p.isNew=true; }
            g_knownPids=cur;
            { std::lock_guard<std::mutex> lk(g_mtx);
              g_si=si; g_procs=std::move(procs);
              g_cpuH.push_back(si.cpuPct); g_memH.push_back(si.memPct); g_dskH.push_back(si.diskPct);
              while((int)g_cpuH.size()>HIST_LEN) g_cpuH.pop_front();
              while((int)g_memH.size()>HIST_LEN) g_memH.pop_front();
              while((int)g_dskH.size()>HIST_LEN) g_dskH.pop_front();
              g_dataReady=true; }
            if(g_hMain) PostMessage(g_hMain,WM_NEWDATA,0,0);
        }
        Sleep(1500);
    }
}

// ─── GDI helpers ─────────────────────────────────────────────────────────────
static void FillRR(HDC h,RECT r,int rx,COLORREF c){
    HBRUSH b=CreateSolidBrush(c); HPEN p=(HPEN)GetStockObject(NULL_PEN);
    SelectObject(h,p); SelectObject(h,b);
    RoundRect(h,r.left,r.top,r.right,r.bottom,rx,rx); DeleteObject(b);
}
static void HLine(HDC h,int x1,int x2,int y,COLORREF c){
    HPEN p=CreatePen(PS_SOLID,1,c),po=(HPEN)SelectObject(h,p);
    MoveToEx(h,x1,y,nullptr); LineTo(h,x2,y); SelectObject(h,po); DeleteObject(p);
}
static void VLine(HDC h,int x,int y1,int y2,COLORREF c){
    HPEN p=CreatePen(PS_SOLID,1,c),po=(HPEN)SelectObject(h,p);
    MoveToEx(h,x,y1,nullptr); LineTo(h,x,y2); SelectObject(h,po); DeleteObject(p);
}
static void TxtL(HDC h,RECT r,const wchar_t* t,COLORREF c,HFONT f,UINT fmt=DT_LEFT|DT_VCENTER|DT_SINGLELINE){
    SetBkMode(h,TRANSPARENT); SetTextColor(h,c); SelectObject(h,f);
    DrawTextW(h,t,-1,&r,fmt);
}
static void DrawStatRow(HDC hdc,int x,int y,int w,const wchar_t* lbl,const wchar_t* val){
    RECT lr={x,y,x+w/2,y+22}; TxtL(hdc,lr,lbl,C_TEXT_SEC,g_fSm);
    RECT vr={x+w/2,y,x+w,y+22}; TxtL(hdc,vr,val,C_TEXT,g_fSm,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
}
// Draw a history graph in rect r
static void DrawGraph(HDC hdc,RECT r,const std::deque<double>& data,
                      COLORREF lineC,COLORREF fillC){
    int W=r.right-r.left, H=r.bottom-r.top;
    // background
    HBRUSH bb=CreateSolidBrush(C_GRAPH_BG);
    FillRect(hdc,&r,bb); DeleteObject(bb);
    // grid at 25/50/75%
    for(int pct : {25,50,75}) HLine(hdc,r.left,r.right,r.bottom-(int)(pct/100.0*H),C_GRID);
    if(data.size()<2) return;
    int N=std::min((int)data.size(),HIST_LEN);
    int off=(int)data.size()-N;
    // points
    std::vector<POINT> pts; pts.reserve(N+2);
    pts.push_back({r.right,r.bottom});
    for(int i=0;i<N;i++){
        int x=r.left+(int)((double)i/(N-1)*(W-1));
        int y=r.bottom-(int)(data[off+i]/100.0*H)-1;
        pts.push_back({x,y});
    }
    pts.push_back({r.left,r.bottom});
    // fill
    HBRUSH bf=CreateSolidBrush(fillC); HPEN np=(HPEN)GetStockObject(NULL_PEN);
    SelectObject(hdc,np); SelectObject(hdc,bf);
    Polygon(hdc,pts.data(),(int)pts.size()); DeleteObject(bf);
    // line
    HPEN pl=CreatePen(PS_SOLID,2,lineC),po=(HPEN)SelectObject(hdc,pl);
    MoveToEx(hdc,pts[1].x,pts[1].y,nullptr);
    for(int i=2;i<(int)pts.size()-1;i++) LineTo(hdc,pts[i].x,pts[i].y);
    SelectObject(hdc,po); DeleteObject(pl);
}
// Tiny sparkline (no fill, no grid, just line) for sidebar cards
static void DrawSparkline(HDC hdc,RECT r,const std::deque<double>& data,COLORREF c){
    int W=r.right-r.left, H=r.bottom-r.top;
    if((int)data.size()<2||W<3) return;
    int N=std::min((int)data.size(),30);
    int off=(int)data.size()-N;
    HPEN p=CreatePen(PS_SOLID,1,c),po=(HPEN)SelectObject(hdc,p);
    for(int i=0;i<N;i++){
        int x=r.left+(int)((double)i/(N-1)*(W-1));
        int y=r.bottom-(int)(data[off+i]/100.0*H)-1;
        if(i==0) MoveToEx(hdc,x,y,nullptr); else LineTo(hdc,x,y);
    }
    SelectObject(hdc,po); DeleteObject(p);
}

// ─── Sort / filter ────────────────────────────────────────────────────────────
static void DoSort(std::vector<ProcInfo>& v){
    bool asc=g_sortAsc;
    switch(g_sortCol){
        case SCOL_NAME: std::sort(v.begin(),v.end(),[asc](auto& a,auto& b){ return asc?a.name<b.name:a.name>b.name; }); break;
        case SCOL_CPU:  std::sort(v.begin(),v.end(),[asc](auto& a,auto& b){ return asc?a.cpu<b.cpu:a.cpu>b.cpu; }); break;
        case SCOL_MEM:  std::sort(v.begin(),v.end(),[asc](auto& a,auto& b){ return asc?a.ws<b.ws:a.ws>b.ws; }); break;
        case SCOL_HANDLES: std::sort(v.begin(),v.end(),[asc](auto& a,auto& b){ return asc?a.handles<b.handles:a.handles>b.handles; }); break;
        case SCOL_THREADS: std::sort(v.begin(),v.end(),[asc](auto& a,auto& b){ return asc?a.threads<b.threads:a.threads>b.threads; }); break;
        default: std::sort(v.begin(),v.end(),[](auto& a,auto& b){ return a.cpu>b.cpu; }); break;
    }
}
static void RefreshProcs(){
    wchar_t sb[256]={}; if(g_hProcSearch) GetWindowTextW(g_hProcSearch,sb,255);
    { std::lock_guard<std::mutex> lk(g_mtx); g_sorted=g_procs; }
    if(sb[0]){
        std::wstring fl=sb; for(auto& c:fl) c=(wchar_t)towlower(c);
        g_sorted.erase(std::remove_if(g_sorted.begin(),g_sorted.end(),[&](auto& p){
            std::wstring nm=p.name; for(auto& c:nm) c=(wchar_t)towlower(c);
            return nm.find(fl)==std::wstring::npos; }),g_sorted.end());
    }
    DoSort(g_sorted);
    if(!g_hProcList) return;
    int prev=ListView_GetNextItem(g_hProcList,-1,LVNI_SELECTED);
    DWORD selPid=prev>=0&&prev<(int)g_sorted.size()?g_sorted[prev].pid:0;
    ListView_SetItemCountEx(g_hProcList,(int)g_sorted.size(),LVSICF_NOINVALIDATEALL);
    ListView_RedrawItems(g_hProcList,0,(int)g_sorted.size()-1);
    if(selPid) for(int i=0;i<(int)g_sorted.size();i++)
        if(g_sorted[i].pid==selPid){
            ListView_SetItemState(g_hProcList,i,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            break; }
}
static void RefreshDetails(){
    if(!g_hDetList) return;
    { std::lock_guard<std::mutex> lk(g_mtx); g_detSorted=g_procs; }
    std::sort(g_detSorted.begin(),g_detSorted.end(),[](auto& a,auto& b){ return a.pid<b.pid; });
    ListView_SetItemCountEx(g_hDetList,(int)g_detSorted.size(),LVSICF_NOINVALIDATEALL);
    ListView_RedrawItems(g_hDetList,0,(int)g_detSorted.size()-1);
}
static void UpdateProcHeaders(){
    if(!g_hProcList) return;
    const wchar_t* nm[]=  {L"Name",L"CPU",L"Memory",L"Handles",L"Threads"};
    SortCol        sc[]=  {SCOL_NAME,SCOL_CPU,SCOL_MEM,SCOL_HANDLES,SCOL_THREADS};
    HWND hHdr=ListView_GetHeader(g_hProcList);
    for(int i=0;i<5;i++){
        wchar_t b[64];
        if(g_sortCol==sc[i]) swprintf(b,64,L"%s %s",nm[i],g_sortAsc?L"▲":L"▼");
        else wcscpy_s(b,nm[i]);
        HDITEMW h={HDI_TEXT,0,b}; Header_SetItem(hHdr,i,&h);
    }
}

// ─── Kill ─────────────────────────────────────────────────────────────────────
static void KillSel(){
    int idx=ListView_GetNextItem(g_hProcList,-1,LVNI_SELECTED);
    if(idx<0||idx>=(int)g_sorted.size()) return;
    DWORD pid=g_sorted[idx].pid; if(!pid||pid==4) return;
    wchar_t msg[256]; swprintf(msg,256,L"End process '%s' (PID %lu)?",g_sorted[idx].name.c_str(),pid);
    if(MessageBoxW(g_hMain,msg,L"End Task",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
    HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pid);
    if(h){ TerminateProcess(h,1); CloseHandle(h); }
}

// ─── Processes page ───────────────────────────────────────────────────────────
static WNDPROC g_origSearch=nullptr;
static LRESULT CALLBACK SearchProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_KEYDOWN&&wp==VK_ESCAPE){ SetWindowTextW(h,L""); return 0; }
    if(msg==WM_KEYDOWN&&wp==VK_RETURN){ SetFocus(g_hProcList); return 0; }
    return CallWindowProcW(g_origSearch,h,msg,wp,lp);
}

static LRESULT ProcGetDisp(NMLVDISPINFOW* d){
    int i=d->item.iItem; if(i<0||i>=(int)g_sorted.size()) return 0;
    const ProcInfo& p=g_sorted[i]; static wchar_t buf[MAX_PATH];
    if(d->item.mask&LVIF_TEXT){
        switch(d->item.iSubItem){
            case 0: wcsncpy_s(buf,MAX_PATH,p.name.c_str(),_TRUNCATE); break;
            case 1: swprintf(buf,32,L"%.1f%%",p.cpu); break;
            case 2: wcsncpy_s(buf,32,FmtMB(p.ws).c_str(),_TRUNCATE); break;
            case 3: swprintf(buf,32,L"%lu",p.handles); break;
            case 4: swprintf(buf,32,L"%lu",p.threads); break;
            default: buf[0]=0;
        } d->item.pszText=buf; } return 0;
}
static LRESULT ProcCD(NMLVCUSTOMDRAW* cd){
    switch(cd->nmcd.dwDrawStage){
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:{
            int i=(int)cd->nmcd.dwItemSpec; bool s=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            cd->clrTextBk=s?C_ROW_SEL:(i<(int)g_sorted.size()&&g_sorted[i].isNew)?C_ROW_NEW:(i%2?C_ROW_ODD:C_ROW_EVEN);
            cd->clrText=C_TEXT;
            if(i<(int)g_sorted.size()&&!g_sorted[i].accessible) cd->clrText=C_TEXT_DIM;
            return CDRF_NOTIFYSUBITEMDRAW|CDRF_NEWFONT;
        }
        case CDDS_ITEMPREPAINT|CDDS_SUBITEM:{
            int i=(int)cd->nmcd.dwItemSpec; bool s=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            if(i>=(int)g_sorted.size()) return CDRF_DODEFAULT;
            auto& p=g_sorted[i];
            cd->clrTextBk=s?C_ROW_SEL:(p.isNew?C_ROW_NEW:(i%2?C_ROW_ODD:C_ROW_EVEN));
            switch(cd->iSubItem){
                case 1: cd->clrText=CpuClr(p.cpu); break;
                case 2: cd->clrText=MemClr(p.ws); break;
                default: cd->clrText=C_TEXT;
            } return CDRF_NEWFONT;
        }
    } return CDRF_DODEFAULT;
}

static LRESULT CALLBACK ProcPageProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        // Toolbar
        g_hProcSearch=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,8,8,250,26,hWnd,(HMENU)IDC_PROC_SEARCH,g_inst,nullptr);
        SendMessage(g_hProcSearch,WM_SETFONT,(WPARAM)g_fUI,TRUE);
        SendMessage(g_hProcSearch,EM_SETCUEBANNER,TRUE,(LPARAM)L"🔍 Filter by name...");
        g_origSearch=(WNDPROC)SetWindowLongPtrW(g_hProcSearch,GWLP_WNDPROC,(LONG_PTR)SearchProc);

        g_hEndTask=CreateWindowExW(0,L"BUTTON",L"End task",
            WS_CHILD|WS_VISIBLE|BS_FLAT,268,6,84,30,hWnd,(HMENU)IDC_PROC_ENDTASK,g_inst,nullptr);
        SendMessage(g_hEndTask,WM_SETFONT,(WPARAM)g_fUI,TRUE);

        // ListView
        g_hProcList=CreateWindowExW(0,WC_LISTVIEWW,L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
            0,44,100,100,hWnd,(HMENU)IDC_PROC_LIST,g_inst,nullptr);
        SendMessage(g_hProcList,WM_SETFONT,(WPARAM)g_fUI,TRUE);
        ListView_SetExtendedListViewStyle(g_hProcList,
            LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_HEADERDRAGDROP|LVS_EX_GRIDLINES);
        ListView_SetBkColor(g_hProcList,C_ROW_EVEN);
        ListView_SetTextBkColor(g_hProcList,C_ROW_EVEN);
        ListView_SetTextColor(g_hProcList,C_TEXT);
        struct CD2{const wchar_t*n;int w;int f;};
        CD2 cols[]={{L"Name",240,LVCFMT_LEFT},{L"CPU",70,LVCFMT_RIGHT},
                    {L"Memory",110,LVCFMT_RIGHT},{L"Handles",80,LVCFMT_RIGHT},{L"Threads",80,LVCFMT_RIGHT}};
        for(int i=0;i<5;i++){
            LVCOLUMNW c={LVCF_TEXT|LVCF_WIDTH|LVCF_FMT,cols[i].f,cols[i].w,(LPWSTR)cols[i].n,0,i};
            ListView_InsertColumn(g_hProcList,i,&c);
        }
        return 0;
    }
    case WM_SIZE: SetWindowPos(g_hProcList,nullptr,0,44,LOWORD(lp),HIWORD(lp)-44,SWP_NOZORDER); return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_PROC_ENDTASK) KillSel();
        if(LOWORD(wp)==IDC_PROC_SEARCH&&HIWORD(wp)==EN_CHANGE){ RefreshProcs(); UpdateProcHeaders(); }
        return 0;
    case WM_NOTIFY:{
        NMHDR* h=(NMHDR*)lp;
        if(h->idFrom==IDC_PROC_LIST){
            switch(h->code){
                case LVN_GETDISPINFOW: ProcGetDisp((NMLVDISPINFOW*)lp); break;
                case NM_CUSTOMDRAW: return ProcCD((NMLVCUSTOMDRAW*)lp);
                case LVN_COLUMNCLICK:{
                    int c2=((NMLISTVIEW*)lp)->iSubItem;
                    SortCol nc=(SortCol)c2;
                    if(g_sortCol==nc) g_sortAsc=!g_sortAsc;
                    else{ g_sortCol=nc; g_sortAsc=(nc==SCOL_NAME); }
                    RefreshProcs(); UpdateProcHeaders();
                } break;
                case NM_DBLCLK: KillSel(); break;
                case LVN_KEYDOWN: if(((NMLVKEYDOWN*)lp)->wVKey==VK_DELETE) KillSel(); break;
                case NM_RCLICK:{
                    int idx=((NMITEMACTIVATE*)lp)->iItem;
                    if(idx<0||idx>=(int)g_sorted.size()) break;
                    auto& p2=g_sorted[idx];
                    HMENU hM=CreatePopupMenu();
                    wchar_t hdr2[80]; swprintf(hdr2,80,L"PID %lu — %s",p2.pid,p2.name.c_str());
                    AppendMenuW(hM,MF_STRING|MF_GRAYED,0,hdr2);
                    AppendMenuW(hM,MF_SEPARATOR,0,nullptr);
                    AppendMenuW(hM,MF_STRING,1,L"End task\tDel");
                    AppendMenuW(hM,MF_SEPARATOR,0,nullptr);
                    AppendMenuW(hM,MF_STRING,2,L"Copy name");
                    AppendMenuW(hM,MF_STRING,3,L"Copy PID");
                    AppendMenuW(hM,MF_STRING,4,L"Open file location");
                    HMENU hPri=CreatePopupMenu();
                    DWORD pc2=p2.priClass;
                    AppendMenuW(hPri,MF_STRING|(pc2==REALTIME_PRIORITY_CLASS?MF_CHECKED:0),10,L"Realtime");
                    AppendMenuW(hPri,MF_STRING|(pc2==HIGH_PRIORITY_CLASS?MF_CHECKED:0),11,L"High");
                    AppendMenuW(hPri,MF_STRING|(pc2==ABOVE_NORMAL_PRIORITY_CLASS?MF_CHECKED:0),12,L"Above Normal");
                    AppendMenuW(hPri,MF_STRING|(pc2==NORMAL_PRIORITY_CLASS?MF_CHECKED:0),13,L"Normal");
                    AppendMenuW(hPri,MF_STRING|(pc2==BELOW_NORMAL_PRIORITY_CLASS?MF_CHECKED:0),14,L"Below Normal");
                    AppendMenuW(hPri,MF_STRING|(pc2==IDLE_PRIORITY_CLASS?MF_CHECKED:0),15,L"Idle");
                    AppendMenuW(hM,MF_POPUP,(UINT_PTR)hPri,L"Set priority");
                    POINT pt; GetCursorPos(&pt);
                    int cmd=TrackPopupMenu(hM,TPM_RIGHTBUTTON|TPM_RETURNCMD,pt.x,pt.y,0,g_hMain,nullptr);
                    DestroyMenu(hM);
                    if(cmd==1) KillSel();
                    else if(cmd==2) ClipCopy(p2.name);
                    else if(cmd==3) ClipCopy(std::to_wstring(p2.pid));
                    else if(cmd==4){
                        std::wstring path=GetExePath(p2.pid);
                        if(!path.empty()){
                            std::wstring arg=L"/select,\""+path+L"\"";
                            ShellExecuteW(g_hMain,L"open",L"explorer.exe",arg.c_str(),nullptr,SW_SHOW);
                        }
                    } else if(cmd>=10&&cmd<=15){
                        const DWORD pcs[]={REALTIME_PRIORITY_CLASS,HIGH_PRIORITY_CLASS,
                            ABOVE_NORMAL_PRIORITY_CLASS,NORMAL_PRIORITY_CLASS,
                            BELOW_NORMAL_PRIORITY_CLASS,IDLE_PRIORITY_CLASS};
                        HANDLE hp=OpenProcess(PROCESS_SET_INFORMATION,FALSE,p2.pid);
                        if(hp){ SetPriorityClass(hp,pcs[cmd-10]); CloseHandle(hp); }
                    }
                } break;
            }
        } break;
    }
    case WM_CTLCOLOREDIT:{ SetBkColor((HDC)wp,C_BG); SetTextColor((HDC)wp,C_TEXT); return (LRESULT)g_brBG; }
    case WM_CTLCOLORBTN:{ SetBkColor((HDC)wp,C_PANEL); SetTextColor((HDC)wp,C_TEXT); return (LRESULT)g_brPanel; }
    case WM_ERASEBKGND:{ RECT rc; GetClientRect(hWnd,&rc); FillRect((HDC)wp,&rc,g_brBG); return 1; }
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc); FillRect(hdc,&rc,g_brBG);
        HLine(hdc,0,rc.right,42,C_SEP);
        // Column hint (keyboard shortcuts)
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,C_TEXT_DIM); SelectObject(hdc,g_fSm);
        RECT kr={360,10,rc.right-8,30};
        DrawTextW(hdc,L"C=CPU  M=Mem  H=Handles  T=Threads  Del=End task  Space=Pause",-1,&kr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        EndPaint(hWnd,&ps); return 0;
    }
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ─── Performance page ─────────────────────────────────────────────────────────
static LRESULT CALLBACK PerfPageProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_LBUTTONDOWN:{
        int x=LOWORD(lp),y=HIWORD(lp);
        if(x<PERF_LEFT_W){
            int cardY=y-36; if(cardY>=0){
                int idx=cardY/CARD_H; if(idx>=0&&idx<PERF_COUNT&&g_perfRes!=(PerfRes)idx){
                    g_perfRes=(PerfRes)idx; InvalidateRect(hWnd,nullptr,FALSE); }
            }
        }
    } return 0;
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT){
            POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd,&pt);
            SetCursor(LoadCursorW(nullptr,pt.x<PERF_LEFT_W?IDC_HAND:IDC_ARROW));
            return TRUE; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc); int W=rc.right, H=rc.bottom;
        HDC mdc=CreateCompatibleDC(hdc);
        HBITMAP bmp=CreateCompatibleBitmap(hdc,W,H);
        HBITMAP ob=(HBITMAP)SelectObject(mdc,bmp);

        SysInfo si; std::deque<double> cH,mH,dH;
        { std::lock_guard<std::mutex> lk(g_mtx); si=g_si; cH=g_cpuH; mH=g_memH; dH=g_dskH; }

        // ── Left sidebar ──
        RECT lr2={0,0,PERF_LEFT_W,H}; FillRect(mdc,&lr2,g_brSide);
        TxtL(mdc,{12,10,PERF_LEFT_W-8,32},L"PERFORMANCE",C_TEXT_SEC,g_fSm);
        HLine(mdc,0,PERF_LEFT_W,34,C_SEP);

        // Resource cards
        struct RCard{ const wchar_t* nm; double pct; COLORREF c; const std::deque<double>* h; };
        RCard cards[]={
            {L"CPU",    si.cpuPct,  C_GCPU,  &cH},
            {L"Memory", si.memPct,  C_GMEM,  &mH},
            {L"Disk",   si.diskPct, C_GDISK, &dH},
        };
        for(int i=0;i<3;i++){
            bool sel=(g_perfRes==(PerfRes)i);
            int cy=36+i*CARD_H;
            RECT cr={0,cy,PERF_LEFT_W,cy+CARD_H};
            HBRUSH cb=CreateSolidBrush(sel?C_SEL_NAV:C_SIDEBAR);
            FillRect(mdc,&cr,cb); DeleteObject(cb);
            if(sel){ // left accent bar
                HPEN pa=CreatePen(PS_SOLID,3,C_ACCENT),po=(HPEN)SelectObject(mdc,pa);
                MoveToEx(mdc,1,cy+4,nullptr); LineTo(mdc,1,cy+CARD_H-4);
                SelectObject(mdc,po); DeleteObject(pa);
            }
            // name
            COLORREF tc=sel?C_TEXT:C_TEXT_SEC;
            TxtL(mdc,{14,cy+8,PERF_LEFT_W-76,cy+26},cards[i].nm,tc,g_fBold);
            // pct
            wchar_t pv[12]; swprintf(pv,12,L"%.0f%%",cards[i].pct);
            TxtL(mdc,{14,cy+28,PERF_LEFT_W-76,cy+48},pv,sel?cards[i].c:C_TEXT_DIM,g_fSm);
            // sparkline
            RECT sr={PERF_LEFT_W-70,cy+12,PERF_LEFT_W-6,cy+CARD_H-12};
            HBRUSH sgb=CreateSolidBrush(C_GRAPH_BG); FillRect(mdc,&sr,sgb); DeleteObject(sgb);
            if(cards[i].h->size()>=2) DrawSparkline(mdc,sr,*cards[i].h,cards[i].c);
            HLine(mdc,0,PERF_LEFT_W,cy+CARD_H-1,C_SEP);
        }
        VLine(mdc,PERF_LEFT_W,0,H,C_SEP);

        // ── Right panel ──
        int RX=PERF_LEFT_W+1, RW=W-RX;
        RECT rr={RX,0,W,H}; FillRect(mdc,&rr,g_brBG);

        // Select data by resource
        const wchar_t* rTitle=L"CPU"; COLORREF rColor=C_GCPU;
        const std::deque<double>* rH=&cH;
        if(g_perfRes==PERF_MEMORY){ rTitle=L"Memory"; rColor=C_GMEM; rH=&mH; }
        if(g_perfRes==PERF_DISK)  { rTitle=L"Disk";   rColor=C_GDISK;rH=&dH; }

        // Title
        TxtL(mdc,{RX+20,12,W-20,42},rTitle,C_TEXT,g_fTitle);

        // Sub title  (current %)
        wchar_t sub[64]; double curPct=rH->empty()?0:rH->back();
        swprintf(sub,64,L"%.1f%% utilization",curPct);
        TxtL(mdc,{RX+20,44,W-20,62},sub,C_TEXT_SEC,g_fSm);

        // Graph
        int gT=72, gH=H/2-20, gB=gT+gH;
        RECT gR={RX+20,gT,W-20,gB};
        COLORREF fillC=RGB(GetRValue(rColor)/6,GetGValue(rColor)/6,GetBValue(rColor)/6);
        DrawGraph(mdc,gR,*rH,rColor,fillC);
        // y-axis labels
        TxtL(mdc,{RX+22,gT+2,RX+60,gT+16},L"100%",C_TEXT_DIM,g_fGraph);
        TxtL(mdc,{RX+22,gB-16,RX+60,gB},L"0%",C_TEXT_DIM,g_fGraph);
        TxtL(mdc,{W-90,gB+2,W-20,gB+16},L"60 seconds",C_TEXT_DIM,g_fGraph,DT_RIGHT|DT_SINGLELINE);

        // Horizontal separator before stats
        HLine(mdc,RX+20,W-20,gB+22,C_SEP);

        // Stats grid
        int sY=gB+30, sX=RX+20, sW=RW-40, col2=sX+sW/2+10;
        TxtL(mdc,{sX,sY,sX+sW,sY+20},L"Statistics",C_TEXT,g_fBold);
        sY+=26;

        wchar_t tmp[64];
        if(g_perfRes==PERF_CPU){
            swprintf(tmp,64,L"%.1f%%",si.cpuPct);
            DrawStatRow(mdc,sX,sY,sW/2,L"Utilization",tmp); sY+=22;
            swprintf(tmp,64,L"%d",si.procs);
            DrawStatRow(mdc,sX,sY,sW/2,L"Processes",tmp);
            swprintf(tmp,64,L"%d",si.logCpus);
            DrawStatRow(mdc,col2,sY,sW/2,L"Logical CPUs",tmp); sY+=22;
            swprintf(tmp,64,L"%d",si.threads);
            DrawStatRow(mdc,sX,sY,sW/2,L"Threads",tmp);
            DrawStatRow(mdc,col2,sY,sW/2,L"Up time",FmtUp(si.uptime).c_str()); sY+=22;
            swprintf(tmp,64,L"%d",si.handles);
            DrawStatRow(mdc,sX,sY,sW/2,L"Handles",tmp);
        } else if(g_perfRes==PERF_MEMORY){
            DrawStatRow(mdc,sX,sY,sW/2,L"In use",FmtKB(si.memUsed).c_str());
            DrawStatRow(mdc,col2,sY,sW/2,L"Available",FmtKB(si.memFree).c_str()); sY+=22;
            DrawStatRow(mdc,sX,sY,sW/2,L"Total",FmtKB(si.memTotal).c_str());
            DrawStatRow(mdc,col2,sY,sW/2,L"Page file total",FmtKB(si.pageTotal).c_str()); sY+=22;
            DrawStatRow(mdc,sX,sY,sW/2,L"Committed",FmtKB(si.pageUsed).c_str());
            swprintf(tmp,64,L"%.1f%%",si.memPct);
            DrawStatRow(mdc,col2,sY,sW/2,L"Utilization",tmp);
        } else if(g_perfRes==PERF_DISK){
            swprintf(tmp,64,L"%.1f%%",si.diskPct);
            DrawStatRow(mdc,sX,sY,sW/2,L"Active time",tmp); sY+=22;
            DrawStatRow(mdc,sX,sY,sW/2,L"Read speed",FmtBps(si.diskRd).c_str());
            DrawStatRow(mdc,col2,sY,sW/2,L"Write speed",FmtBps(si.diskWr).c_str());
        }

        BitBlt(hdc,0,0,W,H,mdc,0,0,SRCCOPY);
        SelectObject(mdc,ob); DeleteObject(bmp); DeleteDC(mdc);
        EndPaint(hWnd,&ps); return 0;
    }
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ─── Details page ─────────────────────────────────────────────────────────────
static LRESULT DetGetDisp(NMLVDISPINFOW* d){
    int i=d->item.iItem; if(i<0||i>=(int)g_detSorted.size()) return 0;
    const ProcInfo& p=g_detSorted[i]; static wchar_t buf[MAX_PATH];
    if(d->item.mask&LVIF_TEXT){
        switch(d->item.iSubItem){
            case 0: swprintf(buf,32,L"%lu",p.pid); break;
            case 1: wcsncpy_s(buf,MAX_PATH,p.name.c_str(),_TRUNCATE); break;
            case 2: wcscpy_s(buf,p.accessible?L"Running":L"—"); break;
            case 3: swprintf(buf,32,L"%.1f%%",p.cpu); break;
            case 4: wcsncpy_s(buf,32,FmtMB(p.ws).c_str(),_TRUNCATE); break;
            case 5: wcsncpy_s(buf,32,FmtMB(p.priv).c_str(),_TRUNCATE); break;
            case 6: swprintf(buf,32,L"%lu",p.handles); break;
            case 7: swprintf(buf,32,L"%lu",p.threads); break;
            case 8: wcsncpy_s(buf,32,PriStr(p.priClass).c_str(),_TRUNCATE); break;
            default: buf[0]=0;
        } d->item.pszText=buf; } return 0;
}
static LRESULT DetCD(NMLVCUSTOMDRAW* cd){
    switch(cd->nmcd.dwDrawStage){
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:{
            int i=(int)cd->nmcd.dwItemSpec; bool s=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            cd->clrTextBk=s?C_ROW_SEL:(i%2?C_ROW_ODD:C_ROW_EVEN); cd->clrText=C_TEXT;
            if(i<(int)g_detSorted.size()&&!g_detSorted[i].accessible) cd->clrText=C_TEXT_DIM;
            return CDRF_NOTIFYSUBITEMDRAW|CDRF_NEWFONT;
        }
        case CDDS_ITEMPREPAINT|CDDS_SUBITEM:{
            int i=(int)cd->nmcd.dwItemSpec; bool s=(cd->nmcd.uItemState&CDIS_SELECTED)!=0;
            if(i>=(int)g_detSorted.size()) return CDRF_DODEFAULT;
            auto& p=g_detSorted[i];
            cd->clrTextBk=s?C_ROW_SEL:(i%2?C_ROW_ODD:C_ROW_EVEN);
            switch(cd->iSubItem){
                case 3: cd->clrText=CpuClr(p.cpu); break;
                case 4: case 5: cd->clrText=MemClr(p.ws); break;
                case 8: cd->clrText=(p.priClass==REALTIME_PRIORITY_CLASS)?RGB(214,50,50):
                                    (p.priClass==HIGH_PRIORITY_CLASS)?RGB(232,118,26):C_TEXT; break;
                default: cd->clrText=C_TEXT;
            } return CDRF_NEWFONT;
        }
    } return CDRF_DODEFAULT;
}

static LRESULT CALLBACK DetPageProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        g_hDetList=CreateWindowExW(0,WC_LISTVIEWW,L"",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
            0,0,100,100,hWnd,(HMENU)IDC_DET_LIST,g_inst,nullptr);
        SendMessage(g_hDetList,WM_SETFONT,(WPARAM)g_fUI,TRUE);
        ListView_SetExtendedListViewStyle(g_hDetList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_GRIDLINES);
        ListView_SetBkColor(g_hDetList,C_ROW_EVEN);
        ListView_SetTextBkColor(g_hDetList,C_ROW_EVEN);
        ListView_SetTextColor(g_hDetList,C_TEXT);
        struct CD3{const wchar_t*n;int w;int f;};
        CD3 cols[]={{L"PID",70,LVCFMT_RIGHT},{L"Name",180,LVCFMT_LEFT},{L"Status",75,LVCFMT_LEFT},
                    {L"CPU",65,LVCFMT_RIGHT},{L"Working Set",105,LVCFMT_RIGHT},
                    {L"Private",100,LVCFMT_RIGHT},{L"Handles",75,LVCFMT_RIGHT},
                    {L"Threads",70,LVCFMT_RIGHT},{L"Priority",110,LVCFMT_LEFT}};
        for(int i=0;i<9;i++){
            LVCOLUMNW c={LVCF_TEXT|LVCF_WIDTH|LVCF_FMT,cols[i].f,cols[i].w,(LPWSTR)cols[i].n,0,i};
            ListView_InsertColumn(g_hDetList,i,&c);
        }
        return 0;
    }
    case WM_SIZE: SetWindowPos(g_hDetList,nullptr,0,0,LOWORD(lp),HIWORD(lp),SWP_NOZORDER); return 0;
    case WM_NOTIFY:{
        NMHDR* h=(NMHDR*)lp;
        if(h->idFrom==IDC_DET_LIST){
            if(h->code==LVN_GETDISPINFOW) DetGetDisp((NMLVDISPINFOW*)lp);
            else if(h->code==NM_CUSTOMDRAW) return DetCD((NMLVCUSTOMDRAW*)lp);
        } break;
    }
    case WM_ERASEBKGND:{ RECT r; GetClientRect(hWnd,&r); FillRect((HDC)wp,&r,g_brBG); return 1; }
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ─── Sidebar nav items ────────────────────────────────────────────────────────
struct NavItem { const wchar_t* icon; const wchar_t* label; TabPage tab; };
static const NavItem g_nav[]={
    {L"☰",  L"Processes",   TAB_PROCESSES},
    {L"📈", L"Performance", TAB_PERFORMANCE},
    {L"📋", L"Details",     TAB_DETAILS},
};

static void ShowPage(TabPage t){
    ShowWindow(g_hProcPage, t==TAB_PROCESSES  ?SW_SHOW:SW_HIDE);
    ShowWindow(g_hPerfPage, t==TAB_PERFORMANCE?SW_SHOW:SW_HIDE);
    ShowWindow(g_hDetPage,  t==TAB_DETAILS    ?SW_SHOW:SW_HIDE);
    g_curTab=t;
    if(t==TAB_PROCESSES){ RefreshProcs(); UpdateProcHeaders(); }
    if(t==TAB_DETAILS)   RefreshDetails();
    if(t==TAB_PERFORMANCE) InvalidateRect(g_hPerfPage,nullptr,FALSE);
}
static void LayoutMain(HWND hWnd){
    RECT rc; GetClientRect(hWnd,&rc);
    int W=rc.right,H=rc.bottom,cx=SIDEBAR_W;
    SetWindowPos(g_hProcPage,nullptr,cx,0,W-cx,H,SWP_NOZORDER);
    SetWindowPos(g_hPerfPage,nullptr,cx,0,W-cx,H,SWP_NOZORDER);
    SetWindowPos(g_hDetPage, nullptr,cx,0,W-cx,H,SWP_NOZORDER);
}

// ─── Main WndProc ─────────────────────────────────────────────────────────────
static LRESULT CALLBACK MainWndProc(HWND hWnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){

    case WM_CREATE:{
        LOGFONTW lf={}; lf.lfCharSet=DEFAULT_CHARSET; wcscpy_s(lf.lfFaceName,L"Segoe UI");
        lf.lfHeight=-13; lf.lfWeight=FW_NORMAL;  g_fUI   =CreateFontIndirectW(&lf);
        lf.lfWeight=FW_SEMIBOLD;                  g_fBold =CreateFontIndirectW(&lf);
        lf.lfHeight=-11; lf.lfWeight=FW_NORMAL;   g_fSm   =CreateFontIndirectW(&lf);
        lf.lfHeight=-17; lf.lfWeight=FW_BOLD;     g_fTitle=CreateFontIndirectW(&lf);
        lf.lfHeight=-10; lf.lfWeight=FW_NORMAL;   g_fGraph=CreateFontIndirectW(&lf);
        g_brBG   =CreateSolidBrush(C_BG);
        g_brSide =CreateSolidBrush(C_SIDEBAR);
        g_brPanel=CreateSolidBrush(C_PANEL);
        g_brGBG  =CreateSolidBrush(C_GRAPH_BG);

        WNDCLASSW pc={CS_HREDRAW|CS_VREDRAW,ProcPageProc,0,0,g_inst,nullptr,
                      LoadCursorW(nullptr,IDC_ARROW),(HBRUSH)(COLOR_WINDOW+1),nullptr,L"ProcPage"};
        RegisterClassW(&pc);
        pc={CS_HREDRAW|CS_VREDRAW,PerfPageProc,0,0,g_inst,nullptr,
            LoadCursorW(nullptr,IDC_ARROW),(HBRUSH)(COLOR_WINDOW+1),nullptr,L"PerfPage"};
        RegisterClassW(&pc);
        pc={CS_HREDRAW|CS_VREDRAW,DetPageProc,0,0,g_inst,nullptr,
            LoadCursorW(nullptr,IDC_ARROW),(HBRUSH)(COLOR_WINDOW+1),nullptr,L"DetPage"};
        RegisterClassW(&pc);

        g_hProcPage=CreateWindowW(L"ProcPage",L"",WS_CHILD,0,0,100,100,hWnd,nullptr,g_inst,nullptr);
        g_hPerfPage=CreateWindowW(L"PerfPage",L"",WS_CHILD,0,0,100,100,hWnd,nullptr,g_inst,nullptr);
        g_hDetPage =CreateWindowW(L"DetPage", L"",WS_CHILD,0,0,100,100,hWnd,nullptr,g_inst,nullptr);
        ShowPage(TAB_PROCESSES);
        SetTimer(hWnd,TIMER_UI,500,nullptr);
        return 0;
    }

    case WM_SIZE:
        LayoutMain(hWnd);
        { RECT sr={0,0,SIDEBAR_W,HIWORD(lp)}; InvalidateRect(hWnd,&sr,FALSE); }
        return 0;

    case WM_TIMER:
        if(wp==TIMER_UI){
            RECT sr={0,0,SIDEBAR_W,9999}; InvalidateRect(hWnd,&sr,FALSE);
            if(g_curTab==TAB_PERFORMANCE) InvalidateRect(g_hPerfPage,nullptr,FALSE);
        }
        return 0;

    case WM_NEWDATA:
        if(g_dataReady){
            g_dataReady=false;
            if(g_curTab==TAB_PROCESSES){ RefreshProcs(); UpdateProcHeaders(); }
            if(g_curTab==TAB_DETAILS)   RefreshDetails();
            if(g_curTab==TAB_PERFORMANCE) InvalidateRect(g_hPerfPage,nullptr,FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:{
        int x=LOWORD(lp),y=HIWORD(lp);
        if(x<SIDEBAR_W){
            int navY=y-NAV_TOP; if(navY>=0){ int idx=navY/NAV_H;
                if(idx>=0&&idx<3){ ShowPage(g_nav[idx].tab);
                    RECT sr={0,0,SIDEBAR_W,9999}; InvalidateRect(hWnd,&sr,FALSE); }
            }
        }
    } return 0;

    case WM_MOUSEMOVE:{
        int x=LOWORD(lp),y=HIWORD(lp); int nh=-1;
        if(x<SIDEBAR_W){ int navY=y-NAV_TOP; if(navY>=0){ int idx=navY/NAV_H; if(idx<3) nh=idx; } }
        if(nh!=g_hoverNav){ g_hoverNav=nh; RECT sr={0,0,SIDEBAR_W,9999}; InvalidateRect(hWnd,&sr,FALSE);
            if(nh>=0){ TRACKMOUSEEVENT t={sizeof(t),TME_LEAVE,hWnd,0}; TrackMouseEvent(&t); } }
    } return 0;

    case WM_MOUSELEAVE:
        g_hoverNav=-1; { RECT sr={0,0,SIDEBAR_W,9999}; InvalidateRect(hWnd,&sr,FALSE); } return 0;

    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT){
            POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd,&pt);
            SetCursor(LoadCursorW(nullptr,(pt.x<SIDEBAR_W&&pt.y>NAV_TOP)?IDC_HAND:IDC_ARROW));
            return TRUE; }
        break;

    case WM_KEYDOWN:
        switch(wp){
            case 'Q': DestroyWindow(hWnd); break;
            case VK_SPACE: g_paused=!g_paused.load(); break;
            case 'C': g_sortCol=SCOL_CPU;     g_sortAsc=false; RefreshProcs(); UpdateProcHeaders(); break;
            case 'M': g_sortCol=SCOL_MEM;     g_sortAsc=false; RefreshProcs(); UpdateProcHeaders(); break;
            case 'N': g_sortCol=SCOL_NAME;    g_sortAsc=true;  RefreshProcs(); UpdateProcHeaders(); break;
            case 'H': g_sortCol=SCOL_HANDLES; g_sortAsc=false; RefreshProcs(); UpdateProcHeaders(); break;
            case 'T': g_sortCol=SCOL_THREADS; g_sortAsc=false; RefreshProcs(); UpdateProcHeaders(); break;
            case VK_DELETE: if(g_curTab==TAB_PROCESSES) KillSel(); break;
            case VK_F5: g_dataReady=false; break;
            case 'F': if(GetKeyState(VK_CONTROL)&0x8000) SetFocus(g_hProcSearch); break;
        }
        break;

    case WM_PAINT:{
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hWnd,&ps);
        RECT rc; GetClientRect(hWnd,&rc); int H=rc.bottom;

        // Double-buffer sidebar
        HDC mdc=CreateCompatibleDC(hdc);
        HBITMAP bmp=CreateCompatibleBitmap(hdc,SIDEBAR_W,H);
        HBITMAP ob=(HBITMAP)SelectObject(mdc,bmp);

        // Sidebar bg
        RECT sr={0,0,SIDEBAR_W,H}; FillRect(mdc,&sr,g_brSide);

        // App title area
        TxtL(mdc,{14,16,SIDEBAR_W-8,40},L"Task Manager",C_TEXT,g_fBold);
        HLine(mdc,0,SIDEBAR_W,NAV_TOP-2,C_SEP);

        // Pct badges from sys info
        SysInfo si; { std::lock_guard<std::mutex> lk(g_mtx); si=g_si; }
        double navPcts[]={si.cpuPct,si.memPct,si.diskPct};
        COLORREF navClrs[]={C_GCPU,C_GMEM,C_GDISK};

        for(int i=0;i<3;i++){
            bool sel=(g_curTab==g_nav[i].tab);
            bool hov=(g_hoverNav==i&&!sel);
            int cy=NAV_TOP+i*NAV_H;
            RECT ir={0,cy,SIDEBAR_W,cy+NAV_H};
            if(sel){ HBRUSH b=CreateSolidBrush(C_SEL_NAV); FillRect(mdc,&ir,b); DeleteObject(b);
                HPEN pa=CreatePen(PS_SOLID,3,C_ACCENT),po=(HPEN)SelectObject(mdc,pa);
                MoveToEx(mdc,1,cy+5,nullptr); LineTo(mdc,1,cy+NAV_H-5);
                SelectObject(mdc,po); DeleteObject(pa);
            } else if(hov){ HBRUSH b=CreateSolidBrush(C_HOVER); FillRect(mdc,&ir,b); DeleteObject(b); }

            // Icon
            TxtL(mdc,{10,cy,42,cy+NAV_H},g_nav[i].icon,sel?C_ACCENT:C_TEXT_DIM,g_fBold);
            // Label
            TxtL(mdc,{42,cy,SIDEBAR_W-56,cy+NAV_H},g_nav[i].label,sel?C_TEXT:C_TEXT_SEC,sel?g_fBold:g_fUI);
            // % badge
            wchar_t pv[12]; swprintf(pv,12,L"%.0f%%",navPcts[i]);
            TxtL(mdc,{SIDEBAR_W-52,cy,SIDEBAR_W-8,cy+NAV_H},pv,navClrs[i],g_fSm,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            HLine(mdc,0,SIDEBAR_W,cy+NAV_H-1,C_SEP);
        }

        // Right border
        VLine(mdc,SIDEBAR_W-1,0,H,C_SEP);

        BitBlt(hdc,0,0,SIDEBAR_W,H,mdc,0,0,SRCCOPY);
        SelectObject(mdc,ob); DeleteObject(bmp); DeleteDC(mdc);
        EndPaint(hWnd,&ps); return 0;
    }

    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:
        g_running=false; KillTimer(hWnd,TIMER_UI);
        if(g_pdhQ) PdhCloseQuery(g_pdhQ);
        DeleteObject(g_fUI); DeleteObject(g_fBold); DeleteObject(g_fSm);
        DeleteObject(g_fTitle); DeleteObject(g_fGraph);
        DeleteObject(g_brBG); DeleteObject(g_brSide);
        DeleteObject(g_brPanel); DeleteObject(g_brGBG);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd,msg,wp,lp);
}

// ─── WinMain ──────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int nCmdShow){
    g_inst=hInst;
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    InitPDH();
    SYSTEM_INFO si{}; GetSystemInfo(&si); g_cpuCount=(int)si.dwNumberOfProcessors;
    ReadProcs(); Sleep(900); // prime per-process CPU deltas

    WNDCLASSEXW wc={sizeof(wc),CS_HREDRAW|CS_VREDRAW,MainWndProc,0,0,hInst,
                    LoadIconW(nullptr,IDI_APPLICATION),
                    LoadCursorW(nullptr,IDC_ARROW),
                    (HBRUSH)(COLOR_WINDOW+1),nullptr,L"ProcmonMain",
                    LoadIconW(nullptr,IDI_APPLICATION)};
    RegisterClassExW(&wc);

    g_hMain=CreateWindowExW(WS_EX_APPWINDOW,L"ProcmonMain",
        L"Task Manager  —  PROCMON v3",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1160,760,
        nullptr,nullptr,hInst,nullptr);

    ShowWindow(g_hMain,nCmdShow);
    UpdateWindow(g_hMain);

    std::thread dt(DataThread);
    MSG msgLoop;
    while(GetMessageW(&msgLoop,nullptr,0,0)){
        if(!IsDialogMessageW(g_hMain,&msgLoop)){
            TranslateMessage(&msgLoop); DispatchMessageW(&msgLoop);
        }
    }
    g_running=false; dt.join();
    return (int)msgLoop.wParam;
}
