#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
using namespace std;



void marquee(string text, int width, int delay_ms) {
    text = string(width, ' ') + text + string(width, ' ');
    for (size_t i = 0; i < text.size() - width; i++) {
        cout << "\r" << text.substr(i, width) << flush;
        this_thread::sleep_for(chrono::milliseconds(delay_ms));
    }
}

int main() {

    // Chatgpt way of getting the size of the cli window
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int columns, rows;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    columns = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    // can change the \ sign to back slash srry my keybaord korean
    std::cout << "Rows: " << rows << "\n";
    std::cout << "Cols: " << columns << "\n";

    //using vector cause apprantly arrays set window size at compile time 
    //and vector can change screen size at runtime
	std::vector<std::vector<char>> screen(rows, std::vector<char>(columns, ' '));


     
  
    thread t(marquee, " Multi-tasking marquee running... ", columns, 120);
    t.join();

    return 0;
}
