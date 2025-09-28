#include <iostream>
#define NOMINMAX 1
#include <windows.h>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <mutex>
#include <limits>
#include <algorithm>
using namespace std;

std::atomic<bool> marquee_running(false);
std::thread marquee_thread;
std::string marquee_text = "GIT GUD";
int marquee_speed = 100;
int marquee_width = 0;
int marquee_height = 0;
std::mutex marquee_mutex;
std::mutex io_mutex; 

void showMainMenu() {
    std::lock_guard<std::mutex> lock(io_mutex);
    cout << "Marquee Main Menu\n";
    cout << "=================\n";
    cout << "Type 'help' to see available commands.\n\n";
}

void showHelp() {
    std::lock_guard<std::mutex> lock(io_mutex);
    cout << "\nAvailable commands:\n";
    cout << "1. start_marquee - starts the bouncing screensaver.\n";
    cout << "2. stop_marquee - stops the animation.\n";
    cout << "3. set_text - sets the bouncing label text.\n";
    cout << "4. set_speed - sets the marquee animation refresh in milliseconds.\n";
    cout << "5. exit - terminates the console.\n\n";
}

void displayMessage(const string& msg) {
    std::lock_guard<std::mutex> lock(io_mutex);
    cout << msg << endl;
}

void displayPrompt() {
    std::lock_guard<std::mutex> lock(io_mutex);
    cout << "Command input: ";
    cout.flush();
}

// Keyboard Handler
string getCommandInput() {
    string cmd;
    cin >> cmd;
    return cmd;
}

string getTextInput() {
    string text;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    getline(cin, text);
    return text;
}

int getIntegerInput() {
    int value;
    cin >> value;
    return value;
}

// Bouncing DVD-style animation
static HANDLE getConsoleHandle() {
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

static void setCursorPos(short x, short y) {
    COORD coord{ x, y };
    SetConsoleCursorPosition(getConsoleHandle(), coord);
}

static void queryConsoleSize(int& outCols, int& outRows) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(getConsoleHandle(), &csbi)) {
        outCols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        outRows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    else {
        outCols = marquee_width;
        outRows = marquee_height;
    }
}

void marquee() {
    // initial position and direction
    int x = 0;
    int y = 0;
    int dx = 1;
    int dy = 1;
    while (marquee_running) {
        std::string current_text;
        int current_speed;
        int cols, rows;
        // Always query latest console size to react to window resizes
        queryConsoleSize(cols, rows);
        {
            std::lock_guard<std::mutex> lock(marquee_mutex);
            marquee_width = cols;
            marquee_height = rows;
            current_text = marquee_text;
            current_speed = marquee_speed;
        }

        const int textLen = static_cast<int>(current_text.size());
        const int maxX = std::max(0, cols - textLen);
        const int maxRow = std::max(0, rows - 2); // avoid last line for input

        // erase previous
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            setCursorPos(static_cast<short>(x), static_cast<short>(y));
            cout << string(textLen, ' ');
        }

        // step
        x += dx;
        y += dy;

        if (x <= 0) { x = 0; dx = 1; }
        else if (x >= maxX) { x = maxX; dx = -1; }

        if (y <= 0) { y = 0; dy = 1; }
        else if (y >= maxRow) { y = maxRow; dy = -1; }

        // draw new
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            setCursorPos(static_cast<short>(x), static_cast<short>(y));
            cout << current_text << flush;
            // move caret back to prompt line start to reduce disruption
            setCursorPos(0, static_cast<short>(rows - 1));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(current_speed));
    }
    // clear last position when stopping
    {
        std::lock_guard<std::mutex> lock(io_mutex);
        // best effort: clear entire first rows area where text might be
        for (int r = 0; r < std::max(0, marquee_height - 1); ++r) {
            setCursorPos(0, static_cast<short>(r));
            cout << string(marquee_width, ' ');
        }
        setCursorPos(0, static_cast<short>(marquee_height - 1));
        cout.flush();
    }
}

void startMarquee() {
    if (!marquee_running) {
        marquee_running = true;
        marquee_thread = std::thread(marquee);
        displayMessage("Starting marquee...");
    }
    else {
        displayMessage("Marquee is already running.");
    }
}

void stopMarquee() {
    if (marquee_running) {
        marquee_running = false;
        if (marquee_thread.joinable())
            marquee_thread.join();
        displayMessage("Stopping marquee...");
    }
    else {
        displayMessage("Marquee is not running.");
    }
}

int main() {


    string command;

    // Command Interpreter Loop
    do {
        showMainMenu();
        displayPrompt();
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
