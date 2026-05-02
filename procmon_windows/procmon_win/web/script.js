let procsData = [];
let sortCol = 'cpu';
let sortAsc = false;
let searchFilter = '';
let selectedPid = null;

// Context Menu 
const ctxMenu = document.getElementById('context-menu');
let ctxPid = 0;

document.addEventListener('click', (e) => {
    if(!e.target.closest('#context-menu')) {
        ctxMenu.classList.remove('active');
    }
});

// Invoked by C++ backend
window.updateData = function(data) {
    if(!data) return;

    // Update Sys Stats
    document.getElementById('cpu-val').innerText = data.sys.cpu.toFixed(1) + '%';
    document.getElementById('cpu-fill').style.width = data.sys.cpu + '%';
    
    document.getElementById('mem-val').innerText = data.sys.mem.toFixed(1) + '%';
    document.getElementById('mem-fill').style.width = data.sys.mem + '%';

    document.getElementById('stat-procs').innerText = data.sys.procs;
    document.getElementById('stat-threads').innerText = data.sys.threads;
    document.getElementById('stat-handles').innerText = data.sys.handles;
    
    // Uptime formatting
    let s = Math.floor(data.sys.uptimeMs / 1000);
    let d = Math.floor(s / 86400);
    let h = Math.floor((s % 86400) / 3600);
    let m = Math.floor((s % 3600) / 60);
    let tm = (d>0 ? `${d}d ` : '') + `${h.toString().padStart(2,'0')}h ${m.toString().padStart(2,'0')}m`;
    document.getElementById('stat-uptime').innerText = tm;

    // Save processes and render
    procsData = data.procs;
    renderTable();
};

function formatMB(bytes) {
    let mb = bytes / 1048576;
    if (mb > 1024) return (mb / 1024).toFixed(2) + ' GB';
    return mb.toFixed(1) + ' MB';
}

function getPriorityStr(cls) {
    switch(cls) {
        case 256: return "Realtime";
        case 128: return "High";
        case 32768: return "Above Normal";
        case 32: return "Normal";
        case 16384: return "Below Normal";
        case 64: return "Idle";
        default: return "Unknown";
    }
}

function setSort(col) {
    if (sortCol === col) {
        sortAsc = !sortAsc;
    } else {
        sortCol = col;
        sortAsc = (col === 'pid' || col === 'name');
    }
    
    document.querySelectorAll('.sort-icon').forEach(el => el.innerText = '');
    document.getElementById('sort-' + col).innerText = sortAsc ? '▲' : '▼';
    
    renderTable();
}

function applyFilter() {
    searchFilter = document.getElementById('search-box').value.toLowerCase();
    renderTable();
}

function renderTable() {
    let filtered = procsData;
    if (searchFilter) {
        filtered = filtered.filter(p => !p.name || p.name.toLowerCase().includes(searchFilter) || p.pid.toString().includes(searchFilter));
    }

    filtered.sort((a, b) => {
        let va = a[sortCol], vb = b[sortCol];
        if(sortCol === 'name') {
            va = va ? va.toLowerCase() : ''; vb = vb ? vb.toLowerCase() : '';
            return sortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
        }
        return sortAsc ? (va - vb) : (vb - va);
    });

    const tbody = document.getElementById('proc-body');
    let html = '';

    filtered.forEach(p => {
        let isSel = p.pid === selectedPid ? 'selected' : '';
        let isNew = p.isNew ? 'is-new' : '';
        let unacc = !p.access ? 'text-dim' : '';

        // Colors
        let cpuCls = p.cpu > 50 ? 'c-red' : (p.cpu > 20 ? 'c-yellow' : (p.cpu > 2 ? 'c-green' : 'text-dim'));
        let mbW = p.mem / 1048576;
        let memCls = mbW > 1000 ? 'c-red' : (mbW > 200 ? 'c-yellow' : (mbW > 10 ? 'c-acc-purp' : 'text-dim'));
        let mbP = p.privateMem / 1048576;
        let privCls = mbP > 1000 ? 'c-red' : (mbP > 200 ? 'c-yellow' : (mbP > 10 ? 'c-acc-blue' : 'text-dim'));
        
        let hanCls = p.handles > 5000 ? 'c-red' : (p.handles > 1000 ? 'c-yellow' : '');
        let thdCls = p.threads > 50 ? 'c-orange' : (p.threads > 20 ? 'c-yellow' : '');
        let priCls = p.priority === 256 ? 'c-red' : (p.priority === 128 ? 'c-orange' : '');

        html += `<tr class="${isSel} ${isNew} ${unacc}" data-pid="${p.pid}" onclick="selectRow(${p.pid}, '${p.name}')" oncontextmenu="showCtxMenu(event, ${p.pid}, '${p.name}')">
            <td class="text-dim">${p.pid}</td>
            <td>${p.name}</td>
            <td class="${cpuCls}">${p.cpu.toFixed(1)}%</td>
            <td class="${memCls}">${formatMB(p.mem)}</td>
            <td class="${privCls}">${formatMB(p.privateMem)}</td>
            <td class="${hanCls}">${p.handles}</td>
            <td class="${thdCls}">${p.threads}</td>
            <td class="${priCls}">${getPriorityStr(p.priority)}</td>
        </tr>`;
    });

    tbody.innerHTML = html;
}

function selectRow(pid, name) {
    selectedPid = pid;
    renderTable();
}

function killSelected() {
    if(selectedPid) {
        if(confirm(`Kill process PID ${selectedPid}?`)) {
            // Using WebUI binding
            killProcess(selectedPid);
        }
    }
}

function showCtxMenu(e, pid, name) {
    e.preventDefault();
    selectRow(pid, name);
    ctxPid = pid;
    document.getElementById('ctx-header').innerText = `PID ${pid} — ${name}`;
    ctxMenu.style.left = e.pageX + 'px';
    ctxMenu.style.top = e.pageY + 'px';
    ctxMenu.classList.add('active');
}

function ctxKill() {
    ctxMenu.classList.remove('active');
    if(confirm(`Kill process PID ${ctxPid}?`)) {
        killProcess(ctxPid);
    }
}

function ctxSetPriority(clsId) {
    ctxMenu.classList.remove('active');
    // Using WebUI binding with string format pid,clsId
    setPriority(`${ctxPid},${clsId}`);
}

function changeRefreshRate() {
    let rate = document.getElementById('refresh-rate').value;
    setRefreshRate(parseInt(rate));
}

// Initial sorting UI
document.getElementById('sort-cpu').innerText = '▼';
