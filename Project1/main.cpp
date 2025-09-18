#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <mutex>
using namespace std;

std::atomic<bool> marquee_running(false);
std::thread marquee_thread;
std::string marquee_text = " Multi-tasking marquee running... ";
int marquee_speed = 100;
int marquee_width = 0;
std::mutex marquee_mutex; // Protects marquee_text and marquee_speed

void showMainMenu() {
    cout << "Marquee Main Menu\n";
    cout << "=================\n";
    cout << "Type 'help' to see available commands.\n";
    cout << "\nCommand input: ";
}

void showHelp() {
    cout << "\nAvailable commands:\n";
    cout << "1. start_marquee - starts the marquee animation.\n";
    cout << "2. stop_marquee - stops the marquee animation.\n";
    cout << "3. set_text - Accepts a text input and displays it as a marquee.\n";
    cout << "4. set_speed - sets the marquee animation refresh in milliseconds.\n";
    cout << "5. exit - terminates the console.\n\n";
}

void displayMessage(const string& msg) {
    cout << msg << endl;
}

// Keyboard Handler
string getCommandInput() {
    string cmd;
    cin >> cmd;
    return cmd;
}

string getTextInput() {
    string text;
    cin.ignore();
    getline(cin, text);
    return text;
}

int getIntegerInput() {
    int value;
    cin >> value;
    return value;
}

// Marquee Logic
void marquee(int width) {
    size_t i = 0;
    while (marquee_running) {
        std::string current_text;
        int current_speed;
        {
            std::lock_guard<std::mutex> lock(marquee_mutex);
            current_text = marquee_text;
            current_speed = marquee_speed;
        }
        std::string padded = std::string(width, ' ') + current_text + std::string(width, ' ');
        if (i >= padded.size() - width) i = 0;
        cout << "\r" << padded.substr(i, width) << flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(current_speed));
        ++i;
    }
    cout << "\r" << std::string(width, ' ') << flush; // Clear line
}

void startMarquee() {
    if (!marquee_running) {
        marquee_running = true;
        marquee_thread = std::thread(marquee, marquee_width);
        displayMessage("Starting marquee...");
    } else {
        displayMessage("Marquee is already running.");
    }
}

void stopMarquee() {
    if (marquee_running) {
        marquee_running = false;
        if (marquee_thread.joinable())
            marquee_thread.join();
        displayMessage("Stopping marquee...");
    } else {
        displayMessage("Marquee is not running.");
    }
}

int main() {
    // Get console size
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    marquee_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    cout << "Rows: " << rows << "\n";
    cout << "Cols: " << marquee_width << "\n";

    string command;

    // Command Interpreter Loop
    do {
        showMainMenu();
        command = getCommandInput();

        if (command == "help") {
            showHelp();
        }
        else if (command == "start_marquee") {
            startMarquee();
        }
        else if (command == "stop_marquee") {
            stopMarquee();
        }
        else if (command == "set_text") {
            displayMessage("Enter text for marquee: ");
            std::string new_text = getTextInput();
            {
                std::lock_guard<std::mutex> lock(marquee_mutex);
                marquee_text = new_text;
            }
            displayMessage("Text set to: " + new_text);
        }
        else if (command == "set_speed") {
            displayMessage("Enter speed in milliseconds: ");
            int new_speed = getIntegerInput();
            {
                std::lock_guard<std::mutex> lock(marquee_mutex);
                marquee_speed = new_speed;
            }
            displayMessage("Speed set to: " + std::to_string(new_speed) + " ms");
        }
        else if (command == "exit") {
            displayMessage("Exiting...");
            stopMarquee();
            break;
        }
        else {
            displayMessage("Unknown command. Type 'help' for a list of commands.");
        }
    } while (true);

    return 0;
}
