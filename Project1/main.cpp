// Updated per MO2 spec: demand paging, backing store, READ/WRITE instructions,
// new commands: process-smi, vmstat, screen -s/-c with memory sizing and validation.

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <random>
#include <chrono>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

using namespace std;

/* ======================================================
   Utility & Console UI implementation
   ====================================================== */
static std::mutex cout_mtx;
void safe_print(const string& s) {
    lock_guard<std::mutex> lg(cout_mtx);
    cout << s << flush;
}
void clear_console() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static vector<string> split_args(const string& line) {
    vector<string> out;
    istringstream iss(line);
    string t;
    while (iss >> t) out.push_back(t);
    return out;
}
static bool is_power_of_two(uint64_t x) { return x && ((x & (x - 1)) == 0); }
static string to_hex(uint32_t v) { ostringstream oss; oss << "0x" << hex << uppercase << v; return oss.str(); }
static bool parse_hex(const string& s, uint32_t& out) {
    try {
        size_t idx = 0;
        unsigned long v = stoul(s, &idx, 0); // handles 0x
        out = (uint32_t)v;
        return true;
    } catch (...) { return false; }
}

/* ======================================================
   Config (global configuration loaded by 'initialize')
   ====================================================== */
struct Config {
    int num_cpu = 1;
    string scheduler = "fcfs"; // fcfs or rr
    uint64_t quantum_cycles = 1;
    uint64_t batch_process_freq = 10; // cycles
    uint64_t min_ins = 1;
    uint64_t max_ins = 5;
    uint64_t delays_per_exec = 0;
    // MO2: memory config
    uint64_t max_overall_mem = 65536; // bytes
    uint64_t mem_per_frame = 256;     // bytes
    uint64_t min_mem_per_proc = 64;   // bytes
    uint64_t max_mem_per_proc = 1024; // bytes
    bool valid = false;
};
Config global_cfg;

/* ======================================================
   Process representation & new memory fields
   ====================================================== */
enum class InstType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR_START, FOR_END, NOOP, READ, WRITE };

struct Instruction {
    InstType type;
    string a, b, c; // flexible operands (var names, hex addresses, messages)
    uint64_t numeric = 0;
    bool c_is_const = false;
};

enum class ProcState { READY, RUNNING, SLEEPING, FINISHED, TERMINATED_BY_MEM };

struct Process {
    string name;
    uint64_t pid;
    vector<Instruction> instrs;
    size_t pc = 0;
    unordered_map<string, uint16_t> vars;
    vector<string> logs;
    ProcState state = ProcState::READY;
    uint64_t remaining_sleep = 0;
    uint64_t ticks_used = 0;
    vector<pair<size_t, uint64_t>> for_stack;
    uint64_t quantum_left = 0;
    uint64_t delay_left = 0;
    bool attached = false;
    time_t created_time = 0;

    // MO2 memory fields
    uint32_t memBytes = 0;                    // total virtual memory bytes allocated to process
    uint32_t numPages = 0;                    // number of pages
    unordered_map<uint32_t, int> vpageToFrame; // vpage -> frame index if resident
    unordered_set<uint32_t> dirtyPages;       // set of dirty vpages
    bool memViolation = false;
    uint32_t violationAddr = 0;
    time_t violationTime = 0;
    // symbol table layout: first 64 bytes reserved for up to 32 uint16 variables
    static constexpr uint32_t symbol_table_size = 64;
};
static std::mutex procs_mtx;
static unordered_map<string, shared_ptr<Process>> proc_table;
static vector<shared_ptr<Process>> proc_list;
static std::atomic<uint64_t> next_pid{ 1 };

/* --------------------- Small helpers --------------------- */
static bool is_number(const string& s) {
    if (s.empty()) return false;
    for (unsigned char ch : s) if (!isdigit(ch)) return false;
    return true;
}
static uint32_t resolve_operand_value_u16(const shared_ptr<Process>& p, const string& operand, uint64_t numeric_field, bool operand_const_flag) {
    if (operand_const_flag) return (uint32_t)numeric_field;
    if (is_number(operand)) return (uint32_t)stoul(operand);
    auto it = p->vars.find(operand);
    if (it != p->vars.end()) return it->second;
    return 0;
}

/* ======================================================
   Backing store and frame table (demand paging)
   ====================================================== */
struct Frame {
    bool free = true;
    uint64_t ownerPid = 0; // 0 = free
    uint32_t vpage = 0;
    vector<uint8_t> data; // mem_per_frame sized buffer
    uint64_t fifo_seq = 0; // for FIFO eviction
};
static vector<Frame> frames;
static std::mutex frames_mtx;
static uint64_t next_fifo_seq = 1;
static uint64_t numPagedIn = 0;
static uint64_t numPagedOut = 0;
static atomic<uint64_t> idle_cpu_ticks{ 0 }; // ticks when no core was busy
static atomic<uint64_t> active_cpu_ticks{ 0 };

/* backing store file helpers (simple text mapping pid:vpage -> hex data) */
static std::mutex backing_mtx;
static const char* backing_store_path = "csopesy-backing-store.txt";

static unordered_map<uint64_t, unordered_map<uint32_t, string>> backing_cache; // pid -> (vpage -> hex)
static void load_backing_store() {
    lock_guard<mutex> lg(backing_mtx);
    backing_cache.clear();
    ifstream fin(backing_store_path);
    if (!fin.is_open()) return;
    string line;
    while (getline(fin, line)) {
        trim(line);
        if (line.empty()) continue;
        // format: pid vpage hexdata
        istringstream iss(line);
        uint64_t pid; uint32_t vpage; string hexdata;
        if (!(iss >> pid >> vpage >> hexdata)) continue;
        backing_cache[pid][vpage] = hexdata;
    }
}
static void flush_backing_store() {
    lock_guard<mutex> lg(backing_mtx);
    ofstream fout(backing_store_path, ios::trunc);
    if (!fout.is_open()) return;
    for (auto &pp : backing_cache) {
        uint64_t pid = pp.first;
        for (auto &vv : pp.second) {
            fout << pid << " " << vv.first << " " << vv.second << "\n";
        }
    }
}

/* helper: hex encode/decode page buffer */
static string encode_hex(const vector<uint8_t>& data) {
    ostringstream oss;
    oss << hex << setfill('0');
    for (auto b : data) oss << setw(2) << static_cast<int>(b);
    return oss.str();
}
static vector<uint8_t> decode_hex(const string& hexs, size_t expect_len) {
    vector<uint8_t> out(expect_len, 0);
    size_t len = min(hexs.size()/2, expect_len);
    for (size_t i = 0; i < len; ++i) {
        string byte = hexs.substr(i*2, 2);
        out[i] = static_cast<uint8_t>(stoul(byte, nullptr, 16));
    }
    return out;
}

/* ======================================================
   Memory manager helpers
   ====================================================== */
static int find_free_frame_locked() {
    for (size_t i = 0; i < frames.size(); ++i) if (frames[i].free) return (int)i;
    return -1;
}
static int pick_victim_fifo_locked() {
    // pick frame with smallest fifo_seq (oldest)
    uint64_t best_seq = UINT64_MAX;
    int best = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i].free && frames[i].fifo_seq < best_seq) {
            best_seq = frames[i].fifo_seq;
            best = (int)i;
        }
    }
    return best;
}

// pageOut a frame to backing store (ownerPid, vpage)
static void page_out_locked(int frameIdx) {
    Frame &f = frames[frameIdx];
    if (f.free) return;
    // serialize data to hex
    string hexdata = encode_hex(f.data);
    backing_cache[f.ownerPid][f.vpage] = hexdata;
    ++numPagedOut;
}

// pageIn: fill frame from backing store for (pid, vpage)
static void page_in_locked(int frameIdx, uint64_t pid, uint32_t vpage) {
    Frame &f = frames[frameIdx];
    f.free = false;
    f.ownerPid = pid;
    f.vpage = vpage;
    f.fifo_seq = next_fifo_seq++;
    string hexdata;
    auto pit = backing_cache.find(pid);
    if (pit != backing_cache.end()) {
        auto vit = pit->second.find(vpage);
        if (vit != pit->second.end()) hexdata = vit->second;
    }
    if (!hexdata.empty()) {
        f.data = decode_hex(hexdata, f.data.size());
    } else {
        // empty => zero
        fill(f.data.begin(), f.data.end(), 0);
    }
    ++numPagedIn;
}

// Ensure page resident; returns frame index or -1 on violation
static int ensure_page_resident(shared_ptr<Process> &p, uint32_t vaddr, string &errHex) {
    // compute vpage & offset
    uint64_t frameSize = global_cfg.mem_per_frame;
    if (frameSize == 0) { errHex = "0x0"; return -1; }
    if (vaddr + 1 >= p->memBytes) { // need 2 bytes for uint16 read/write
        p->memViolation = true;
        p->violationAddr = vaddr;
        p->violationTime = time(nullptr);
        errHex = to_hex(vaddr);
        return -1;
    }
    uint32_t vpage = vaddr / (uint32_t)frameSize;
    // check bounds
    if (vpage >= p->numPages) {
        p->memViolation = true;
        p->violationAddr = vaddr;
        p->violationTime = time(nullptr);
        errHex = to_hex(vaddr);
        return -1;
    }
    // resident?
    {
        lock_guard<mutex> lg(frames_mtx);
        auto it = p->vpageToFrame.find(vpage);
        if (it != p->vpageToFrame.end()) {
            int fidx = it->second;
            frames[fidx].fifo_seq = next_fifo_seq++; // update access seq for FIFO order bias
            return fidx;
        }
        // page fault: allocate or evict
        int freeIdx = find_free_frame_locked();
        if (freeIdx == -1) {
            int victim = pick_victim_fifo_locked();
            if (victim == -1) {
                // no frame -> should not happen
                p->memViolation = true;
                p->violationAddr = vaddr;
                p->violationTime = time(nullptr);
                errHex = to_hex(vaddr);
                return -1;
            }
            // write back if owned and dirty
            uint64_t victimPid = frames[victim].ownerPid;
            uint32_t victimVpage = frames[victim].vpage;
            // mark backing store
            // find owner process to check dirty
            shared_ptr<Process> ownerProc = nullptr;
            {
                lock_guard<mutex> lgp(procs_mtx);
                for (auto &pp : proc_list) {
                    if (pp->pid == victimPid) { ownerProc = pp; break; }
                }
            }
            if (ownerProc && ownerProc->dirtyPages.count(victimVpage)) {
                page_out_locked(victim);
                ownerProc->dirtyPages.erase(victimVpage);
            } else {
                // still update backing cache to reflect current data (needed)
                page_out_locked(victim);
            }
            // remove mapping from ownerProc
            if (ownerProc) {
                ownerProc->vpageToFrame.erase(victimVpage);
            }
            freeIdx = victim;
            frames[freeIdx].free = true; // will be re-used by page_in
        }
        // now page in
        frames[freeIdx].data.assign((size_t)global_cfg.mem_per_frame, 0);
        page_in_locked(freeIdx, p->pid, vpage);
        p->vpageToFrame[vpage] = freeIdx;
        return freeIdx;
    }
}

/* ======================================================
   Process table & creation (with memory)
   ====================================================== */
shared_ptr<Process> create_process_with_mem(const string& name, const vector<Instruction>& instrs, uint32_t memBytes) {
    auto p = make_shared<Process>();
    p->name = name;
    p->pid = next_pid++;
    p->instrs = instrs;
    p->pc = 0;
    p->state = ProcState::READY;
    p->remaining_sleep = 0;
    p->ticks_used = 0;
    p->quantum_left = global_cfg.quantum_cycles;
    p->delay_left = 0;
    p->created_time = time(nullptr);
    p->vars["x"] = 0;
    // memory fields
    p->memBytes = memBytes;
    // compute pages using ceiling division so small allocations still occupy at least one page
    if (global_cfg.mem_per_frame == 0) p->numPages = 0;
    else p->numPages = (uint32_t)((memBytes + (uint32_t)global_cfg.mem_per_frame - 1) / (uint32_t)global_cfg.mem_per_frame); // updated for mo2
    {
        lock_guard<mutex> lg(procs_mtx);
        proc_table[name] = p;
        proc_list.push_back(p);
    }
    return p;
}
shared_ptr<Process> find_process(const string& name) {
    lock_guard<mutex> lg(procs_mtx);
    auto it = proc_table.find(name);
    if (it == proc_table.end()) return nullptr;
    return it->second;
}

/* ======================================================
   Instruction generation (includes READ/WRITE)
   ====================================================== */
std::random_device rd;
std::mt19937 rng(rd());

Instruction mk_print(const string& msg) { Instruction i; i.type = InstType::PRINT; i.a = msg; return i; }
Instruction mk_declare(const string& var, uint16_t val) { Instruction i; i.type = InstType::DECLARE; i.a = var; i.numeric = val; return i; }
Instruction mk_add(const string& dst, const string& op1, const string& op2_or_val, bool second_is_const = false) {
    Instruction i; i.type = InstType::ADD; i.a = dst; i.b = op1; i.c = op2_or_val; i.c_is_const = second_is_const;
    if (second_is_const) i.numeric = stoull(op2_or_val);
    return i;
}
Instruction mk_sub(const string& dst, const string& op1, const string& op2_or_val, bool second_is_const = false) {
    Instruction i; i.type = InstType::SUBTRACT; i.a = dst; i.b = op1; i.c = op2_or_val; i.c_is_const = second_is_const;
    if (second_is_const) i.numeric = stoull(op2_or_val);
    return i;
}
Instruction mk_sleep(uint64_t x) { Instruction i; i.type = InstType::SLEEP; i.numeric = x; return i; }
Instruction mk_for_start(uint64_t repeats) { Instruction i; i.type = InstType::FOR_START; i.numeric = repeats; return i; }
Instruction mk_for_end() { Instruction i; i.type = InstType::FOR_END; return i; }
Instruction mk_noop() { Instruction i; i.type = InstType::NOOP; return i; }
Instruction mk_read(const string& var, const string& addrHex) { Instruction i; i.type = InstType::READ; i.a = var; i.b = addrHex; return i; }
Instruction mk_write(const string& addrHex, const string& valOrVar) { Instruction i; i.type = InstType::WRITE; i.a = addrHex; i.b = valOrVar; return i; }

vector<Instruction> generate_random_instructions(const string& procname, uint64_t min_ins, uint64_t max_ins, uint32_t memBytes) {
    uniform_int_distribution<int> len_d((int)min_ins, (int)max_ins);
    uniform_int_distribution<int> add_val_d(1, 10);
    uniform_int_distribution<int> addr_offset_d(0, (int)(memBytes > 2 ? memBytes - 2 : 0));
    int len = len_d(rng);
    vector<Instruction> out;
    for (int i = 0; i < len; i++) {
        if (i % 3 == 0) {
            Instruction ins = mk_print("Value from: ");
            ins.b = "x";
            out.push_back(ins);
        } else if (i % 3 == 1) {
            int add_val = add_val_d(rng);
            string val_str = to_string(add_val);
            out.push_back(mk_add("x", "x", val_str, true));
        } else {
            // include READ/WRITE
            uint32_t offs = addr_offset_d(rng);
            ostringstream ah; ah << "0x" << hex << uppercase << offs;
            if (i % 2 == 0) out.push_back(mk_write(ah.str(), "x"));
            else out.push_back(mk_read("x", ah.str()));
        }
    }
    return out;
}

/* ======================================================
   Scheduler (integrate memory checks)
   ====================================================== */
struct CPUCore { bool busy = false; shared_ptr<Process> current; };
static vector<CPUCore> cpus;
static atomic<bool> scheduler_running{ false };
static atomic<bool> scheduler_generating{ false };
static atomic<uint64_t> cpu_tick{ 0 };
static atomic<bool> initialized{ false };
static mutex scheduler_mtx;
static condition_variable scheduler_cv;
static atomic<uint64_t> total_ticks_consumed{ 0 };
static time_t init_time = 0;

shared_ptr<Process> pick_next_process_fcfs() {
    lock_guard<mutex> lg(procs_mtx);
    for (auto& p : proc_list) if (p->state == ProcState::READY) return p;
    return nullptr;
}
shared_ptr<Process> pick_next_process_rr() { return pick_next_process_fcfs(); }

void terminate_process_memory_violation(shared_ptr<Process>& p) {
    p->state = ProcState::TERMINATED_BY_MEM;
    {
        lock_guard<mutex> lg(procs_mtx);
        proc_table.erase(p->name);
    }
}

void scheduler_tick_loop() {
    const chrono::milliseconds tick_interval(50);
    uint64_t local_tick_counter = 0;
    while (scheduler_running.load()) {
        this_thread::sleep_for(tick_interval);
        local_tick_counter++;
        cpu_tick.fetch_add(1);

        // generate batch processes
        if (scheduler_generating.load()) {
            if (global_cfg.batch_process_freq > 0 && (local_tick_counter % global_cfg.batch_process_freq == 0)) {
                static atomic<int> gnum{ 1 };
                int n = gnum++;
                char buf[32]; snprintf(buf, sizeof(buf), "p%03d", n);
                string pname(buf);
                // choose mem size = min_mem_per_proc (could randomize among powers)
                uint32_t mem = (uint32_t)global_cfg.min_mem_per_proc;
                auto instrs = generate_random_instructions(pname, global_cfg.min_ins, global_cfg.max_ins, mem);
                auto p = create_process_with_mem(pname, instrs, mem);
            }
        }

        bool anyBusy = false;
        for (int cid = 0; cid < (int)cpus.size(); ++cid) {
            CPUCore& core = cpus[cid];
            if (!core.busy) {
                shared_ptr<Process> next = nullptr;
                if (global_cfg.scheduler == "fcfs") next = pick_next_process_fcfs();
                else next = pick_next_process_rr();
                if (next) {
                    core.current = next;
                    core.busy = true;
                    next->state = ProcState::RUNNING;
                    next->quantum_left = global_cfg.quantum_cycles;
                    next->delay_left = 0;
                } else {
                    core.current = nullptr;
                    core.busy = false;
                }
            }
            if (core.busy && core.current) {
                anyBusy = true;
                auto p = core.current;
                if (p->state == ProcState::SLEEPING) {
                    if (p->remaining_sleep > 0) { p->remaining_sleep--; continue; }
                    else p->state = ProcState::RUNNING;
                }
                if (p->state == ProcState::READY) p->state = ProcState::RUNNING;
                if (p->delay_left > 0) {
                    p->delay_left--; p->ticks_used++; total_ticks_consumed++; p->quantum_left = (p->quantum_left>0?p->quantum_left-1:0);
                    if (global_cfg.scheduler == "rr" && p->quantum_left == 0) { p->state = ProcState::READY; core.current = nullptr; core.busy = false; }
                    continue;
                }
                if (p->pc >= p->instrs.size()) {
                    p->state = ProcState::FINISHED;
                    { lock_guard<mutex> lg(procs_mtx); proc_table.erase(p->name); }
                    core.current = nullptr; core.busy = false; continue;
                }
                Instruction& ins = p->instrs[p->pc];
                // Before executing READ/WRITE or any variable access, ensure symbol table page is resident
                bool instructionExecuted = false;
                // Helper lambda to perform READ
                auto do_read = [&](Instruction &I)->bool {
                    uint32_t addr; if (!parse_hex(I.b, addr)) { p->memViolation = true; p->violationAddr = 0; p->violationTime = time(nullptr); return false; }
                    string err; int frameIdx = ensure_page_resident(p, addr, err);
                    if (frameIdx < 0) return false;
                    // read 2 bytes from frame at offset
                    uint64_t frameOffset = addr % global_cfg.mem_per_frame;
                    uint16_t val = 0;
                    {
                        lock_guard<mutex> lg(frames_mtx);
                        val = (uint16_t)(frames[frameIdx].data[frameOffset] | (frames[frameIdx].data[frameOffset+1] << 8));
                    }
                    // store into variable I.a (create if not exists, but ensure symbol table constraints)
                    if (p->vars.size() < 32 || p->vars.count(I.a)) p->vars[I.a] = val;
                    return true;
                };
                auto do_write = [&](Instruction &I)->bool {
                    uint32_t addr; if (!parse_hex(I.a, addr)) { p->memViolation = true; p->violationAddr = 0; p->violationTime = time(nullptr); return false; }
                    uint32_t val = resolve_operand_value_u16(p, I.b, I.numeric, I.c_is_const);
                    string err; int frameIdx = ensure_page_resident(p, addr, err);
                    if (frameIdx < 0) return false;
                    uint64_t frameOffset = addr % global_cfg.mem_per_frame;
                    {
                        lock_guard<mutex> lg(frames_mtx);
                        frames[frameIdx].data[frameOffset] = (uint8_t)(val & 0xFF);
                        frames[frameIdx].data[frameOffset+1] = (uint8_t)((val >> 8) & 0xFF);
                    }
                    // mark dirty
                    uint32_t vpage = addr / (uint32_t)global_cfg.mem_per_frame;
                    p->dirtyPages.insert(vpage);
                    return true;
                };
                // Execute instruction with memory checks where needed
                switch (ins.type) {
                case InstType::PRINT: {
                    string msg = ins.a;
                    if (!ins.b.empty() && p->vars.count(ins.b)) msg = ins.a + to_string(p->vars[ins.b]);
                    p->logs.push_back(msg);
                    instructionExecuted = true;
                    break;
                }
                case InstType::DECLARE: {
                    if (p->vars.size() < 32) {
                        uint16_t v = (uint16_t)(ins.numeric & 0xFFFF);
                        p->vars[ins.a] = v;
                    } // else ignore beyond limit
                    instructionExecuted = true;
                    break;
                }
                case InstType::ADD: {
                    uint32_t v1 = 0; if (p->vars.count(ins.b)) v1 = p->vars[ins.b];
                    uint32_t v2 = resolve_operand_value_u16(p, ins.c, ins.numeric, ins.c_is_const);
                    uint32_t res = v1 + v2; if (res > 0xFFFF) res = 0xFFFF;
                    p->vars[ins.a] = (uint16_t)res;
                    instructionExecuted = true;
                    break;
                }
                case InstType::SUBTRACT: {
                    uint32_t v1 = 0; if (p->vars.count(ins.b)) v1 = p->vars[ins.b];
                    uint32_t v2 = resolve_operand_value_u16(p, ins.c, ins.numeric, ins.c_is_const);
                    int32_t res = (int32_t)v1 - (int32_t)v2; if (res < 0) res = 0;
                    p->vars[ins.a] = (uint16_t)res;
                    instructionExecuted = true;
                    break;
                }
                case InstType::SLEEP: {
                    p->remaining_sleep = ins.numeric; p->state = ProcState::SLEEPING; instructionExecuted = true; break;
                }
                case InstType::FOR_START: {
                    p->for_stack.emplace_back(p->pc, ins.numeric); instructionExecuted = true; break;
                }
                case InstType::FOR_END: {
                    if (!p->for_stack.empty()) {
                        auto &top = p->for_stack.back();
                        if (top.second > 1) { top.second--; p->pc = top.first; } else p->for_stack.pop_back();
                    }
                    instructionExecuted = true; break;
                }
                case InstType::READ: {
                    if (!do_read(ins)) {
                        terminate_process_memory_violation(p); instructionExecuted = false;
                    } else instructionExecuted = true;
                    break;
                }
                case InstType::WRITE: {
                    if (!do_write(ins)) {
                        terminate_process_memory_violation(p); instructionExecuted = false;
                    } else instructionExecuted = true;
                    break;
                }
                default: break;
                }
                if (!instructionExecuted) { // memory violation occurred
                    core.current = nullptr; core.busy = false; continue;
                }
                // simulate delays-per-exec
                if (global_cfg.delays_per_exec > 0) p->delay_left = global_cfg.delays_per_exec;
                p->pc++; p->ticks_used++; total_ticks_consumed++;
                // RR quantum handling
                if (global_cfg.scheduler == "rr") {
                    if (p->quantum_left > 0) p->quantum_left--;
                    if (p->quantum_left == 0 && p->state == ProcState::RUNNING) {
                        p->state = ProcState::READY; core.current = nullptr; core.busy = false;
                    }
                }
            } // end if core.busy && current
        } // end for cores
        if (!anyBusy) idle_cpu_ticks++;
    } // end while
}

/* ======================================================
   Console commands: process-smi, vmstat, screen changes
   ====================================================== */

void cmd_process_smi() {
    lock_guard<mutex> lg(procs_mtx);
    lock_guard<mutex> lg2(frames_mtx);
    ostringstream oss;
    oss << "CSOPESY process-smi\n";
    uint64_t totalMem = global_cfg.max_overall_mem;
    uint64_t freeFrames = 0;
    for (auto &f : frames) if (f.free) freeFrames++;
    uint64_t freeMem = freeFrames * global_cfg.mem_per_frame;
    uint64_t usedMem = totalMem - freeMem;
    oss << "Total memory: " << totalMem << " bytes\n";
    oss << "Used memory: " << usedMem << " bytes\n";
    oss << "Free memory: " << freeMem << " bytes\n\n";
    oss << left << setw(12) << "Name" << setw(10) << "PID" << setw(12) << "MemBytes" << setw(12) << "PagesRes" << setw(12) << "PagesTot" << "\n";
    for (auto &p : proc_list) {
        if (p->state == ProcState::FINISHED && p->memBytes==0) continue;
        size_t resident = p->vpageToFrame.size();
        oss << left << setw(12) << p->name << setw(10) << p->pid << setw(12) << p->memBytes << setw(12) << resident << setw(12) << p->numPages << "\n";
    }
    safe_print(oss.str());
}

void cmd_vmstat() {
    lock_guard<mutex> lg(frames_mtx);
    ostringstream oss;
    uint64_t totalMem = global_cfg.max_overall_mem;
    uint64_t freeFrames = 0;
    for (auto &f : frames) if (f.free) freeFrames++;
    uint64_t freeMem = freeFrames * global_cfg.mem_per_frame;
    uint64_t usedMem = totalMem - freeMem;
    uint64_t idle = idle_cpu_ticks.load();
    uint64_t active = active_cpu_ticks.load();
    uint64_t total = idle + active;
    oss << "vmstat\n";
    oss << "Total memory: " << totalMem << "\n";
    oss << "Used memory: " << usedMem << "\n";
    oss << "Free memory: " << freeMem << "\n";
    oss << "Idle CPU ticks: " << idle << "\n";
    oss << "Active CPU ticks: " << active << "\n";
    oss << "Total CPU ticks: " << total << "\n";
    oss << "Num paged in: " << numPagedIn << "\n";
    oss << "Num paged out: " << numPagedOut << "\n";
    // active/inactive processes
    int active_proc = 0, inactive_proc = 0;
    {
        lock_guard<mutex> pl(procs_mtx);
        for (auto &p : proc_list) {
            if (p->state == ProcState::FINISHED || p->state == ProcState::TERMINATED_BY_MEM) inactive_proc++;
            else active_proc++;
        }
    }
    oss << "Active processes: " << active_proc << "\n";
    oss << "Inactive processes: " << inactive_proc << "\n";
    safe_print(oss.str());
}

/* screen -r updated behavior (show memory violation info if present) */
void screen_attach_loop(shared_ptr<Process> p) {
    if (!p) { safe_print("Process not found.\n"); return; }
    clear_console();
    p->attached = true;
    // show shutdown message if terminated by mem violation
    if (p->state == ProcState::TERMINATED_BY_MEM || p->memViolation) {
        time_t t = p->violationTime;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
        ostringstream oss;
        oss << "Process " << p->name << " shut down due to memory access violation error\n";
        oss << "that occurred at " << buf << ". " << to_hex(p->violationAddr) << " invalid.\n";
        safe_print(oss.str());
    }
    safe_print("---- Attached to process " + p->name + " (pid " + to_string(p->pid) + ") ----\n");
    safe_print("Type \"process-smi\" to show status, \"exit\" to return to main console.\n");
    string line;
    while (true) {
        safe_print(p->name + "> ");
        if (!getline(cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;
        if (line == "process-smi") {
            ostringstream oss;
            oss << "Process: " << p->name << " (pid " << p->pid << ")\n";
            string state;
            if (p->state == ProcState::RUNNING) state = "RUNNING";
            else if (p->state == ProcState::READY) state = "READY";
            else if (p->state == ProcState::SLEEPING) state = "SLEEPING";
            else if (p->state == ProcState::FINISHED) state = "FINISHED";
            else if (p->state == ProcState::TERMINATED_BY_MEM) state = "TERMINATED_BY_MEM";
            oss << "State: " << state << "\n";
            oss << "PC: " << p->pc << " / " << p->instrs.size() << "\n";
            oss << "Ticks used: " << p->ticks_used << "\n";
            oss << "Memory: " << p->memBytes << " bytes (" << p->numPages << " pages), resident: " << p->vpageToFrame.size() << "\n";
            oss << "Variables:\n";
            for (auto& kv : p->vars) oss << "  " << kv.first << " = " << kv.second << "\n";
            oss << "Logs:\n";
            for (auto& l : p->logs) oss << "  " << l << "\n";
            if (p->state == ProcState::TERMINATED_BY_MEM || p->memViolation) {
                time_t t = p->violationTime; struct tm tmv;
#ifdef _WIN32
                localtime_s(&tmv, &t);
#else
                localtime_r(&t, &tmv);
#endif
                char buf[16]; strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
                oss << "Shutdown reason: memory access violation at " << buf << " address " << to_hex(p->violationAddr) << "\n";
            }
            safe_print(oss.str());
        } else if (line == "exit") {
            p->attached = false; safe_print("Detached from " + p->name + "\n"); break;
        } else {
            safe_print("Unknown command inside screen: " + line + "\n");
        }
    }
}

/* ======================================================
   CLI Interface: parse new screen -s and -c, process-smi, vmstat
   ====================================================== */
void print_main_prompt() { safe_print("> "); }
static string fmt_time(time_t t) {
    if (t == 0) return "N/A";
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%Y %I:%M:%S%p", &tmv);
    return string(buf);
}

void cmd_screen_ls(ofstream* logfile = nullptr) {
    lock_guard<mutex> lg(procs_mtx);
    lock_guard<mutex> lg2(scheduler_mtx);
    ostringstream oss;
    oss << "Welcome to CSOPESY Emulator!\n\n";
    uint64_t used = total_ticks_consumed.load();
    time_t now = time(nullptr);
    double seconds_running = difftime(now, init_time); if (seconds_running < 1.0) seconds_running = 1.0;
    int usedcores = 0; for (auto& c : cpus) if (c.busy) usedcores++;
    oss << "root:\\> screen -ls\n";
    int cpu_util = 0;
    if (global_cfg.num_cpu > 0) { cpu_util = (int)((usedcores * 100.0 / global_cfg.num_cpu) + 0.5); if (cpu_util > 100) cpu_util = 100; }
    oss << "CPU utilization: " << setw(3) << right << cpu_util << "%\n";
    oss << "Cores used: " << usedcores << "\n";
    oss << "Cores available: " << (global_cfg.num_cpu - usedcores) << "\n\n";
    oss << "--------------------------------------------------------\n\n";
    oss << left << setw(20) << "Running processes" << "\n";
    oss << left << setw(15) << "Name" << left << setw(26) << "Created" << left << setw(8) << "Core" << right << setw(12) << "Used" << " / " << left << setw(8) << "Total" << "\n";
    auto find_core_for = [](const shared_ptr<Process>& p)->int {
        for (size_t i = 0; i < cpus.size(); ++i) if (cpus[i].current && cpus[i].current.get() == p.get()) return (int)i; return -1;
    };
    for (auto& p : proc_list) {
        if (p->state == ProcState::FINISHED) continue;
        string created = fmt_time(p->created_time);
        int core = find_core_for(p);
        uint64_t total_est = p->instrs.size() * 100;
        oss << left << setw(15) << p->name << left << setw(26) << created << left << setw(8) << (core>=0?to_string(core):"-") << right << setw(7) << p->ticks_used << " / " << left << setw(8) << total_est << "\n";
    }
    oss << "\nFinished processes:\n";
    oss << left << setw(15) << "Name" << left << setw(26) << "Created" << left << setw(12) << "Status" << right << setw(12) << "Used" << " / " << left << setw(8) << "Total" << "\n";
    for (auto& p : proc_list) {
        if (p->state != ProcState::FINISHED) continue;
        string created = fmt_time(p->created_time);
        uint64_t total_est = p->instrs.size() * 100;
        oss << left << setw(15) << p->name << left << setw(26) << created << left << setw(12) << "Finished" << right << setw(7) << p->ticks_used << " / " << left << setw(8) << total_est << "\n";
    }
    oss << "--------------------------------------------------------\n\n";
    string out = oss.str();
    safe_print(out);
    if (logfile) (*logfile) << out;
}

void cmd_report_util() {
    ofstream fout("csopesy-log.txt");
    cmd_screen_ls(&fout);
    fout.close();
    safe_print("root:\\> Report generated at C:/csopesy-log.txt!\n");
}

/* ======================================================
   load_config extended for MO2 keys
   ====================================================== */
void load_config(const string& cfgpath) {
    vector<string> tried; vector<string> candidates; candidates.push_back(cfgpath);
#ifdef _WIN32
    char cwd_buf[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd_buf)) candidates.push_back(string(cwd_buf) + "\\" + cfgpath);
#else
    char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) candidates.push_back(string(cwd_buf) + "/" + cfgpath);
#endif
    string exe_dir;
#ifdef _WIN32
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) { exe_dir = string(buf); size_t pos = exe_dir.find_last_of("\\/"); if (pos != string::npos) exe_dir = exe_dir.substr(0, pos); }
#else
    char buf[PATH_MAX]; ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = 0; exe_dir = string(buf); size_t pos = exe_dir.find_last_of('/'); if (pos != string::npos) exe_dir = exe_dir.substr(0, pos); }
#endif
    if (!exe_dir.empty()) {
#ifdef _WIN32
        candidates.push_back(exe_dir + "\\" + cfgpath);
        size_t pos = exe_dir.find_last_of("\\/");
        if (pos != string::npos) candidates.push_back(exe_dir.substr(0, pos) + "\\" + cfgpath);
#else
        candidates.push_back(exe_dir + "/" + cfgpath);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != string::npos) candidates.push_back(exe_dir.substr(0, pos) + "/" + cfgpath);
#endif
    }
    bool opened = false; Config cfg;
    for (auto& p : candidates) {
        tried.push_back(p);
        ifstream fin(p.c_str()); if (!fin.is_open()) continue;
        while (!fin.eof()) {
            string key; if (!(fin >> key)) break;
            if (key == "num-cpu") fin >> cfg.num_cpu;
            else if (key == "scheduler") fin >> cfg.scheduler;
            else if (key == "quantum-cycles") fin >> cfg.quantum_cycles;
            else if (key == "batch-process-freq") fin >> cfg.batch_process_freq;
            else if (key == "min-ins") fin >> cfg.min_ins;
            else if (key == "max-ins") fin >> cfg.max_ins;
            else if (key == "delays-per-exec") fin >> cfg.delays_per_exec;
            else if (key == "max-overall-mem") fin >> cfg.max_overall_mem;
            else if (key == "mem-per-frame") fin >> cfg.mem_per_frame;
            else if (key == "min-mem-per-proc") fin >> cfg.min_mem_per_proc;
            else if (key == "max-mem-per-proc") fin >> cfg.max_mem_per_proc;
            else { string val; fin >> val; }
        }
        fin.close(); opened = true; safe_print(string("Loaded config from: ") + p + "\n"); break;
    }
    if (!opened) {
        ostringstream oss; oss << "Could not open config.txt. Tried paths:\n"; for (auto& t : tried) oss << "  " << t << "\n";
        throw runtime_error(oss.str());
    }
    if (cfg.num_cpu < 1) cfg.num_cpu = 1;
    if (cfg.min_ins < 1) cfg.min_ins = 1;
    if (cfg.max_ins < cfg.min_ins) cfg.max_ins = cfg.min_ins;
    // mem sanity: enforce powers of two and ranges
    if (!is_power_of_two(cfg.mem_per_frame)) throw runtime_error("mem-per-frame must be power of two");
    if (!is_power_of_two(cfg.max_overall_mem)) throw runtime_error("max-overall-mem must be power of two");
    if (!is_power_of_two(cfg.min_mem_per_proc) || !is_power_of_two(cfg.max_mem_per_proc)) throw runtime_error("per-proc mem must be power of two");
    if (cfg.min_mem_per_proc < 64) cfg.min_mem_per_proc = 64;
    global_cfg = cfg; global_cfg.valid = true;
}

/* ======================================================
   main_cli_loop: extended parsing for MO2 commands
   ====================================================== */
void main_cli_loop() {
    string line;
    bool running = true;
    print_main_prompt();
    while (running && getline(cin, line)) {
        line = trim(line);
        if (line.empty()) { print_main_prompt(); continue; }
        auto args = split_args(line);
        string cmd = args.size() ? args[0] : "";
        if (cmd == "exit") { safe_print("Exiting console...\n"); running = false; break; }
        else if (cmd == "initialize") {
            if (initialized.load()) { safe_print("Already initialized.\n"); print_main_prompt(); continue; }
            try {
                // extend load_config to new keys is handled below in load_config caller
                load_config("config.txt");
                // init frames
                if (global_cfg.mem_per_frame == 0) throw runtime_error("mem-per-frame must be > 0");
                uint64_t framesCount = global_cfg.max_overall_mem / global_cfg.mem_per_frame;
                if (framesCount == 0) throw runtime_error("Invalid memory configuration");
                frames.clear(); frames.resize((size_t)framesCount);
                for (auto &f : frames) f.data.assign((size_t)global_cfg.mem_per_frame, 0);
                load_backing_store();
                cpus.clear(); cpus.resize(global_cfg.num_cpu);
                initialized = true;
                init_time = time(nullptr);
                ostringstream oss;
                oss << "Initialized with config:\n num-cpu=" << global_cfg.num_cpu << " scheduler=" << global_cfg.scheduler << " quantum=" << global_cfg.quantum_cycles << " batch_freq=" << global_cfg.batch_process_freq << " min-ins=" << global_cfg.min_ins << " max-ins=" << global_cfg.max_ins << " delays-per-exec=" << global_cfg.delays_per_exec << " max-overall-mem=" << global_cfg.max_overall_mem << " mem-per-frame=" << global_cfg.mem_per_frame << " min-mem-per-proc=" << global_cfg.min_mem_per_proc << " max-mem-per-proc=" << global_cfg.max_mem_per_proc << "\n";
                safe_print(oss.str());
                scheduler_running = true;
                thread(scheduler_tick_loop).detach();
            } catch (exception& e) {
                safe_print(string("Error initializing: ") + e.what() + "\n");
            }
            print_main_prompt(); continue;
        } else {
            if (!initialized.load()) { safe_print("Please run \"initialize\" first. Available commands until then: initialize, exit\n"); print_main_prompt(); continue; }
            if (cmd == "process-smi") { cmd_process_smi(); print_main_prompt(); continue; }
            if (cmd == "vmstat") { cmd_vmstat(); print_main_prompt(); continue; }
            if (cmd == "screen") {
                if (args.size() >= 2) {
                    string flag = args[1];
                    if (flag == "-s") {
                        // usage: screen -s <process_name> <mem_bytes>
                        if (args.size() < 4) { safe_print("Usage: screen -s <process name> <mem_bytes>\n"); }
                        else {
                            string pname = args[2];
                            uint32_t memBytes = 0;
                            try { memBytes = (uint32_t)stoul(args[3]); } catch(...) { memBytes = 0; }
                            if (!is_power_of_two(memBytes) || memBytes < (1u<<6) || memBytes > (1u<<16) || memBytes < global_cfg.min_mem_per_proc || memBytes > global_cfg.max_mem_per_proc) {
                                safe_print("invalid memory allocation\n");
                            } else {
                                auto instrs = generate_random_instructions(pname, global_cfg.min_ins, global_cfg.max_ins, memBytes);
                                auto p = create_process_with_mem(pname, instrs, memBytes);
                                screen_attach_loop(p);
                            }
                        }
                    } else if (flag == "-c") {
                        // screen -c <name> <mem> "<instructions>"
                        if (args.size() < 5) { safe_print("Usage: screen -c <process name> <mem> \"<instructions>\"\n"); }
                        else {
                            string pname = args[2];
                            uint32_t memBytes = 0; try { memBytes = (uint32_t)stoul(args[3]); } catch(...) { memBytes = 0; }
                            if (!is_power_of_two(memBytes) || memBytes < (1u<<6) || memBytes > (1u<<16) || memBytes < global_cfg.min_mem_per_proc || memBytes > global_cfg.max_mem_per_proc) {
                                safe_print("invalid memory allocation\n");
                            } else {
                                // instructions are the remaining after the 3rd token — join and then extract quoted string
                                size_t pos = line.find("\"");
                                if (pos==string::npos) { safe_print("invalid command\n"); }
                                else {
                                    size_t pos2 = line.rfind("\"");
                                    if (pos2==pos) { safe_print("invalid command\n"); }
                                    else {
                                        string instrsRaw = line.substr(pos+1, pos2-pos-1);
                                        // split by semicolon
                                        vector<Instruction> instrsVec;
                                        istringstream iss(instrsRaw);
                                        string token;
                                        int count = 0;
                                        while (getline(iss, token, ';')) {
                                            token = trim(token);
                                            if (token.empty()) continue;
                                            // naive parse: support DECLARE, ADD, SUBTRACT, WRITE, READ, PRINT
                                            // DECLARE var val
                                            vector<string> toks = split_args(token);
                                            if (toks.empty()) continue;
                                            string kw = toks[0];
                                            for (auto &c: kw) c = toupper(c);
                                            if (kw == "DECLARE" && toks.size() >= 3) {
                                                instrsVec.push_back(mk_declare(toks[1], (uint16_t)stoul(toks[2])));
                                            } else if ((kw=="ADD" || kw=="SUB") && toks.size()>=4) {
                                                if (kw=="ADD") instrsVec.push_back(mk_add(toks[1], toks[2], toks[3], is_number(toks[3])));
                                                else instrsVec.push_back(mk_sub(toks[1], toks[2], toks[3], is_number(toks[3])));
                                            } else if (kw=="WRITE" && toks.size()>=3) {
                                                instrsVec.push_back(mk_write(toks[1], toks[2]));
                                            } else if (kw=="READ" && toks.size()>=3) {
                                                instrsVec.push_back(mk_read(toks[1], toks[2]));
                                            } else if (kw=="PRINT") {
                                                // join rest
                                                size_t pstart = token.find("PRINT");
                                                string content = token.substr(pstart + 5);
                                                content = trim(content);
                                                instrsVec.push_back(mk_print(content));
                                            } else {
                                                // unsupported - treat as NOOP
                                                instrsVec.push_back(mk_noop());
                                            }
                                            ++count;
                                            if (count > 50) break;
                                        }
                                        if (instrsVec.empty()) { safe_print("invalid command\n"); }
                                        else {
                                            auto p = create_process_with_mem(pname, instrsVec, memBytes);
                                            screen_attach_loop(p);
                                        }
                                    }
                                }
                            }
                        }
                    } else if (flag == "-ls") {
                        cmd_screen_ls();
                    } else if (flag == "-r") {
                        if (args.size() < 3) { safe_print("Usage: screen -r <process name>\n"); }
                        else {
                            string pname = args[2];
                            auto p = find_process(pname);
                            if (!p) { safe_print("Process " + pname + " not found.\n"); }
                            else { screen_attach_loop(p); }
                        }
                    } else {
                        safe_print("Unknown screen option\n");
                    }
                } else {
                    safe_print("Usage: screen -s/-c/-ls/-r ...\n");
                }
                print_main_prompt(); continue;
            } else if (cmd == "scheduler-start") {
                scheduler_generating = true; safe_print("Scheduler started generating processes.\n"); print_main_prompt(); continue;
            } else if (cmd == "scheduler-stop") {
                scheduler_generating = false; safe_print("Scheduler stopped generating processes.\n"); print_main_prompt(); continue;
            } else if (cmd == "report-util") {
                cmd_report_util(); print_main_prompt(); continue;
            } else {
                safe_print("Unknown command: " + cmd + "\n"); print_main_prompt(); continue;
            }
        }
    }
    scheduler_running = false; scheduler_generating = false;
    this_thread::sleep_for(chrono::milliseconds(100));
}

/* ======================================================
   main
   ====================================================== */
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    safe_print(
        "   _____  _____  ____  _____  ______  _______     __ \n"
        "  / ____|/ ____|/ __ \\|  __ \\|  ____|/ ____\\ \\   / / \n"
        " | |    | (___ | |  | | |__) | |__  | (___  \\ \\_/ /  \n"
        " | |     \\___ \\| |  | |  ___/|  __|  \\___ \\  \\   /   \n"
        " | |____ ____) | |__| | |    | |____ ____) |  | |    \n"
        "  \\_____|_____/ \\____/|_|    |______|_____/   |_|    \n"
        "--------------------------------------------------------\n\n"
        "Welcome to CSOPESY Process Scheduler!\n"
        "Available commands:\n"
        "\ninitialize\nexit\nprocess-smi\nvmstat\nscreen\nscheduler-start\nscheduler-stop\nreport-util\n"
        "\nType \"initialize\" to begin (reads config.txt).\n"
    );
    main_cli_loop();
    // flush backing store on exit
    flush_backing_store();
    safe_print("Console terminated.\n");
    return 0;
}
