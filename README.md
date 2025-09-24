CSOPESY – MCO2: Marquee Operator
================================

Created by: S13 Group 5
Lorenzo Donaire, Yohan Ko, Margaux Siongco, Wesley Sy 



1. Project Description
--------------------------------
Our project implements a simple OS emulator with a text-based 
marquee operator. The program provides a command-line interface 
(CLI) that accepts commands such as to start/stop the marquee, 
set text, and configure speed, request help, or exit the console. It mimics a shell environment 
and demonstrates command interpretation, display handling, 
and marquee animation.


2. Features
--------------------------------
- Command interpreter (CLI input loop)
- Marquee text scrolling 
- Adjustable marquee text (set_text)
- Adjustable marquee speed (set_speed)
- Help menu with list of commands
- Exit command to terminate the program


3. Commands
--------------------------------
1. help          – Displays available commands.
2. start_marquee – Starts the marquee animation.
3. stop_marquee  – Stops the marquee animation. 
4. set_text      – Accepts a text input and displays it as marquee.
5. set_speed     – Sets marquee refresh speed in milliseconds. 
6. exit          – Terminates the program.


4. Requirements
--------------------------------
- Windows OS
- Microsoft Visual Studio 
- Console with Windows.h support


5. How to Run
--------------------------------
1. Open the project in Visual Studio 2022.
2. Compile the solution (`Ctrl + F5`).
3. Run the program from the terminal.
4. Type commands as instructed in the main menu.



6. File Structure
--------------------------------
- main.cpp     – Entry point of the program (contains main()).
- README.txt   – Project description and usage instructions.

=============
End of README

