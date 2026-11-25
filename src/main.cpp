#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

int runWindowsApp(HINSTANCE instance, int showCommand);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    return runWindowsApp(instance, showCommand);
}

#else

int runLinuxApp(int argc, char** argv);

int main(int argc, char** argv) { return runLinuxApp(argc, argv); }

#endif
