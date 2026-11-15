// Hand-written Win32 declarations (no windows.h).

#define W32(r) __declspec(dllimport) r

enum {
    STD_INPUT_HANDLE  = -10,
    STD_OUTPUT_HANDLE = -11,
    STD_ERROR_HANDLE  = -12,

    MEM_COMMIT     = 0x1000,
    MEM_RESERVE    = 0x2000,
    PAGE_NOACCESS  = 0x01,
    PAGE_READWRITE = 0x04,

    GENERIC_READ          = (i32)0x80000000,
    FILE_SHARE_READ       = 0x0001,
    FILE_SHARE_WRITE      = 0x0002,
    FILE_SHARE_DELETE     = 0x0004,
    OPEN_EXISTING         = 3,
    FILE_ATTRIBUTE_NORMAL = 0x80,
};

#define INFINITE             0xffffffffu  // does not fit in an enum
#define INVALID_HANDLE_VALUE ((uptr)-1)

W32(void *) VirtualAlloc(void *, uz, u32, u32);
W32(b32)    VirtualProtect(void *, uz, u32, u32 *);
W32(uptr)   GetStdHandle(i32);
W32(b32)    GetConsoleMode(uptr, u32 *);
W32(b32)    WriteConsoleW(uptr, c16 const *, u32, u32 *, void *);
W32(b32)    WriteFile(uptr, u8 const *, u32, u32 *, void *);
W32(uptr)   CreateFileW(c16 const *, u32, u32, void *, u32, u32, uptr);
W32(b32)    ReadFile(uptr, u8 *, u32, u32 *, void *);
W32(b32)    GetFileSizeEx(uptr, i64 *);
W32(b32)    CloseHandle(uptr);
W32(u32)    GetLastError(void);
W32(u64)    GetTickCount64(void);
W32(b32)    QueryPerformanceCounter(i64 *);
W32(b32)    QueryPerformanceFrequency(i64 *);
W32(c16 *)  GetCommandLineW(void);
__declspec(dllimport) __attribute((noreturn))
void ExitProcess(u32);
