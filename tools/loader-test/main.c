// Freestanding test program for the PE loader: no CRT, only kernel32 calls the
// Win32 layer passes through or replaces. Prints one line and exits with 42.
#include <windows.h>

void start(void)
{
    static const char line[] = "loader-test: hello from a mapped exe\r\n";
    DWORD written = 0;
    OutputDebugStringA(line);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, sizeof(line) - 1, &written, NULL);
    ExitProcess(42);
}
