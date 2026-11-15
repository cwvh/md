// Tests for the viewer: a separate unity build of the portable code (parser,
// highlighter, layout, view) on a fake platform (fakeos.c), with the debug
// checks on: UBSan traps, guard pages behind every allocation.
//   $ make view-test.exe
//   $ view-test.exe [-s SEED] [-n ITERS] [-i START] [-t WHICH]
//
// Three kinds of test:
//   unit     fixed documents and scripted input: layout invariants at many
//            widths, resizing, links, history, images, zoom, reload, leaks
//   random   random Markdown, random window sizes, scrolls, zooms, keys and
//            clicks, with invariants checked after every paint
//   time     a 1 MB document must load and paint within a budget
//
// Iteration i of seed s is self-contained: it derives its own generator from
// (s, i), so "-s S -i I -n 1 -t random" replays exactly it.
#include "base.c"
#include "platform.h"
#include "md.c"
#include "highlight.c"
#include "layout.c"
#include "view.c"
#include "win32.c"
#include "fakeos.c"

#ifndef DEBUG
#error "viewtest.c needs -DDEBUG (guard pages)"
#endif

W32(void *) AddVectoredExceptionHandler(u32, i32 (*)(void *));


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

static u64 rnd(u64 *s, u64 n) {
    return rand64(s) % n;
}

static b32 chance(u64 *s, u64 percent) {
    return rnd(s, 100) < percent;
}

static f32 rndf(u64 *s, f32 lo, f32 hi) {
    return lo + (hi - lo)*(f32)(rnd(s, 1000001))/1000000.0f;
}


// Reporting

typedef struct {
    u64         seed;
    i64         iter;
    char const *phase;
    Output     *out;
    b32         dead;
    volatile u64 progress;
} Ctx;
static Ctx ctx;

static Str cstr(char const *s) {
    iz n = 0;
    for (; s[n]; n++) {}
    return (Str){(u8 *)s, n};
}

static void printhex(Output *o, u64 v) {
    u8  t[16];
    i32 n = 0;
    do {
        t[n++] = (u8)"0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (n) {
        printc(o, t[--n]);
    }
}

static _Noreturn void context(void) {
    Output *o = ctx.out;
    print(o, S("  seed:      0x"));
    printhex(o, ctx.seed);
    print(o, S("\n  iteration: "));
    printi64(o, ctx.iter);
    print(o, S("\n  phase:     "));
    print(o, cstr(ctx.phase ? ctx.phase : "?"));
    if (ctx.iter >= 0) {
        print(o, S("\n  reproduce: view-test.exe -t random -s 0x"));
        printhex(o, ctx.seed);
        print(o, S(" -i "));
        printi64(o, ctx.iter);
        print(o, S(" -n 1"));
    }
    print(o, S("\n"));
    flush(o);
    ExitProcess(1);
}

static _Noreturn void fail(char const *check, char const *what, i64 a, i64 b) {
    Output *o = ctx.out;
    if (ctx.dead) {
        ExitProcess(1);
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

#define CHECK(c, what) do { if (!(c)) fail(#c, what, 0, 0); } while (0)
#define CHECK2(c, what, a, b) do { if (!(c)) fail(#c, what, (i64)(a), (i64)(b)); } while (0)

typedef struct {
    u32 code;
    u32 flags;
    void *next;
    void *addr;
    u32 nparams;
    u64 info[15];
} ExcRecord;

typedef struct {
    ExcRecord *rec;
    void      *context;
} ExcPointers;

static i32 onfault(void *p) {
    ExcPointers *ep = p;
    ExcRecord *e = ep->rec;
    if ((e->code & 0xc0000000u) != 0xc0000000u || ctx.dead) {
        return 0;
    }
    ctx.dead = 1;
    Output *o = ctx.out;
    print(o, S("FAULT: "));
    if (e->code == 0xc0000005u) {
        print(o, e->info[0] ? S("write") : S("read"));
        print(o, S(" access violation (guard page overrun or stale pointer?) at 0x"));
        printhex(o, e->info[1]);
    } else if (e->code == 0xc000001du) {
        print(o, S("illegal instruction (UBSan trap or assert)"));
    } else {
        print(o, S("exception 0x"));
        printhex(o, e->code);
    }
    print(o, S(", instruction at 0x"));
    printhex(o, (u64)(uptr)e->addr);
    print(o, S("\n"));
    context();
}

static void alive(void) {
    ctx.progress = GetTickCount64();
}

W32(void *) CreateThread(void *, uz, u32 (*)(void *), void *, u32, u32 *);
W32(void)   Sleep(u32);

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


// The rig: memory, and driving the app

enum {
    PERM_SIZE    = (iz)160<<20,
    SCRATCH_SIZE = (iz)16<<20,
    DATA_SIZE    = (iz)48<<20,
    GEN_SIZE     = (iz)8<<20,
};

static struct {
    Arena perm, scratch, data, gen;
    Arena data0, gen0;
} rig;

typedef struct {
    u8 *p;
    iz  len, cap;
} Buf;

static void bput(Buf *b, Str s) {
    if (s.len > b->cap - b->len) {
        s.len = b->cap - b->len;
    }
    memcpy(b->p + b->len, s.data, (uz)s.len);
    b->len += s.len;
}

static void bs(Buf *b, char const *s) {
    bput(b, cstr(s));
}

static void bc(Buf *b, u8 c) {
    bput(b, (Str){&c, 1});
}

static void bnum(Buf *b, i64 v) {
    u8 t[24];
    bput(b, (Str){t, fmt_int(t, v)});
}

static Buf bnew(iz cap) {
    return (Buf){new(&rig.gen, cap, u8), 0, cap};
}

static Str bstr(Buf *b) {
    return (Str){b->p, b->len};
}

// Copy into the data arena, which the fake files live in.
static Str keep(Str s) {
    u8 *d = new(&rig.data, s.len, u8);
    memcpy(d, s.data, (uz)s.len);
    return (Str){d, s.len};
}

static void put(char const *path, Str data) {
    fk_put(keep(cstr(path)), keep(data));
}

static void put_s(char const *path, char const *data) {
    put(path, cstr(data));
}

// A fake image.
static void put_img(char const *path, i32 w, i32 h, i32 frames, i32 delay) {
    u8 b[11] = {'F', 'I', 'M', 'G', (u8)w, (u8)(w>>8), (u8)h, (u8)(h>>8), (u8)frames,
                (u8)delay, (u8)(delay>>8)};
    put(path, (Str){b, 11});
}

static void rig_reset(void) {
    rig.data = rig.data0;
    rig.gen  = rig.gen0;
    memset(&fk, 0, sizeof(fk));
    fk.window = (Size){900, 700};
}

static void paint(void) {
    fk.painting = 1;
    Arena s = rig.scratch;
    app_paint(s, fk.window);
    CHECK(fk.clips == 0, "unbalanced clips");
    fk.painting = 0;
    fk.invalid  = 0;
    alive();
}

static void start(char const *path, f32 w, f32 h) {
    fk.window = (Size){w, h};
    Arena p   = rig.perm;
    app_init(&p, path ? cstr(path) : (Str){0}, fk.window);
}

static void resize(f32 w, f32 h) {
    fk.window = (Size){w, h};
    app_resize(fk.window);
}

static void key(Key k, u32 mods) {
    app_key(k, mods);
}

static void click(Point p) {
    app_mouse_move(p, 0);
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
}

// Build every leaf, keeping the reading position.
static void layout_all(void) {
    paint();
    Anchor a = doc_anchor(V.scroll);
    Arena  s = rig.scratch;
    RB rb;
    rb_init(&rb, &s);
    doc_make_all(&rb);
    V.scroll = clampf(doc_anchor_scroll(a), 0, max_scroll());
    V.dirty  = 1;
    paint();
}

static b32 near(f32 a, f32 b, f32 eps) {
    return a - b <= eps && b - a <= eps;
}


// Invariants of a laid out document

static void check_layout(void) {
    CHECK(V.state == VS_DOC, "no document");
    f32 z = D.zoom;
    CHECK(D.colx >= 0 && D.colx + D.colw <= D.winw + 0.01f, "column outside the window");
    f32 prev = 0;
    f32 slack = 40*z;
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        CHECK2(l->hepoch == D.epoch, "leaf height not exact", i, 0);
        CHECK2(l->x >= 0 && l->x + l->w <= D.colw + 0.01f, "leaf outside the column", i, (i64)l->x);
        CHECK2(l->h > 0, "leaf without height", i, 0);
        CHECK2(l->y >= prev - 0.01f, "leaves overlap", i, (i64)prev);
        prev = l->y + l->h;
        if (l->kind == LK_TABLE) {
            Table *tb = l->tb;
            CHECK2(tb && tb->built, "table not built", i, 0);
            CHECK2(tb->colx[tb->ncols] + 1 <= l->w + 0.5f, "table wider than its leaf", (i64)(tb->colx[tb->ncols] + 1), (i64)l->w);
            for (i32 r = 0; r < tb->nrows; r++) {
                CHECK2(tb->rowy[r+1] > tb->rowy[r], "table row without height", i, r);
            }
            CHECK2(near(l->h, tb->rowy[tb->nrows] + 1, 0.01f), "table height", i, 0);
            for (i32 k = 0; k < tb->nrows*tb->ncols; k++) {
                // (An image in a cell may be wider than a column shrunk to fit.)
                if (tb->cell[k] && !(l->flags & LF_IMAGES)) {
                    i32 c = k % tb->ncols;
                    CHECK2(os_para_size(tb->cell[k]).w <= tb->colw[c] + slack, "cell text too wide", i, k);
                }
            }
        } else if (l->kind != LK_RULE && l->kind != LK_EMPTY && !(l->flags & LF_FAILED)) {
            CHECK2(l->p, "leaf without paragraph", i, l->kind);
            f32 wrapw = l->w - 2*l->padl;
            CHECK2(os_para_size(l->p).w <= wrapw + slack, "text wider than its leaf", i, (i64)wrapw);
            CHECK2(l->h + 0.01f >= os_para_size(l->p).h + l->padt + l->padb, "leaf shorter than its text", i, 0);
        }
    }
    CHECK(D.total >= prev, "total height too small");
    for (i32 i = 0; i < D.ndecors; i++) {
        Decor *d = D.decors + i;
        CHECK2(d->lo >= 0 && d->lo <= d->hi && d->hi <= D.nleaves, "decoration range", i, 0);
        if (d->kind == DK_ITEM || d->kind == DK_CODE) {
            CHECK2(d->lo < d->hi, "empty decoration", i, d->kind);
        }
    }
}

// What is true after any paint.
static void check_state(void) {
    if (V.state != VS_DOC) {
        return;
    }
    CHECK(V.scroll == V.scroll, "scroll is NaN");
    CHECK2(V.scroll >= 0 && V.scroll <= max_scroll() + 0.01f, "scroll out of range", (i64)V.scroll, (i64)max_scroll());
    f32 prev = 0;
    for (i32 i = 0; i < D.nleaves; i++) {
        CHECK2(D.leaves[i].y >= prev - 0.01f, "leaves overlap after paint", i, 0);
        prev = D.leaves[i].y + D.leaves[i].h;
    }
    CHECK2(D.nlive <= M_MAX_LIVE + 200 + D.ndecors, "too many live paragraphs", D.nlive, 0);
    CHECK2(fk.live_paras >= D.nlive, "paragraph count mismatch", fk.live_paras, D.nlive);
}


// Finding things on screen

// Where text is in window coordinates, after scrolling it into view.
static b32 goto_text(Str needle, Point *pos) {
    layout_all();
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        Table *tb = l->kind == LK_TABLE ? l->tb : 0;
        i32 ncell = tb ? tb->nrows*tb->ncols : 1;
        for (i32 k = 0; k < ncell; k++) {
            Paragraph *p = tb ? tb->cell[k] : l->p;
            if (!p) {
                continue;
            }
            for (iz o = 0; o + needle.len <= p->len; o++) {
                if (!equals((Str){p->text + o, needle.len}, needle)) {
                    continue;
                }
                scroll_to(l->y - 30);
                paint();
                Point org = {D.colx + l->x + l->padl, l->y - V.scroll + l->padt};
                if (tb) {
                    f32 z = D.zoom;
                    i32 r = k/tb->ncols, c = k%tb->ncols;
                    org = (Point){D.colx + l->x + tb->colx[c] + 1 + tb->padx,
                                  l->y - V.scroll + tb->rowy[r] + 1 + M_CELL_PADY*z};
                }
                Rect rc[1];
                CHECK(os_para_rects(p, o, o + needle.len, rc, 1) >= 1, "no rect for text");
                *pos = (Point){org.x + rc[0].x + 1, org.y + rc[0].y + rc[0].h/2};
                return 1;
            }
        }
    }
    return 0;
}

static void click_text(char const *needle) {
    Point p;
    CHECK2(goto_text(cstr(needle), &p), "text not found", 0, 0);
    click(p);
    paint();
}

static b32 path_is(char const *p) {
    return equals((Str){V.path, V.pathlen}, cstr(p));
}

static b32 title_is(char const *t) {
    return equals((Str){fk.title, fk.titlelen}, cstr(t));
}


// Fixed documents

static char const doc_all[] =
    "<p align=\"center\"><img src=\"img/a.fimg\" width=\"20\"></p>\n"
    "\n"
    "# Title *with* `code`\n"
    "\n"
    "Intro paragraph with **strong**, *emphasis*, ~~strike~~, `inline code`, a [link](https://example.org/x) and\n"
    "a soft break, and a hard break  \n"
    "after it. Jump to [the table](#the-table) or [last](#last) or [missing](#nope).\n"
    "\n"
    "<!-- a comment -->\n"
    "\n"
    "## Lists\n"
    "\n"
    "- one\n"
    "- two\n"
    "  - nested\n"
    "    - deeper\n"
    "      - deepest\n"
    "- three\n"
    "\n"
    "1. first\n"
    "2. second\n"
    "   1. inner\n"
    "\n"
    "- [x] done\n"
    "- [ ] open\n"
    "\n"
    "- loose\n"
    "\n"
    "- loose two\n"
    "\n"
    "  with a paragraph\n"
    "\n"
    "-\n"
    "\n"
    "## Quotes\n"
    "\n"
    "> quoted text\n"
    ">\n"
    "> > nested quote\n"
    ">\n"
    "> - list in quote\n"
    ">\n"
    "> ```c\n"
    "> int x;\n"
    "> ```\n"
    "\n"
    ">\n"
    "\n"
    "---\n"
    "\n"
    "## Code\n"
    "\n"
    "```c\n"
    "int main(void) { return 0; }\n"
    "\tindented with a tab and a very long line that has to wrap around because it is longer than any column here\n"
    "\n"
    "```\n"
    "\n"
    "    indented code\n"
    "\n"
    "```\n"
    "```\n"
    "\n"
    "## The Table\n"
    "\n"
    "| Left | Center | Right |\n"
    "|:-----|:------:|------:|\n"
    "| a | `b` | [c](#last) |\n"
    "| a very long cell with many many words in it to force wrapping | x | 1 |\n"
    "| ragged |\n"
    "| 日本語のテキスト | y |\n"
    "\n"
    "<div align=\"center\">\n"
    "<b>html</b> <a href=\"https://example.org/html\">link</a><br>\n"
    "<img src=\"img/missing.fimg\" alt=\"gone\">\n"
    "</div>\n"
    "\n"
    "![alt text](img/b.fimg) and ![big](img/big.fimg) ![bad](img/bad.fimg)\n"
    "\n"
    "###### Last\n"
    "\n"
    "Wide: 日本語のテキスト、中文文本 😀 and a long-unbreakable-token-"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "\n"
    "End.\n";

static void setup_files(void) {
    put_s("/docs/all.md", doc_all);
    put_img("/docs/img/a.fimg", 40, 30, 1, 0);
    put_img("/docs/img/b.fimg", 300, 100, 1, 0);
    put_img("/docs/img/big.fimg", 3000, 600, 1, 0);
    put_img("/docs/img/anim.fimg", 64, 64, 3, 50);
    put_s("/docs/img/bad.fimg", "not an image");
}

// Widths and zooms the layout must hold up at.
static f32 const test_widths[] = {160, 200, 320, 480, 700, 900, 1000, 1500, 2400};
static f32 const test_zooms[]  = {0.5f, 1, 1.5f, 3};

static void test_layout(void) {
    ctx.phase = "layout";
    rig_reset();
    setup_files();
    start("/docs/all.md", 900, 700);
    paint();
    CHECK(V.state == VS_DOC, "all.md did not load");
    CHECK(title_is("all.md - md"), "title");
    for (i32 zi = 0; zi < (i32)countof(test_zooms); zi++) {
        set_zoom(test_zooms[zi]);
        for (i32 wi = 0; wi < (i32)countof(test_widths); wi++) {
            resize(test_widths[wi], 600);
            layout_all();
            check_layout();
            check_state();
        }
    }
    // Every kind of block is there.
    b32 seen[8] = {0};
    for (i32 i = 0; i < D.nleaves; i++) {
        seen[D.leaves[i].kind] = 1;
    }
    for (i32 k = 0; k < 7; k++) {
        CHECK2(seen[k], "leaf kind missing from the sample", k, 0);
    }
    b32 dk[3] = {0};
    for (i32 i = 0; i < D.ndecors; i++) {
        dk[D.decors[i].kind] = 1;
    }
    CHECK(dk[0] && dk[1] && dk[2], "decoration kind missing");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak after close", fk.live_paras, fk.live_images);
}

static void test_theme(void) {
    ctx.phase = "theme";
    for (i32 k = 1; k < HL_COUNT; k++) {
        CHECK2(theme_light.hl[k].a == 255, "light theme lacks a highlight color", k, 0);
        CHECK2(theme_dark.hl[k].a == 255, "dark theme lacks a highlight color", k, 0);
    }
    rig_reset();
    setup_files();
    start("/docs/all.md", 900, 700);
    paint();
    CHECK(D.th == &theme_light, "light by default");
    fk.dark = 1;
    app_theme_changed();
    paint();
    CHECK(D.th == &theme_dark, "dark after the theme changed");
    layout_all();
    check_layout();
    fk.dark = 0;
    app_theme_changed();
    layout_all();
    check_layout();
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}

static void test_resize(void) {
    ctx.phase = "resize";
    rig_reset();
    // A long document of paragraphs and headings.
    Buf b = bnew(1<<20);
    for (i32 i = 0; i < 80; i++) {
        bs(&b, "## Heading ");
        bnum(&b, i);
        bs(&b, "\n\nSome words in paragraph number ");
        bnum(&b, i);
        bs(&b, " that are long enough to wrap onto more than one line when the window is narrow, "
               "and a few more words so that they wrap onto a second and a third line too.\n\n");
    }
    put("/docs/long.md", bstr(&b));
    start("/docs/long.md", 900, 500);
    paint();
    for (i32 trial = 0; trial < 6; trial++) {
        i32 target = 10 + trial*21;
        scroll_to(D.leaves[target].y + 3);
        paint();
        Anchor a = doc_anchor(V.scroll);
        CHECK2(a.leaf == target, "setup: not at the leaf", a.leaf, target);
        f32 ws[] = {500, 300, 1200, 200, 900, 640};
        for (i32 k = 0; k < (i32)countof(ws); k++) {
            resize(ws[k], 500 + (f32)k*30);
            paint();
            Anchor n = doc_anchor(V.scroll);
            CHECK2(n.leaf == target, "first visible block moved on resize", n.leaf, target);
            CHECK2(near(n.off, 3, 0.5f), "offset in the first block changed on resize", (i64)n.off, 3);
            check_state();
        }
    }
    // Zoom keeps the reading position too.
    scroll_to(D.leaves[50].y + 3);
    paint();
    for (i32 i = 0; i < 4; i++) {
        key(KEY_PLUS, MOD_PRIMARY);
        paint();
        Anchor a = doc_anchor(V.scroll);
        CHECK2(a.leaf == 50, "zoom lost the position", a.leaf, i);
    }
    CHECK(V.zoom > 1.5f, "zoom did not grow");
    key(KEY_0, MOD_PRIMARY);
    paint();
    CHECK(V.zoom == 1, "ctrl+0 resets zoom");
    CHECK(doc_anchor(V.scroll).leaf == 50, "reset zoom lost the position");
    for (i32 i = 0; i < 30; i++) {
        key(KEY_MINUS, MOD_PRIMARY);
    }
    paint();
    CHECK(V.zoom == 0.5f, "zoom out stops at 50%");
    for (i32 i = 0; i < 30; i++) {
        key(KEY_PLUS, MOD_PRIMARY);
    }
    paint();
    CHECK(V.zoom == 4, "zoom in stops at 400%");
    // Ctrl+wheel zooms too.
    key(KEY_0, MOD_PRIMARY);
    app_wheel((Point){100, 100}, 0, 1, MOD_PRIMARY);
    CHECK(V.zoom > 1, "ctrl+wheel up zooms in");
    app_wheel((Point){100, 100}, 0, -1, MOD_PRIMARY);
    CHECK(V.zoom == 1, "ctrl+wheel down zooms out");
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}

static void test_scroll(void) {
    ctx.phase = "scroll";
    rig_reset();
    setup_files();
    start("/docs/all.md", 700, 400);
    paint();
    CHECK(V.scroll == 0, "starts at the top");
    key(KEY_DOWN, 0);
    paint();
    CHECK(V.scroll > 0, "arrow down scrolls");
    f32 s1 = V.scroll;
    key(KEY_UP, 0);
    paint();
    CHECK(V.scroll < s1, "arrow up scrolls back");
    key(KEY_UP, 0);
    key(KEY_UP, 0);
    paint();
    CHECK(V.scroll == 0, "clamped at the top");
    app_wheel((Point){300, 200}, 0, 1, 0);
    paint();
    CHECK(V.scroll == 0, "wheel up at the top stays");
    app_wheel((Point){300, 200}, 0, -1, 0);
    paint();
    CHECK(near(V.scroll, 3*24, 0.5f), "a wheel notch is about 3 lines");
    key(KEY_PGDN, 0);
    paint();
    CHECK(near(V.scroll, 3*24 + 360, 0.5f), "page down");
    key(KEY_SPACE, 0);
    key(KEY_SPACE, MOD_SHIFT);
    paint();
    CHECK(near(V.scroll, 3*24 + 360, 0.5f), "space and shift+space");
    key(KEY_PGUP, 0);
    paint();
    key(KEY_END, 0);
    paint();
    CHECK(near(V.scroll, max_scroll(), 0.5f), "end reaches the bottom");
    CHECK(D.leaves[D.nleaves-1].y + D.leaves[D.nleaves-1].h + M_PAGE_PAD <= V.scroll + 400 + 1, "last leaf visible after end");
    key(KEY_PGDN, 0);
    paint();
    CHECK(near(V.scroll, max_scroll(), 0.5f), "clamped at the bottom");
    key(KEY_HOME, 0);
    paint();
    CHECK(V.scroll == 0, "home");
    // The scrollbar: drag the thumb, and click the track.
    Bar bar = bar_geometry();
    CHECK(bar.show, "scrollbar shows");
    Point t = {700 - 5, bar.thumb.y + 5};
    app_mouse_down(t, BUTTON_LEFT, 0);
    CHECK(V.drag, "thumb grabbed");
    app_mouse_move((Point){t.x, t.y + bar.track/2}, 0);
    app_mouse_up((Point){t.x, t.y + bar.track/2}, BUTTON_LEFT, 0);
    paint();
    CHECK(near(V.scroll, max_scroll()/2, max_scroll()*0.05f), "dragging the thumb scrolls proportionally");
    app_mouse_down((Point){695, 395}, BUTTON_LEFT, 0);
    app_mouse_up((Point){695, 395}, BUTTON_LEFT, 0);
    paint();
    check_state();
    // Wild input.
    scroll_by(-1e9f);
    paint();
    CHECK(V.scroll == 0, "huge scroll up clamps");
    scroll_by(1e9f);
    paint();
    CHECK(near(V.scroll, max_scroll(), 0.5f), "huge scroll down clamps");
    app_close();
}

static void test_links(void) {
    ctx.phase = "links";
    rig_reset();
    setup_files();
    start("/docs/all.md", 900, 500);
    paint();
    // External.
    click_text("link");
    CHECK2(fk.nurls == 1, "external link not opened", fk.nurls, 0);
    CHECK(equals(fk.urls[0], S("https://example.org/x")), "wrong url opened");
    // Hover shows the address and a hand.
    Point p;
    CHECK(goto_text(S("link"), &p), "text");
    app_mouse_move(p, 0);
    paint();
    CHECK(fk.cursor == CURSOR_HAND, "hand over a link");
    CHECK(equals(V.hover, S("https://example.org/x")), "status shows the url");
    CHECK(V.status != 0, "status bubble made");
    app_mouse_move((Point){p.x, p.y - 200}, 0);
    paint();
    CHECK(V.hover.len == 0 || fk.cursor != CURSOR_HAND, "hand only over links");
    app_mouse_leave();
    paint();
    CHECK(V.hover.len == 0 && V.status == 0, "mouse leave clears the status");
    // Inline code is text with no link; plain text gets the I-beam.
    CHECK(goto_text(S("inline code"), &p), "code text");
    app_mouse_move(p, 0);
    CHECK(fk.cursor == CURSOR_IBEAM, "I-beam over text");
    Hit h = doc_hit(p, V.scroll);
    CHECK(h.leaf >= 0 && h.p && h.th.inside && h.th.link == 0, "code has no link");
    CHECK(fk.cursor != CURSOR_HAND, "no hand over code");
    // Anchors: to a heading, and back.
    key(KEY_HOME, 0);
    paint();
    click_text("the table");
    i32 hd = doc_find_slug(S("the-table"));
    CHECK(hd >= 0, "heading slug");
    CHECK2(near(V.scroll, min(D.leaves[hd].y, max_scroll()), 1), "anchor link scrolls to the heading", (i64)V.scroll, (i64)D.leaves[hd].y);
    key(KEY_LEFT, MOD_ALT);
    paint();
    CHECK2(V.scroll < D.leaves[hd].y - 10, "alt+left goes back", (i64)V.scroll, 0);
    key(KEY_RIGHT, MOD_ALT);
    paint();
    CHECK(near(V.scroll, min(D.leaves[hd].y, max_scroll()), 1), "alt+right goes forward");
    // A missing anchor does nothing.
    CHECK(goto_text(S("missing"), &p), "text");
    f32 before = V.scroll;
    click(p);
    paint();
    CHECK(V.scroll == before, "unknown anchor does nothing");
    // The same through a link inside a table cell.
    click_text("c");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
}

static void test_files(void) {
    ctx.phase = "files";
    rig_reset();
    setup_files();
    put_s("/docs/a.md",
          "# A\n\n[to b](b.md) [to c](sub/c.md#deep) [up](../top.md) [txt](notes.txt) "
          "[ftp](ftp://x.org/f) [mail](mailto:me@x.org) [enc](sub/d%20e.md) [anchor](#a)\n");
    put_s("/docs/b.md", "# B\n\n[back to a](a.md)\n");
    put_s("/docs/sub/c.md", "# C\n\nsome text\n\n## Deep\n\n[home](../a.md)\n");
    put_s("/top.md", "# Top\n");
    put_s("/docs/sub/d e.md", "# Space\n");
    put_s("/docs/notes.txt", "not markdown");
    start("/docs/a.md", 900, 400);
    paint();
    CHECK(path_is("/docs/a.md"), "initial path");
    click_text("to b");
    CHECK(path_is("/docs/b.md"), "relative link opens the document");
    CHECK(title_is("b.md - md"), "title follows");
    CHECK(equals((Str){fk.watch, fk.watchlen}, S("/docs/b.md")), "watching the new file");
    click_text("back to a");
    CHECK(path_is("/docs/a.md"), "link back");
    key(KEY_BACKSPACE, 0);
    paint();
    CHECK(path_is("/docs/b.md"), "backspace goes back");
    key(KEY_LEFT, MOD_ALT);
    paint();
    CHECK(path_is("/docs/a.md"), "alt+left goes back again");
    key(KEY_LEFT, MOD_ALT);
    paint();
    CHECK(path_is("/docs/a.md"), "nothing before the first");
    key(KEY_RIGHT, MOD_ALT);
    key(KEY_RIGHT, MOD_ALT);
    paint();
    CHECK(path_is("/docs/a.md"), "forward twice ends at the last");
    click_text("to c");
    CHECK(path_is("/docs/sub/c.md"), "subdirectory link");
    CHECK(V.scroll > 0 || max_scroll() == 0, "fragment of a file link scrolls");
    click_text("home");
    CHECK(path_is("/docs/a.md"), "../ resolved");
    click_text("up");
    CHECK(path_is("/top.md"), "../ above the directory");
    key(KEY_BACKSPACE, 0);
    paint();
    CHECK(path_is("/docs/a.md"), "back from top");
    click_text("txt");
    CHECK(path_is("/docs/a.md"), "other relative links do nothing");
    click_text("enc");
    CHECK(path_is("/docs/sub/d e.md"), "percent-encoded path");
    key(KEY_BACKSPACE, 0);
    paint();
    CHECK2(fk.nurls == 0, "no urls so far", fk.nurls, 0);
    click_text("ftp");
    click_text("mail");
    CHECK2(fk.nurls == 2, "schemes go to the OS", fk.nurls, 0);
    CHECK(equals(fk.urls[1], S("mailto:me@x.org")), "mail url");
    // Opening by other means.
    fk.dialog = S("/docs/b.md");
    key(KEY_O, MOD_PRIMARY);
    paint();
    CHECK(fk.dialog_asked && path_is("/docs/b.md"), "ctrl+o opens a file");
    app_file_dropped(S("/docs/a.md"));
    paint();
    CHECK(path_is("/docs/a.md"), "dropped file opens");
    app_file_dropped(S("/docs/missing.md"));
    paint();
    CHECK(V.state == VS_ERROR, "missing file shows an error");
    CHECK(title_is("md"), "title of an error");
    key(KEY_LEFT, MOD_ALT);
    paint();
    CHECK(V.state == VS_DOC && path_is("/docs/a.md"), "back from an error");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
}

static void test_images(void) {
    ctx.phase = "images";
    rig_reset();
    setup_files();
    put_s("/docs/img.md",
          "![small](img/a.fimg)\n\n![wide](img/big.fimg)\n\n"
          "<img src=\"img/b.fimg\" width=\"60\"> <img src=\"img/b.fimg\" width=\"60\" height=\"60\">\n\n"
          "![gone](img/none.fimg) ![bad](img/bad.fimg) ![remote](https://example.org/i.png)\n\n"
          "[![linked](img/a.fimg)](https://example.org/l)\n\n"
          "![anim](img/anim.fimg)\n");
    start("/docs/img.md", 900, 4000);
    paint();
    layout_all();
    check_layout();
    CHECK2(fk.live_images == 4, "decoded each distinct image once", fk.live_images, 0);
    // Paint everything and look at where images went.
    paint();
    i32 big = 0, small = 0, w60 = 0, sq = 0;
    for (i32 i = 0; i < fk.nimage_draws; i++) {
        FDrawImage *d = fk.image_draws + i;
        CHECK(d->dst.w <= D.colw + 0.01f, "image wider than the column");
        if (d->im->w == 3000) {
            big++;
            CHECK(near(d->dst.w, D.colw, 0.6f), "wide image scaled to the column");
            CHECK(near(d->dst.h/d->dst.w, 600.0f/3000, 0.01f), "aspect kept when scaling");
        } else if (d->im->w == 40) {
            small++;
            CHECK(near(d->dst.w, 40, 0.01f) && near(d->dst.h, 30, 0.01f), "natural size");
        } else if (d->im->w == 300) {
            if (near(d->dst.w, 60, 0.01f) && near(d->dst.h, 20, 0.01f)) {
                w60++;
            } else if (near(d->dst.w, 60, 0.01f) && near(d->dst.h, 60, 0.01f)) {
                sq++;
            }
        }
    }
    CHECK2(big == 1 && small == 2, "images drawn", big, small);
    if (!(w60 == 1 && sq == 1)) {
        for (i32 i = 0; i < fk.nimage_draws; i++) {
            Output *o = ctx.out;
            printi64(o, (i64)fk.image_draws[i].im->w);
            print(o, S(": "));
            printi64(o, (i64)fk.image_draws[i].dst.w);
            print(o, S("x"));
            printi64(o, (i64)fk.image_draws[i].dst.h);
            print(o, S("\n"));
        }
    }
    CHECK2(w60 == 1 && sq == 1, "images drawn with sizes from HTML", w60, sq);
    // Placeholders are text: alt, or the file name.
    Point p;
    CHECK(goto_text(S("gone"), &p), "placeholder with alt text");
    CHECK(goto_text(S("bad"), &p), "undecodable image gets a placeholder");
    CHECK(goto_text(S("remote"), &p), "remote image gets a placeholder");
    // Narrow: the wide image shrinks again, and the width change rebuilds it.
    resize(300, 4000);
    layout_all();
    check_layout();
    paint();
    for (i32 i = 0; i < fk.nimage_draws; i++) {
        CHECK(fk.image_draws[i].dst.w <= D.colw + 0.01f, "image wider than the narrow column");
    }
    CHECK(fk.live_images == 4, "images are kept across resizes");
    // The linked image is a link.
    CHECK(goto_text(S("\xEF\xBF\xBC"), &p), "image box in the text");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
    // Reload frees them too.
    start("/docs/img.md", 900, 4000);
    paint();
    CHECK(fk.live_images > 0, "images loaded");
    app_file_changed();
    CHECK2(fk.live_images == 0 && fk.live_paras == 0, "reload frees everything first", fk.live_images, fk.live_paras);
    paint();
    app_close();
    CHECK(fk.live_images == 0 && fk.live_paras == 0, "leak after reload");
}

static void test_anim(void) {
    ctx.phase = "anim";
    rig_reset();
    setup_files();
    Buf b = bnew(1<<16);
    bs(&b, "![anim](img/anim.fimg)\n\n");
    for (i32 i = 0; i < 60; i++) {
        bs(&b, "filler paragraph\n\n");
    }
    put("/docs/anim.md", bstr(&b));
    start("/docs/anim.md", 600, 300);
    paint();
    CHECK(fk.timer_on[ANIM_TIMER], "timer runs while an animated image is visible");
    CHECK(fk.nimage_draws == 1 && fk.image_draws[0].frame == 0, "first frame");
    for (i32 i = 0; i < 4; i++) {
        app_timer(ANIM_TIMER);      // 16 ms each
    }
    CHECK(fk.invalid, "a new frame asks for a repaint");
    paint();
    CHECK2(fk.image_draws[0].frame == 1, "frame advanced after its delay", fk.image_draws[0].frame, 0);
    for (i32 i = 0; i < 20; i++) {
        app_timer(ANIM_TIMER);
        paint();
    }
    CHECK(fk.image_draws[0].frame < 3, "frames wrap");
    key(KEY_END, 0);
    paint();
    CHECK(fk.nimage_draws == 0, "scrolled away");
    app_timer(ANIM_TIMER);
    CHECK(!fk.timer_on[ANIM_TIMER], "timer stops when the animation is out of view");
    key(KEY_HOME, 0);
    paint();
    CHECK(fk.timer_on[ANIM_TIMER], "timer restarts when it comes back");
    app_close();
    CHECK(!fk.timer_on[ANIM_TIMER], "timer stopped on close");
    CHECK(fk.live_images == 0 && fk.live_paras == 0, "leak");
}

static void test_reload(void) {
    ctx.phase = "reload";
    rig_reset();
    Buf b = bnew(1<<20);
    for (i32 i = 0; i < 100; i++) {
        bs(&b, "paragraph ");
        bnum(&b, i);
        bs(&b, " with a little text in it\n\n");
    }
    Str v1 = keep(bstr(&b));
    put("/d/x.md", v1);
    start("/d/x.md", 500, 300);
    paint();
    scroll_to(D.leaves[40].y + 7);
    paint();
    Anchor a = doc_anchor(V.scroll);
    app_file_changed();
    paint();
    Anchor n = doc_anchor(V.scroll);
    CHECK2(n.leaf == a.leaf && near(n.off, a.off, 0.5f), "reload keeps the position", n.leaf, a.leaf);
    // The file grows at the end: still there.
    Buf c = bnew(1<<20);
    bput(&c, v1);
    bs(&c, "one more\n");
    put("/d/x.md", bstr(&c));
    app_file_changed();
    paint();
    n = doc_anchor(V.scroll);
    CHECK2(n.leaf == a.leaf, "reload of a longer file keeps the position", n.leaf, a.leaf);
    CHECK(D.nleaves == 101, "reloaded content");
    // The file shrinks below the position: clamped, no crash.
    put_s("/d/x.md", "short\n");
    app_file_changed();
    paint();
    CHECK(V.scroll == 0 && D.nleaves == 1, "reload of a shorter file");
    // The file disappears.
    fk_remove(S("/d/x.md"));
    app_file_changed();
    paint();
    CHECK(V.state == VS_ERROR, "file removed");
    put_s("/d/x.md", "back\n");
    app_file_changed();
    paint();
    CHECK(V.state == VS_DOC, "file is back");
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}

static void test_empty(void) {
    ctx.phase = "empty";
    rig_reset();
    start(0, 500, 300);
    paint();
    CHECK(V.state == VS_EMPTY && title_is("md"), "no document");
    CHECK(fk.para_draws && fk.npara_draws >= 1, "hint drawn");
    CHECK(fk.live_paras == 0, "hint paragraphs freed");
    key(KEY_DOWN, 0);
    key(KEY_END, 0);
    app_wheel((Point){10, 10}, 0, -1, 0);
    app_mouse_move((Point){10, 10}, 0);
    click((Point){10, 10});
    key(KEY_PLUS, MOD_PRIMARY);
    resize(300, 200);
    paint();
    fk.dark = 1;
    app_theme_changed();
    paint();
    put_s("/e.md", "# Hi\n");
    fk.dialog = S("/e.md");
    key(KEY_O, MOD_PRIMARY);
    paint();
    CHECK(V.state == VS_DOC, "opened from the empty state");
    app_close();
    CHECK(fk.live_paras == 0, "leak");
    // A file that cannot be read.
    rig_reset();
    start("/nowhere.md", 500, 300);
    paint();
    CHECK(V.state == VS_ERROR, "missing file at start");
    CHECK(fk.live_paras == 0, "error message freed");
    app_close();
    // A file too large for the arena.
    rig_reset();
    Buf big = bnew(5<<20);
    while (big.len + 100 < big.cap) {
        bs(&big, "some text that makes up a very large file\n\n");
    }
    put("/big.md", bstr(&big));
    Arena small = {rig.perm.beg, rig.perm.beg + ((iz)10<<20)};
    fk.window = (Size){500, 300};
    app_init(&small, S("/big.md"), fk.window);
    paint();
    CHECK(V.state == VS_ERROR, "too large for the arena shows an error");
    app_close();
    // The parser's worst case on a modest arena: an error or a document, no crash.
    rig_reset();
    Buf hostile = bnew(1<<20);
    while (hostile.len + 10 < 600000) {
        bs(&hostile, "a|b\n");
    }
    put("/h.md", bstr(&hostile));
    Arena small2 = {rig.perm.beg, rig.perm.beg + ((iz)12<<20)};
    app_init(&small2, S("/h.md"), fk.window);
    paint();
    CHECK(V.state == VS_ERROR || V.state == VS_DOC, "hostile input");
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}

static void test_leaks(void) {
    ctx.phase = "leaks";
    rig_reset();
    setup_files();
    put_s("/docs/other.md", "# Other\n\ntext **bold** `code` ![x](img/a.fimg)\n");
    start("/docs/all.md", 700, 500);
    for (i32 i = 0; i < 6; i++) {
        paint();
        layout_all();
        CHECK(fk.live_paras > 0, "paragraphs alive");
        i32 before = fk.live_paras;
        app_file_changed();
        CHECK2(fk.live_paras == 0 && fk.live_images == 0, "reload frees before loading", fk.live_paras, fk.live_images);
        paint();
        layout_all();
        // (Markers are made when painted, so they may differ.)
        CHECK2(fk.live_paras <= before && fk.live_paras + 8 >= before, "same document, same objects", fk.live_paras, before);
        app_file_dropped(S("/docs/other.md"));
        paint();
        app_file_dropped(S("/docs/all.md"));
    }
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak at close", fk.live_paras, fk.live_images);
    CHECK(fk.made_paras > 100, "test made paragraphs");
}

static void test_all(void) {
    test_layout();
    test_theme();
    test_resize();
    test_scroll();
    test_links();
    test_files();
    test_images();
    test_anim();
    test_reload();
    test_empty();
    test_leaks();
}


// Random documents

static char const *const words[] = {
    "alpha", "beta", "gamma", "delta", "lorem", "ipsum", "dolor", "sit", "amet", "the", "quick",
    "brown", "fox", "jumps", "over", "lazy", "dog", "日本語", "中文", "😀", "ünïcödé", "a", "of",
};

static void gen_words(Buf *b, u64 *r, i32 n) {
    for (i32 i = 0; i < n; i++) {
        if (i) {
            bc(b, ' ');
        }
        bs(b, words[rnd(r, countof(words))]);
    }
}

static void gen_inline(Buf *b, u64 *r) {
    switch (rnd(r, 22)) {
    case 0:  bs(b, "**"); gen_words(b, r, 2); bs(b, "**"); break;
    case 1:  bs(b, "*"); gen_words(b, r, 2); bs(b, "*"); break;
    case 2:  bs(b, "~~"); gen_words(b, r, 1); bs(b, "~~"); break;
    case 3:  bs(b, "`"); gen_words(b, r, 2); bs(b, "`"); break;
    case 4:  bs(b, "[link text](https://example.org/"); gen_words(b, r, 1); bs(b, ")"); break;
    case 5:  bs(b, "[anchor](#heading-"); bnum(b, (i64)rnd(r, 6)); bs(b, ")"); break;
    case 6:  bs(b, "[doc](other.md)"); break;
    case 7:  bs(b, "![alt](img/a.fimg)"); break;
    case 8:  bs(b, "![alt big](img/big.fimg)"); break;
    case 9:  bs(b, "![none](img/none.png)"); break;
    case 10: bs(b, "<br>"); break;
    case 11: bs(b, "  \n"); break;
    case 12: bs(b, "<img src=\"img/b.fimg\" width=\""); bnum(b, (i64)rnd(r, 500)); bs(b, "\">"); break;
    case 13: bs(b, "<b>raw</b> <!-- c --> <span>x</span>"); break;
    case 14: bs(b, "https://www.example.org/auto"); break;
    case 15: bs(b, "&amp; &copy; \\* \\_"); break;
    case 16: bs(b, "\n"); break;
    case 17: bs(b, "verylongunbreakable"); for (i32 i = 0; i < 40; i++) { bs(b, "word"); } break;
    default: gen_words(b, r, 1 + (i32)rnd(r, 5)); break;
    }
}

static void gen_para(Buf *b, u64 *r) {
    i32 n = 1 + (i32)rnd(r, 14);
    for (i32 i = 0; i < n; i++) {
        if (i) {
            bc(b, ' ');
        }
        if (chance(r, 35)) {
            gen_inline(b, r);
        } else {
            gen_words(b, r, 1 + (i32)rnd(r, 4));
        }
    }
    bc(b, '\n');
}

static void gen_table(Buf *b, u64 *r) {
    i32 nc = 1 + (i32)rnd(r, 5);
    i32 nr = (i32)rnd(r, 5);
    for (i32 c = 0; c < nc; c++) {
        bs(b, "| h");
        bnum(b, c);
        bc(b, ' ');
    }
    bs(b, "|\n");
    for (i32 c = 0; c < nc; c++) {
        bs(b, chance(r, 30) ? "|:--:" : chance(r, 30) ? "|--:" : "|---");
    }
    bs(b, "|\n");
    for (i32 i = 0; i < nr; i++) {
        i32 cells = chance(r, 25) ? 1 + (i32)rnd(r, (u64)nc) : nc;
        for (i32 c = 0; c < cells; c++) {
            bs(b, "| ");
            if (chance(r, 20)) {
                gen_inline(b, r);
            } else {
                gen_words(b, r, (i32)rnd(r, 8));
            }
            bc(b, ' ');
        }
        bs(b, chance(r, 80) ? "|\n" : "\n");
    }
}

static void gen_block(Buf *b, u64 *r, i32 depth);

// Lines of src, with first on the first and rest on the others.
static void gen_prefixed(Buf *b, Str first, Str rest, Str src) {
    b32 isfirst = 1;
    for (iz i = 0; i < src.len;) {
        iz e = i;
        while (e<src.len && src.data[e]!='\n') {
            e++;
        }
        bput(b, isfirst ? first : rest);
        bput(b, (Str){src.data+i, e-i});
        bc(b, '\n');
        isfirst = 0;
        i = e + 1;
    }
}

static void gen_block(Buf *b, u64 *r, i32 depth) {
    i32 k = (i32)rnd(r, 100);
    if (k < 28 || depth > 4) {
        gen_para(b, r);
    } else if (k < 36) {
        for (i32 i = 0; i < 1 + (i32)rnd(r, 6); i++) {
            bc(b, '#');
        }
        bs(b, " Heading ");
        bnum(b, (i64)rnd(r, 6));
        bc(b, '\n');
    } else if (k < 46) {
        Buf t = bnew(4096);
        for (i32 i = 0; i < 1 + (i32)rnd(r, 3); i++) {
            gen_block(&t, r, depth + 1);
            bc(&t, '\n');
        }
        gen_prefixed(b, S("> "), S("> "), bstr(&t));
    } else if (k < 64) {
        i32 ordered = chance(r, 40);
        i32 tight   = chance(r, 50);
        for (i32 i = 0; i < 1 + (i32)rnd(r, 5); i++) {
            Buf t = bnew(4096);
            if (chance(r, 20)) {
                bs(&t, chance(r, 50) ? "[ ] " : "[x] ");
            }
            gen_block(&t, r, depth + 1);
            if (chance(r, 30)) {
                bc(&t, '\n');
                gen_block(&t, r, depth + 1);
            }
            u8 num[24];
            Str  mark = ordered ? (Str){num, fmt_int(num, 1 + i)} : S("-");
            u8   head[32];
            iz   hn = 0;
            if (ordered) {
                memcpy(head, num, (uz)mark.len);
                hn = mark.len;
                head[hn++] = '.';
            } else {
                head[hn++] = '-';
            }
            head[hn++] = ' ';
            Str rest = {(u8 *)"    ", hn};
            gen_prefixed(b, (Str){head, hn}, rest, bstr(&t));
            if (!tight) {
                bc(b, '\n');
            }
        }
    } else if (k < 74) {
        bs(b, "```");
        bs(b, chance(r, 50) ? "c" : chance(r, 50) ? "python" : "");
        bc(b, '\n');
        i32 n = (i32)rnd(r, 8);
        if (chance(r, 3)) {
            n = 150;
        }
        for (i32 i = 0; i < n; i++) {
            if (chance(r, 15)) {
                bc(b, '\t');
            }
            if (chance(r, 20)) {
                bs(b, "    ");
            }
            gen_words(b, r, (i32)rnd(r, chance(r, 10) ? 40 : 8));
            bc(b, '\n');
        }
        bs(b, "```\n");
    } else if (k < 82) {
        gen_table(b, r);
    } else if (k < 86) {
        bs(b, "---\n");
    } else if (k < 90) {
        bs(b, "<p align=\"center\"><img src=\"img/a.fimg\" width=\"20\"></p>\n");
    } else if (k < 93) {
        bs(b, "<div align=\"center\">\n<b>html</b> <a href=\"https://example.org\">x</a>\n</div>\n");
    } else if (k < 95) {
        bs(b, "<!-- comment -->\n");
    } else if (k < 97) {
        bs(b, "    indented code\n");
    } else {
        bs(b, "> ");
        gen_para(b, r);
    }
}

static Str gen_doc(u64 *r) {
    Buf b = bnew(200000);
    i32 n = 1 + (i32)rnd(r, 30);
    for (i32 i = 0; i < n; i++) {
        gen_block(&b, r, 0);
        bc(&b, '\n');
    }
    return keep(bstr(&b));
}

static void random_op(u64 *r) {
    Size w = fk.window;
    switch (rnd(r, 30)) {
    case 0: case 1:
        resize(rndf(r, 160, 1500), rndf(r, 120, 900));
        break;
    case 2: case 3: case 4:
        app_wheel((Point){rndf(r, 0, w.w), rndf(r, 0, w.h)}, 0, rndf(r, -6, 6), 0);
        break;
    case 5: case 6: {
        static Key const ks[] = {KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END, KEY_SPACE,
                                 KEY_LEFT, KEY_RIGHT, KEY_BACKSPACE, KEY_F5, KEY_TAB, KEY_A};
        key(ks[rnd(r, countof(ks))], (u32)rnd(r, 8));
        break;
    }
    case 7:
        key(rnd(r, 2) ? KEY_PLUS : KEY_MINUS, MOD_PRIMARY);
        break;
    case 8:
        if (chance(r, 20)) {
            key(KEY_0, MOD_PRIMARY);
        }
        break;
    case 9: case 10: case 11:
        click((Point){rndf(r, 0, w.w), rndf(r, 0, w.h)});
        break;
    case 12: case 13: case 14: case 15:
        app_mouse_move((Point){rndf(r, -10, w.w + 10), rndf(r, -10, w.h + 10)}, 0);
        break;
    case 16:
        app_mouse_leave();
        break;
    case 17:
        fk.dark = !fk.dark;
        app_theme_changed();
        break;
    case 18: case 19:
        app_timer(ANIM_TIMER);
        break;
    case 20:
        app_wheel((Point){10, 10}, 0, rndf(r, -2, 2), MOD_PRIMARY);
        break;
    case 21:
        put("/r/doc.md", gen_doc(r));
        app_file_changed();
        break;
    case 22:
        if (chance(r, 30)) {
            app_file_dropped(chance(r, 70) ? S("/r/other.md") : S("/r/none.md"));
        }
        break;
    case 23: {
        Bar bar = bar_geometry();
        if (bar.show) {
            Point t = {w.w - 4, bar.thumb.y + 3};
            app_mouse_down(t, BUTTON_LEFT, 0);
            app_mouse_move((Point){t.x, rndf(r, 0, w.h)}, 0);
            app_mouse_up((Point){t.x, rndf(r, 0, w.h)}, BUTTON_LEFT, 0);
        }
        break;
    }
    case 24:
        if (V.state == VS_DOC && D.nleaves) {
            layout_all();
            check_layout();
        }
        break;
    case 25:
        app_mouse_down((Point){rndf(r, 0, w.w), rndf(r, 0, w.h)}, (Button)rnd(r, 3), (u32)rnd(r, 8));
        break;
    case 26:
        app_double_click((Point){rndf(r, 0, w.w), rndf(r, 0, w.h)}, 0);
        break;
    case 27:
        app_char(0x41);
        break;
    default:
        break;
    }
}

static void random_iteration(u64 seed, i64 iter) {
    u64 st = seed ^ ((u64)(iter + 1) * 0x9e3779b97f4a7c15ULL);
    u64 *r = &st;
    rand64(r);
    rand64(r);
    ctx.iter  = iter;
    ctx.phase = "random";
    rig_reset();
    setup_files();
    put_s("/r/other.md", "# Other\n\n[back](doc.md) [heading](#heading-1)\n");
    put("/r/doc.md", gen_doc(r));
    put_img("/r/img/a.fimg", 40, 30, 1, 0);
    put_img("/r/img/b.fimg", 300, 100, 1, 0);
    put_img("/r/img/big.fimg", 3000, 600, 1, 0);
    put_img("/r/img/anim.fimg", 64, 64, 3, 50);
    start("/r/doc.md", rndf(r, 160, 1500), rndf(r, 120, 900));
    paint();
    check_state();
    i32 nops = 20 + (i32)rnd(r, 60);
    for (i32 i = 0; i < nops; i++) {
        random_op(r);
        if (chance(r, 60)) {
            paint();
            check_state();
        }
    }
    if (V.state == VS_DOC) {
        resize(rndf(r, 160, 1500), rndf(r, 120, 900));
        layout_all();
        check_layout();
        check_state();
    }
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak after close", fk.live_paras, fk.live_images);
}


// Timing

// A megabyte of mixed Markdown.
static Str big_doc(void) {
    Buf b = bnew((iz)1100000);
    u64 st = 12345;
    u64 *r = &st;
    iz target = 1000000;
    i32 n = 0;
    while (b.len < target) {
        bs(&b, "## Section ");
        bnum(&b, n++);
        bs(&b, "\n\n");
        gen_para(&b, r);
        bc(&b, '\n');
        gen_para(&b, r);
        bc(&b, '\n');
        bs(&b, "- item one with `code`\n- item two with a [link](https://example.org)\n  - nested item\n\n");
        bs(&b, "```c\nint x = 1;\nreturn x;\n```\n\n");
        if (n%7 == 0) {
            bs(&b, "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n");
        }
        bs(&b, "> a quote with some words in it\n\n");
    }
    return bstr(&b);
}

static i64 now_us(void) {
    i64 f = 0, t = 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return t*1000000/f;
}

static void test_time(void) {
    ctx.phase = "time";
    rig_reset();
    Str doc = big_doc();
    put("/big.md", doc);
    i64 t0 = now_us();
    start("/big.md", 900, 700);
    i64 t1 = now_us();
    CHECK(V.state == VS_DOC, "big document did not load");
    paint();
    i64 t2 = now_us();
    i64 worst = 0, total = 0;
    for (i32 i = 0; i < 100; i++) {
        key(KEY_PGDN, 0);
        i64 a = now_us();
        paint();
        i64 d = now_us() - a;
        total += d;
        worst  = max(worst, d);
    }
    check_state();
    resize(500, 700);
    i64 t3 = now_us();
    paint();
    i64 t4 = now_us();
    Output *o = ctx.out;
    print(o, S("time: "));
    printi64(o, doc.len);
    print(o, S(" bytes, "));
    printi64(o, D.nleaves);
    print(o, S(" leaves: load "));
    printi64(o, (t1 - t0)/1000);
    print(o, S(" ms, first paint "));
    printi64(o, (t2 - t1)/1000);
    print(o, S(" ms, page down avg "));
    printi64(o, total/100/1000);
    print(o, S(" ms worst "));
    printi64(o, worst/1000);
    print(o, S(" ms, resize paint "));
    printi64(o, (t4 - t3)/1000);
    print(o, S(" ms (debug build with guard pages)\n"));
    flush(o);
    CHECK2((t1 - t0)/1000 < 1500, "loading 1 MB took too long (ms)", (t1 - t0)/1000, 0);
    CHECK2((t2 - t1)/1000 < 500, "first paint took too long (ms)", (t2 - t1)/1000, 0);
    CHECK2(worst/1000 < 250, "a scrolled paint took too long (ms)", worst/1000, 0);
    CHECK(fk.live_paras < 700, "lazy layout keeps few paragraphs");
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}


// Main

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
                base==16 && c>='a' && c<='f' ? c - 'a' + 10 : 99;
        if (d >= base) {
            return 0;
        }
        x = x*base + d;
    }
    *v = x;
    return 1;
}

static i32 run(Arena all) {
    rig.perm    = carve(&all, PERM_SIZE);
    rig.scratch = carve(&all, SCRATCH_SIZE);
    rig.data0   = carve(&all, DATA_SIZE);
    rig.gen0    = carve(&all, GEN_SIZE);
    Arena small = carve(&all, (iz)1<<20);
    ctx.out = newoutput(&small, GetStdHandle(STD_OUTPUT_HANDLE), 0, 1<<16);
    AddVectoredExceptionHandler(1, onfault);
    alive();
    CreateThread(0, 0, watchdog, 0, 0, 0);

    u64 seed = 0x5eed, iters = 300, start_i = 0;
    Str which = {0};
    Args args = getargs(&small);
    for (i32 i = 1; i < args.argc; i++) {
        Str  arg = args.argv[i];
        u64 *dst = 0;
        if (equals(arg, S("-t")) && i+1 < args.argc) {
            which = args.argv[++i];
            continue;
        } else if (equals(arg, S("-s"))) {
            dst = &seed;
        } else if (equals(arg, S("-n"))) {
            dst = &iters;
        } else if (equals(arg, S("-i"))) {
            dst = &start_i;
        }
        if (!dst || i+1>=args.argc || !parseu64(args.argv[++i], dst)) {
            print(ctx.out, S("usage: view-test [-s SEED] [-n ITERS] [-i START] [-t unit|random|time]\n"));
            flush(ctx.out);
            return 2;
        }
    }
    ctx.seed = seed;
    ctx.iter = -1;
    Output *o = ctx.out;
    u64 t0 = GetTickCount64();
    b32 all_tests = !which.len;
    if (all_tests || equals(which, S("unit"))) {
        test_all();
        print(o, S("unit: ok\n"));
    }
    if (all_tests || equals(which, S("random"))) {
        for (u64 i = 0; i < iters; i++) {
            random_iteration(seed, (i64)(start_i + i));
            alive();
        }
        ctx.iter  = -1;
        ctx.phase = 0;
        print(o, S("random: ok seed=0x"));
        printhex(o, seed);
        print(o, S(" iters="));
        printi64(o, (i64)iters);
        print(o, S("\n"));
    }
    if (all_tests || equals(which, S("time"))) {
        test_time();
    }
    print(o, S("view-test: ok, "));
    printi64(o, (i64)(GetTickCount64() - t0));
    print(o, S(" ms\n"));
    flush(o);
    return 0;
}

void mainCRTStartup(void) {
    iz   cap = PERM_SIZE + SCRATCH_SIZE + DATA_SIZE + GEN_SIZE + ((iz)2<<20);
    u8  *mem = VirtualAlloc(0, (uz)cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem+cap}));
}
