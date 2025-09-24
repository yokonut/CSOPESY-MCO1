CSOPESY – MCO2: Marquee Operator
================================

Created by: S13 Group 5
Lorenzo Donaire, Yohan Ko, Margaux Siongco, Wesley Sy 
------------------------------------------------------


1. Project Description
- Our project implements a simple OS emulator with a text-based 
marquee operator. The program provides a command-line interface 
(CLI) that accepts commands such as to start/stop the marquee, 
set text, and configure speed, request help, or exit the console. It mimics a shell environment 
and demonstrates command interpretation, display handling, 
and marquee animation.
--------------------------------


3. Features
- Command interpreter (CLI input loop)
- Marquee text scrolling 
- Adjustable marquee text (set_text)
- Adjustable marquee speed (set_speed)
- Help menu with list of commands
- Exit command to terminate the program
---------------------------------

3. Commands
- help          – Displays available commands.
- start_marquee – Starts the marquee animation.
- stop_marquee  – Stops the marquee animation.
- set_text      – Accepts a text input and displays it as marquee.
- set_speed     – Sets marquee refresh speed in milliseconds. 
-  exit          – Terminates the program.
-----------------------------------------

4. Requirements
- Windows OS
- Microsoft Visual Studio 
- Console with Windows.h support
--------------------------------


5. How to Run
- Open the project in Visual Studio 2022.
- Compile the solution (`Ctrl + F5`).
- Run the program from the terminal.
- Type commands as instructed in the main menu.
-----------------------------------------------

6. Example Usage
After running, type the following commands:

> help  
> set_text  
HELLO WORLD  
> set_speed  
100  
> start_marquee  
> stop_marquee  
> exit
--------------------------------


7. File Structure
- main.cpp     – Entry point of the program (contains main()).
- README.txt   – Project description and usage instructions.
------------------------------------------------------------


End of README


