#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>

#include "webui_repo/include/webui.hpp"

// ── Data structures ───────────────────────────────────────────────────────────
struct ProcessInfo {
    int    pid{0};
    int    ppid{0};
    std::string name;
    double cpuPercent{0.0};
    long   rssBytes{0};
    long   vszBytes{0};
    int    threadCount{0};
    int    handleCount{0};
    int    priorityClass{0};
    bool   accessible{true};
    bool   isNew{false};
};

struct SysInfo {
    double cpuPercent{0.0};
    long   memTotalKB{0};
    long   memUsedKB{0};
    double memPercent{0.0};
    int    procCount{0};
    int    threadCount{0};
    int    handleCount{0};
    long long uptimeMs{0};
};

// ── Globals ───────────────────────────────────────────────────────────────────
static std::mutex g_mtx;
static std::atomic<bool> g_running{true};
static std::atomic<int>  g_refreshMs{1000};
static std::set<int>     g_knownPids;

struct CpuStat {
    long long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    long long total() const { return user+nice+system+idle+iowait+irq+softirq+steal; }
    long long active() const { return total()-idle-iowait; }
};

static CpuStat g_cpuPrev;
static std::map<int, std::pair<long, long long>> g_prevTimes; // pid -> (utime+stime, wallclock_ns)
static long g_hz = 0;

// ── Utilities ─────────────────────────────────────────────────────────────────
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
        else ss << c;
    }
    return ss.str();
}

static CpuStat readCpuStat(const std::string& line){
    CpuStat s;
    sscanf(line.c_str(), "%*s %lld %lld %lld %lld %lld %lld %lld %lld",
           &s.user,&s.nice,&s.system,&s.idle,&s.iowait,&s.irq,&s.softirq,&s.steal);
    return s;
}

// ── Data collection ────────────────────────────────────────────────────────────
static void readSysInfo(SysInfo& si){
    // CPU
    std::ifstream fsCpu("/proc/stat");
    std::string line;
    if(std::getline(fsCpu, line) && line.substr(0,3)=="cpu"){
        CpuStat cs = readCpuStat(line);
        long long dTotal = cs.total() - g_cpuPrev.total();
        long long dActive = cs.active() - g_cpuPrev.active();
        si.cpuPercent = dTotal>0 ? 100.0*dActive/dTotal : 0.0;
        g_cpuPrev = cs;
    }

    // Memory
    std::ifstream fsMem("/proc/meminfo");
    long memFree=0, memAvail=0;
    while(std::getline(fsMem, line)){
        char key[64]; long val;
        if(sscanf(line.c_str(),"%63s %ld",key,&val)==2){
            std::string k(key);
            if(k=="MemTotal:") si.memTotalKB = val;
            else if(k=="MemFree:") memFree = val;
            else if(k=="MemAvailable:") memAvail = val;
        }
    }
    si.memUsedKB = si.memTotalKB - memAvail;
    si.memPercent = si.memTotalKB>0 ? 100.0*si.memUsedKB/si.memTotalKB : 0.0;

    // Uptime
    std::ifstream fsUp("/proc/uptime");
    double up; fsUp >> up;
    si.uptimeMs = (long long)(up * 1000.0);
}

static int countFd(int pid) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR* d = opendir(path);
    if(!d) return 0;
    int count = 0;
    struct dirent* ent;
    while((ent = readdir(d)) != nullptr) {
        if(ent->d_name[0] != '.') count++;
    }
    closedir(d);
    return count;
}

static bool readProcess(int pid, ProcessInfo& p, long memTotalKB){
    if(g_hz==0) g_hz = sysconf(_SC_CLK_TCK);

    char path[256];
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    std::ifstream stat(path);
    if(!stat.is_open()) return false;

    std::string line;
    std::getline(stat,line);
    size_t lp = line.find('(');
    size_t rp = line.rfind(')');
    if(lp==std::string::npos||rp==std::string::npos) return false;

    p.pid  = pid;
    p.name = line.substr(lp+1, rp-lp-1);
    
    std::istringstream iss(line.substr(rp+2));
    char state;
    long utime, stime, cutime, cstime, dummy, nice;
    int threads;
    long long starttime;
    long vsize, rss;

    iss >> state >> p.ppid;
    for(int i=0;i<9;i++) iss >> dummy; 
    iss >> utime >> stime >> cutime >> cstime
        >> dummy >> nice >> threads >> dummy
        >> starttime >> vsize >> rss;

    p.priorityClass = (int)nice;
    p.threads = threads;
    long pageKB = sysconf(_SC_PAGE_SIZE);
    p.rssBytes = rss * pageKB;
    p.vszBytes = vsize;
    
    p.handleCount = countFd(pid);
    p.accessible = (p.handleCount > 0);

    // CPU %
    long cpuTicks = utime + stime;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto it = g_prevTimes.find(pid);
    if(it!=g_prevTimes.end()){
        long dt_ticks = cpuTicks - it->second.first;
        double dt_sec = (now - it->second.second) / 1e9;
        p.cpuPercent = dt_sec>0.01 ? 100.0*dt_ticks / (g_hz*dt_sec) : 0.0;
    } else {
        p.cpuPercent = 0.0;
    }
    g_prevTimes[pid] = {cpuTicks, now};

    return true;
}

static std::vector<ProcessInfo> readAllProcesses(long memTotalKB){
    std::vector<ProcessInfo> procs;
    DIR* d = opendir("/proc");
    if(!d) return procs;
    struct dirent* ent;
    while((ent=readdir(d))!=nullptr){
        if(ent->d_type!=DT_DIR) continue;
        int pid = atoi(ent->d_name);
        if(pid<=0) continue;
        ProcessInfo p;
        if(readProcess(pid, p, memTotalKB))
            procs.push_back(p);
    }
    closedir(d);
    return procs;
}

// ── Background thread ─────────────────────────────────────────────────────────
static void dataThread(webui::window* win){
    while(g_running){
        SysInfo si; readSysInfo(si);
        auto procs = readAllProcesses(si.memTotalKB);
        
        si.procCount = (int)procs.size();
        for(auto& p:procs){ si.threadCount += p.threads; si.handleCount += p.handleCount; }

        std::set<int> curPids;
        for(auto& p:procs) curPids.insert(p.pid);
        for(auto& p:procs){
            if(g_knownPids.find(p.pid) == g_knownPids.end()) p.isNew=true;
        }
        g_knownPids = curPids;

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
            // Format nice value safely back to Windows priority class IDs for seamless frontend matching
            int winPrio = 32; // Normal (nice 0)
            if(p.priorityClass <= -20) winPrio = 256;      // Realtime
            else if(p.priorityClass <= -10) winPrio = 128; // High
            else if(p.priorityClass <= -5) winPrio = 32768;// Above Normal
            else if(p.priorityClass >= 19) winPrio = 64;   // Idle
            else if(p.priorityClass >= 5) winPrio = 16384; // Below Normal

            ss << "{";
            ss << "\"pid\":" << p.pid << ",";
            ss << "\"name\":\"" << escapeJSON(p.name) << "\",";
            ss << "\"cpu\":" << p.cpuPercent << ",";
            ss << "\"mem\":" << p.rssBytes << ",";
            ss << "\"privateMem\":" << p.vszBytes << ",";
            ss << "\"threads\":" << p.threads << ",";
            ss << "\"handles\":" << p.handleCount << ",";
            ss << "\"priority\":" << winPrio << ",";
            ss << "\"access\":" << (p.accessible ? "true" : "false") << ",";
            ss << "\"isNew\":" << (p.isNew ? "true" : "false");
            ss << "}";
            if(i != procs.size()-1) ss << ",";
        }
        ss << "]}";

        std::string js = "if(typeof updateData !== 'undefined') updateData(" + ss.str() + ");";
        win->run(js);

        std::this_thread::sleep_for(std::chrono::milliseconds(g_refreshMs.load()));
    }
}

// ── WebUI Callbacks ───────────────────────────────────────────────────────────
void killProcessHandler(webui::window::event* e) {
    long long pid = e->get_int();
    if(pid > 1) {
        kill((pid_t)pid, SIGTERM);
    }
}

void setPriorityHandler(webui::window::event* e) {
    std::string s = e->get_string();
    size_t comma = s.find(',');
    if(comma != std::string::npos){
        int pid = std::stoi(s.substr(0,comma));
        int cls = std::stoi(s.substr(comma+1));
        
        int nice_val = 0;
        if(cls == 256) nice_val = -20;
        else if(cls == 128) nice_val = -10;
        else if(cls == 32768) nice_val = -5;
        else if(cls == 32) nice_val = 0;
        else if(cls == 16384) nice_val = 5;
        else if(cls == 64) nice_val = 19;

        if(pid > 1) {
            setpriority(PRIO_PROCESS, pid, nice_val);
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
    webui::window win;
    win.bind("killProcess", killProcessHandler);
    win.bind("setPriority", setPriorityHandler);
    win.bind("setRefreshRate", setRefreshRateHandler);
    
    std::thread t1(dataThread, &win);
    
    win.set_size(1024, 768);
    win.set_root_folder("web");
    win.show("index.html");
    
    webui::wait();
    
    g_running = false;
    if(t1.joinable()) t1.join();
    
    return 0;
}
