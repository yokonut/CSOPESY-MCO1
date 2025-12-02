CSOPESY – MO2 Multitasking OS
======================================================

Created by: S13 Group 5
Miguel Delos Reyes, Lorenzo Donaire, Yohan Ko, Margaux Siongco, Wesley Sy

------------------------------------------------------

## Overview
CSOPESY – MO2 Process Scheduler is a console-based multi-core process scheduler emulator with **demand paging** and **memory management**. It simulates CPU scheduling, virtual memory, page fault handling, and backing store operations.  

## Entry point
The program entry point (contains the main function) is:
- Project file: `Project1/main.cpp`
  
## Build (Visual Studio 2022)
1. Open Visual Studio 2022.
2. Open the repository folder via __File > Open > Folder__ (or open the solution if available).
3. If needed, create a Console Application project and add `Project1/main.cpp` to it.
4. Set the project as startup via the Solution Explorer (right-click project -> __Set as Startup Project__).
5. Build: use __Build > Build Solution__.

   ## Run
1. From Visual Studio:
   - Run without debugger: __Debug > Start Without Debugging__ (or press __Ctrl+F5__).
2. From terminal:
   - Run the produced executable (e.g., `./csopesy` or `csopesy.exe`).

After start, the program shows a banner and an interactive prompt.

## Commands

### System Commands
- **initialize** — Load configuration (reads `config.txt`), initialize memory frames, and start scheduler thread
- **exit** — Exit the console

### Scheduler Commands
- **scheduler-start** — Enable automatic batch process generation
- **scheduler-stop** — Disable automatic generation

### Process Commands
- **screen -ls** — List all running and finished processes
- **screen -s \<name\> \<mem_bytes\>** — Create and attach to a new process with specified memory allocation (must be power of 2, between 64-65536 bytes)
- **screen -c \<name\> \<mem\> "\<instructions\>"** — Create a process with custom instructions (supports DECLARE, ADD, SUB, READ, WRITE, PRINT)
- **screen -r \<name\>** — Re-attach to an existing process

### Memory Commands
- **process-smi** — Display detailed process and memory information (similar to nvidia-smi):
  - CPU utilization and core status
  - Per-process memory usage and page allocation
  - Memory violation status
- **vmstat** — Display virtual memory statistics:
  - Total memory and usage
  - Number of pages in/out (paging statistics)
  - Idle/active CPU ticks

### Reporting Commands
- **report-util** — Write utilization report to `csopesy-log.txt`

### Inside Attached Process Screen
When attached to a process screen, additional commands are available:
- **process-smi** — Show process status and memory info
- **exit** — Detach from process (process continues running)

## Configuration (`config.txt`)
The app searches for `config.txt` in multiple candidate locations including working directory and executable directory. It expects whitespace-separated key/value pairs:

### CPU & Scheduling
- **num-cpu \<int\>** — Number of CPU cores (default: 1)
- **scheduler \<fcfs|rr\>** — Scheduling policy: First-Come-First-Serve or Round-Robin (default: fcfs)
- **quantum-cycles \<int\>** — Round-Robin quantum cycles (default: 1)
- **batch-process-freq \<int\>** — Ticks between auto-generated processes (default: 10)
- **delays-per-exec \<int\>** — Artificial delay ticks per instruction (default: 0)

### Process Generation
- **min-ins \<int\>** — Minimum instructions for generated processes (default: 1)
- **max-ins \<int\>** — Maximum instructions for generated processes (default: 5)

### Memory Management (MO2)
- **max-overall-mem \<int\>** — Total physical memory in bytes (must be power of 2, default: 65536)
- **mem-per-frame \<int\>** — Frame/page size in bytes (must be power of 2, default: 256)
- **min-mem-per-proc \<int\>** — Minimum memory per process in bytes (must be power of 2, default: 64)
- **max-mem-per-proc \<int\>** — Maximum memory per process in bytes (must be power of 2, default: 1024)

**Note:** Memory values must be powers of 2. The system uses demand paging with FIFO page replacement.

## Features

### Memory Management (MO2)
- **Demand Paging**: Pages are loaded into memory only when accessed
- **Page Fault Handling**: Automatic page-in from backing store when page faults occur
- **FIFO Page Replacement**: Oldest page evicted when memory is full
- **Backing Store**: Pages stored in `csopesy-backing-store.txt` when evicted
- **Memory Validation**: Out-of-bounds access detection and violation tracking
- **READ/WRITE Instructions**: Processes can read/write to virtual memory addresses

### Process Scheduling
- **Multi-core CPU**: Configurable number of CPU cores
- **FCFS or Round-Robin**: Choose scheduling algorithm
- **Automatic Process Generation**: Batch process creation at configurable intervals
- **Process States**: READY, RUNNING, SLEEPING, FINISHED, TERMINATED_BY_MEM

### Monitoring & Reporting
- **process-smi**: Real-time view of CPU and memory utilization per process
- **vmstat**: Virtual memory statistics including paging activity
- **report-util**: Generate detailed utilization reports to log file

## Output Files
- **csopesy-log.txt** — Utilization reports generated by `report-util` command
- **csopesy-backing-store.txt** — Backing store for paged-out memory pages (hex format)
------------------------------------------------------


End of README


