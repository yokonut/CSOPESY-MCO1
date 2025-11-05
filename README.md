CSOPESY – MO1 Process Scheduler
================================

Created by: S13 Group 5
Miguel Delos Reyes, Lorenzo Donaire, Yohan Ko, Margaux Siongco, Wesley Sy

------------------------------------------------------

## Overview
CSOPESY – MO1 Process Scheduler is a console-based multi-core process scheduler emulator.  

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

After start the program shows a banner and an interactive prompt. Common commands:
- initialize — load configuration (reads `config.txt`) and start scheduler thread
- scheduler-start — enable automatic batch process generation
- scheduler-stop — disable automatic generation
- screen -ls — list running and finished processes
- screen -s <name> — create and attach to a new process (interactive)
- screen -r <name> — re-attach to an existing process
- process-smi (inside attached session) — show process status
- report-util — write utilization report to `csopesy-log.txt`
- exit — exit the console

## Configuration (`config.txt`)
The app searches for `config.txt` in multiple candidate locations including working directory and executable directory. It expects whitespace-separated key/value pairs:

- num-cpu <int>             — number of CPU cores (default 1)
- scheduler <fcfs|rr>       — scheduling policy (default fcfs)
- quantum-cycles <int>      — RR quantum (default 1)
- batch-process-freq <int>  — ticks between auto-generated processes (default 10)
- min-ins <int>             — min instructions for generated processes (default 1)
- max-ins <int>             — max instructions for generated processes (default 5)
- delays-per-exec <int>     — artificial delay ticks per instruction (default 0)
------------------------------------------------------


End of README


