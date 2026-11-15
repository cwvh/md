// Placeholder until its milestone fills it in.
__declspec(dllimport) __attribute((noreturn)) void ExitProcess(unsigned);

void mainCRTStartup(void) {
    ExitProcess(0);
}
