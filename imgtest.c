// Tests for image decoding (image.c): a separate unity build with the debug
// checks on (UBSan traps, guard pages behind every allocation). There is no
// window and no Direct2D; the pictures are the files in test/img, which
// test/img/gen.py wrote.
//   $ make img-test.exe
//   $ img-test.exe [-s SEED] [-n ITERS] [-i START] [-d DIR] [-t WHICH] [-v]
//
// Four kinds of test:
//   files    every format and every GIF disposal rule: size, frame count,
//            delays, and exact pixels of every frame, in order and after
//            jumping backward; damaged and oversize files must be refused
//   many     GIFs of 10000 and 10001 frames
//   random   the files damaged at random (flipped bytes, cuts, splices):
//            no crash, sane results, and the same pixels for a frame
//            whichever way it was reached
//   leaks    after each of the above, nothing is left allocated
//
// Iteration i of seed s is self-contained: "-t random -s S -i I -n 1"
// replays exactly it, and a failing input is saved as img-fail.bin.
#include "base.c"
#include "platform.h"
#include "win32.c"
#include "image.c"

#ifndef DEBUG
enum { PAGE = 4096 };
#endif


// Platform hooks

static b32 os_write(Output *o) {
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

static void os_protect(void *p, iz size, b32 readwrite) {
    u32 old = 0;
    b32 ok  = VirtualProtect(p, (uz)size, readwrite ? PAGE_READWRITE : PAGE_NOACCESS, &old);
    assert(ok);
}

typedef struct ExceptionRecord {
    u32                     code;
    u32                     flags;
    struct ExceptionRecord *next;
    void                   *addr;
    u32                     nparams;
    u64                     info[15];
} ExceptionRecord;

typedef struct {
    ExceptionRecord *rec;
    void            *context;
} ExceptionPointers;

W32(void *) AddVectoredExceptionHandler(u32, i32 (*)(ExceptionPointers *));
W32(void *) CreateThread(void *, uz, u32 (*)(void *), void *, u32, u32 *);
W32(void)   Sleep(u32);


// Random numbers: 64-bit LCG with an xorshift-multiply output permutation.

static u64 rand64(u64 *s) {
    *s = *s*0x3243f6a8885a308dULL + 1;
    u64 r = *s;
    r ^= r >> 32;
    r *= 0xdaba0b6eb09322e3ULL;
    r ^= r >> 32;
    r *= 0xdaba0b6eb09322e3ULL;
    r ^= r >> 32;
    return r;
}

// Uniform-ish in [0, n)
static u64 rnd(u64 *s, u64 n) {
    return rand64(s) % n;
}

static b32 chance(u64 *s, u64 percent) {
    return rnd(s, 100) < percent;
}


// Reporting

typedef struct {
    u64          seed;
    i64          iter;      // -1 outside the random tests
    const char  *phase;
    Str          input;     // saved on failure
    Output      *out;
    b32          dead;
    volatile u64 progress;  // tick of the last sign of life
} Ctx;
static Ctx ctx;

static Str cstr(const char *s) {
    iz n = 0;
    for (; s[n]; n++) {}
    return (Str){(u8 *)s, n};
}

static void printhex(Output *o, u64 v, i32 digits) {
    u8  t[16];
    i32 n = 0;
    do {
        t[n++] = (u8)"0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    for (i32 i = n; i < digits; i++) {
        printc(o, '0');
    }
    while (n) {
        printc(o, t[--n]);
    }
}

// The failing input goes to a file, since it is binary.
static void save_input(void) {
    if (!ctx.input.len) {
        return;
    }
    uptr h = CreateFileW(L"img-fail.bin", GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    u32 n = 0;
    WriteFile(h, ctx.input.data, (u32)ctx.input.len, &n, 0);
    CloseHandle(h);
    print(ctx.out, S("  input ("));
    printi64(ctx.out, ctx.input.len);
    print(ctx.out, S(" bytes) saved as img-fail.bin\n"));
}

static _Noreturn void context(void) {
    Output *o = ctx.out;
    print(o, S("  seed:      0x"));
    printhex(o, ctx.seed, 1);
    print(o, S("\n  iteration: "));
    printi64(o, ctx.iter);
    print(o, S("\n  phase:     "));
    print(o, cstr(ctx.phase ? ctx.phase : "?"));
    if (ctx.iter >= 0) {
        print(o, S("\n  reproduce: img-test.exe -t random -s 0x"));
        printhex(o, ctx.seed, 1);
        print(o, S(" -i "));
        printi64(o, ctx.iter);
        print(o, S(" -n 1   (gdb: gdb --args img-test.exe ...)"));
    }
    print(o, S("\n"));
    save_input();
    flush(o);
    ExitProcess(1);
}

static _Noreturn void fail(const char *check, const char *what, i64 a, i64 b) {
    Output *o = ctx.out;
    if (ctx.dead) {
        ExitProcess(1);  // failed while reporting
    }
    ctx.dead = 1;
    print(o, S("FAIL: "));
    print(o, cstr(check));
    print(o, S(": "));
    print(o, cstr(what));
    print(o, S(" (a="));
    printi64(o, a);
    print(o, S(" b="));
    printi64(o, b);
    print(o, S(")\n"));
    context();
}

// Is the address in this program's own code?
static b32 in_image(void *addr) {
    u8 *base = (u8 *)GetModuleHandleW(0);
    u32 nt   = *(u32 *)(base + 0x3c);
    u32 size = *(u32 *)(base + nt + 0x50);      // OptionalHeader.SizeOfImage
    return (u8 *)addr >= base && (u8 *)addr < base + size;
}

// A fault (guard page, UBSan trap, assert) gets the same report as a failed
// check, so the seed and the input are never lost.
static i32 onfault(ExceptionPointers *ep) {
    ExceptionRecord *e = ep->rec;
    // WIC raises and handles exceptions of its own, so only a fault in this
    // program, or one of the kinds that nothing handles, is a failure.
    b32 ours = in_image(e->addr);
    b32 hard = e->code==0xc0000005u || e->code==0xc000001du || e->code==0xc00000fdu;
    if (!(ours || hard) || (e->code & 0xc0000000u) != 0xc0000000u || ctx.dead) {
        return 0;
    }
    ctx.dead = 1;
    Output *o = ctx.out;
    print(o, S("FAULT: "));
    if (e->code == 0xc0000005u) {
        print(o, e->info[0] ? S("write") : S("read"));
        print(o, S(" access violation at 0x"));
        printhex(o, e->info[1], 1);
        print(o, S(" (guard page overrun or stale pointer?)"));
    } else if (e->code == 0xc000001du) {
        print(o, S("illegal instruction (UBSan trap or assert) "));
    } else if (e->code == 0xc00000fdu) {
        print(o, S("stack overflow"));
    } else {
        print(o, S("exception 0x"));
        printhex(o, e->code, 8);
    }
    print(o, S(", instruction at 0x"));
    printhex(o, (u64)(uptr)e->addr, 1);
    print(o, S("\n"));
    context();
}

// If a test stops making progress it is stuck in a loop: say where.
static u32 watchdog(void *arg) {
    (void)arg;
    for (;;) {
        Sleep(500);
        if (GetTickCount64() - ctx.progress > 60000) {
            ctx.dead = 1;
            print(ctx.out, S("HANG: no progress for 60 seconds\n"));
            context();
        }
    }
}

static void alive(void) {
    ctx.progress = GetTickCount64();
}

static i64 now_us(void) {
    static i64 freq;
    i64 t = 0;
    if (!freq) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&t);
    return t*1000000/freq;
}

static void printpx(Output *o, u32 p) {
    print(o, S("0x"));
    printhex(o, p, 8);
}


// Reading files

typedef struct {
    Arena   files;      // file contents, reset by the user
    Str     dir;        // the test directory, "test"
    Output *out;
    b32     verbose;
    i32     checks;
    i32     decoded;    // random inputs that still decoded...
    i32     animated;   // ...and had more than one frame
    b32     webp;       // the system decodes WebP
} Rig;

static Str readpath(Arena *a, Str name) {
    c16 path[512];
    if (name.len >= countof(path)) {
        return (Str){0};
    }
    iz n    = toutf16(path, name);
    path[n] = 0;
    uptr h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return (Str){0};
    }
    i64 size = 0;
    GetFileSizeEx(h, &size);
    u8 *data = new(a, size, u8);
    iz  off  = 0;
    while (off < size) {
        u32 got = 0;
        if (!ReadFile(h, data+off, (u32)min(size - off, (i64)1<<30), &got, 0) || !got) {
            break;
        }
        off += got;
    }
    CloseHandle(h);
    return (Str){data, off};
}

// test/img/NAME. A missing file is a failure, not an empty input, except for
// the one that is meant to be empty.
static Str readimg(Rig *r, Arena *a, const char *name) {
    Str  n = cstr(name);
    iz   d = r->dir.len;
    u8  *p = new(a, d + 5 + n.len, u8);
    memcpy(p, r->dir.data, (uz)d);
    memcpy(p + d, "/img/", 5);
    memcpy(p + d + 5, n.data, (uz)n.len);
    Str s = readpath(a, (Str){p, d + 5 + n.len});
    if (!s.len && !equals(n, S("empty.bin"))) {
        fail("file", name, 0, 0);
    }
    return s;
}


// Pixels. A pixel is premultiplied BGRA read as a u32: 0xaarrggbb.

enum {
    CLEAR   = 0x00000000,
    BLACK   = 0xff000000,
    WHITE   = 0xffffffff,
    RED     = 0xffff0000,
    GREEN   = 0xff00ff00,
    BLUE    = 0xff0000ff,
    YELLOW  = 0xffffff00,
    CYAN    = 0xff00ffff,
    MAGENTA = 0xffff00ff,
    GRAY    = 0xff808080,
};
#define ARGB(a, r, g, b) ((u32)(a)<<24 | (u32)(r)<<16 | (u32)(g)<<8 | (u32)(b))

static i32 chan_diff(u32 a, u32 b) {
    i32 worst = 0;
    for (i32 sh = 0; sh < 32; sh += 8) {
        i32 d = (i32)((a>>sh) & 255) - (i32)((b>>sh) & 255);
        worst = max(worst, d<0 ? -d : d);
    }
    return worst;
}

static u32 pixel_at(Image *im, i32 frame, i32 x, i32 y) {
    const u32 *p = image_pixels(im, frame);
    Size sz = os_image_size(im);
    assert(x>=0 && y>=0 && (f32)x<sz.w && (f32)y<sz.h);
    return p[(iz)y*(i32)sz.w + x];
}

static _Noreturn void fail_pixel(Rig *r, i32 frame, i32 x, i32 y, u32 got, u32 want) {
    Output *o = r->out;
    ctx.dead = 1;
    print(o, S("FAIL: pixel: "));
    print(o, cstr(ctx.phase));
    print(o, S(" frame "));
    printi64(o, frame);
    print(o, S(" at ("));
    printi64(o, x);
    print(o, S(", "));
    printi64(o, y);
    print(o, S("): got "));
    printpx(o, got);
    print(o, S(" want "));
    printpx(o, want);
    print(o, S("\n"));
    context();
}

typedef struct {
    i32 frame, x, y;
    u32 want;
} Px;

typedef struct {
    const char *file;
    i32  w, h, frames;
    i32  tol;                   // allowed difference per channel
    i32  delays[12];            // for animations, 0 for the rest
    Px   px[40];                // ends at the first entry with x < 0
} Case;

#define END {0, -1, 0, 0}

// Expected results, worked out from how each file was made (gen.py), not
// from what the decoder says. The GIFs' colors are palette entries: 1 red,
// 2 green, 3 blue.
static const Case cases[] = {
    {"still.gif", 4, 3, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,BLUE}, {0,3,0,WHITE},
        {0,0,1,YELLOW}, {0,1,1,CYAN}, {0,2,1,MAGENTA}, {0,3,1,BLACK},
        {0,0,2,RED}, {0,3,2,GREEN}, END}},

    // Later frames draw over earlier ones, which stay
    {"keep.gif", 8, 8, 3, 0, {50, 100, 200}, {
        {0,0,0,RED}, {0,3,3,RED}, {0,7,7,RED},
        {1,3,3,GREEN}, {1,5,5,GREEN}, {1,6,6,RED}, {1,0,0,RED},
        {2,5,5,BLUE}, {2,7,7,BLUE}, {2,3,3,GREEN}, {2,3,4,GREEN}, {2,0,0,RED}, {2,4,3,GREEN},
        END}},

    // Disposal 2: the frame's own rectangle goes clear before the next
    {"background.gif", 8, 8, 3, 0, {100, 100, 100}, {
        {0,0,0,RED}, {0,7,7,RED},
        {1,3,3,GREEN}, {1,0,0,CLEAR}, {1,7,7,CLEAR}, {1,2,2,GREEN}, {1,5,5,GREEN}, {1,6,6,CLEAR},
        {2,0,0,BLUE}, {2,1,1,BLUE}, {2,2,2,CLEAR}, {2,3,3,CLEAR}, {2,7,7,CLEAR},
        END}},

    // Disposal 3: the canvas as it was before the frame comes back
    {"previous.gif", 8, 8, 3, 0, {100, 100, 100}, {
        {0,0,0,RED}, {0,3,3,RED},
        {1,3,3,GREEN}, {1,0,0,RED}, {1,2,2,GREEN},
        {2,3,3,RED}, {2,2,2,RED}, {2,5,1,BLUE}, {2,4,0,BLUE}, {2,0,0,RED}, {2,7,7,RED},
        END}},
    {"previous-first.gif", 8, 8, 2, 0, {100, 100}, {
        {0,1,1,RED}, {0,5,5,CLEAR},
        {1,1,1,CLEAR}, {1,0,0,CLEAR}, {1,5,5,GREEN}, {1,7,7,GREEN}, {1,3,3,CLEAR},
        END}},

    // The transparent index lets the canvas show through
    {"transparent.gif", 8, 8, 2, 0, {100, 100}, {
        {0,0,0,RED}, {0,7,7,RED},
        {1,0,0,RED}, {1,1,0,GREEN}, {1,0,1,GREEN}, {1,1,1,RED}, {1,3,3,RED}, {1,3,2,GREEN},
        {1,5,5,RED}, {1,7,7,RED},
        END}},

    // Frames smaller than the screen, and a transparent pixel over nothing
    {"offsets.gif", 8, 8, 2, 0, {100, 100}, {
        {0,0,0,CLEAR}, {0,1,1,RED}, {0,2,2,RED}, {0,3,3,CLEAR}, {0,7,7,CLEAR},
        {1,5,5,CLEAR}, {1,6,5,BLUE}, {1,5,6,BLUE}, {1,6,6,BLUE}, {1,1,1,RED}, {1,7,7,CLEAR},
        END}},

    // The second frame is entirely off the screen, the first partly
    {"outside.gif", 4, 4, 3, 0, {100, 100, 100}, {
        {0,3,3,RED}, {0,2,2,RED}, {0,1,1,CLEAR}, {0,0,3,CLEAR},
        {1,3,3,RED}, {1,2,2,RED}, {1,1,1,CLEAR},
        {2,0,0,BLUE}, {2,1,1,BLUE}, {2,2,2,RED}, {2,3,3,RED}, {2,2,1,CLEAR},
        END}},

    // 0 and 1 hundredths show as 100 ms, no delay at all as well
    {"delays.gif", 2, 2, 6, 0, {100, 100, 20, 50, 1000, 100}, {
        {0,0,0,RED}, {1,1,1,GREEN}, {2,0,1,BLUE}, {3,1,0,WHITE}, {4,0,0,YELLOW},
        {5,1,1,CYAN}, {2,0,0,BLUE},
        END}},

    // The stored rows are 0, 4, 2, 6, 1, 3, 5, 7 of the picture
    {"interlaced.gif", 2, 8, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,RED}, {0,0,1,GREEN}, {0,1,1,GREEN}, {0,0,2,BLUE},
        {0,0,3,WHITE}, {0,1,3,WHITE}, {0,0,4,YELLOW}, {0,0,5,CYAN}, {0,1,5,CYAN},
        {0,0,6,MAGENTA}, {0,1,7,RED}, END}},

    // A logical screen of 0x0: the first frame (at 1,1, 2x2) makes it 3x3
    {"lsd0.gif", 3, 3, 1, 0, {0}, {
        {0,0,0,CLEAR}, {0,1,1,RED}, {0,2,2,RED}, {0,2,0,CLEAR}, END}},

    // Straight alpha in the file, premultiplied here
    {"alpha.png", 4, 2, 1, 1, {0}, {
        {0,0,0,RED}, {0,1,0,ARGB(128,0,128,0)}, {0,2,0,CLEAR}, {0,3,0,WHITE},
        {0,0,1,ARGB(128,0,0,128)}, {0,1,1,ARGB(64,64,0,0)}, {0,2,1,BLACK},
        {0,3,1,ARGB(255,10,20,30)}, END}},
    {"rgb.png", 3, 3, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,BLUE},
        {0,0,1,YELLOW}, {0,1,1,CYAN}, {0,2,1,MAGENTA},
        {0,0,2,BLACK}, {0,1,2,GRAY}, {0,2,2,WHITE}, END}},
    {"palette.png", 4, 1, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,CLEAR}, {0,3,0,YELLOW}, END}},
    {"gray.png", 2, 2, 1, 1, {0}, {
        {0,0,0,BLACK}, {0,1,0,WHITE}, {0,0,1,GRAY}, {0,1,1,0xff404040}, END}},
    {"rgb16.png", 1, 1, 1, 1, {0}, {{0,0,0,0xffff8000}, END}},
    {"bmp24.bmp", 3, 2, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,BLUE},
        {0,0,1,WHITE}, {0,1,1,BLACK}, {0,2,1,YELLOW}, END}},
    {"bmp8.bmp", 4, 2, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,BLUE}, {0,3,0,WHITE},
        {0,0,1,WHITE}, {0,1,1,BLUE}, {0,2,1,GREEN}, {0,3,1,RED}, END}},
    {"rgb.tif", 3, 2, 1, 0, {0}, {
        {0,0,0,RED}, {0,1,0,GREEN}, {0,2,0,BLUE},
        {0,0,1,WHITE}, {0,1,1,BLACK}, {0,2,1,YELLOW}, END}},
    // The larger of the two images in the icon
    {"icon.ico", 32, 32, 1, 0, {0}, {
        {0,0,0,BLUE}, {0,31,31,BLUE}, {0,16,16,BLUE}, END}},

    // JPEG is lossy: a few steps of tolerance
    {"color.jpg", 24, 16, 1, 4, {0}, {
        {0,0,0,RED}, {0,23,0,GREEN}, {0,0,15,BLUE}, {0,23,15,YELLOW}, {0,12,8,GRAY},
        {0,7,7,RED}, {0,16,7,GREEN}, END}},
    {"odd.jpg", 10, 6, 1, 4, {0}, {
        {0,0,0,RED}, {0,7,5,RED}, {0,8,0,BLUE}, {0,9,5,BLUE}, END}},
};

// EXIF orientation 1 to 8 of a picture whose corners are red (top left),
// green (top right), blue (bottom left) and yellow (bottom right), in the
// order top left, top right, bottom left, bottom right.
static const u32 orient_corners[9][4] = {
    {RED,    GREEN,  BLUE,   YELLOW},   // 1: as stored
    {GREEN,  RED,    YELLOW, BLUE},     // 2: mirrored
    {YELLOW, BLUE,   GREEN,  RED},      // 3: half a turn
    {BLUE,   YELLOW, RED,    GREEN},    // 4: flipped
    {RED,    BLUE,   GREEN,  YELLOW},   // 5: transposed
    {BLUE,   RED,    YELLOW, GREEN},    // 6: a quarter turn clockwise
    {YELLOW, GREEN,  BLUE,   RED},      // 7: transposed the other way
    {GREEN,  YELLOW, RED,    BLUE},     // 8: a quarter turn counterclockwise
    {RED,    GREEN,  BLUE,   YELLOW},   // 9: not an orientation
};

// Files that must not decode.
static const char *const refused[] = {
    "empty.bin", "garbage.bin", "header.png", "trunc.png", "badhdr.png",
    "wide.png", "big.png", "huge.gif",
};

// Files that may or may not decode, but never crash. The valid ones are
// also what the random test damages.
static const char *const maybe[] = {
    "trunc.jpg", "trunc.gif", "corrupt.png",
};

static const char *const seeds[] = {
    "still.gif", "keep.gif", "background.gif", "previous.gif", "previous-first.gif",
    "transparent.gif", "offsets.gif", "outside.gif", "delays.gif", "interlaced.gif",
    "lsd0.gif", "mixed.gif", "alpha.png", "rgb.png", "palette.png", "gray.png",
    "rgb16.png", "bmp24.bmp", "bmp8.bmp", "rgb.tif", "icon.ico", "color.jpg",
    "orient6.jpg", "odd.jpg", "lossless.webp",
};


// Checks that hold for any image that decoded

static u64 hash_canvas(Image *im, i32 frame) {
    const u32 *p = image_pixels(im, frame);
    Size sz = os_image_size(im);
    iz   n  = (iz)sz.w * (iz)sz.h;
    u64  h  = 0xcbf29ce484222325ULL;
    for (iz i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ULL;
    }
    return h;
}

// What any decoded image promises: its size is within the limits, its
// frames have delays of at least 10 ms, every pixel is a valid premultiplied
// one, and a frame looks the same however it was reached.
static void check_invariants(Image *im, u64 *s) {
    Size sz = os_image_size(im);
    i32  w  = (i32)sz.w;
    i32  h  = (i32)sz.h;
    i32  n  = os_image_frames(im);
    if (w<1 || h<1 || w>IMG_MAX_SIDE || h>IMG_MAX_SIDE || (i64)w*h*4>IMG_MAX_BYTES) {
        fail("invariant", "size out of range", w, h);
    }
    if (n<1 || n>IMG_MAX_FRAMES) {
        fail("invariant", "frame count out of range", n, 0);
    }
    i32 look = min(n, (i32)24);
    u64 hash[24];
    for (i32 f = 0; f < look; f++) {
        i32 d = os_image_delay(im, f);
        if (n == 1 ? d != 0 : d < 10) {
            fail("invariant", "delay", f, d);
        }
        const u32 *p = image_pixels(im, f);
        for (iz i = 0; i < (iz)w*h; i++) {
            u32 a = p[i] >> 24;
            if ((p[i]>>16 & 255) > a || (p[i]>>8 & 255) > a || (p[i] & 255) > a) {
                fail("invariant", "pixel is not premultiplied", f, (i64)i);
            }
        }
        hash[f] = hash_canvas(im, f);
    }
    // Jump around: backward, forward and repeat.
    for (i32 k = 0; k < 2*look; k++) {
        i32 f = (i32)rnd(s, (u64)look);
        if (hash_canvas(im, f) != hash[f]) {
            fail("invariant", "a frame differs when reached another way", f, k);
        }
    }
    if (os_image_delay(im, -1) != 0 || os_image_delay(im, n) != 0) {
        fail("invariant", "a frame out of range has a delay", n, 0);
    }
    // Reduced for drawing small: halved sizes, still valid pixels. An
    // animation is not reduced at all.
    i32 level = 5;
    i32 rw, rh;
    const u32 *rp = image_reduced(im, &level, &rw, &rh);
    i32 ew = w;
    i32 eh = h;
    for (i32 k = 0; k < level; k++) {
        ew = (ew + 1)/2;
        eh = (eh + 1)/2;
    }
    if (level != (n > 1 ? 0 : 5) || rw != ew || rh != eh) {
        fail("invariant", "reduced size", level, (i64)rw*1000 + rh);
    }
    for (iz i = 0; i < (iz)rw*rh; i++) {
        u32 a = rp[i] >> 24;
        if ((rp[i]>>16 & 255) > a || (rp[i]>>8 & 255) > a || (rp[i] & 255) > a) {
            fail("invariant", "reduced pixel is not premultiplied", level, (i64)i);
        }
    }
}


// The known files

// Decode a file, check what it looks like, free it, check nothing leaked.
static void run_case(Rig *r, const Case *c) {
    ctx.phase = c->file;
    alive();
    i32 blocks = imgstat.blocks;
    i32 objects = imgstat.objects;
    Arena a = r->files;
    Str   bytes = readimg(r, &a, c->file);
    ctx.input = bytes;
    Image *im = os_image_decode(bytes);
    if (!im) {
        fail("decode", "returned nothing", 0, 0);
    }
    // The decoder keeps its own copy of the bytes.
    memset(bytes.data, 0xcc, (uz)bytes.len);
    Size sz = os_image_size(im);
    if ((i32)sz.w != c->w || (i32)sz.h != c->h) {
        fail("size", c->file, (i64)sz.w*1000 + (i64)sz.h, c->w*1000 + c->h);
    }
    if (os_image_frames(im) != c->frames) {
        fail("frames", c->file, os_image_frames(im), c->frames);
    }
    for (i32 f = 0; f < c->frames; f++) {
        if (os_image_delay(im, f) != c->delays[f]) {
            fail("delay", c->file, f, os_image_delay(im, f));
        }
    }
    i32 np = 0;
    while (c->px[np].x >= 0) {
        np++;
    }
    // In order of the table, then backward, then in a shuffled order. A
    // frame is reached in all the ways a player could.
    for (i32 pass = 0; pass < 3; pass++) {
        for (i32 k = 0; k < np; k++) {
            i32 i = pass == 0 ? k : pass == 1 ? np - 1 - k : (k*7 + 3) % np;
            if (pass == 2 && np%7 == 0) {
                i = (k*5 + 1) % np;
            }
            const Px *p = c->px + i;
            u32 got = pixel_at(im, p->frame, p->x, p->y);
            if (chan_diff(got, p->want) > c->tol) {
                fail_pixel(r, p->frame, p->x, p->y, got, p->want);
            }
            r->checks++;
        }
    }
    u64 s = 77;
    check_invariants(im, &s);
    image_free(im);
    if (imgstat.blocks != blocks || imgstat.objects != objects) {
        fail("leak", c->file, imgstat.blocks - blocks, imgstat.objects - objects);
    }
    ctx.input = (Str){0};
}

static void run_orientation(Rig *r) {
    static const char *const names[] = {
        "orient1.jpg", "orient2.jpg", "orient3.jpg", "orient4.jpg", "orient5.jpg",
        "orient6.jpg", "orient7.jpg", "orient8.jpg", "orient9.jpg",
    };
    for (i32 o = 1; o <= 9; o++) {
        ctx.phase = names[o-1];
        Arena a = r->files;
        Str   bytes = readimg(r, &a, names[o-1]);
        Image *im = os_image_decode(bytes);
        if (!im) {
            fail("decode", "returned nothing", o, 0);
        }
        Size sz = os_image_size(im);
        b32 turned = o>=5 && o<=8;
        if ((i32)sz.w != (turned ? 16 : 24) || (i32)sz.h != (turned ? 24 : 16)) {
            fail("size", "orientation", (i64)sz.w, (i64)sz.h);
        }
        i32 w = (i32)sz.w;
        i32 h = (i32)sz.h;
        i32 x[4] = {0, w - 1, 0, w - 1};
        i32 y[4] = {0, 0, h - 1, h - 1};
        for (i32 k = 0; k < 4; k++) {
            u32 got = pixel_at(im, 0, x[k], y[k]);
            if (chan_diff(got, orient_corners[o-1][k]) > 4) {
                fail_pixel(r, o, x[k], y[k], got, orient_corners[o-1][k]);
            }
            r->checks++;
        }
        image_free(im);
    }
}

static void run_refused(Rig *r) {
    for (iz i = 0; i < countof(refused); i++) {
        ctx.phase = refused[i];
        i32 blocks = imgstat.blocks;
        i32 objects = imgstat.objects;
        Arena a = r->files;
        Str   bytes = readimg(r, &a, refused[i]);
        Image *im = os_image_decode(bytes);
        if (im) {
            fail("refused", refused[i], 0, 0);
        }
        if (imgstat.blocks != blocks || imgstat.objects != objects) {
            fail("leak", refused[i], imgstat.blocks - blocks, imgstat.objects - objects);
        }
        r->checks++;
    }
    for (iz i = 0; i < countof(maybe); i++) {
        ctx.phase = maybe[i];
        Arena a = r->files;
        Str   bytes = readimg(r, &a, maybe[i]);
        Image *im = os_image_decode(bytes);
        if (im) {
            u64 s = 5;
            check_invariants(im, &s);
            image_free(im);
        }
        r->checks++;
    }
    // Zero bytes, one byte, a signature and nothing else
    Str tiny[] = {{(u8 *)"", 0}, {(u8 *)"G", 1}, {(u8 *)"GIF89a", 6},
                  {(u8 *)"\x89PNG\r\n\x1a\n", 8}, {(u8 *)"\xff\xd8\xff", 3}, {(u8 *)"BM", 2}};
    for (iz i = 0; i < countof(tiny); i++) {
        ctx.phase = "tiny";
        if (os_image_decode(tiny[i])) {
            fail("refused", "tiny input", i, 0);
        }
        r->checks++;
    }
}

// Pictures halved for drawing small: sizes round up, each pixel is the
// average of four (the edge ones counting twice when the side is odd).
static void run_reduced(Rig *r) {
    ctx.phase = "reduced";
    Arena a = r->files;

    Image *im = os_image_decode(readimg(r, &a, "icon.ico"));
    i32 want[] = {32, 16, 8, 4, 2, 1, 1, 1};
    for (i32 k = 0; k < 8; k++) {
        i32 level = k, w, h;
        const u32 *p = image_reduced(im, &level, &w, &h);
        if (level != k || w != want[k] || h != want[k]) {
            fail("reduced", "icon size", k, w);
        }
        for (i32 i = 0; i < w*h; i++) {
            if (p[i] != BLUE) {
                fail("reduced", "a flat picture changed", k, i);
            }
        }
        r->checks++;
    }
    image_free(im);

    // 3x3: red green blue / yellow cyan magenta / black gray white
    im = os_image_decode(readimg(r, &a, "rgb.png"));
    i32 level = 1, w, h;
    const u32 *p = image_reduced(im, &level, &w, &h);
    const u32 want1[] = {ARGB(255,128,191,64), ARGB(255,128,0,255),
                         ARGB(255,64,64,64),   WHITE};
    if (level != 1 || w != 2 || h != 2) {
        fail("reduced", "3x3 size", w, h);
    }
    for (i32 i = 0; i < 4; i++) {
        if (chan_diff(p[i], want1[i]) > 0) {
            fail_pixel(r, 1, i%2, i/2, p[i], want1[i]);
        }
        r->checks++;
    }
    level = 2;
    p = image_reduced(im, &level, &w, &h);
    if (level != 2 || w != 1 || h != 1 || p[0] != ARGB(255,144,128,160)) {
        fail_pixel(r, 2, 0, 0, p[0], ARGB(255,144,128,160));
    }
    image_free(im);

    // Alpha is averaged premultiplied: 4x2 becomes 2x1
    im = os_image_decode(readimg(r, &a, "alpha.png"));
    level = 1;
    p = image_reduced(im, &level, &w, &h);
    if (level != 1 || w != 2 || h != 1) {
        fail("reduced", "alpha size", w, h);
    }
    if (chan_diff(p[0], ARGB(144,80,32,32)) > 1) {
        fail_pixel(r, 1, 0, 0, p[0], ARGB(144,80,32,32));
    }
    image_free(im);

    // An animation stays as it is
    im = os_image_decode(readimg(r, &a, "keep.gif"));
    level = 3;
    image_reduced(im, &level, &w, &h);
    if (level != 0 || w != 8 || h != 8) {
        fail("reduced", "an animation was reduced", level, w);
    }
    image_free(im);
    r->checks += 4;
}

// WebP depends on the codec the system has. If it decodes, it must do so
// properly.
static void run_webp(Rig *r) {
    static const char *const names[] = {"lossless.webp", "lossy.webp"};
    for (iz i = 0; i < countof(names); i++) {
        ctx.phase = names[i];
        Arena a = r->files;
        Str   bytes = readimg(r, &a, names[i]);
        Image *im = os_image_decode(bytes);
        if (!im) {
            continue;
        }
        r->webp = 1;
        Size sz = os_image_size(im);
        if ((i32)sz.w != 1 || (i32)sz.h != 1 || os_image_frames(im) != 1) {
            fail("webp", "wrong size", (i64)sz.w, (i64)sz.h);
        }
        u64 s = 9;
        check_invariants(im, &s);
        image_free(im);
        r->checks++;
    }
}


// GIFs of many frames, built here: each 1x1, black, with a delay of 20 ms.

static Str many_gif(Arena *a, i32 frames) {
    static const u8 head[] = {
        'G','I','F','8','9','a', 1,0, 1,0, 0x80, 0, 0,
        0,0,0, 255,255,255,
    };
    static const u8 frame[] = {
        0x21,0xf9,0x04, 0x00, 2,0, 0, 0,                // delay 20 ms, no transparency
        0x2c, 0,0, 0,0, 1,0, 1,0, 0,                    // 1x1 at the origin
        0x02, 0x02, 0x44, 0x01, 0x00,                   // one pixel, index 0
    };
    iz len = (iz)sizeof(head) + (iz)frames*(iz)sizeof(frame) + 1;
    u8 *p  = new(a, len, u8);
    memcpy(p, head, sizeof(head));
    for (i32 i = 0; i < frames; i++) {
        memcpy(p + sizeof(head) + (iz)i*(iz)sizeof(frame), frame, sizeof(frame));
    }
    p[len-1] = 0x3b;
    return (Str){p, len};
}

static void run_many(Rig *r) {
    ctx.phase = "10001 frames";
    Arena a = r->files;
    Str   g = many_gif(&a, IMG_MAX_FRAMES + 1);
    if (os_image_decode(g)) {
        fail("many", "too many frames were accepted", 0, 0);
    }
    r->checks++;

    ctx.phase = "10000 frames";
    g = many_gif(&a, IMG_MAX_FRAMES);
    i32 blocks  = imgstat.blocks;
    i32 objects = imgstat.objects;
    i64 t0 = now_us();
    Image *im = os_image_decode(g);
    if (!im) {
        fail("many", "10000 frames were refused", 0, 0);
    }
    i64 t1 = now_us();
    if (os_image_frames(im) != IMG_MAX_FRAMES) {
        fail("many", "frame count", os_image_frames(im), IMG_MAX_FRAMES);
    }
    // Decoding is lazy: opening costs far less than the 10000 frames do
    // to play. Every frame has its delay, and the last is reached in order.
    for (i32 f = 0; f < IMG_MAX_FRAMES; f++) {
        if (os_image_delay(im, f) != 20) {
            fail("many", "delay", f, os_image_delay(im, f));
        }
        if (f%100 == 0) {
            alive();
        }
    }
    i64 t2 = now_us();
    for (i32 f = 0; f < IMG_MAX_FRAMES; f += 7) {
        pixel_at(im, f, 0, 0);
    }
    if (pixel_at(im, IMG_MAX_FRAMES - 1, 0, 0) != BLACK) {
        fail("many", "last frame", 0, 0);
    }
    if (pixel_at(im, 0, 0, 0) != BLACK) {
        fail("many", "first frame again", 0, 0);
    }
    i64 t3 = now_us();
    image_free(im);
    if (imgstat.blocks != blocks || imgstat.objects != objects) {
        fail("leak", "many frames", imgstat.blocks - blocks, imgstat.objects - objects);
    }
    r->checks += 4;
    if (r->verbose) {
        print(r->out, S("many: open "));
        printi64(r->out, (t1 - t0)/1000);
        print(r->out, S(" ms, delays "));
        printi64(r->out, (t2 - t1)/1000);
        print(r->out, S(" ms, play "));
        printi64(r->out, (t3 - t2)/1000);
        print(r->out, S(" ms\n"));
    }
}


// Damaged files

typedef struct {
    u8 *p;
    iz  len, cap;
} Bytes;

// A random length of 1 to most, but no more than room.
static iz rndlen(u64 *s, iz most, iz room) {
    iz n = (iz)(1 + rnd(s, (u64)most));
    return min(n, room);
}

static void mutate(Bytes *b, u64 *s, Str other) {
    i32 count = 1 + (i32)rnd(s, 4);
    for (i32 m = 0; m < count && b->len > 0; m++) {
        // Half the time near the start, where the headers are.
        iz pos = (iz)rnd(s, (u64)(chance(s, 50) ? min(b->len, (iz)80) : b->len));
        switch (rnd(s, 8)) {
        case 0:
        case 1:                 // flip a bit
            b->p[pos] ^= (u8)(1u << rnd(s, 8));
            break;
        case 2:                 // a random byte
            b->p[pos] = (u8)rnd(s, 256);
            break;
        case 3:                 // cut it short
            b->len = pos;
            break;
        case 4: {               // delete a range
            iz n = rndlen(s, 16, b->len - pos);
            memmove(b->p + pos, b->p + pos + n, (uz)(b->len - pos - n));
            b->len -= n;
            break;
        }
        case 5: {               // insert a range of another file
            if (!other.len) {
                break;
            }
            iz from = (iz)rnd(s, (u64)other.len);
            iz n = rndlen(s, 24, other.len - from);
            if (b->len + n > b->cap) {
                break;
            }
            memmove(b->p + pos + n, b->p + pos, (uz)(b->len - pos));
            memcpy(b->p + pos, other.data + from, (uz)n);
            b->len += n;
            break;
        }
        case 6: {               // overwrite with a range of another file
            if (!other.len) {
                break;
            }
            iz from = (iz)rnd(s, (u64)other.len);
            iz n = rndlen(s, 24, min(other.len - from, b->len - pos));
            memcpy(b->p + pos, other.data + from, (uz)n);
            break;
        }
        default: {              // a run of the same byte
            iz n = rndlen(s, 8, b->len - pos);
            memset(b->p + pos, rnd(s, 2) ? 0xff : 0, (uz)n);
        }
        }
    }
}

static void iteration(Rig *r, u64 seed, i64 iter) {
    u64 s = seed ^ ((u64)iter * 0x9e3779b97f4a7c15ULL);
    rand64(&s);
    rand64(&s);
    ctx.iter  = iter;
    ctx.phase = "random";
    alive();

    Arena a = r->files;
    const char *name  = seeds[rnd(&s, (u64)countof(seeds))];
    const char *name2 = seeds[rnd(&s, (u64)countof(seeds))];
    Str base  = readimg(r, &a, name);
    Str other = readimg(r, &a, name2);
    Bytes b = {new(&a, base.len + 64, u8), base.len, base.len + 64};
    memcpy(b.p, base.data, (uz)base.len);
    mutate(&b, &s, other);
    Str input = {b.p, b.len};
    ctx.input = input;

    i32 blocks  = imgstat.blocks;
    i32 objects = imgstat.objects;
    Image *im = os_image_decode(input);
    if (im) {
        r->decoded++;
        r->animated += os_image_frames(im) > 1;
        check_invariants(im, &s);
        image_free(im);
    }
    if (imgstat.blocks != blocks || imgstat.objects != objects) {
        fail("leak", "random", imgstat.blocks - blocks, imgstat.objects - objects);
    }
    ctx.input = (Str){0};
}


// Arguments

typedef struct {
    Str *argv;
    i32  argc;
} Args;

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
        u8 *out    = beg;
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

// Decimal or 0x-prefixed hex.
static b32 parseu64(Str s, u64 *v) {
    u64 base = 10;
    if (s.len>2 && s.data[0]=='0' && (s.data[1]=='x' || s.data[1]=='X')) {
        base = 16;
        s    = (Str){s.data+2, s.len-2};
    }
    if (!s.len) {
        return 0;
    }
    u64 x = 0;
    for (iz i = 0; i < s.len; i++) {
        u64 c = s.data[i];
        u64 d = c>='0' && c<='9' ? c - '0' :
                base==16 && c>='a' && c<='f' ? c - 'a' + 10 :
                base==16 && c>='A' && c<='F' ? c - 'A' + 10 : 99;
        if (d >= base) {
            return 0;
        }
        x = x*base + d;
    }
    *v = x;
    return 1;
}

static Str usage(void) {
    return S(
        "usage: img-test [-s SEED] [-n ITERS] [-i START] [-d DIR] [-t WHICH] [-v]\n"
        "  -s SEED   random seed, decimal or 0x hex (default 0x5eed)\n"
        "  -n ITERS  random iterations to run (default 2000)\n"
        "  -i START  first iteration number, to replay a failure (default 0)\n"
        "  -d DIR    where test/img is (default test)\n"
        "  -t WHICH  only run files, many or random\n"
        "  -v        say what was found, and timings\n"
        "  -f FILE   decode FILE and describe it, no tests\n"
    );
}

static void describe(Rig *r, Str path) {
    Output *o = r->out;
    Arena a = r->files;
    Str bytes = readpath(&a, path);
    if (!bytes.len) {
        print(o, S("img-test: cannot read the file\n"));
        return;
    }
    i64 t0 = now_us();
    Image *im = os_image_decode(bytes);
    i64 t1 = now_us();
    if (!im) {
        print(o, S("not decoded ("));
        printi64(o, (t1 - t0)/1000);
        print(o, S(" ms)\n"));
        return;
    }
    Size sz = os_image_size(im);
    print(o, S("size "));
    printi64(o, (i64)sz.w);
    print(o, S("x"));
    printi64(o, (i64)sz.h);
    print(o, S(", "));
    printi64(o, os_image_frames(im));
    print(o, S(" frames, decoded in "));
    printi64(o, (t1 - t0)/1000);
    print(o, S(" ms\n"));
    for (i32 f = 0; f < min(os_image_frames(im), (i32)8); f++) {
        print(o, S("frame "));
        printi64(o, f);
        print(o, S(": delay "));
        printi64(o, os_image_delay(im, f));
        print(o, S(" ms, corners "));
        i32 w = (i32)sz.w;
        i32 h = (i32)sz.h;
        printpx(o, pixel_at(im, f, 0, 0));
        print(o, S(" "));
        printpx(o, pixel_at(im, f, w - 1, 0));
        print(o, S(" "));
        printpx(o, pixel_at(im, f, 0, h - 1));
        print(o, S(" "));
        printpx(o, pixel_at(im, f, w - 1, h - 1));
        print(o, S("\n"));
    }
    image_free(im);
}

static i32 run(Arena all) {
    Rig r = {0};
    iz  sz[] = {(iz)64<<20, (iz)1<<20};
    Arena mem[2];
    for (i32 i = 0; i < 2; i++) {
        all.beg = (u8 *)(((uptr)all.beg + PAGE - 1) & ~(uptr)(PAGE - 1));
        mem[i]  = (Arena){all.beg, all.beg + sz[i]};
        all.beg += sz[i];
    }
    r.files = mem[0];
    Arena small = mem[1];
    ctx.out = newoutput(&small, GetStdHandle(STD_OUTPUT_HANDLE), 0, 1<<16);
    r.out   = ctx.out;
    AddVectoredExceptionHandler(1, onfault);
    alive();
    CreateThread(0, 0, watchdog, 0, 0, 0);

    u64 seed  = 0x5eed;
    u64 iters = 2000;
    u64 start = 0;
    Str which = {0};
    Str file  = {0};
#ifdef __TCC_BCHECK__
    // tcc's runtime stops on exceptions that Windows raises and handles
    // internally, as WIC does when decoding broken images, so by default
    // only run the phase that decodes good ones.
    which     = S("many");
#endif
    r.dir     = S("test");
    Args args = getargs(&small);
    for (i32 i = 1; i < args.argc; i++) {
        Str arg  = args.argv[i];
        u64 *dst = 0;
        if (equals(arg, S("-v"))) {
            r.verbose = 1;
            continue;
        } else if (equals(arg, S("-d")) && i+1<args.argc) {
            r.dir = args.argv[++i];
            continue;
        } else if (equals(arg, S("-t")) && i+1<args.argc) {
            which = args.argv[++i];
            continue;
        } else if (equals(arg, S("-f")) && i+1<args.argc) {
            file = args.argv[++i];
            continue;
        } else if (equals(arg, S("-s"))) {
            dst = &seed;
        } else if (equals(arg, S("-n"))) {
            dst = &iters;
        } else if (equals(arg, S("-i"))) {
            dst = &start;
        }
        if (!dst || i+1>=args.argc || !parseu64(args.argv[++i], dst)) {
            print(ctx.out, S("img-test: bad argument: "));
            print(ctx.out, arg);
            print(ctx.out, S("\n"));
            print(ctx.out, usage());
            flush(ctx.out);
            return 2;
        }
    }
    ctx.seed = seed;
    ctx.iter = -1;
    Output *o = ctx.out;
    if (file.len) {
        describe(&r, file);
        flush(o);
        return 0;
    }
    b32 all_tests = !which.len;
    u64 t0 = GetTickCount64();

    if (all_tests || equals(which, S("files"))) {
        for (iz i = 0; i < countof(cases); i++) {
            run_case(&r, cases + i);
        }
        run_orientation(&r);
        run_refused(&r);
        run_reduced(&r);
        run_webp(&r);
        ctx.phase = 0;
        print(o, S("files: ok, "));
        printi64(o, r.checks);
        print(o, S(" checks, WebP "));
        print(o, r.webp ? S("decodes\n") : S("not installed\n"));
    }
    if (all_tests || equals(which, S("many"))) {
        run_many(&r);
        ctx.phase = 0;
        print(o, S("many: ok\n"));
    }
    if (all_tests || equals(which, S("random"))) {
        ctx.iter = 0;
        for (u64 i = 0; i < iters; i++) {
            iteration(&r, seed, (i64)(start + i));
        }
        ctx.iter  = -1;
        ctx.phase = 0;
        print(o, S("random: ok seed=0x"));
        printhex(o, seed, 1);
        print(o, S(" iters="));
        printi64(o, (i64)iters);
        print(o, S(", "));
        printi64(o, r.decoded);
        print(o, S(" still decoded, "));
        printi64(o, r.animated);
        print(o, S(" of them animated\n"));
    }
    if (imgstat.blocks != 0 || imgstat.objects != 0) {
        fail("leak", "at the end", imgstat.blocks, imgstat.objects);
    }
    u64 ms = max(GetTickCount64() - t0, (u64)1);
    print(o, S("img-test: ok, "));
    printi64(o, (i64)ms);
    print(o, S(" ms\n"));
    flush(o);
    return 0;
}

void mainCRTStartup(void) {
    u8 *mem = VirtualAlloc(0, (uz)((iz)128<<20), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem + ((iz)128<<20)}));
}
