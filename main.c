// md: a tiny native Markdown viewer, built without a C runtime
//   $ make                 # md.exe
//   $ make md-debug.exe    # debug info, UBSan traps, guard pages
// Unity build: this file includes all the others. Portable code (the parser,
// layout and view) talks to the OS only through platform.h; this file and
// gfx.c are its Windows implementation.
#include "base.c"
#include "platform.h"
#include "md.c"
#include "highlight.c"
#include "layout.c"
#include "view.c"
#include "win32.c"
#include "image.c"
#include "gfx.c"

// Debug builds (and the BENCH build of the Makefile) take --bench: time the
// start up and a run of scrolled paints, write the numbers to md-bench.txt in
// the current directory, and quit.
#if defined(DEBUG) || defined(BENCH)
#  define BENCHMARK 1
#endif


// Platform state

enum {
    WATCH_TIMER = 0x7ffffff0,   // internal timer id, outside the app's range
    WATCH_MS    = 250,
};

typedef struct {
    u64 written;
    u32 size;
    b32 ok;
} FileStat;

static struct {
    uptr     hwnd;
    uptr     cursors[3];        // indexed by Cursor
    Cursor   cursor;
    u32      dpi;               // window DPI, 96 at scale 1
    b32      ready;             // app_init has run
    b32      tracking;          // a mouse leave notification is pending
    b32      dark;
    i32      darkforce;         // debug builds: 1 dark, 2 light, 0 system
    i32      zoomsteps;         // debug builds: keys to press after start up, for
    i32      pagedowns;         //   screenshots without typing into someone's window
    Arena    tmp;               // platform scratch, for string conversion
    Arena    scratch;           // passed to app_paint
    c16      highsurrogate;     // pending first half of a typed pair

    // File watching
    c16      *watchpath;        // UTF-16, null-terminated, or 0
    FileStat  watchlast;
    FileStat  watchpend;
    b32       watchpending;

    // Optional Windows 10 entry points, found at runtime
    b32 (*setdpiaware)(uptr);
    u32 (*getdpi)(uptr);
    b32 (*adjustrect)(WinRect *, u32, b32, u32, u32);
} g;


// Platform: memory and output

// Buffered Output, see base.c. A GUI process has no console, so this only
// matters for a debug trace redirected to a file or pipe.
static b32 os_write(Output *o) {
    if (!o->console) {
        for (iz off = 0; off < o->len;) {
            u32 n     = 0;
            u32 count = (u32)min(o->len - off, (iz)1<<30);
            if (!WriteFile(o->handle, o->buf+off, count, &n, 0) || !n) {
                return 0;
            }
            off += n;
        }
        return 1;
    }
    iz len = toutf16(o->wbuf, (Str){o->buf, o->len});
    for (iz off = 0; off < len;) {
        iz count = min(len - off, (iz)1<<15);
        c16 last = o->wbuf[off+count-1];
        if (off+count<len && last>=0xd800 && last<=0xdbff) {
            count--;  // don't split a surrogate pair
        }
        u32 n = 0;
        if (!WriteConsoleW(o->handle, o->wbuf+off, (u32)count, &n, 0)) {
            return 0;
        }
        off += count;
    }
    return 1;
}

#ifdef DEBUG
// Debug memory checking, see alloc() in base.c.
static void os_protect(void *p, iz size, b32 readwrite) {
    u32 old = 0;
    b32 ok  = VirtualProtect(p, (uz)size, readwrite ? PAGE_READWRITE : PAGE_NOACCESS, &old);
    assert(ok);
}
#endif

// NUL-terminate and convert to UTF-16 in the platform scratch arena.
static c16 *win_towide(Arena *a, Str s) {
    c16 *w = new(a, s.len+1, c16);
    iz   n = toutf16(w, s);
    w[n]   = 0;
    return w;
}

static Str ci_prefix(Str s, Str p) {
    if (s.len < p.len) {
        return (Str){0};
    }
    for (iz i = 0; i < p.len; i++) {
        if (lower(s.data[i]) != p.data[i]) {
            return (Str){0};
        }
    }
    return (Str){s.data+p.len, s.len-p.len};
}


// Platform: window and system

static void os_set_cursor(Cursor c) {
    if (c != g.cursor) {
        g.cursor = c;
        // The system asks for the cursor (WM_SETCURSOR) before it delivers
        // the mouse move that lets the app choose, so apply the change now
        // rather than at the next move.
        SetCursor(g.cursors[c]);
    }
}

static void os_invalidate(void) {
    InvalidateRect(g.hwnd, 0, 0);
}

static void os_set_title(Str title) {
    Arena t = g.tmp;
    SetWindowTextW(g.hwnd, win_towide(&t, title));
}

static void os_timer_start(i32 id, i32 ms) {
    SetTimer(g.hwnd, (uptr)id, (u32)max(ms, (i32)1), 0);
}

static void os_timer_stop(i32 id) {
    KillTimer(g.hwnd, (uptr)id);
}

static b32 query_dark(void) {
#ifdef DEBUG
    if (g.darkforce) {
        return g.darkforce == 1;
    }
#endif
    uptr key = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    u32 value = 1;
    u32 size  = sizeof(value);
    u32 type  = 0;
    i32 r = RegQueryValueExW(key, L"AppsUseLightTheme", 0, &type, &value, &size);
    RegCloseKey(key);
    return r==ERROR_SUCCESS && value==0;
}

static b32 os_dark_mode(void) {
    return g.dark;
}

// Dark title bar. Attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE.
static void apply_title_theme(void) {
    b32 on = g.dark;
    DwmSetWindowAttribute(g.hwnd, 20, &on, sizeof(on));
}

static f32 os_scale(void) {
    return (f32)g.dpi / 96.0f;
}

static Size os_window_size(void) {
    WinRect rc = {0};
    GetClientRect(g.hwnd, &rc);
    f32 s = 96.0f / (f32)g.dpi;
    return (Size){(f32)(rc.right - rc.left)*s, (f32)(rc.bottom - rc.top)*s};
}

static void os_quit(void) {
    DestroyWindow(g.hwnd);
}

static b32 os_read_file(Arena *a, Str path, Str *out) {
    Arena t = g.tmp;
    uptr  f = CreateFileW(win_towide(&t, path), GENERIC_READ,
                          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                          0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    i64 size = 0;
    // Debug allocations carry a guard page on each side, so leave room.
    b32 ok = GetFileSizeEx(f, &size) && size>=0 && size < (a->end - a->beg) - 3*4096;
    u8 *data = 0;
    if (ok) {
        data = new(a, (iz)size, u8);
    }
    for (i64 off = 0; ok && off<size;) {
        u32 n = 0;
        u32 want = (u32)min(size - off, (i64)1<<30);
        ok = ReadFile(f, data+off, want, &n, 0) && n;
        off += n;
    }
    CloseHandle(f);
    if (ok) {
        *out = (Str){data, (iz)size};
    }
    return ok;
}

static FileStat win_stat(c16 const *path) {
    FileStat r = {0};
    FileAttributeData d = {0};
    if (GetFileAttributesExW(path, 0, &d)) {
        r.ok      = 1;
        r.written = (u64)d.written[1]<<32 | d.written[0];
        r.size    = d.sizelow;
    }
    return r;
}

static b32 samestat(FileStat a, FileStat b) {
    return a.ok==b.ok && a.written==b.written && a.size==b.size;
}

// Polling rather than change notifications: a stat every 250 ms is free,
// needs no thread, and a change must read the same twice in a row before it
// is reported, which debounces editors that save in several steps.
static void watch_tick(void) {
    FileStat now = win_stat(g.watchpath);
    if (!now.ok || samestat(now, g.watchlast)) {
        g.watchpending = 0;     // replaced mid-save, or nothing new
    } else if (g.watchpending && samestat(now, g.watchpend)) {
        g.watchlast    = now;
        g.watchpending = 0;
        app_file_changed();
    } else {
        g.watchpend    = now;
        g.watchpending = 1;
    }
}

static void os_watch_file(Str path) {
    KillTimer(g.hwnd, WATCH_TIMER);
    g.watchpending = 0;
    g.watchpath    = 0;
    if (path.len) {
        // Kept in the permanent platform scratch: paths are short and a
        // few watches over a run cost nothing.
        static c16 buf[1024];
        iz n = min(path.len, countof(buf) - 1);
        n = toutf16(buf, (Str){path.data, n});
        buf[n] = 0;
        g.watchpath = buf;
        g.watchlast = win_stat(buf);
        SetTimer(g.hwnd, WATCH_TIMER, WATCH_MS, 0);
    }
}

static b32 os_open_url(Str url) {
    b32 ok = ci_prefix(url, S("http://")).len || ci_prefix(url, S("https://")).len ||
             ci_prefix(url, S("mailto:")).len;
    for (iz i = 0; ok && i<url.len; i++) {
        ok = url.data[i] > ' ' && url.data[i] != 0x7f;
    }
    if (!ok) {
        return 0;
    }
    Arena t = g.tmp;
    return (iz)ShellExecuteW(0, L"open", win_towide(&t, url), 0, 0, SW_SHOWNORMAL) > 32;
}

static b32 os_open_dialog(Arena *a, Str *path) {
    Arena t = g.tmp;
    enum { CAP = 32768 };
    c16 *buf = new(&t, CAP, c16);
    OpenFileName ofn = {0};
    ofn.size      = sizeof(ofn);
    ofn.owner     = g.hwnd;
    ofn.filter    = L"Markdown\0*.md;*.markdown;*.mdown;*.mkd\0All files\0*.*\0";
    ofn.filterindex = 1;
    ofn.file      = buf;
    ofn.maxfile   = CAP;
    ofn.flags     = OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return 0;
    }
    iz len = 0;
    for (; len<CAP && buf[len]; len++) {}
    *path = fromutf16(a, buf, len);
    return 1;
}

static void os_clipboard_set(Str utf8) {
    iz   units = utf8.len + 1;      // at most one unit per byte
    uptr mem   = GlobalAlloc(GMEM_MOVEABLE, (uz)units*sizeof(c16));
    if (!mem) {
        return;
    }
    c16 *w = GlobalLock(mem);
    if (!w) {
        return;
    }
    w[toutf16(w, utf8)] = 0;
    GlobalUnlock(mem);
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);     // the system owns mem now
        CloseClipboard();
    }
}


// Window procedure

static f32 todip(i32 px) {
    return (f32)px * 96.0f / (f32)g.dpi;
}

static b32 win_keydown(i32 vk) {
    return GetKeyState(vk) < 0;
}

static u32 currentmods(void) {
    return (win_keydown(VK_SHIFT) ? MOD_SHIFT : 0) |
           (win_keydown(VK_CONTROL) ? MOD_PRIMARY : 0) |
           (win_keydown(VK_MENU) ? MOD_ALT : 0);
}

static Key translatekey(uptr vk) {
    if ((vk>='A' && vk<='Z') || (vk>='0' && vk<='9')) {
        return (Key)vk;
    }
    if (vk>=VK_F1 && vk<=VK_F12) {
        return (Key)(KEY_F1 + (vk - VK_F1));
    }
    switch (vk) {
    case VK_SPACE:      return KEY_SPACE;
    case VK_UP:         return KEY_UP;
    case VK_DOWN:       return KEY_DOWN;
    case VK_LEFT:       return KEY_LEFT;
    case VK_RIGHT:      return KEY_RIGHT;
    case VK_PRIOR:      return KEY_PGUP;
    case VK_NEXT:       return KEY_PGDN;
    case VK_HOME:       return KEY_HOME;
    case VK_END:        return KEY_END;
    case VK_INSERT:     return KEY_INSERT;
    case VK_DELETE:     return KEY_DELETE;
    case VK_BACK:       return KEY_BACKSPACE;
    case VK_TAB:        return KEY_TAB;
    case VK_RETURN:     return KEY_ENTER;
    case VK_ESCAPE:     return KEY_ESCAPE;
    case VK_OEM_PLUS:
    case VK_ADD:        return KEY_PLUS;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:   return KEY_MINUS;
    case VK_OEM_2:      return KEY_SLASH;
    }
    return KEY_NONE;
}

static Point mousepos(iptr lparam) {
    return (Point){todip((i16)(lparam & 0xffff)), todip((i16)((lparam >> 16) & 0xffff))};
}

#ifdef BENCHMARK
static struct {
    b32 on;
    b32 done;
    i64 freq, start, init;
    i64 paint;              // time in the app's paint, the last frame
    Str name;
} bench;

static i64 bench_now(void) {
    i64 t = 0;
    QueryPerformanceCounter(&t);
    return t;
}

static i64 bench_us(i64 from, i64 to) {
    return (to - from)*1000000/bench.freq;
}
#endif

static void draw_frame(void) {
#ifdef BENCHMARK
    i64 t = bench_now();
#endif
    app_paint(g.scratch, os_window_size());
#ifdef BENCHMARK
    bench.paint = bench_now() - t;
#endif
}

#ifdef BENCHMARK

// After the first paint: report it, then page down through the document and
// report the average and worst paint.
static void bench_report(i64 first) {
    bench.done = 1;
    // A GUI process has no stdout to speak of, so the report goes to a file.
    uptr    f = CreateFileW(L"md-bench.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, 0);
    static u8 buf[4096];
    Output out = {buf, 0, 0, sizeof(buf), f, 0, 0};
    Output *o  = &out;
    print(o, S("bench "));
    print(o, bench.name);
    print(o, S("\n  init        "));
    printi64(o, bench_us(bench.start, bench.init));
    print(o, S(" us\n  first paint, app only "));
    printi64(o, bench_us(0, bench.paint));
    print(o, S(" us\n  first paint, with the window "));
    printi64(o, bench_us(bench.init, first));
    print(o, S(" us\n  to first paint "));
    printi64(o, bench_us(bench.start, first));
    print(o, S(" us\n"));
    // Page down through the document. Presenting waits for the display, so
    // only the app's own time is counted.
    i64 total = 0;
    i64 worst = 0;
    i32 n     = 0;
    for (; n < 200; n++) {
        app_key(KEY_PGDN, 0);
        gfx_paint(draw_frame);
        i64 us = bench_us(0, bench.paint);
        total += us;
        worst  = max(worst, us);
    }
    print(o, S("  scrolled paint, "));
    printi64(o, n);
    print(o, S(" pages: avg "));
    printi64(o, total/n);
    print(o, S(" us, worst "));
    printi64(o, worst);
    print(o, S(" us (app_paint only)\n"));
    flush(o);
    CloseHandle(f);
    os_quit();
}
#endif

static void win_paint(void) {
    WinRect rc = {0};
    GetClientRect(g.hwnd, &rc);
    u32 pw = (u32)max(rc.right - rc.left, (i32)1);
    u32 ph = (u32)max(rc.bottom - rc.top, (i32)1);
    if (!g.ready || !gfx_target(g.hwnd, pw, ph, (f32)g.dpi)) {
        return;
    }
    if (gfx_paint(draw_frame) == D2DERR_RECREATE_TARGET) {
        os_invalidate();    // paint again on a new target
    }
#ifdef BENCHMARK
    if (bench.on && !bench.done) {
        bench_report(bench_now());
    }
#endif
}

static void win_resized(void) {
    WinRect rc = {0};
    GetClientRect(g.hwnd, &rc);
    gfx_resize((u32)max(rc.right - rc.left, (i32)1), (u32)max(rc.bottom - rc.top, (i32)1),
               (f32)g.dpi);
    if (g.ready) {
        app_resize(os_window_size());
    }
    os_invalidate();
}

static b32 isimmersive(iptr lparam) {
    c16 const *s = (c16 const *)lparam;
    c16 const *t = L"ImmersiveColorSet";
    if (!s) {
        return 0;
    }
    for (; *t && *s==*t; s++, t++) {}
    return !*t && !*s;
}

static iptr wndproc(uptr hwnd, u32 msg, uptr wp, iptr lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PaintStruct ps;
        BeginPaint(hwnd, &ps);
        win_paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            win_resized();
        }
        return 0;

    case WM_DPICHANGED: {
        g.dpi = LOWORD(wp);
        WinRect *r = (WinRect *)lp;
        SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER|SWP_NOACTIVATE);
        win_resized();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MinMaxInfo *mm = (MinMaxInfo *)lp;
        mm->mintrack = (WinPoint){(i32)(320*os_scale()), (i32)(240*os_scale())};
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(g.cursors[g.cursor]);
            return 1;
        }
        break;

    case WM_SETTINGCHANGE:
        if (isimmersive(lp)) {
            b32 dark = query_dark();
            if (dark != g.dark) {
                g.dark = dark;
                apply_title_theme();
                if (g.ready) {
                    app_theme_changed();
                }
                os_invalidate();
            }
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        Key key = translatekey(wp);
        if (msg==WM_SYSKEYDOWN && (!key || key==KEY_SPACE || key==KEY_F4)) {
            break;      // Alt+F4, the window menu and so on
        }
        if (key && g.ready) {
            app_key(key, currentmods());
        }
        if (msg == WM_SYSKEYDOWN) {
            return 0;
        }
        break;          // still translate to WM_CHAR
    }

    case WM_CHAR: {
        i32 rune = (i32)wp;
        if (rune>=0xd800 && rune<=0xdbff) {
            g.highsurrogate = (c16)rune;
            return 0;
        }
        if (rune>=0xdc00 && rune<=0xdfff) {
            if (!g.highsurrogate) {
                return 0;
            }
            c16 pair[2] = {g.highsurrogate, (c16)rune};
            iz  i = 0;
            rune = utf16next(pair, 2, &i);
        }
        g.highsurrogate = 0;
        if (rune>=0x20 && rune!=0x7f && g.ready) {
            app_char(rune);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        if (!g.tracking) {
            TrackMouse tm = {sizeof(tm), TME_LEAVE, hwnd, 0};
            g.tracking = TrackMouseEvent(&tm);
        }
        if (g.ready) {
            app_mouse_move(mousepos(lp), currentmods());
        }
        return 0;

    case WM_MOUSELEAVE:
        g.tracking = 0;
        if (g.ready) {
            app_mouse_leave();
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        if (g.ready) {
            Button b = msg==WM_LBUTTONDOWN ? BUTTON_LEFT :
                       msg==WM_RBUTTONDOWN ? BUTTON_RIGHT : BUTTON_MIDDLE;
            app_mouse_down(mousepos(lp), b, currentmods());
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        ReleaseCapture();
        if (g.ready) {
            Button b = msg==WM_LBUTTONUP ? BUTTON_LEFT :
                       msg==WM_RBUTTONUP ? BUTTON_RIGHT : BUTTON_MIDDLE;
            app_mouse_up(mousepos(lp), b, currentmods());
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        SetCapture(hwnd);
        if (g.ready) {
            app_double_click(mousepos(lp), currentmods());
        }
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        WinPoint pt = {(i16)(lp & 0xffff), (i16)((lp >> 16) & 0xffff)};
        ScreenToClient(hwnd, &pt);      // wheel messages carry screen coordinates
        f32 notches = (f32)(i16)HIWORD(wp) / 120.0f;
        if (g.ready) {
            Point p = {todip(pt.x), todip(pt.y)};
            if (msg == WM_MOUSEWHEEL) {
                app_wheel(p, 0, notches, currentmods());
            } else {
                app_wheel(p, notches, 0, currentmods());
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == WATCH_TIMER) {
            watch_tick();
        } else if (g.ready) {
            app_timer((i32)wp);
        }
        return 0;

    case WM_DROPFILES: {
        c16 *buf  = 0;
        Arena t   = g.tmp;
        u32 count = DragQueryFileW(wp, 0, 0, 0);
        if (count) {
            buf = new(&t, count+1, c16);
            DragQueryFileW(wp, 0, buf, count+1);
            if (g.ready) {
                app_file_dropped(fromutf16(&t, buf, count));
            }
        }
        DragFinish(wp);
        return 0;
    }

    case WM_CLOSE:
        if (g.ready) {
            app_close();
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// Command line

typedef struct {
    Str *argv;
    i32  argc;
} Args;

// Split the command line on whitespace, honoring double quotes (but not
// CommandLineToArgvW's backslash rules). argv[0] is the program.
static Args getargs(Arena *perm) {
    c16 *cmd = GetCommandLineW();
    iz   len = 0;
    for (; cmd[len]; len++) {}
    Str  s = fromutf16(perm, cmd, len);

    Args r = {0};
    r.argv = new(perm, s.len/2 + 1, Str);
    for (iz i = 0;;) {
        for (; i<s.len && (at(s, i)==' ' || at(s, i)=='\t'); i++) {}
        if (i == s.len) {
            break;
        }
        b32 quoted = 0;
        u8 *beg    = s.data + i;
        u8 *out    = beg;  // unquote in place
        for (; i < s.len; i++) {
            u8 c = at(s, i);
            if (c == '"') {
                quoted = !quoted;
            } else if (!quoted && (c==' ' || c=='\t')) {
                break;
            } else {
                *out++ = c;
            }
        }
        r.argv[r.argc++] = span(beg, out);
    }
    return r;
}


// Startup

static void win_fatal(c16 const *msg) {
    MessageBoxW(0, msg, L"md", MB_OK|MB_ICONERROR);
    ExitProcess(1);
}

static void *findproc(c16 const *dll, char const *name) {
    uptr m = GetModuleHandleW(dll);
    return m ? GetProcAddress(m, name) : 0;
}

#ifdef DEBUG
enum { PERM_SIZE = (iz)192<<20, SCRATCH_SIZE = (iz)16<<20, TMP_SIZE = (iz)8<<20 };
#else
enum { PERM_SIZE = (iz)192<<20, SCRATCH_SIZE = (iz)16<<20, TMP_SIZE = (iz)1<<20 };
#endif
enum { TOTAL_SIZE = PERM_SIZE + SCRATCH_SIZE + TMP_SIZE };

static i32 run(Arena all) {
    Arena perm = carve(&all, PERM_SIZE);
    g.scratch  = carve(&all, SCRATCH_SIZE);
    g.tmp      = carve(&all, TMP_SIZE);

    Args args = getargs(&perm);
    Str  path = {0};
    for (i32 i = 1; i < args.argc; i++) {
#ifdef DEBUG
        // Force a theme for testing without touching the system setting.
        if (equals(args.argv[i], S("--dark"))) {
            g.darkforce = 1;
            continue;
        } else if (equals(args.argv[i], S("--light"))) {
            g.darkforce = 2;
            continue;
        } else if (equals(args.argv[i], S("--zoom")) && i+1 < args.argc) {
            parseint(args.argv[++i], &g.zoomsteps);     // zoom in this many steps
            continue;
        } else if (equals(args.argv[i], S("--pgdn")) && i+1 < args.argc) {
            parseint(args.argv[++i], &g.pagedowns);     // then page down this many times
            continue;
        }
#endif
#ifdef BENCHMARK
        if (equals(args.argv[i], S("--bench"))) {
            bench.on = 1;
            continue;
        }
#endif
        if (!path.len) {
            path = args.argv[i];
        }
    }

    // Per-monitor DPI v2 needs Windows 10 1703; older systems get a blurry
    // but working window.
    g.setdpiaware = (b32 (*)(uptr))findproc(L"user32.dll", "SetProcessDpiAwarenessContext");
    g.getdpi      = (u32 (*)(uptr))findproc(L"user32.dll", "GetDpiForWindow");
    g.adjustrect  = (b32 (*)(WinRect *, u32, b32, u32, u32))
                        findproc(L"user32.dll", "AdjustWindowRectExForDpi");
    if (g.setdpiaware) {
        g.setdpiaware(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    g.dpi  = 96;
    g.dark = query_dark();

    if (!gfx_init()) {
        win_fatal(L"Direct2D and DirectWrite are not available.");
    }

    g.cursors[CURSOR_ARROW] = LoadCursorW(0, MAKEINTRESOURCEW(IDC_ARROW));
    g.cursors[CURSOR_HAND]  = LoadCursorW(0, MAKEINTRESOURCEW(IDC_HAND));
    g.cursors[CURSOR_IBEAM] = LoadCursorW(0, MAKEINTRESOURCEW(IDC_IBEAM));

    uptr inst = GetModuleHandleW(0);
    WndClassEx wc = {0};
    wc.size      = sizeof(wc);
    wc.style     = CS_DBLCLKS;
    wc.proc      = wndproc;
    wc.instance  = inst;
    wc.cursor    = g.cursors[CURSOR_ARROW];
    wc.classname = L"md";
    if (!RegisterClassExW(&wc)) {
        win_fatal(L"Could not register the window class.");
    }

    g.hwnd = CreateWindowExW(0, L"md", L"md", WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             0, 0, inst, 0);
    if (!g.hwnd) {
        win_fatal(L"Could not create the window.");
    }
    if (g.getdpi) {
        g.dpi = g.getdpi(g.hwnd);
    }
    // Initial size: 900x700 DIPs of client area, on whichever monitor the
    // system chose.
    {
        f32     s  = os_scale();
        WinRect rc = {0, 0, (i32)(900*s), (i32)(700*s)};
        if (g.adjustrect) {
            g.adjustrect(&rc, WS_OVERLAPPEDWINDOW, 0, 0, g.dpi);
        } else {
            AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, 0, 0);
        }
        SetWindowPos(g.hwnd, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    apply_title_theme();
    DragAcceptFiles(g.hwnd, 1);

#ifdef BENCHMARK
    QueryPerformanceFrequency(&bench.freq);
    bench.start = bench_now();
    bench.name  = path;
#endif
    app_init(&perm, path, os_window_size());
#ifdef BENCHMARK
    bench.init = bench_now();
#endif
#ifdef DEBUG
    for (i32 i = 0; i < g.zoomsteps; i++) {
        app_key(KEY_PLUS, MOD_PRIMARY);
    }
    for (i32 i = 0; i < g.pagedowns; i++) {
        app_key(KEY_PGDN, 0);
    }
#endif
    // Show the window cloaked, paint the first frame, then uncloak, so it
    // appears with its content instead of a white flash while Direct2D
    // starts up. Attribute 13 is DWMWA_CLOAK.
    g.ready = 1;
    b32 cloak = 1;
    DwmSetWindowAttribute(g.hwnd, 13, &cloak, sizeof(cloak));
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    cloak = 0;
    DwmSetWindowAttribute(g.hwnd, 13, &cloak, sizeof(cloak));

    Msg m = {0};
    while (GetMessageW(&m, 0, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (i32)m.wparam;
}

void WinMainCRTStartup(void) {
    iz    cap = TOTAL_SIZE;
    u8   *mem = VirtualAlloc(0, (uz)cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem+cap}));
}
