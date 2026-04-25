/**
 * ╔══════════════════════════════════════════════════════════════╗
 * ║          PROCMON — Real-Time Process Monitor v1.0            ║
 * ║          Linux | ANSI Terminal | No dependencies             ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * Features:
 *   • Live CPU / Memory / Swap / Load gauges
 *   • Per-process table: PID, name, user, state, CPU%, MEM%, RSS, cmd
 *   • Sort by: CPU, MEM, PID, NAME  (keys: c/m/p/n)
 *   • Filter by name substring      (key: /)
 *   • Kill process                  (key: k)
 *   • Renice process                (key: r)
 *   • Scroll up/down                (j/k arrows)
 *   • Toggle tree view              (key: t)
 *   • Pause/Resume refresh          (key: space)
 *   • Quit                          (key: q)
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <signal.h>

// ─── ANSI helpers ────────────────────────────────────────────────────────────
#define ESC "\033["
#define RESET       ESC "0m"
#define BOLD        ESC "1m"
#define DIM         ESC "2m"
#define ITALIC      ESC "3m"
#define UNDERLINE   ESC "4m"
#define BLINK       ESC "5m"
#define REVERSE     ESC "7m"

// 256-color foreground/background
inline std::string fg(int c) { return ESC + std::to_string(38) + ";5;" + std::to_string(c) + "m"; }
inline std::string bg(int c) { return ESC + std::to_string(48) + ";5;" + std::to_string(c) + "m"; }

// True-color
inline std::string fgRGB(int r,int g,int b){
    return ESC "38;2;"+std::to_string(r)+";"+std::to_string(g)+";"+std::to_string(b)+"m";
}
inline std::string bgRGB(int r,int g,int b){
    return ESC "48;2;"+std::to_string(r)+";"+std::to_string(g)+";"+std::to_string(b)+"m";
}

// Cursor
inline std::string moveTo(int row,int col){
    return ESC + std::to_string(row) + ";" + std::to_string(col) + "H";
}
inline void clearScreen(){ std::cout << ESC "2J" ESC "H" << std::flush; }
inline void hideCursor()  { std::cout << ESC "?25l" << std::flush; }
inline void showCursor()  { std::cout << ESC "?25h" << std::flush; }
inline void clearLine()   { std::cout << ESC "2K"; }

// ─── Terminal size ────────────────────────────────────────────────────────────
struct TermSize { int rows=24, cols=80; };
TermSize getTermSize(){
    struct winsize w{};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    TermSize ts;
    ts.rows = w.ws_row > 0 ? w.ws_row : 24;
    ts.cols = w.ws_col > 0 ? w.ws_col : 80;
    return ts;
}

// ─── Raw mode ────────────────────────────────────────────────────────────────
static struct termios g_origTermios;
void enableRawMode(){
    tcgetattr(STDIN_FILENO, &g_origTermios);
    struct termios raw = g_origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
void disableRawMode(){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios);
}

// ─── Data structures ─────────────────────────────────────────────────────────
struct ProcessInfo {
    int    pid{0};
    int    ppid{0};
    char   state{'?'};
    std::string name;
    std::string user;
    std::string cmdline;
    long   rss{0};       // kB
    long   vsz{0};       // kB
    double cpuPercent{0.0};
    double memPercent{0.0};
    int    nice{0};
    int    threads{0};
    long   utime{0};
    long   stime{0};
    long long starttime{0};
    bool   isKernelThread{false};
};

struct CpuStat {
    long long user=0,nice=0,system=0,idle=0,iowait=0,irq=0,softirq=0,steal=0;
    long long total() const { return user+nice+system+idle+iowait+irq+softirq+steal; }
    long long active() const { return total()-idle-iowait; }
};

struct SysInfo {
    double   cpuPercent{0.0};
    double   memPercent{0.0};
    double   swapPercent{0.0};
    long     memTotalKB{0};
    long     memFreeKB{0};
    long     memAvailKB{0};
    long     memUsedKB{0};
    long     swapTotalKB{0};
    long     swapUsedKB{0};
    double   load1{0},load5{0},load15{0};
    int      procCount{0};
    int      runningProcs{0};
    int      cpuCount{0};
    long long uptime{0};
    std::vector<double> perCpuPercent;
    std::vector<CpuStat> perCpuPrev;
};

// ─── /proc readers ───────────────────────────────────────────────────────────
static CpuStat g_cpuPrev;

CpuStat readCpuStat(const std::string& line){
    CpuStat s;
    sscanf(line.c_str(), "%*s %lld %lld %lld %lld %lld %lld %lld %lld",
           &s.user,&s.nice,&s.system,&s.idle,&s.iowait,&s.irq,&s.softirq,&s.steal);
    return s;
}

void readSysInfo(SysInfo& si){
    // CPU
    {
        std::ifstream f("/proc/stat");
        std::string line;
        si.perCpuPrev = si.perCpuPercent.size() == 0
                        ? std::vector<CpuStat>{}
                        : std::vector<CpuStat>{};
        std::vector<CpuStat> newCpus;
        CpuStat newTotal;
        bool first=true;
        while(std::getline(f,line)){
            if(line.substr(0,3)=="cpu"){
                CpuStat cs = readCpuStat(line);
                if(first){ first=false; newTotal=cs; }
                else      { newCpus.push_back(cs); }
            }
        }
        long long dTotal  = newTotal.total()  - g_cpuPrev.total();
        long long dActive = newTotal.active() - g_cpuPrev.active();
        si.cpuPercent = dTotal>0 ? 100.0*dActive/dTotal : 0.0;

        // Per-CPU
        if(si.perCpuPrev.size()==newCpus.size()){
            si.perCpuPercent.resize(newCpus.size());
            for(size_t i=0;i<newCpus.size();i++){
                long long dt = newCpus[i].total()  - si.perCpuPrev[i].total();
                long long da = newCpus[i].active() - si.perCpuPrev[i].active();
                si.perCpuPercent[i] = dt>0 ? 100.0*da/dt : 0.0;
            }
        } else {
            si.perCpuPercent.assign(newCpus.size(),0.0);
        }
        si.perCpuPrev = newCpus;
        si.cpuCount = (int)newCpus.size();
        g_cpuPrev = newTotal;
    }

    // Memory
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        std::map<std::string,long> m;
        while(std::getline(f,line)){
            char key[64]; long val;
            if(sscanf(line.c_str(),"%63s %ld",key,&val)==2){
                std::string k(key);
                if(k.back()==':') k.pop_back();
                m[k]=val;
            }
        }
        si.memTotalKB  = m["MemTotal"];
        si.memFreeKB   = m["MemFree"];
        si.memAvailKB  = m["MemAvailable"];
        si.memUsedKB   = si.memTotalKB - si.memAvailKB;
        si.swapTotalKB = m["SwapTotal"];
        long swapFree  = m["SwapFree"];
        si.swapUsedKB  = si.swapTotalKB - swapFree;
        si.memPercent  = si.memTotalKB>0 ? 100.0*si.memUsedKB/si.memTotalKB : 0.0;
        si.swapPercent = si.swapTotalKB>0 ? 100.0*si.swapUsedKB/si.swapTotalKB : 0.0;
    }

    // Load + uptime
    {
        std::ifstream f("/proc/loadavg");
        int run,tot;
        f >> si.load1 >> si.load5 >> si.load15;
        char slash; f >> run >> slash >> tot;
        si.runningProcs = run;
        si.procCount    = tot;
    }
    {
        std::ifstream f("/proc/uptime");
        double up; f >> up;
        si.uptime = (long long)up;
    }
}

// User lookup cache
static std::map<uid_t, std::string> g_userCache;
std::string uidToUser(uid_t uid){
    auto it = g_userCache.find(uid);
    if(it!=g_userCache.end()) return it->second;
    struct passwd* pw = getpwuid(uid);
    std::string name = pw ? pw->pw_name : std::to_string(uid);
    g_userCache[uid] = name;
    return name;
}

// Previous CPU times per PID
static std::map<int, std::pair<long,long long>> g_prevTimes; // pid -> (utime+stime, wallclock_ns)
static long g_hz = 0;

bool readProcess(int pid, ProcessInfo& p, long memTotalKB){
    if(g_hz==0) g_hz = sysconf(_SC_CLK_TCK);

    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    std::ifstream stat(path);
    if(!stat.is_open()) return false;

    std::string line;
    std::getline(stat,line);

    // Parse comm (may contain spaces/parens)
    size_t lp = line.find('(');
    size_t rp = line.rfind(')');
    if(lp==std::string::npos||rp==std::string::npos) return false;

    p.pid  = pid;
    p.name = line.substr(lp+1, rp-lp-1);

    std::istringstream iss(line.substr(rp+2));
    long utime,stime,cutime,cstime;
    long priority,nice;
    int threads;
    long long starttime;
    long vsize; long rss;

    iss >> p.state
        >> p.ppid
        >> std::ws; // skip pgrp,session,tty_nr,tpgid,flags
    long dummy;
    for(int i=0;i<4;i++) iss>>dummy;
    // minflt cminflt majflt cmajflt
    for(int i=0;i<4;i++) iss>>dummy;
    iss >> utime >> stime >> cutime >> cstime
        >> priority >> nice >> threads >> dummy
        >> starttime >> vsize >> rss;

    p.utime     = utime;
    p.stime     = stime;
    p.starttime = starttime;
    p.nice      = (int)nice;
    p.threads   = threads;
    p.vsz       = vsize/1024;
    long pageKB = sysconf(_SC_PAGE_SIZE)/1024;
    p.rss       = rss * pageKB;
    p.memPercent = memTotalKB>0 ? 100.0*p.rss/memTotalKB : 0.0;
    p.isKernelThread = (vsize==0);

    // CPU %
    long cpuTicks = utime + stime;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto it = g_prevTimes.find(pid);
    if(it!=g_prevTimes.end()){
        long dt_ticks = cpuTicks - it->second.first;
        double dt_sec = (now - it->second.second) / 1e9;
        p.cpuPercent = dt_sec>0.01 ? 100.0*dt_ticks / (g_hz*dt_sec) : 0.0;
    }
    g_prevTimes[pid] = {cpuTicks, now};

    // User
    struct stat st{};
    snprintf(path,sizeof(path),"/proc/%d",pid);
    if(::stat(path,&st)==0) p.user = uidToUser(st.st_uid);
    else p.user = "?";

    // Cmdline
    snprintf(path,sizeof(path),"/proc/%d/cmdline",pid);
    std::ifstream cmd(path);
    std::string token;
    p.cmdline.clear();
    while(std::getline(cmd,token,'\0')){
        if(!p.cmdline.empty()) p.cmdline+=' ';
        p.cmdline+=token;
        if((int)p.cmdline.size()>200) break;
    }
    if(p.cmdline.empty()) p.cmdline = "["+p.name+"]";

    return true;
}

std::vector<ProcessInfo> readAllProcesses(long memTotalKB){
    std::vector<ProcessInfo> procs;
    DIR* d = opendir("/proc");
    if(!d) return procs;
    struct dirent* ent;
    while((ent=readdir(d))!=nullptr){
        if(ent->d_type!=DT_DIR) continue;
        int pid = atoi(ent->d_name);
        if(pid<=0) continue;
        ProcessInfo p;
        if(readProcess(pid,p,memTotalKB))
            procs.push_back(p);
    }
    closedir(d);
    return procs;
}

// ─── UI helpers ──────────────────────────────────────────────────────────────
std::string repeat(const std::string& s, int n){
    std::string r;
    for(int i=0;i<n;i++) r+=s;
    return r;
}

std::string truncate(const std::string& s, int w){
    if((int)s.size()<=w) return s;
    return s.substr(0,w-1)+"…";
}

std::string padRight(const std::string& s, int w){
    if((int)s.size()>=w) return s.substr(0,w);
    return s + std::string(w-s.size(),' ');
}
std::string padLeft(const std::string& s, int w){
    if((int)s.size()>=w) return s.substr(0,w);
    return std::string(w-s.size(),' ') + s;
}

std::string fmtKB(long kb){
    if(kb<1024)     return std::to_string(kb)+"K";
    if(kb<1024*1024)return std::to_string(kb/1024)+"M";
    return std::to_string(kb/(1024*1024))+"G";
}
std::string fmtUptime(long long s){
    char buf[64];
    long long d=s/86400, h=(s%86400)/3600, m=(s%3600)/60, sec=s%60;
    if(d>0) snprintf(buf,sizeof(buf),"%lldd %02lldh %02lldm",d,h,m);
    else    snprintf(buf,sizeof(buf),"%02lldh %02lldm %02llds",h,m,sec);
    return buf;
}

// Gradient bar
std::string makeBar(double pct, int width, bool color=true){
    int fill = (int)(pct/100.0*width+0.5);
    fill = std::max(0,std::min(width,fill));

    // Color: green→yellow→red
    std::string barColor;
    if(color){
        if(pct<50)       barColor = fgRGB(80,220,100);
        else if(pct<75)  barColor = fgRGB(240,200,50);
        else if(pct<90)  barColor = fgRGB(240,130,30);
        else             barColor = fgRGB(220,50,50);
    }

    std::string bar = color ? barColor : "";
    for(int i=0;i<fill;i++)        bar += "\xe2\x96\x88"; // █
    bar += fg(238);
    for(int i=fill;i<width;i++)    bar += "\xe2\x96\x91"; // ░
    bar += RESET;
    return bar;
}

// State color
std::string stateColor(char s){
    switch(s){
        case 'R': return fgRGB(80,220,100);   // running - green
        case 'S': return fgRGB(100,170,255);  // sleeping - blue
        case 'D': return fgRGB(240,200,50);   // uninterruptible - yellow
        case 'Z': return fgRGB(220,50,50);    // zombie - red
        case 'T': return fgRGB(200,100,240);  // stopped - purple
        case 'I': return fg(238);             // idle kernel - dark
        default:  return RESET;
    }
}
std::string stateName(char s){
    switch(s){
        case 'R': return "RUN";
        case 'S': return "SLP";
        case 'D': return "DSK";
        case 'Z': return "ZOM";
        case 'T': return "STP";
        case 'I': return "IDL";
        default:  return "???";
    }
}

// ─── Dashboard state ─────────────────────────────────────────────────────────
enum SortBy { SORT_CPU, SORT_MEM, SORT_PID, SORT_NAME, SORT_RSS };

struct DashState {
    SysInfo          si;
    std::vector<ProcessInfo> procs;
    std::mutex       mtx;

    SortBy   sortBy     = SORT_CPU;
    bool     sortAsc    = false;
    int      scrollTop  = 0;
    bool     paused     = false;
    bool     treeView   = false;
    bool     showKernel = false;
    std::string filter;
    bool     filtering  = false;

    int      selectedPid = -1;
    bool     showHelp    = false;

    std::string statusMsg;
    std::chrono::steady_clock::time_point statusExpiry;
};

static DashState g_state;
static std::atomic<bool> g_running{true};

void setStatus(const std::string& msg, int ms=2500){
    g_state.statusMsg   = msg;
    g_state.statusExpiry = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(ms);
}

// ─── Data fetch thread ───────────────────────────────────────────────────────
void dataThread(){
    while(g_running){
        if(!g_state.paused){
            SysInfo si;
            readSysInfo(si);
            auto procs = readAllProcesses(si.memTotalKB);
            {
                std::lock_guard<std::mutex> lk(g_state.mtx);
                g_state.si    = si;
                g_state.procs = procs;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
}

// ─── Rendering ───────────────────────────────────────────────────────────────
void renderHeader(const SysInfo& si, const TermSize& ts, const DashState& ds){
    int col = ts.cols;

    // ── Title bar ──
    std::string title = " ◈ PROCMON  Real-Time Process Monitor";
    std::string right = " uptime: "+fmtUptime(si.uptime)+" │ "+std::to_string(si.procCount)+" procs ";
    int pad = col - (int)title.size() - (int)right.size() - 2;

    std::cout << moveTo(1,1)
              << bgRGB(15,30,55) << fgRGB(80,200,255) << BOLD
              << " " << title
              << std::string(std::max(0,pad),' ')
              << fgRGB(150,170,200) << right
              << RESET << "\n";

    // ── CPU + MEM row ──
    int barW = std::max(10, (col/2) - 22);

    // CPU total
    char cpuf[16]; snprintf(cpuf,sizeof(cpuf),"%.1f%%",si.cpuPercent);
    std::cout << moveTo(2,1)
              << bgRGB(10,20,40) << fgRGB(80,200,255) << BOLD << " CPU " << RESET
              << bgRGB(10,20,40)
              << " " << makeBar(si.cpuPercent, barW)
              << " " << bgRGB(10,20,40);
    if(si.cpuPercent>=90)      std::cout << fgRGB(220,50,50);
    else if(si.cpuPercent>=75) std::cout << fgRGB(240,130,30);
    else                        std::cout << fgRGB(80,220,100);
    std::cout << BOLD << padLeft(cpuf,6) << RESET
              << bgRGB(10,20,40) << fgRGB(150,160,180)
              << "  " << std::to_string(si.cpuCount) << " cores"
              << "  load: "
              << fgRGB(200,200,200) << std::fixed<<std::setprecision(2)
              << si.load1 << " " << si.load5 << " " << si.load15
              << RESET;

    // MEM
    char memf[32]; snprintf(memf,sizeof(memf),"%.1f%%",si.memPercent);
    std::cout << moveTo(3,1)
              << bgRGB(10,20,40) << fgRGB(150,100,255) << BOLD << " MEM " << RESET
              << bgRGB(10,20,40)
              << " " << makeBar(si.memPercent, barW)
              << " " << bgRGB(10,20,40);
    if(si.memPercent>=90)      std::cout << fgRGB(220,50,50);
    else if(si.memPercent>=75) std::cout << fgRGB(240,130,30);
    else                        std::cout << fgRGB(150,100,255);
    std::cout << BOLD << padLeft(memf,6) << RESET
              << bgRGB(10,20,40) << fgRGB(150,160,180)
              << "  used: " << fgRGB(200,200,200) << fmtKB(si.memUsedKB)
              << fgRGB(150,160,180) << " / " << fgRGB(200,200,200) << fmtKB(si.memTotalKB)
              << fgRGB(150,160,180) << "  avail: " << fgRGB(200,200,200) << fmtKB(si.memAvailKB)
              << RESET;

    // SWAP
    if(si.swapTotalKB>0){
        char swpf[16]; snprintf(swpf,sizeof(swpf),"%.1f%%",si.swapPercent);
        std::cout << moveTo(4,1)
                  << bgRGB(10,20,40) << fgRGB(255,140,80) << BOLD << " SWP " << RESET
                  << bgRGB(10,20,40)
                  << " " << makeBar(si.swapPercent, barW)
                  << " " << bgRGB(10,20,40);
        if(si.swapPercent>=90)      std::cout << fgRGB(220,50,50);
        else if(si.swapPercent>0)   std::cout << fgRGB(255,140,80);
        else                         std::cout << fgRGB(80,220,100);
        std::cout << BOLD << padLeft(swpf,6) << RESET
                  << bgRGB(10,20,40) << fgRGB(150,160,180)
                  << "  used: " << fgRGB(200,200,200) << fmtKB(si.swapUsedKB)
                  << fgRGB(150,160,180) << " / " << fgRGB(200,200,200) << fmtKB(si.swapTotalKB)
                  << RESET;
    }

    // Per-CPU sparkline row
    int row5col = 2;
    std::cout << moveTo(5,1) << bgRGB(10,20,40) << " " << fgRGB(100,130,170) << "CORES ";
    for(int i=0;i<(int)si.perCpuPercent.size() && row5col<col-4; i++){
        double p = si.perCpuPercent[i];
        // mini bar 5 chars
        std::string clr;
        if(p<50) clr=fgRGB(60,180,80);
        else if(p<80) clr=fgRGB(220,180,40);
        else clr=fgRGB(220,60,60);
        char buf[16]; snprintf(buf,sizeof(buf),"%3.0f%%",p);
        std::cout << clr << buf << fgRGB(60,70,90) << "│";
        row5col += 5;
    }
    std::cout << RESET;

    // Separator
    std::cout << moveTo(6,1) << fgRGB(30,50,80) << repeat("─",col) << RESET << "\n";
}

void renderTableHeader(int row, int col, const DashState& ds){
    auto mark = [&](SortBy sb, const char* label) -> std::string {
        if(ds.sortBy==sb){
            return std::string(bgRGB(30,60,100)) + fgRGB(255,220,60) + BOLD + label + RESET
                 + bgRGB(18,28,48) + (ds.sortAsc ? "↑" : "↓") + " ";
        }
        return std::string(fgRGB(140,160,180)) + label + "  ";
    };

    std::cout << moveTo(row,1)
              << bgRGB(18,28,48) << fgRGB(100,130,170)
              << " " << padRight("PID",7)
              << padRight("USER",10)
              << " ST "
              << mark(SORT_CPU," CPU% ")
              << mark(SORT_MEM," MEM% ")
              << padRight("RSS",8)
              << padRight("VSZ",8)
              << "THR  NI  "
              << mark(SORT_NAME,"COMMAND")
              << std::string(col,' ').substr(0,std::max(0,col-80))
              << RESET;
}

void renderProcessRow(int row, const ProcessInfo& p, bool selected, int nameWidth){
    std::string rowBg = selected ? bgRGB(30,55,90) : bgRGB(12,20,35);
    std::string altBg = selected ? bgRGB(30,55,90) : bgRGB(14,23,40);
    std::string rb    = (row%2==0) ? rowBg : altBg;

    // PID
    std::cout << moveTo(row,1) << rb
              << fgRGB(150,180,220) << padLeft(std::to_string(p.pid),7) << " "
              // user
              << fgRGB(120,200,160) << padRight(truncate(p.user,9),10)
              // state
              << stateColor(p.state) << BOLD << " " << stateName(p.state) << " " << RESET << rb
              ;

    // CPU%
    char cpuf[16]; snprintf(cpuf,sizeof(cpuf),"%6.1f",p.cpuPercent);
    std::string cpuClr;
    if(p.cpuPercent>80)      cpuClr=fgRGB(220,60,60);
    else if(p.cpuPercent>40) cpuClr=fgRGB(240,180,50);
    else if(p.cpuPercent>5)  cpuClr=fgRGB(80,210,100);
    else                      cpuClr=fgRGB(80,100,120);
    std::cout << rb << cpuClr << BOLD << cpuf << RESET << rb << "  ";

    // MEM%
    char memf[16]; snprintf(memf,sizeof(memf),"%6.1f",p.memPercent);
    std::string memClr;
    if(p.memPercent>20)      memClr=fgRGB(220,80,80);
    else if(p.memPercent>10) memClr=fgRGB(240,180,50);
    else if(p.memPercent>1)  memClr=fgRGB(150,100,240);
    else                      memClr=fgRGB(80,100,120);
    std::cout << memClr << BOLD << memf << RESET << rb << "  ";

    // RSS / VSZ
    std::cout << fgRGB(160,175,195) << padLeft(fmtKB(p.rss),7) << " "
              << fgRGB(100,115,140) << padLeft(fmtKB(p.vsz),7) << " ";

    // Threads + Nice
    std::cout << fgRGB(130,160,190) << padLeft(std::to_string(p.threads),3) << "  ";
    if(p.nice<0)       std::cout << fgRGB(240,180,50);
    else if(p.nice>0)  std::cout << fgRGB(100,160,240);
    else               std::cout << fgRGB(100,115,140);
    char nicef[8]; snprintf(nicef,sizeof(nicef),"%+3d",p.nice);
    std::cout << nicef << "  ";

    // Command
    std::string cmd = truncate(p.cmdline, nameWidth);
    if(p.isKernelThread)
        std::cout << fg(238) << ITALIC << padRight(cmd,nameWidth);
    else if(p.state=='Z')
        std::cout << fgRGB(220,60,60) << padRight(cmd,nameWidth);
    else
        std::cout << fgRGB(190,205,225) << padRight(cmd,nameWidth);

    std::cout << RESET;
}

void renderFooter(int row, int col, const DashState& ds){
    std::cout << moveTo(row,1) << fgRGB(30,50,80) << repeat("─",col) << RESET;
    std::cout << moveTo(row+1,1) << bgRGB(10,18,35);

    // Status / filter
    if(ds.filtering){
        std::cout << fgRGB(255,220,60) << BOLD << " Filter: " << RESET
                  << fgRGB(200,220,255) << ds.filter << fgRGB(255,255,255) << "█"
                  << RESET;
    } else if(!ds.statusMsg.empty() &&
              std::chrono::steady_clock::now()<ds.statusExpiry){
        std::cout << fgRGB(100,220,140) << BOLD << " ✓ " << RESET
                  << fgRGB(200,220,200) << ds.statusMsg << RESET;
    } else {
        std::string sort_label = ds.sortBy==SORT_CPU?"CPU":
                                 ds.sortBy==SORT_MEM?"MEM":
                                 ds.sortBy==SORT_PID?"PID":
                                 ds.sortBy==SORT_RSS?"RSS":"NAME";
        std::cout << fgRGB(70,90,120)
                  << " [c]pu [m]em [p]id [n]ame  "
                  << "[/]filter  [k]ill  [r]enice  "
                  << "[t]ree  [K]thread  "
                  << "[spc]pause  [q]uit"
                  << "   sort:" << fgRGB(255,220,60) << sort_label
                  << (ds.paused ? fgRGB(220,80,80) + std::string("  ⏸ PAUSED") : "")
                  << RESET;
    }
    // fill rest of line
    std::cout << ESC "K" << RESET;
}

void render(DashState& ds){
    TermSize ts = getTermSize();
    int col = ts.cols;
    int rows= ts.rows;

    SysInfo si;
    std::vector<ProcessInfo> procs;
    {
        std::lock_guard<std::mutex> lk(ds.mtx);
        si    = ds.si;
        procs = ds.procs;
    }

    // Filter
    if(!ds.filter.empty()){
        std::vector<ProcessInfo> filtered;
        for(auto& p: procs){
            if(p.cmdline.find(ds.filter)!=std::string::npos ||
               p.name.find(ds.filter)!=std::string::npos)
                filtered.push_back(p);
        }
        procs = filtered;
    }

    // Hide kernel threads unless toggled
    if(!ds.showKernel){
        procs.erase(std::remove_if(procs.begin(),procs.end(),
            [](const ProcessInfo& p){ return p.isKernelThread; }),
            procs.end());
    }

    // Sort
    auto cmp = [&](const ProcessInfo& a, const ProcessInfo& b) -> bool {
        bool res;
        switch(ds.sortBy){
            case SORT_CPU:  res = a.cpuPercent > b.cpuPercent; break;
            case SORT_MEM:  res = a.memPercent  > b.memPercent; break;
            case SORT_PID:  res = a.pid < b.pid; break;
            case SORT_NAME: res = a.name < b.name; break;
            case SORT_RSS:  res = a.rss  > b.rss; break;
            default:        res = a.cpuPercent > b.cpuPercent;
        }
        return ds.sortAsc ? !res : res;
    };
    std::sort(procs.begin(), procs.end(), cmp);

    // Rendering area
    int headerRows = 7;  // rows 1-6 + blank
    int footerRows = 2;
    int tableHeaderRow = headerRows+1;
    int tableStartRow  = tableHeaderRow+1;
    int tableRows      = rows - tableStartRow - footerRows;
    int nameWidth      = std::max(10, col - 62);

    // Clamp scroll
    int maxScroll = std::max(0,(int)procs.size()-tableRows);
    ds.scrollTop  = std::max(0,std::min(ds.scrollTop,maxScroll));

    std::string frame;
    frame.reserve(65536);
    // Redirect output through buffer for flicker-free draw
    // (We just cout directly but move cursor to top first)
    std::cout << ESC "?25l"; // hide cursor during draw

    // Header
    renderHeader(si, ts, ds);

    // Table header
    renderTableHeader(tableHeaderRow, col, ds);

    // Rows
    for(int i=0;i<tableRows;i++){
        int idx = ds.scrollTop+i;
        int drawRow = tableStartRow+i;
        if(idx < (int)procs.size()){
            bool sel = (procs[idx].pid == ds.selectedPid);
            renderProcessRow(drawRow, procs[idx], sel, nameWidth);
        } else {
            std::cout << moveTo(drawRow,1) << bgRGB(12,20,35) << ESC "K" << RESET;
        }
    }

    // Footer
    renderFooter(rows-1, col, ds);

    // Scrollbar
    if((int)procs.size()>tableRows){
        int sbH = tableRows;
        int sbPos = (int)((double)ds.scrollTop/maxScroll*(sbH-1));
        for(int i=0;i<sbH;i++){
            std::cout << moveTo(tableStartRow+i,col);
            if(i==sbPos) std::cout << fgRGB(80,150,255) << "█" << RESET;
            else          std::cout << fgRGB(30,50,80)   << "│" << RESET;
        }
    }

    // Stats line on row 7
    std::cout << moveTo(7,1) << bgRGB(10,20,40) << fgRGB(80,110,150)
              << " Tasks: " << fgRGB(200,200,200) << procs.size()
              << fgRGB(80,110,150) << "  running: " << fgRGB(80,220,100) << si.runningProcs
              << fgRGB(80,110,150) << "  filter: "
              << (ds.filter.empty() ? fgRGB(80,100,120)+"none" : fgRGB(255,220,60)+ds.filter)
              << RESET << ESC "K";

    std::cout << ESC "?25h"; // show cursor
    std::cout.flush();
}

// ─── Input handler ───────────────────────────────────────────────────────────
// Returns selected PID index in current sorted list
int getSelectedIdx(DashState& ds){
    // find selectedPid in current procs
    for(int i=0;i<(int)ds.procs.size();i++)
        if(ds.procs[i].pid==ds.selectedPid) return i;
    return -1;
}

void handleInput(DashState& ds){
    char c=0;
    if(read(STDIN_FILENO,&c,1)!=1) return;

    if(ds.filtering){
        if(c=='\n'||c==27){ ds.filtering=false; return; }
        if(c==127||c==8){
            if(!ds.filter.empty()) ds.filter.pop_back();
            return;
        }
        ds.filter += c;
        return;
    }

    // Arrow keys: ESC [ A/B/C/D
    if(c==27){
        char seq[3]={};
        if(read(STDIN_FILENO,&seq[0],1)==1 && seq[0]=='['){
            read(STDIN_FILENO,&seq[1],1);
            if(seq[1]=='A') { ds.scrollTop=std::max(0,ds.scrollTop-1); return; } // up
            if(seq[1]=='B') { ds.scrollTop++; return; }                           // down
        }
        return;
    }

    switch(c){
        case 'q': case 'Q': g_running=false; break;
        case 'c': ds.sortBy=SORT_CPU;  ds.sortAsc=false; break;
        case 'm': ds.sortBy=SORT_MEM;  ds.sortAsc=false; break;
        case 'p': ds.sortBy=SORT_PID;  ds.sortAsc=true;  break;
        case 'n': ds.sortBy=SORT_NAME; ds.sortAsc=true;  break;
        case 'r': ds.sortBy=SORT_RSS;  ds.sortAsc=false; break;
        case ' ': ds.paused=!ds.paused;
                  setStatus(ds.paused?"Paused":"Resumed"); break;
        case 't': ds.treeView=!ds.treeView; break;
        case 'K': ds.showKernel=!ds.showKernel;
                  setStatus(ds.showKernel?"Kernel threads shown":"Kernel threads hidden"); break;
        case '/': ds.filtering=true; ds.filter.clear(); break;
        case 27:  ds.filter.clear(); break; // ESC clears filter

        case 'j': ds.scrollTop++; break;
        case 'k': ds.scrollTop=std::max(0,ds.scrollTop-1); break;

        case 'G': ds.scrollTop=999999; break; // will be clamped
        case 'g': ds.scrollTop=0; break;

        default: break;
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────
static void sigHandler(int){ g_running=false; }

int main(){
    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);
    signal(SIGWINCH, [](int){ /* will re-query on next render */ });

    // Init CPU baseline
    {
        std::ifstream f("/proc/stat");
        std::string line; std::getline(f,line);
        g_cpuPrev = readCpuStat(line);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    enableRawMode();
    hideCursor();
    clearScreen();

    // Start data thread
    std::thread dt(dataThread);

    // Render loop
    while(g_running){
        handleInput(g_state);
        if(g_running) render(g_state);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    g_running=false;
    dt.join();

    disableRawMode();
    showCursor();
    clearScreen();
    std::cout << fgRGB(80,200,255) << BOLD << "\n  PROCMON exited.\n" << RESET;
    return 0;
}
