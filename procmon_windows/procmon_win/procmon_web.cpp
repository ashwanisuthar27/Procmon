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
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ws2_32.lib")

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <winternl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <vector>
#include <cstdio>
#include <cmath>

#include "webui_repo/include/webui.hpp"

// ── Data structures ───────────────────────────────────────────────────────────
struct ProcessInfo {
    DWORD        pid{0};
    DWORD        parentPid{0};
    std::wstring name;
    std::wstring path;
    double       cpuPercent{0.0};
    SIZE_T       workingSetBytes{0};
    SIZE_T       privateBytes{0};
    DWORD        handleCount{0};
    DWORD        threadCount{0};
    DWORD        priorityClass{0};
    ULONGLONG    createTime{0};
    bool         accessible{true};
    bool         isNew{false};
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

// ── Globals ───────────────────────────────────────────────────────────────────
static std::mutex              g_mtx;
static std::vector<ProcessInfo> g_procs;
static SysInfo                 g_si;
static std::atomic<bool>       g_running{true};
static std::atomic<int>        g_refreshMs{1000};

static std::set<DWORD>    g_knownPids;

// PDH
static PDH_HQUERY   g_pdhQuery = nullptr;
static PDH_HCOUNTER g_pdhCpu   = nullptr;

// Per-process CPU tracking
static std::map<DWORD, std::tuple<ULONGLONG,ULONGLONG,ULONGLONG>> g_prevCpu;
static int g_cpuCount = 1;

// ── Utilities ─────────────────────────────────────────────────────────────────
static std::string ws2s(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// Simple JSON escaping for string
static std::string escapeJSON(const std::string& input) {
    std::ostringstream ss;
    for (auto c : input) {
        if (c == '\"') ss << "\\\"";
        else if (c == '\\') ss << "\\\\";
        else if (c == '\b') ss << "\\b";
        else if (c == '\f') ss << "\\f";
        else if (c == '\n') ss << "\\n";
        else if (c == '\r') ss << "\\r";
        else if (c == '\t') ss << "\\t";
        else if ('\x00' <= c && c <= '\x1f') {
            ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
        } else {
            ss << c;
        }
    }
    return ss.str();
}

static ULONGLONG filetimeToULL(const FILETIME& ft){
    ULARGE_INTEGER ui; ui.LowPart=ft.dwLowDateTime; ui.HighPart=ft.dwHighDateTime;
    return ui.QuadPart;
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

// ── PDH / CPU ─────────────────────────────────────────────────────────────────
static void initPDH(){
    PdhOpenQuery(nullptr,0,&g_pdhQuery);
    PdhAddEnglishCounterW(g_pdhQuery,L"\\Processor(_Total)\\% Processor Time",0,&g_pdhCpu);
    PdhCollectQueryData(g_pdhQuery); Sleep(80); PdhCollectQueryData(g_pdhQuery);
    
    SYSTEM_INFO s2{}; GetSystemInfo(&s2); g_cpuCount=(int)s2.dwNumberOfProcessors;
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
    si.cpuLogicalCount=g_cpuCount;
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

static void dataThread(webui::window* win){
    while(g_running){
        SysInfo si; readSysInfo(si);
        auto procs=readAllProcesses();
        si.procCount=(int)procs.size();
        for(auto& p:procs){ si.threadCount+=(int)p.threadCount; si.handleCount+=(int)p.handleCount; }

        std::set<DWORD> curPids;
        for(auto& p:procs) curPids.insert(p.pid);
        for(auto& p:procs){
            if(g_knownPids.find(p.pid)==g_knownPids.end()) p.isNew=true;
        }
        g_knownPids=curPids;

        // Serialize to JSON
        std::stringstream ss;
        ss << "{";
        ss << "\"sys\":{";
        ss << "\"cpu\":" << si.cpuPercent << ",";
        ss << "\"mem\":" << si.memPercent << ",";
        ss << "\"memTotalKB\":" << si.memTotalKB << ",";
        ss << "\"memUsedKB\":" << si.memUsedKB << ",";
        ss << "\"procs\":" << si.procCount << ",";
        ss << "\"threads\":" << si.threadCount << ",";
        ss << "\"handles\":" << si.handleCount << ",";
        ss << "\"uptimeMs\":" << si.uptimeMs;
        ss << "},\"procs\":[";
        
        for(size_t i=0; i<procs.size(); ++i){
            auto& p = procs[i];
            ss << "{";
            ss << "\"pid\":" << p.pid << ",";
            ss << "\"name\":\"" << escapeJSON(ws2s(p.name)) << "\",";
            ss << "\"cpu\":" << p.cpuPercent << ",";
            ss << "\"mem\":" << p.workingSetBytes << ",";
            ss << "\"privateMem\":" << p.privateBytes << ",";
            ss << "\"threads\":" << p.threadCount << ",";
            ss << "\"handles\":" << p.handleCount << ",";
            ss << "\"priority\":" << p.priorityClass << ",";
            ss << "\"access\":" << (p.accessible ? "true" : "false") << ",";
            ss << "\"isNew\":" << (p.isNew ? "true" : "false");
            ss << "}";
            if(i != procs.size()-1) ss << ",";
        }
        ss << "]}";

        // Push to web UI
        std::string js = "updateData(" + ss.str() + ");";
        win->run(js);

        Sleep(g_refreshMs.load());
    }
}

// ── WebUI Callbacks ───────────────────────────────────────────────────────────
void killProcessHandler(webui::window::event* e) {
    long long pid = e->get_int();
    if(pid <= 4) return;
    HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,(DWORD)pid);
    if(h){ 
        TerminateProcess(h,1); 
        CloseHandle(h); 
    }
}

void setPriorityHandler(webui::window::event* e) {
    std::string s = e->get_string();
    // parse format: "pid,class_id"
    size_t comma = s.find(',');
    if(comma != std::string::npos){
        long long pid = std::stoll(s.substr(0,comma));
        long long cls = std::stoll(s.substr(comma+1));
        HANDLE h=OpenProcess(PROCESS_SET_INFORMATION,FALSE,(DWORD)pid);
        if(h){ 
            SetPriorityClass(h,(DWORD)cls); 
            CloseHandle(h); 
        }
    }
}

void setRefreshRateHandler(webui::window::event* e) {
    long long ms = e->get_int();
    if(ms >= 100) {
        g_refreshMs.store((int)ms);
    }
}

int main() {
    initPDH();
    
    webui::window win;
    win.bind("killProcess", killProcessHandler);
    win.bind("setPriority", setPriorityHandler);
    win.bind("setRefreshRate", setRefreshRateHandler);
    
    std::thread t1(dataThread, &win);
    
    // Start maxmized or big window
    win.set_size(1024, 768);
    win.set_root_folder("web");
    win.show("index.html");
    
    webui::wait();
    
    g_running = false;
    if(t1.joinable()) t1.join();
    
    return 0;
}
