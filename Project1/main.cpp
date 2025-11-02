#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
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

/* --------------------- Utility --------------------- */
static std::mutex cout_mtx;
void safe_print(const string& s) {
    lock_guard<std::mutex> lg(cout_mtx);
    cout << s << flush;
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static vector<string> split_args(const string& line) {
    // simple whitespace split
    vector<string> out;
    istringstream iss(line);
    string t;
    while (iss >> t) out.push_back(t);
    return out;
}

/* --------------------- Config --------------------- */
struct Config {
    int num_cpu = 1;
    string scheduler = "fcfs"; // fcfs or rr
    uint64_t quantum_cycles = 1;
    uint64_t batch_process_freq = 10; // cycles
    uint64_t min_ins = 1;
    uint64_t max_ins = 5;
    uint64_t delays_per_exec = 0;
    bool valid = false;
};

Config global_cfg;

/* --------------------- Instruction & Process --------------------- */
enum class InstType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR_START, FOR_END, NOOP };

struct Instruction {
    InstType type;
    // Generic fields to hold operands/text:
    string a, b, c; // variable names or message
    uint64_t numeric = 0; // for constants, repeat counts, sleep ticks
    bool c_is_const = false; // whether 'c' was provided as numeric constant at creation time
    // for FOR: numeric = repeats, and we treat sequence between FOR_START and FOR_END
};

enum class ProcState { READY, RUNNING, SLEEPING, FINISHED };

struct Process {
    string name;
    uint64_t pid;
    vector<Instruction> instrs;
    size_t pc = 0; // instruction pointer (index into instrs)
    unordered_map<string, uint16_t> vars;
    vector<string> logs; // outputs from PRINT
    ProcState state = ProcState::READY;
    uint64_t remaining_sleep = 0;
    uint64_t ticks_used = 0; // total CPU ticks consumed
    // For FOR loops we will maintain a simple stack of (pc_of_for_start, remaining_repeats)
    vector<pair<size_t, uint64_t>> for_stack;
    // For RR quantum counters:
    uint64_t quantum_left = 0;
    // config-derived delays per instruction (simulate busy-wait)
    uint64_t delay_left = 0;
    bool attached = false; // whether a user is currently inside screen for this proc

    // NEW: record creation time for nicer reports
    time_t created_time = 0;

    Process() {}
};

/* --------------------- Process Table & Utilities --------------------- */
static std::mutex procs_mtx;
static unordered_map<string, shared_ptr<Process>> proc_table; // name -> process
static vector<shared_ptr<Process>> proc_list; // every created proc (for listing/report)
static std::atomic<uint64_t> next_pid{ 1 };

shared_ptr<Process> create_process(const string& name, const vector<Instruction>& instrs) {
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
    p->created_time = time(nullptr); // record creation time
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

/* --------------------- Instruction Generator (for dummy processes) --------------------- */
std::random_device rd;
std::mt19937 rng(rd());

Instruction mk_print(const string& msg) {
    Instruction i; i.type = InstType::PRINT; i.a = msg;
    return i;
}
Instruction mk_declare(const string& var, uint16_t val) {
    Instruction i; i.type = InstType::DECLARE; i.a = var; i.numeric = val; return i;
}
Instruction mk_add(const string& dst, const string& op1, const string& op2_or_val, bool second_is_const = false) {
    Instruction i; i.type = InstType::ADD; i.a = dst; i.b = op1; i.c = op2_or_val;
    i.c_is_const = second_is_const;
    if (second_is_const) i.numeric = stoull(op2_or_val);
    return i;
}
Instruction mk_sub(const string& dst, const string& op1, const string& op2_or_val, bool second_is_const = false) {
    Instruction i; i.type = InstType::SUBTRACT; i.a = dst; i.b = op1; i.c = op2_or_val;
    i.c_is_const = second_is_const;
    if (second_is_const) i.numeric = stoull(op2_or_val);
    return i;
}
Instruction mk_sleep(uint64_t x) { Instruction i; i.type = InstType::SLEEP; i.numeric = x; return i; }
Instruction mk_for_start(uint64_t repeats) { Instruction i; i.type = InstType::FOR_START; i.numeric = repeats; return i; }
Instruction mk_for_end() { Instruction i; i.type = InstType::FOR_END; return i; }
Instruction mk_noop() { Instruction i; i.type = InstType::NOOP; return i; }

vector<Instruction> generate_random_instructions(const string& procname, uint64_t min_ins, uint64_t max_ins) {
    uniform_int_distribution<int> inst_d(1, 6);
    uniform_int_distribution<int> len_d((int)min_ins, (int)max_ins);
    int len = len_d(rng);
    vector<Instruction> out;
    // For simplicity, we'll generate sequences that are syntactically valid with FORs possibly nested up to 2.
    int nested_for_allowed = 2;
    int current_for_depth = 0;
    for (int i = 0; i < len; i++) {
        int t = inst_d(rng);
        if (t == 1) {
            // PRINT
            Instruction ins = mk_print("Hello world from " + procname + "!");
            out.push_back(ins);
        }
        else if (t == 2) {
            // DECLARE var
            string var = "x" + to_string((rng() % 5) + 1);
            uint16_t val = rng() % 65536;
            out.push_back(mk_declare(var, val));
        }
        else if (t == 3) {
            // ADD
            string dst = "x" + to_string((rng() % 5) + 1);
            string op1 = "x" + to_string((rng() % 5) + 1);
            if (rng() % 2) {
                string op2 = "x" + to_string((rng() % 5) + 1);
                out.push_back(mk_add(dst, op1, op2, false));
            }
            else {
                string val = to_string(rng() % 100);
                out.push_back(mk_add(dst, op1, val, true));
            }
        }
        else if (t == 4) {
            // SUB
            string dst = "x" + to_string((rng() % 5) + 1);
            string op1 = "x" + to_string((rng() % 5) + 1);
            if (rng() % 2) {
                string op2 = "x" + to_string((rng() % 5) + 1);
                out.push_back(mk_sub(dst, op1, op2, false));
            }
            else {
                string val = to_string(rng() % 100);
                out.push_back(mk_sub(dst, op1, val, true));
            }
        }
        else if (t == 5) {
            // SLEEP small
            uint64_t s = (rng() % 3) + 1;
            out.push_back(mk_sleep(s));
        }
        else {
            // FOR start maybe
            if (current_for_depth < nested_for_allowed && (rng() % 2)) {
                uint64_t repeats = 1 + (rng() % 3);
                out.push_back(mk_for_start(repeats));
                current_for_depth++;
            }
            else {
                // end for if depth > 0
                if (current_for_depth > 0 && (rng() % 2)) {
                    out.push_back(mk_for_end());
                    current_for_depth--;
                }
                else {
                    out.push_back(mk_print("Hello world from " + procname + "!"));
                }
            }
        }
    }
    // close any open FORs
    while (current_for_depth-- > 0) out.push_back(mk_for_end());
    return out;
}

/* --------------------- Scheduler --------------------- */
struct CPUCore {
    bool busy = false;
    shared_ptr<Process> current;
};

static vector<CPUCore> cpus;
static atomic<bool> scheduler_running{ false };
static atomic<bool> scheduler_generating{ false };
static atomic<uint64_t> cpu_tick{ 0 };
static atomic<bool> initialized{ false };
static mutex scheduler_mtx;
static condition_variable scheduler_cv;

/* CPU utilization accounting */
static atomic<uint64_t> total_ticks_consumed{ 0 }; // sum of ticks across cores
static time_t init_time = 0;

void load_config(const string& cfgpath) {
    vector<string> tried;
    vector<string> candidates;
    candidates.push_back(cfgpath);

    // Add explicit working-directory candidate
#ifdef _WIN32
    char cwd_buf[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd_buf)) {
        candidates.push_back(string(cwd_buf) + "\\" + cfgpath);
    }
#else
    char cwd_buf[PATH_MAX];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        candidates.push_back(string(cwd_buf) + "/" + cfgpath);
    }
#endif

    // executable directory and its parent
    string exe_dir;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        exe_dir = string(buf);
        size_t pos = exe_dir.find_last_of("\\/");
        if (pos != string::npos) exe_dir = exe_dir.substr(0, pos);
    }
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        exe_dir = string(buf);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != string::npos) exe_dir = exe_dir.substr(0, pos);
    }
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

    bool opened = false;
    Config cfg;
    for (auto &p : candidates) {
        tried.push_back(p);
        ifstream fin(p.c_str());
        if (!fin.is_open()) continue;

        // parse space-separated key/value pairs
        while (!fin.eof()) {
            string key;
            if (!(fin >> key)) break;
            if (key == "num-cpu") { fin >> cfg.num_cpu; }
            else if (key == "scheduler") { fin >> cfg.scheduler; }
            else if (key == "quantum-cycles") { fin >> cfg.quantum_cycles; }
            else if (key == "batch-process-freq") { fin >> cfg.batch_process_freq; }
            else if (key == "min-ins") { fin >> cfg.min_ins; }
            else if (key == "max-ins") { fin >> cfg.max_ins; }
            else if (key == "delays-per-exec") { fin >> cfg.delays_per_exec; }
            else { string val; fin >> val; } // ignore unknown key/value
        }
        fin.close();
        opened = true;
        safe_print(string("Loaded config from: ") + p + "\n");
        break;
    }

    if (!opened) {
        ostringstream oss;
        oss << "Could not open config.txt. Tried paths:\n";
        for (auto &t : tried) oss << "  " << t << "\n";
        throw runtime_error(oss.str());
    }

    // clamp/sanity
    if (cfg.num_cpu < 1) cfg.num_cpu = 1;
    if (cfg.min_ins < 1) cfg.min_ins = 1;
    if (cfg.max_ins < cfg.min_ins) cfg.max_ins = cfg.min_ins;
    global_cfg = cfg;
    global_cfg.valid = true;
}

/* Pick next process according to scheduler */
shared_ptr<Process> pick_next_process_fcfs() {
    lock_guard<mutex> lg(procs_mtx);
    for (auto& p : proc_list) {
        if (p->state == ProcState::READY) return p;
    }
    return nullptr;
}

shared_ptr<Process> pick_next_process_rr() {
    // round robin simplest: find first READY
    return pick_next_process_fcfs();
}

void scheduler_tick_loop() {
    // We'll simulate a CPU tick every 50ms (configurable by changing sleep).
    const chrono::milliseconds tick_interval(50);
    uint64_t local_tick_counter = 0;
    while (scheduler_running.load()) {
        this_thread::sleep_for(tick_interval);
        local_tick_counter++;
        cpu_tick.fetch_add(1);
        // Generate batch process if scheduler_generating and freq divides tick
        if (scheduler_generating.load()) {
            if (global_cfg.batch_process_freq > 0 && (local_tick_counter % global_cfg.batch_process_freq == 0)) {
                // create new process
                static atomic<int> gnum{ 1 };
                string pname;
                {
                    int n = gnum++;
                    char buf[32]; snprintf(buf, sizeof(buf), "p%03d", n);
                    pname = string(buf);
                }
                auto instrs = generate_random_instructions(pname, global_cfg.min_ins, global_cfg.max_ins);
                auto p = create_process(pname, instrs);
                safe_print("\n[Scheduler] Generated process " + pname + " (pid " + to_string(p->pid) + ")\n> ");
            }
        }

        // for each CPU core, allocate/execute
        for (int cid = 0; cid < (int)cpus.size(); ++cid) {
            CPUCore& core = cpus[cid];
            // If core is free, pick a process
            if (!core.busy) {
                shared_ptr<Process> next = nullptr;
                if (global_cfg.scheduler == "fcfs") next = pick_next_process_fcfs();
                else next = pick_next_process_rr();
                if (next) {
                    core.current = next;
                    core.busy = true;
                    next->state = ProcState::RUNNING;
                    next->quantum_left = global_cfg.quantum_cycles;
                    // set per-instruction delay if configured
                    next->delay_left = 0;
                }
                else {
                    core.current = nullptr;
                    core.busy = false;
                }
            }

            if (core.busy && core.current) {
                auto p = core.current;
                // If sleeping, decrement
                if (p->state == ProcState::SLEEPING) {
                    if (p->remaining_sleep > 0) {
                        p->remaining_sleep--;
                        continue;
                    }
                    else {
                        p->state = ProcState::READY;
                    }
                }

                // Simulate delays-per-exec: if delay_left > 0 we decrease it and consume tick
                if (p->delay_left > 0) {
                    p->delay_left--;
                    p->ticks_used++;
                    total_ticks_consumed++;
                    p->quantum_left = (p->quantum_left > 0 ? p->quantum_left - 1 : 0);
                    if (global_cfg.scheduler == "rr" && p->quantum_left == 0) {
                        // preempt
                        p->state = ProcState::READY;
                        core.current = nullptr;
                        core.busy = false;
                    }
                    continue;
                }

                // Execute a single instruction at pc
                if (p->pc >= p->instrs.size()) {
                    // finished
                    p->state = ProcState::FINISHED;
                    {
                        lock_guard<mutex> lg(procs_mtx);
                        proc_table.erase(p->name); // no longer accessible via screen -r
                    }
                    core.current = nullptr;
                    core.busy = false;
                    safe_print("\n[Scheduler] Process " + p->name + " finished.\n> ");
                    continue;
                }
                Instruction& ins = p->instrs[p->pc];
                // execute ins
                switch (ins.type) {
                case InstType::PRINT: {
                    // message, possibly contain var placeholders? we only print message as is
                    p->logs.push_back(ins.a);
                    break;
                }
                case InstType::DECLARE: {
                    uint16_t v = (uint16_t)(ins.numeric & 0xFFFF);
                    p->vars[ins.a] = v;
                    break;
                }
                case InstType::ADD: {
                    uint32_t v1 = 0;
                    if (p->vars.count(ins.b)) v1 = p->vars[ins.b];
                    uint32_t v2 = resolve_operand_value(p, ins.c, ins.numeric, ins.c_is_const);
                    uint32_t res = v1 + v2;
                    if (res > 0xFFFF) res = 0xFFFF;
                    p->vars[ins.a] = (uint16_t)res;
                    break;
                }
                case InstType::SUBTRACT: {
                    uint32_t v1 = 0;
                    if (p->vars.count(ins.b)) v1 = p->vars[ins.b];
                    uint32_t v2 = resolve_operand_value(p, ins.c, ins.numeric, ins.c_is_const);
                    int32_t res = (int32_t)v1 - (int32_t)v2;
                    if (res < 0) res = 0;
                    p->vars[ins.a] = (uint16_t)res;
                    break;
                }
                case InstType::SLEEP: {
                    p->remaining_sleep = ins.numeric;
                    p->state = ProcState::SLEEPING;
                    break;
                }
                case InstType::FOR_START: {
                    // push (pc_of_for_start, repeats)
                    p->for_stack.emplace_back(p->pc, ins.numeric);
                    break;
                }
                case InstType::FOR_END: {
                    if (!p->for_stack.empty()) {
                        auto& top = p->for_stack.back();
                        if (top.second > 1) {
                            top.second--;
                            // jump back to instruction after FOR_START
                            p->pc = top.first; // when incrementing later, it'll move to start+1
                        }
                        else {
                            // pop and continue
                            p->for_stack.pop_back();
                        }
                    }
                    break;
                }
                default: break;
                }

                // simulate delay per exec
                if (global_cfg.delays_per_exec > 0) {
                    p->delay_left = global_cfg.delays_per_exec;
                }

                // advance pc
                p->pc++;
                p->ticks_used++;
                total_ticks_consumed++;

                // RR quantum handling
                if (global_cfg.scheduler == "rr") {
                    if (p->quantum_left > 0) {
                        p->quantum_left--;
                    }
                    if (p->quantum_left == 0 && p->state == ProcState::RUNNING) {
                        // preempt and move back to READY
                        p->state = ProcState::READY;
                        core.current = nullptr;
                        core.busy = false;
                    }
                }
            } // end if busy
        } // end for cpus
    } // end while
}

/* --------------------- CLI Interface --------------------- */

void print_main_prompt() {
    safe_print("> ");
}

// Helper: format time like (01/18/2024 09:15:22AM)
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

    // Header ASCII art that clearly prints "CSOPESY"
    ostringstream oss;

    oss << "Welcome to CSOPESY Emulator!\n\n";

    // CPU utilization summary
    uint64_t used = total_ticks_consumed.load();
    time_t now = time(nullptr);
    double seconds_running = difftime(now, init_time);
    if (seconds_running < 1.0) seconds_running = 1.0;
    int usedcores = 0;
    for (auto& c : cpus) if (c.busy) usedcores++;
    oss << "root:\\> screen -ls\n";
    oss << "CPU utilization: " << setw(3) << right << (global_cfg.num_cpu ? (usedcores * 100 / global_cfg.num_cpu) : 0) << "%\n";
    oss << "Cores used: " << usedcores << "\n";
    oss << "Cores available: " << (global_cfg.num_cpu - usedcores) << "\n\n";
    oss << "--------------------------------------------------------\n\n";

    // Running processes (not FINISHED)
    oss << left << setw(20) << "Running processes" << "\n";
    // column headers
    oss << left << setw(15) << "Name"
        << left << setw(26) << "Created"
        << left << setw(8) << "Core"
        << right << setw(12) << "Used"
        << " / "
        << left << setw(8) << "Total" << "\n";

    // find per-process assigned core if any
    auto find_core_for = [](const shared_ptr<Process>& p)->int {
        for (size_t i = 0; i < cpus.size(); ++i) {
            if (cpus[i].current && cpus[i].current.get() == p.get()) return (int)i;
        }
        return -1;
    };

    for (auto& p : proc_list) {
        if (p->state == ProcState::FINISHED) continue;
        string created = fmt_time(p->created_time);
        int core = find_core_for(p);
        uint64_t total_est = p->instrs.size() * 100; // heuristic total
        oss << left << setw(15) << p->name
            << left << setw(26) << created
            << left << setw(8) << (core >= 0 ? to_string(core) : "-")
            << right << setw(7) << p->ticks_used
            << " / "
            << left << setw(8) << total_est << "\n";
    }

    oss << "\nFinished processes:\n";
    oss << left << setw(15) << "Name"
        << left << setw(26) << "Created"
        << left << setw(12) << "Status"
        << right << setw(12) << "Used"
        << " / "
        << left << setw(8) << "Total" << "\n";

    for (auto& p : proc_list) {
        if (p->state != ProcState::FINISHED) continue;
        string created = fmt_time(p->created_time);
        uint64_t total_est = p->instrs.size() * 100;
        oss << left << setw(15) << p->name
            << left << setw(26) << created
            << left << setw(12) << "Finished"
            << right << setw(7) << p->ticks_used
            << " / "
            << left << setw(8) << total_est << "\n";
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

void screen_attach_loop(shared_ptr<Process> p) {
    if (!p) { safe_print("Process not found.\n"); return; }
    p->attached = true;
    safe_print("---- Attached to process " + p->name + " (pid " + to_string(p->pid) + ") ----\n");
    safe_print("Type \"process-smi\" to show status, \"exit\" to return to main console.\n");
    string line;
    while (true) {
        safe_print(p->name + "> ");
        if (!std::getline(cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;
        if (line == "process-smi") {
            // show status and logs
            ostringstream oss;
            oss << "Process: " << p->name << " (pid " << p->pid << ")\n";
            string state;
            if (p->state == ProcState::RUNNING) state = "RUNNING";
            else if (p->state == ProcState::READY) state = "READY";
            else if (p->state == ProcState::SLEEPING) state = "SLEEPING";
            else if (p->state == ProcState::FINISHED) state = "FINISHED";
            oss << "State: " << state << "\n";
            oss << "PC: " << p->pc << " / " << p->instrs.size() << "\n";
            oss << "Ticks used: " << p->ticks_used << "\n";
            oss << "Variables:\n";
            for (auto& kv : p->vars) oss << "  " << kv.first << " = " << kv.second << "\n";
            oss << "Logs:\n";
            for (auto& l : p->logs) oss << "  " << l << "\n";
            if (p->state == ProcState::FINISHED) oss << "Finished!\n";
            safe_print(oss.str());
        }
        else if (line == "exit") {
            p->attached = false;
            safe_print("Detached from " + p->name + "\n");
            break;
        }
        else {
            safe_print("Unknown command inside screen: " + line + "\n");
        }
    }
}

void main_cli_loop() {
    string line;
    bool running = true;
    print_main_prompt();
    while (running && std::getline(cin, line)) {
        line = trim(line);
        if (line.empty()) { print_main_prompt(); continue; }
        auto args = split_args(line);
        string cmd = args.size() ? args[0] : "";
        if (cmd == "exit") {
            // terminate console
            safe_print("Exiting console...\n");
            running = false;
            break;
        }
        else if (cmd == "initialize") {
            if (initialized.load()) {
                safe_print("Already initialized.\n");
                print_main_prompt();
                continue;
            }
            try {
                load_config("config.txt");
                // initialize cpus
                cpus.clear();
                cpus.resize(global_cfg.num_cpu);
                initialized = true;
                init_time = time(nullptr);
                safe_print("Initialized with config:\n");
                ostringstream oss;
                oss << " num-cpu=" << global_cfg.num_cpu << " scheduler=" << global_cfg.scheduler
                    << " quantum=" << global_cfg.quantum_cycles << " batch_freq=" << global_cfg.batch_process_freq
                    << " min-ins=" << global_cfg.min_ins << " max-ins=" << global_cfg.max_ins
                    << " delays-per-exec=" << global_cfg.delays_per_exec << "\n";
                safe_print(oss.str());
                // start scheduler thread
                scheduler_running = true;
                thread(scheduler_tick_loop).detach();
            }
            catch (exception& e) {
                safe_print(string("Error initializing: ") + e.what() + "\n");
            }
            print_main_prompt();
            continue;
        }
        else {
            // other commands require initialized (except exit)
            if (!initialized.load()) {
                // Only allow "initialize" and "exit" before initialization
                safe_print("Please run \"initialize\" first. Available commands until then: initialize, exit\n");
                print_main_prompt();
                continue;
            }
            if (cmd == "screen") {
                if (args.size() >= 2) {
                    string flag = args[1];
                    if (flag == "-s") {
                        // create new process with name and attach
                        if (args.size() < 3) {
                            safe_print("Usage: screen -s <process name>\n");
                        }
                        else {
                            string pname = args[2];
                            // create a process with instructions: for user-created screens (not scheduler generated),
                            // create random instructions too.
                            auto instrs = generate_random_instructions(pname, global_cfg.min_ins, global_cfg.max_ins);
                            auto p = create_process(pname, instrs);
                            // Immediately attach (clear console emulation)
                            screen_attach_loop(p);
                        }
                    }
                    else if (flag == "-ls") {
                        cmd_screen_ls();
                    }
                    else if (flag == "-r") {
                        if (args.size() < 3) {
                            safe_print("Usage: screen -r <process name>\n");
                        }
                        else {
                            string pname = args[2];
                            auto p = find_process(pname);
                            if (!p) {
                                safe_print("Process " + pname + " not found.\n");
                            }
                            else {
                                screen_attach_loop(p);
                            }
                        }
                    }
                    else {
                        safe_print("Unknown screen option\n");
                    }
                }
                else {
                    safe_print("Usage: screen -s/-ls/-r ...\n");
                }
                print_main_prompt();
                continue;
            }
            else if (cmd == "scheduler-start") {
                scheduler_generating = true;
                safe_print("Scheduler started generating processes.\n");
                print_main_prompt();
                continue;
            }
            else if (cmd == "scheduler-stop") {
                scheduler_generating = false;
                safe_print("Scheduler stopped generating processes.\n");
                print_main_prompt();
                continue;
            }
            else if (cmd == "report-util") {
                cmd_report_util();
                print_main_prompt();
                continue;
            }
            else {
                safe_print("Unknown command: " + cmd + "\n");
                print_main_prompt();
                continue;
            }
        }
    } // end while
    // cleanup
    scheduler_running = false;
    scheduler_generating = false;
    // give scheduler thread time to exit
    this_thread::sleep_for(chrono::milliseconds(100));
}

/* --------------------- main --------------------- */
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Print ASCII header on program start with the word CSOPESY 
    safe_print(
        "   _____  _____  ____  _____  ______  _______     __ \n"
        "  / ____|/ ____|/ __ \\|  __ \\|  ____|/ ____\\ \\   / / \n"
        " | |    | (___ | |  | | |__) | |__  | (___  \\ \\_/ /  \n"
        " | |     \\___ \\| |  | |  ___/|  __|  \\___ \\  \\   /   \n"
        " | |____ ____) | |__| | |    | |____ ____) |  | |    \n"
        "  \\_____|_____/ \\____/|_|    |______|_____/   |_|    \n"
        "--------------------------------------------------------\n\n"
        
        "Welcome to CSOPESY Emulator!\n"
        "Available commands: initialize, exit, screen, scheduler-start, scheduler-stop, report-util\n"
        "Type \"initialize\" to begin (reads config.txt).\n"
    );

    main_cli_loop();
    safe_print("Console terminated.\n");
    return 0;
}
