#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
using namespace std;

void marquee(string text, int width, int delay_ms) {
    text = string(width, ' ') + text + string(width, ' ');
    for (size_t i = 0; i < text.size() - width; i++) {
        cout << "\r" << text.substr(i, width) << flush;
        this_thread::sleep_for(chrono::milliseconds(delay_ms));
    }
}

void showMainMenu() {
    cout << "Marquee Main Menu\n";
    cout << "=================\n";
    cout << "Type 'help' to see available commands.\n";
    cout << "\nCommand input: ";
}

int main() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int columns, rows;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    columns = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    cout << "Rows: " << rows << "\n";
    cout << "Cols: " << columns << "\n";

    vector<vector<char>> screen(rows, vector<char>(columns, ' '));

    string command;
    string text;

    showMainMenu();
    cin >> command;

    do {
        if (command == "help") {
            cout << "\nAvailable commands:\n";
            cout << "1. start_marquee - starts the marquee animation.\n";
            cout << "2. stop_marquee - stops the marquee animation.\n";
            cout << "3. set_text - Accepts a text input and displays it as a marquee.\n";
            cout << "4. set_speed - sets the marquee animation refresh in milliseconds.\n";
            cout << "5. exit - terminates the console.\n\n";
        }
        else if (command == "start_marquee") {
            cout << "Starting marquee...\n";
            thread t(marquee, text, columns, 100);
            t.join();
        }
        else if (command == "stop_marquee") { 
            cout << "Stopping marquee...\n";
            // To be implemented
        }
        else if (command == "set_text") {
            cout << "Enter text for marquee: ";
            cin.ignore();
            getline(cin, text);
            cout << "Text set to: " << text << "\n";
        }
        else if (command == "set_speed") { 
            // To be implemented
        }
        else if (command == "exit") {
            cout << "Exiting...\n";
            return 0;
        }
        else {
            cout << "Unknown command. Type 'help' for a list of commands.\n";
        }
    } while (command != "exit" && (cout << "Command input: ", cin >> command, true));
    

    return 0;
}
