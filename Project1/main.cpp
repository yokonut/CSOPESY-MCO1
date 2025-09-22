#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <mutex>
#include <map>
using namespace std;

std::atomic<bool> marquee_running(false);
std::thread marquee_thread;
std::vector<std::string> marquee_ascii; // Stores ASCII art text lines
int marquee_speed = 100;
int marquee_width = 0;
std::mutex marquee_mutex; // Protects marquee_ascii and marquee_speed

// ASCII Art Font (Blocky Letters A-Z, Space)
map<char, vector<string>> asciiFont = {
    {'A', {"  ##  ", " #  # ", " #### ", " #  # ", " #  # "}},
    {'B', {" ###  ", " #  # ", " ###  ", " #  # ", " ###  "}},
    {'C', {"  ### ", " #    ", " #    ", " #    ", "  ### "}},
    {'D', {" ###  ", " #  # ", " #  # ", " #  # ", " ###  "}},
    {'E', {" #### ", " #    ", " ###  ", " #    ", " #### "}},
    {'F', {" #### ", " #    ", " ###  ", " #    ", " #    "}},
    {'G', {"  ### ", " #    ", " # ## ", " #  # ", "  ### "}},
    {'H', {" #  # ", " #  # ", " #### ", " #  # ", " #  # "}},
    {'I', {" ### ", "  # ", "  # ", "  # ", " ### "}},
    {'J', {"   ## ", "    # ", "    # ", " #  # ", "  ##  "}},
    {'K', {" #  # ", " # #  ", " ##   ", " # #  ", " #  # "}},
    {'L', {" #    ", " #    ", " #    ", " #    ", " #### "}},
    {'M', {" #   # ", " ## ## ", " # # # ", " #   # ", " #   # "}},
    {'N', {" #   # ", " ##  # ", " # # # ", " #  ## ", " #   # "}},
    {'O', {"  ##  ", " #  # ", " #  # ", " #  # ", "  ##  "}},
    {'P', {" ###  ", " #  # ", " ###  ", " #    ", " #    "}},
    {'Q', {"  ##  ", " #  # ", " #  # ", " # ## ", "  ### "}},
    {'R', {" ###  ", " #  # ", " ###  ", " # #  ", " #  # "}},
    {'S', {"  ### ", " #    ", "  ##  ", "    # ", " ###  "}},
    {'T', {" ##### ", "   #   ", "   #   ", "   #   ", "   #   "}},
    {'U', {" #  # ", " #  # ", " #  # ", " #  # ", "  ##  "}},
    {'V', {" #   # ", " #   # ", " #   # ", "  # #  ", "   #   "}},
    {'W', {" #   # ", " #   # ", " # # # ", " ## ## ", " #   # "}},
    {'X', {" #   # ", "  # #  ", "   #   ", "  # #  ", " #   # "}},
    {'Y', {" #   # ", "  # #  ", "   #   ", "   #   ", "   #   "}},
    {'Z', {" #### ", "    # ", "   #  ", "  #   ", " #### "}},
    {' ', {"      ", "      ", "      ", "      ", "      "}}
};

// Convert normal text into ASCII art (line by line)
vector<string> textToAscii(const string& text) {
    vector<string> output(5, ""); // 5 rows tall
    bool first = true;
    for (char c : text) {
        char up = toupper(c);
        if (asciiFont.find(up) == asciiFont.end()) up = ' ';
        for (int i = 0; i < 5; i++) {
            if (!first) output[i] += " "; // single space between letters
            output[i] += asciiFont[up][i];
        }
        first = false;
    }
    return output;
}

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
    cout << "3. set_text - Accepts text and converts it to ASCII marquee.\n";
    cout << "4. set_speed - sets the marquee animation refresh in ms.\n";
    cout << "5. exit - terminates the console.\n\n";
}

void displayMessage(const string& msg) {
    cout << msg << endl;
}

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

// ASCII Marquee
void marquee(int width) {
    int pos = width;
    while (marquee_running) {
        vector<string> current_ascii;
        int current_speed;
        {
            std::lock_guard<std::mutex> lock(marquee_mutex);
            current_ascii = marquee_ascii;
            current_speed = marquee_speed;
        }

        for (int row = 0; row < current_ascii.size(); row++) {
            string ascii_line = current_ascii[row];
            if (row == 0) {
                size_t endpos = ascii_line.find_last_not_of(" ");
                if (endpos != string::npos) {
                    ascii_line = ascii_line.substr(0, endpos + 1);
                }
            }
            string padded(width + ascii_line.size(), ' ');
            int start = max(0, pos);
            if (start < padded.size())
                padded.replace(start, ascii_line.size(), ascii_line);

            cout << padded.substr(0, width) << "\n";
        }

        this_thread::sleep_for(std::chrono::milliseconds(current_speed));

        pos--;
        if (pos + (int)current_ascii[0].size() < 0) pos = width; // loop back
    }

}

void startMarquee() {
    if (!marquee_running) {
        marquee_running = true;
        marquee_thread = std::thread(marquee, marquee_width);
        displayMessage("Starting ASCII marquee...");
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
            displayMessage("Enter text for ASCII marquee: ");
            std::string new_text = getTextInput();
            {
                std::lock_guard<std::mutex> lock(marquee_mutex);
                marquee_ascii = textToAscii(new_text);
            }
            displayMessage("ASCII marquee set!");
        }
        else if (command == "set_speed") {
            displayMessage("Enter speed in milliseconds: ");
            int new_speed = getIntegerInput();
            {
                std::lock_guard<std::mutex> lock(marquee_mutex);
                marquee_speed = (new_speed < 10) ? 10 : new_speed; // avoid too fast
            }
            displayMessage("Speed set to: " + std::to_string(marquee_speed) + " ms");
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
