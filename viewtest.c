// Tests for the viewer: a separate unity build of the portable code (parser,
// highlighter, layout, view) on a fake platform (fakeos.c), with the debug
// checks on: UBSan traps, guard pages behind every allocation.
//   $ make view-test.exe
//   $ view-test.exe [-s SEED] [-n ITERS] [-i START] [-t WHICH]
//
// Three kinds of test:
//   unit     fixed documents and scripted input: layout invariants at many
//            widths, resizing, links, history, images, zoom, reload, leaks;
//            selecting (by mouse, words, lines, Shift+click, auto-scroll),
//            copying (golden strings for every block) and finding
//   random   random Markdown, random window sizes, scrolls, zooms, keys,
//            clicks, selection drags, copies and find sessions, with
//            invariants checked after every paint
//   time     a 1 MB document must load, paint, select all, copy and be
//            searched within a budget
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
    u64         checks;     // how many CHECKs passed
    u64         n_sel, n_copy, n_find, n_multi;    // what the random test got to see
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

#define CHECK(c, what) do { ctx.checks++; if (!(c)) fail(#c, what, 0, 0); } while (0)
#define CHECK2(c, what, a, b) do { ctx.checks++; if (!(c)) fail(#c, what, (i64)(a), (i64)(b)); } while (0)

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
        while (s.len > 0 && s.len < (iz)0x7fffffff && (s.data[s.len] & 0xc0) == 0x80) {
            s.len--;        // not in the middle of a character
        }
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
    fk_free(fk.clip);
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

static b32 valid_utf8(Str s);
static Str clip(void);
static void check_texts(void);

// A position names a place in the text: a leaf, a cell of it if a table,
// and a character boundary.
static void check_pos(DocPos p, char const *what) {
    CHECK2(p.leaf >= 0 && p.leaf < D.nleaves, what, p.leaf, D.nleaves);
    Leaf *l = D.leaves + p.leaf;
    Arena s = rig.scratch;
    RB rb;
    rb_init(&rb, &s);
    if (l->kind == LK_TABLE) {
        CHECK2(p.cell >= 0 && p.cell < l->n*table_ncols(l), what, p.cell, l->n);
    } else {
        CHECK2(p.cell == -1, what, p.cell, p.leaf);
    }
    Seg sg = seg_get(p.leaf, p.cell, &rb);
    CHECK2(p.off >= 0 && p.off <= sg.len, what, p.off, sg.len);
    CHECK2(p.off == sg.len || (sg.p[p.off] & 0xc0) != 0x80, what, p.off, sg.len);
}

// What is true of the selection, the find bar and the clipboard.
static void check_text_state(void) {
    if (fk.clipset) {
        ctx.n_copy++;
        CHECK(valid_utf8(clip()), "the clipboard text is not valid UTF-8");
        Str c = clip();
        CHECK(!icontains(c, S("\xEF\xBF\xBC")), "an image box got into the copied text");
        fk.clipset = 0;
    }
    if (fk.timer_on[SEL_TIMER]) {
        CHECK(V.sel.down && V.sel.timer, "auto-scroll timer without a drag");
    }
    CHECK(V.sel.timer == fk.timer_on[SEL_TIMER], "timer state matches");
    CHECK(D.sel_on == V.sel.on, "painter knows the selection");
    if (V.sel.on) {
        ctx.n_sel++;
        DocPos lo, hi;
        sel_bounds(&lo, &hi);
        CHECK(pos_cmp(lo, hi) < 0, "selection is ordered and not empty");
        check_pos(lo, "selection start");
        check_pos(hi, "selection end");
        CHECK(pos_cmp(D.sel_lo, lo) == 0 && pos_cmp(D.sel_hi, hi) == 0, "painter knows where");
    }
    Find *f = &V.find;
    CHECK(!f->stale || !f->open, "find results are fresh after a paint");
    CHECK(f->len >= 0 && f->len <= FIND_MAX && f->caret >= 0 && f->caret <= f->len, "find field");
    CHECK(valid_utf8((Str){f->text, f->len}), "find field text is valid UTF-8");
    CHECK(D.find_q.len == (f->open ? f->len : 0), "painter knows the query");
    if (f->open && f->have) {
        ctx.n_find++;
        ctx.n_multi += f->total > 1;
        CHECK2(f->n >= 1 && f->n <= f->total, "match number in range", f->n, f->total);
        check_pos(f->cur, "current match");
        Arena s = rig.scratch;
        RB rb;
        rb_init(&rb, &s);
        Seg sg = seg_get(f->cur.leaf, f->cur.cell, &rb);
        CHECK(f->cur.off + f->len <= sg.len, "the match is inside its text");
        CHECK(equals((Str){sg.p + sg.len + f->cur.off, f->len}, (Str){f->fold, f->len}), "the current match matches");
    } else if (f->open) {
        CHECK(f->total == 0 || f->len == 0, "no current match only without results");
    }
}

// What is true after any paint.
static void check_state(void) {
    if (V.state != VS_DOC) {
        return;
    }
    check_text_state();
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

// Where text is in window coordinates, after scrolling it into view: the
// left edge of its first character, and the right edge of the last one on
// that line.
static b32 goto_span(Str needle, Point *pos, Point *end) {
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
                if (end) {
                    *end = (Point){org.x + rc[0].x + rc[0].w - 1, pos->y};
                }
                return 1;
            }
        }
    }
    return 0;
}

static b32 goto_text(Str needle, Point *pos) {
    return goto_span(needle, pos, 0);
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

// Selection, copy and find

static Str clip(void) {
    return (Str){fk.clip, fk.cliplen};
}

static void print_esc(Output *o, Str s) {
    printc(o, '"');
    for (iz i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == '\n') {
            print(o, S("\\n"));
        } else if (c == '\t') {
            print(o, S("\\t"));
        } else if (c < 32) {
            print(o, S("\\x"));
            printhex(o, c);
        } else {
            printc(o, c);
        }
    }
    printc(o, '"');
}

static b32 valid_utf8(Str s) {
    while (s.len) {
        Utf8 d = utf8decode(s);
        if (d.rune == 0xfffd && s.len - d.tail.len == 1) {
            return 0;
        }
        s = d.tail;
    }
    return 1;
}

static void expect_str(Str got, char const *want, char const *what) {
    if (!equals(got, cstr(want))) {
        Output *o = ctx.out;
        print(o, S("--- "));
        print(o, cstr(what));
        print(o, S(" ---\ngot:  "));
        print_esc(o, got);
        print(o, S("\nwant: "));
        print_esc(o, cstr(want));
        print(o, S("\n"));
        fail("text differs", what, got.len, (i64)cstr(want).len);
    }
}

// Ctrl+C, and what it put on the clipboard.
static void expect_copy(char const *want, char const *what) {
    fk.clipset = 0;
    key(KEY_C, MOD_PRIMARY);
    CHECK2(fk.clipset, what, 0, 0);
    expect_str(clip(), want, what);
}

static void drag(Point a, Point b) {
    app_mouse_move(a, 0);
    app_mouse_down(a, BUTTON_LEFT, 0);
    app_mouse_move((Point){(a.x + b.x)/2 + 4, (a.y + b.y)/2}, 0);
    app_mouse_move(b, 0);
    app_mouse_up(b, BUTTON_LEFT, 0);
    paint();
}

// Select from the first character of one text to the last of another, the
// way a person would: by dragging. The whole document must fit the window.
static void select_text(char const *from, char const *to) {
    Point a, ae, b, be;
    if (!goto_span(cstr(to), &b, &be) || !goto_span(cstr(from), &a, &ae)) {
        print(ctx.out, S("select_text: "));
        print(ctx.out, cstr(from));
        print(ctx.out, S(" to "));
        print(ctx.out, cstr(to));
        print(ctx.out, S("\n"));
        CHECK(0, "text to select not found");
    }
    CHECK(V.scroll == 0, "test document does not fit the window");
    drag(a, be);
}

static void click_at(Point p, u32 mods) {
    app_mouse_move(p, mods);
    app_mouse_down(p, BUTTON_LEFT, mods);
    app_mouse_up(p, BUTTON_LEFT, mods);
}

// Double click the way Windows delivers it: down, up, double click, up.
static void dbl_click(Point p) {
    fk.now += 1000;
    click_at(p, 0);
    app_double_click(p, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
}

static char const doc_copy_md[] =
    "# Title and *more*\n"
    "\n"
    "Para one with **bold** and `code` and a [link](https://x.org). Second\n"
    "soft break.  \n"
    "Hard break line.\n"
    "\n"
    "- one\n"
    "- two\n"
    "  - nested\n"
    "    1. deep ordered\n"
    "- [ ] open\n"
    "- [x] done\n"
    "\n"
    "1. first\n"
    "2. second\n"
    "\n"
    "> quote line\n"
    ">\n"
    "> second para\n"
    "\n"
    "```c\n"
    "int a;\n"
    "\ttab\n"
    "```\n"
    "\n"
    "| A | B |\n"
    "|---|---|\n"
    "| 1 | 2 |\n"
    "| 3 | `x` |\n"
    "\n"
    "![alt text](img/a.fimg) end\n"
    "\n"
    "---\n"
    "\n"
    "Last para.\n";

static void test_copy(void) {
    ctx.phase = "copy";
    rig_reset();
    setup_files();
    put_s("/docs/copy.md", doc_copy_md);
    start("/docs/copy.md", 900, 1800);
    paint();
    CHECK(V.state == VS_DOC && max_scroll() == 0, "the document fits the window");
    CHECK(!V.sel.on, "nothing is selected at first");
    key(KEY_C, MOD_PRIMARY);
    CHECK(!fk.clipset, "copying nothing leaves the clipboard alone");

    // Everything, with every kind of block.
    key(KEY_A, MOD_PRIMARY);
    CHECK(V.sel.on, "ctrl+a selects");
    paint();
    expect_copy("Title and more\n"
                "\n"
                "Para one with bold and code and a link. Second soft break.\n"
                "Hard break line.\n"
                "\n"
                "- one\n"
                "- two\n"
                "  - nested\n"
                "    1. deep ordered\n"
                "[ ] open\n"
                "[x] done\n"
                "\n"
                "1. first\n"
                "2. second\n"
                "\n"
                "> quote line\n"
                ">\n"
                "> second para\n"
                "\n"
                "int a;\n"
                "    tab\n"
                "\n"
                "A\tB\n"
                "1\t2\n"
                "3\tx\n"
                "\n"
                "alt text end\n"
                "\n"
                "Last para.",
                "copy of everything");
    // The image is in the selection, so it is tinted too.
    CHECK(fk.nimage_draws == 1, "image drawn");
    b32 tinted = 0;
    Color sel = theme_light.sel;
    for (i32 i = 0; i < fk.nfill_log; i++) {
        FFill *f = fk.fill_log + i;
        Rect d = fk.image_draws[0].dst;
        if (f->c.r == sel.r && f->c.a == sel.a && near(f->r.x, d.x, 0.01f) && near(f->r.y, d.y, 0.01f) &&
            near(f->r.w, d.w, 0.01f) && near(f->r.h, d.h, 0.01f)) {
            tinted = 1;
        }
    }
    CHECK(tinted, "an image in the selection is tinted");
    // Ctrl+Insert copies as well; Escape clears.
    fk.clipset = 0;
    key(KEY_INSERT, MOD_PRIMARY);
    CHECK(fk.clipset, "ctrl+insert copies");
    key(KEY_ESCAPE, 0);
    CHECK(!V.sel.on, "escape clears the selection");
    fk.clipset = 0;
    key(KEY_C, MOD_PRIMARY);
    CHECK(!fk.clipset, "nothing to copy after escape");

    // Within one paragraph: just the text, no markers.
    select_text("bold", "code");
    expect_copy("bold and code", "partial paragraph");
    select_text("two", "two");
    expect_copy("two", "one list item selected without its marker");
    // Dragging up gives the same.
    {
        Point a, ae, b, be;
        CHECK(goto_span(S("bold"), &a, &ae) && goto_span(S("code"), &b, &be), "text");
        drag(be, a);
        expect_copy("bold and code", "partial paragraph, dragged backward");
    }
    // Across leaves: the first one partial has no marker, the rest do.
    select_text("Second", "deep");
    expect_copy("Second soft break.\n"
                "Hard break line.\n"
                "\n"
                "- one\n"
                "- two\n"
                "  - nested\n"
                "    1. deep",
                "partial across paragraph and list");
    select_text("nested", "done");
    expect_copy("  - nested\n    1. deep ordered\n[ ] open\n[x] done", "from the start of an item");
    select_text("ordered", "done");
    expect_copy("ordered\n[ ] open\n[x] done", "from the middle of an item");
    select_text("second para", "second para");
    expect_copy("second para", "inside a quote");
    select_text("quote line", "second para");
    expect_copy("> quote line\n>\n> second para", "quote across paragraphs");
    // Across a code block and a table.
    select_text("tab", "2");
    expect_copy("tab\n"
                "\n"
                "A\tB\n"
                "1\t2",
                "from code into a table");
    select_text("1", "x");
    expect_copy("1\t2\n3\tx", "table cells");
    select_text("A", "A");
    expect_copy("A", "a header cell");
    select_text("int a;", "tab");
    expect_copy("int a;\n    tab", "code lines");

    // Images are their alt text; partial selections of it.
    select_text("\xEF\xBF\xBC", "end");
    expect_copy("alt text end", "image and text");

    // Positions are in order and within the text.
    CHECK(pos_cmp(V.sel.anchor, V.sel.focus) != 0, "selection has an extent");

    // Clicking clears, and so does Shift-less clicking anywhere.
    Point p;
    CHECK(goto_text(S("Last"), &p), "text");
    click_at(p, 0);
    CHECK(!V.sel.on, "a click clears the selection");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
}

static void test_sel_click(void) {
    ctx.phase = "sel-click";
    rig_reset();
    setup_files();
    put_s("/docs/words.md",
          "alpha beta_gamma, delta42 \xC3\xBCn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 "
          "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E end\n"
          "\n"
          "- an item with words\n"
          "\n"
          "```c\n"
          "foo bar\n"
          "baz qux\n"
          "```\n"
          "\n"
          "[a link](https://example.org/l) and text\n"
          "\n"
          "| cell one | cell two |\n"
          "|---|---|\n"
          "| a b | c d |\n");
    start("/docs/words.md", 900, 900);
    paint();
    Point p, e;
    // Words.
    CHECK(goto_span(S("beta_gamma"), &p, &e), "text");
    dbl_click(p);
    expect_copy("beta_gamma", "double click selects a word, underscore included");
    CHECK(goto_span(S("alpha"), &p, &e), "text");
    dbl_click(p);
    expect_copy("alpha", "first word");
    dbl_click(e);
    expect_copy("alpha", "right half of the last letter");
    CHECK(goto_span(S("delta42"), &p, &e), "text");
    dbl_click((Point){p.x + 20, p.y});
    expect_copy("delta42", "letters and digits");
    CHECK(goto_span(S("\xC3\xBCn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9"), &p, &e), "text");
    dbl_click((Point){p.x + 12, p.y});
    expect_copy("\xC3\xBCn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9", "accented letters");
    CHECK(goto_span(S("\xE6\x97\xA5\xE6\x9C\xAC"), &p, &e), "text");
    dbl_click(p);
    expect_copy("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "a run of CJK");
    // A single other character: the comma, from either half.
    CHECK(goto_span(S(","), &p, &e), "text");
    dbl_click(p);
    expect_copy(",", "left half of a comma");
    dbl_click(e);
    expect_copy(",", "right half of a comma");
    // In code and table cells.
    CHECK(goto_span(S("bar"), &p, &e), "text");
    dbl_click(p);
    expect_copy("bar", "word in code");
    CHECK(goto_span(S("two"), &p, &e), "text");
    dbl_click(p);
    expect_copy("two", "word in a table cell");
    // Dragging after a double click extends by words.
    CHECK(goto_span(S("beta_gamma"), &p, &e), "text");
    Point far;
    CHECK(goto_span(S("delta42"), &far, &e), "text");
    click_at(p, 0);
    app_double_click(p, 0);
    app_mouse_move((Point){far.x + 20, far.y}, 0);
    app_mouse_up((Point){far.x + 20, far.y}, BUTTON_LEFT, 0);
    expect_copy("beta_gamma, delta42", "double click and drag selects whole words");

    // Triple click: the paragraph, or the line of code.
    fk.now = 10000;
    CHECK(goto_span(S("beta_gamma"), &p, &e), "text");
    dbl_click(p);
    fk.now += 120;
    app_mouse_down(p, BUTTON_LEFT, 0);
    expect_copy("alpha beta_gamma, delta42 \xC3\xBCn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 "
                "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E end", "triple click selects the paragraph");
    app_mouse_up(p, BUTTON_LEFT, 0);
    CHECK(goto_span(S("with"), &p, &e), "text");
    dbl_click(p);
    fk.now += 120;
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
    expect_copy("an item with words", "triple click on a list item: no marker");
    CHECK(goto_span(S("baz"), &p, &e), "text");
    dbl_click(p);
    fk.now += 120;
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
    expect_copy("baz qux", "triple click in code: a line");
    CHECK(goto_span(S("c d"), &p, &e), "text");
    dbl_click(p);
    fk.now += 120;
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
    expect_copy("c d", "triple click in a table cell: the cell");
    // Too slow, or far away: just a click.
    CHECK(goto_span(S("beta_gamma"), &p, &e), "text");
    dbl_click(p);
    fk.now += 900;
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_up(p, BUTTON_LEFT, 0);
    CHECK(!V.sel.on, "a slow third click is a plain click");
    dbl_click(p);
    fk.now += 100;
    app_mouse_down((Point){p.x + 100, p.y}, BUTTON_LEFT, 0);
    app_mouse_up((Point){p.x + 100, p.y}, BUTTON_LEFT, 0);
    CHECK(!V.sel.on, "a third click elsewhere is a plain click");

    // Shift+click extends from the last click or the anchor.
    CHECK(goto_span(S("beta_gamma"), &p, &e), "text");
    Point q, qe;
    CHECK(goto_span(S("alpha"), &q, &qe), "text");
    click_at(p, 0);
    CHECK(!V.sel.on, "click: no selection");
    click_at((Point){e.x, e.y}, MOD_SHIFT);
    expect_copy("beta_gamma", "shift+click selects from the last click");
    CHECK(goto_span(S("delta42"), &far, &e), "text");
    click_at((Point){e.x, e.y}, MOD_SHIFT);
    expect_copy("beta_gamma, delta42", "shift+click again extends");
    click_at(q, MOD_SHIFT);
    expect_copy("alpha ", "shift+click on the other side of the anchor");

    // Links: a click follows, a drag from a link selects.
    CHECK(goto_span(S("a link"), &p, &e), "text");
    click_at(p, 0);
    CHECK2(fk.nurls == 1, "link followed by a plain click", fk.nurls, 0);
    CHECK(goto_span(S("a link"), &p, &e), "text");
    CHECK(goto_span(S("text"), &far, &e), "text");
    drag(p, e);
    CHECK2(fk.nurls == 1, "a drag from a link does not follow it", fk.nurls, 0);
    expect_copy("a link and text", "selected from a link");
    // Shift+click on a link doesn't follow it either.
    CHECK(goto_span(S("a link"), &p, &e), "text");
    click_at(p, 0);
    CHECK2(fk.nurls == 2, "click again", fk.nurls, 0);
    click_at(p, MOD_SHIFT);
    CHECK2(fk.nurls == 2, "shift+click on a link doesn't follow", fk.nurls, 0);

    // Cursor: I-beam over text while selecting.
    CHECK(goto_span(S("alpha"), &p, &e), "text");
    app_mouse_move(p, 0);
    CHECK(fk.cursor == CURSOR_IBEAM, "I-beam over text");
    app_mouse_down(p, BUTTON_LEFT, 0);
    app_mouse_move((Point){p.x + 100, p.y + 300}, 0);
    CHECK(fk.cursor == CURSOR_IBEAM, "I-beam while dragging a selection");
    app_mouse_up((Point){p.x + 100, p.y + 300}, BUTTON_LEFT, 0);
    app_close();
    CHECK2(fk.live_paras == 0, "leak", fk.live_paras, 0);
}

static void test_sel_scroll(void) {
    ctx.phase = "sel-scroll";
    rig_reset();
    Buf b = bnew(1<<20);
    for (i32 i = 0; i < 150; i++) {
        bs(&b, "Paragraph ");
        bnum(&b, i);
        bs(&b, " alpha beta gamma.\n\n");
    }
    put("/d/s.md", bstr(&b));
    start("/d/s.md", 600, 300);
    paint();
    scroll_to(D.leaves[70].y);
    paint();
    layout_now();
    Point p = {100, 40};
    app_mouse_move(p, 0);
    app_mouse_down(p, BUTTON_LEFT, 0);
    i32 start_leaf = V.sel.anchor.leaf;
    CHECK(start_leaf >= 70 && start_leaf <= 72, "press in a paragraph near the top");
    CHECK(!fk.timer_on[SEL_TIMER], "no auto-scroll before the drag");
    // Out of the window at the bottom: a timer scrolls and extends.
    app_mouse_move((Point){100, 280}, 0);
    CHECK(!fk.timer_on[SEL_TIMER], "inside the window: no timer");
    app_mouse_move((Point){100, 320}, 0);
    CHECK(fk.timer_on[SEL_TIMER], "dragging below the window starts the timer");
    app_mouse_move((Point){100, 321}, 0);
    f32 s0 = V.scroll;
    app_timer(SEL_TIMER);
    paint();
    f32 d1 = V.scroll - s0;
    CHECK2(d1 > 0 && V.sel.focus.leaf >= start_leaf, "scrolls down while the mouse is below", (i64)d1, 0);
    i32 f1 = V.sel.focus.leaf;
    app_mouse_move((Point){100, 420}, 0);
    for (i32 i = 0; i < 20; i++) {
        app_timer(SEL_TIMER);
        paint();
    }
    CHECK2(V.sel.focus.leaf > f1 + 10, "the selection keeps extending", V.sel.focus.leaf, f1);
    CHECK(V.sel.focus.leaf <= D.nleaves - 1, "within the document");
    // Further out: faster.
    app_mouse_move((Point){100, 300 + 250}, 0);
    s0 = V.scroll;
    app_timer(SEL_TIMER);
    paint();
    f32 d2 = V.scroll - s0;
    CHECK2(d2 > d1*2, "speed grows with the distance", (i64)d2, (i64)d1);
    // Back inside: the timer stops.
    app_mouse_move((Point){100, 150}, 0);
    CHECK(!fk.timer_on[SEL_TIMER], "back in the window: the timer stops");
    // Above the window: scrolls up, and the selection shrinks and flips.
    app_mouse_move((Point){100, -30}, 0);
    CHECK(fk.timer_on[SEL_TIMER], "dragging above starts it again");
    f32 top = V.scroll;
    for (i32 i = 0; i < 400 && V.scroll > 0; i++) {
        app_timer(SEL_TIMER);
        paint();
        CHECK(V.sel.focus.leaf <= D.nleaves, "valid while scrolling up");
    }
    CHECK(V.scroll < top, "scrolled up");
    CHECK(V.scroll == 0, "all the way up");
    CHECK2(V.sel.focus.leaf <= 1, "the selection reaches the top", V.sel.focus.leaf, 0);
    CHECK(pos_cmp(V.sel.focus, V.sel.anchor) < 0, "now it runs backward");
    app_mouse_up((Point){100, -30}, BUTTON_LEFT, 0);
    CHECK(!fk.timer_on[SEL_TIMER], "released: no timer");
    fk.clipset = 0;
    key(KEY_C, MOD_PRIMARY);
    CHECK(fk.clipset && clip().len > 1000, "copied a lot");
    CHECK(valid_utf8(clip()), "valid text");
    CHECK(equals((Str){fk.clip, 11}, S("Paragraph 0")), "starts at the top");
    // The timer does nothing without a drag.
    app_timer(SEL_TIMER);
    CHECK(!fk.timer_on[SEL_TIMER], "stray tick");
    app_close();
    CHECK2(fk.live_paras == 0, "leak", fk.live_paras, 0);
}

static char const doc_find_md[] =
    "# Find Fixture\n"
    "\n"
    "The quick brown fox. Fox again, and FOX.\n"
    "\n"
    "- list fox\n"
    "- no match here\n"
    "\n"
    "| Animal | Note |\n"
    "|---|---|\n"
    "| fox | cell fox |\n"
    "\n"
    "```text\n"
    "code with fox inside\n"
    "second fox\n"
    "```\n"
    "\n"
    "> quoted FOX\n"
    "\n"
    "\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xC3\x89" "COLE \xC3\xA9" "cole\n"
    "\n"
    "Last paragraph without it.\n";

static void type_text(char const *s) {
    Str t = cstr(s);
    while (t.len) {
        Utf8 d = utf8decode(t);
        app_char(d.rune);
        t = d.tail;
    }
    paint();
}

static Str field_text(void) {
    return (Str){V.find.text, V.find.len};
}

static Str find_cur_folded(void) {
    Arena s = rig.scratch;
    RB rb;
    rb_init(&rb, &s);
    Seg sg = seg_get(V.find.cur.leaf, V.find.cur.cell, &rb);
    return (Str){sg.p + sg.len + V.find.cur.off, V.find.len};
}

// The current match is a match, and in the window.
static void check_find_here(char const *what) {
    Find *f = &V.find;
    CHECK2(f->have && f->n >= 1 && f->n <= f->total, what, f->n, f->total);
    CHECK(equals(find_cur_folded(), (Str){f->fold, f->len}), "the current match is the query");
    paint();
    Arena s = rig.scratch;
    RB rb;
    rb_init(&rb, &s);
    f32 top, h;
    CHECK(doc_range_y(f->cur, f->len, &rb, &top, &h), "the match has a place");
    f32 y = D.leaves[f->cur.leaf].y + top - V.scroll;
    CHECK2(y >= 0 && y + h <= V.size.h, "the current match is in view", (i64)y, (i64)V.size.h);
}

static i32 count_fills(Color c) {
    i32 n = 0;
    for (i32 i = 0; i < fk.nfill_log; i++) {
        Color f = fk.fill_log[i].c;
        n += f.r==c.r && f.g==c.g && f.b==c.b && f.a==c.a;
    }
    return n;
}

static void test_find(void) {
    ctx.phase = "find";
    rig_reset();
    setup_files();
    put_s("/docs/find.md", doc_find_md);
    start("/docs/find.md", 700, 200);
    paint();
    CHECK(max_scroll() > 300, "the fixture is longer than the window");
    CHECK(!V.find.open, "closed at first");
    key(KEY_F, MOD_PRIMARY);
    CHECK(V.find.open, "ctrl+f opens");
    paint();
    CHECK(fk.live_paras >= D.nlive, "paragraph count");
    type_text("fox");
    Find *f = &V.find;
    CHECK(equals(field_text(), S("fox")), "typing fills the field");
    CHECK2(f->total == 9 && f->n == 1, "9 matches, the first", f->total, f->n);
    check_find_here("first match");
    CHECK(D.find_q.len == 3 && D.find_ok, "the painter knows");
    // Next, in order, through every kind of block.
    DocPos prev = f->cur;
    for (i32 i = 2; i <= 9; i++) {
        key(KEY_ENTER, 0);
        CHECK2(f->n == i, "enter goes to the next match", f->n, i);
        CHECK(pos_cmp(f->cur, prev) > 0, "matches come in reading order");
        prev = f->cur;
        check_find_here("next match");
        Leaf *l = D.leaves + f->cur.leaf;
        if (i == 4) {
            CHECK(l->kind == LK_PARA && D.lt[f->cur.leaf].first.len == 2, "a list item");
        } else if (i == 5 || i == 6) {
            CHECK2(l->kind == LK_TABLE && f->cur.cell == (i == 5 ? 2 : 3), "in table cells", f->cur.cell, i);
        } else if (i == 7 || i == 8) {
            CHECK2(l->kind == LK_CODE, "in code", l->kind, i);
        } else if (i == 9) {
            CHECK(D.lt[f->cur.leaf].quotes == 1, "in a quote");
        }
    }
    key(KEY_ENTER, 0);
    CHECK2(f->n == 1 && f->total == 9, "wraps around to the first", f->n, f->total);
    check_find_here("wrapped");
    key(KEY_ENTER, MOD_SHIFT);
    CHECK2(f->n == 9, "shift+enter wraps backward", f->n, 9);
    check_find_here("wrapped back");
    key(KEY_ENTER, MOD_SHIFT);
    CHECK2(f->n == 8, "and goes back", f->n, 8);
    key(KEY_F3, 0);
    CHECK2(f->n == 9, "F3 goes on", f->n, 9);
    key(KEY_F3, 0);
    CHECK2(f->n == 1, "F3 wraps", f->n, 1);
    key(KEY_F3, MOD_SHIFT);
    CHECK2(f->n == 9, "shift+F3 goes back", f->n, 9);
    // Highlights: in a window that shows everything, one current and the rest.
    resize(700, 1400);
    paint();
    CHECK2(count_fills(theme_light.find_cur) == 1, "one current match", count_fills(theme_light.find_cur), 0);
    CHECK2(count_fills(theme_light.find) == 8, "the other matches", count_fills(theme_light.find), 0);
    key(KEY_ENTER, 0);
    paint();
    CHECK2(count_fills(theme_light.find_cur) == 1 && count_fills(theme_light.find) == 8, "the current one moved", 0, 0);
    fk.dark = 1;
    app_theme_changed();
    paint();
    CHECK2(count_fills(theme_dark.find_cur) == 1 && count_fills(theme_dark.find) == 8, "dark colors", 0, 0);
    fk.dark = 0;
    app_theme_changed();
    resize(700, 200);
    paint();

    // Case folding beyond ASCII, and no results.
    key(KEY_F, MOD_PRIMARY);
    CHECK(f->all, "ctrl+f again selects the field");
    type_text("\xC3\x89" "COLE");
    CHECK2(f->total == 2, "\xC3\x89" "COLE matches both cases", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("\xC3\x9C" "N\xC3\x8F" "C\xC3\x96" "D\xC3\x89");
    CHECK2(f->total == 1, "accented letters fold", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("zzz");
    CHECK2(f->total == 0 && !f->have && f->n == 0, "no results", f->total, 0);
    CHECK(!D.find_ok, "nothing is current");
    CHECK(f->pc != 0, "the count is shown");
    key(KEY_ENTER, 0);
    key(KEY_ENTER, MOD_SHIFT);
    CHECK(f->total == 0, "stepping through nothing");
    // Spaces, and a match must not cross leaves or cells.
    key(KEY_A, MOD_PRIMARY);
    type_text("fox. fox");
    CHECK2(f->total == 1, "a match with a space", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("find fixture");
    CHECK2(f->total == 1, "across a heading's words", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("fixture the quick");
    CHECK2(f->total == 0, "not across leaves", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("fox fox");
    CHECK2(f->total == 0, "not across table cells", f->total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("o");
    CHECK2(f->total > 9, "a short query", f->total, 0);
    // Matches don't overlap.
    put_s("/docs/aaaa.md", "aaaaa\n\nAAaa\n");
    app_file_dropped(S("/docs/aaaa.md"));
    paint();
    CHECK(f->open, "the bar stays when another document opens");
    key(KEY_A, MOD_PRIMARY);
    type_text("aa");
    CHECK2(f->total == 2 + 2, "overlapping matches are counted once", f->total, 0);
    key(KEY_ESCAPE, 0);
    app_file_dropped(S("/docs/find.md"));
    paint();

    // Editing the field.
    key(KEY_F, MOD_PRIMARY);
    key(KEY_A, MOD_PRIMARY);
    type_text("abc");
    CHECK(equals(field_text(), S("abc")) && f->caret == 3 && !f->all, "typed abc");
    key(KEY_LEFT, 0);
    key(KEY_LEFT, 0);
    CHECK2(f->caret == 1, "left", f->caret, 1);
    type_text("X");
    CHECK(equals(field_text(), S("aXbc")) && f->caret == 2, "insert at the caret");
    key(KEY_BACKSPACE, 0);
    CHECK(equals(field_text(), S("abc")) && f->caret == 1, "backspace");
    key(KEY_END, 0);
    CHECK(f->caret == 3, "end");
    key(KEY_RIGHT, 0);
    CHECK(f->caret == 3, "right at the end");
    key(KEY_HOME, 0);
    CHECK(f->caret == 0, "home");
    key(KEY_BACKSPACE, 0);
    CHECK(equals(field_text(), S("abc")), "backspace at the start");
    key(KEY_DELETE, 0);
    CHECK(equals(field_text(), S("bc")), "delete");
    key(KEY_END, 0);
    type_text(" def ghi");
    key(KEY_BACKSPACE, MOD_PRIMARY);
    CHECK(equals(field_text(), S("bc def ")), "ctrl+backspace deletes a word");
    key(KEY_BACKSPACE, MOD_PRIMARY);
    CHECK(equals(field_text(), S("bc ")), "and again");
    key(KEY_LEFT, MOD_PRIMARY);
    CHECK2(f->caret == 0, "ctrl+left", f->caret, 0);
    key(KEY_RIGHT, MOD_PRIMARY);
    CHECK2(f->caret == 3, "ctrl+right", f->caret, 3);
    // Multi-byte characters move and delete as one.
    key(KEY_A, MOD_PRIMARY);
    type_text("a\xC3\xA9" "b");
    key(KEY_LEFT, 0);
    key(KEY_LEFT, 0);
    CHECK2(f->caret == 1, "left over a character", f->caret, 1);
    key(KEY_DELETE, 0);
    CHECK(equals(field_text(), S("ab")), "delete a whole character");
    // Space is text in the field, it does not scroll the document.
    f32 sc = V.scroll;
    key(KEY_SPACE, 0);
    CHECK(V.scroll == sc, "space does not scroll while the field has the keys");
    type_text(" ");
    CHECK(equals(field_text(), S("a b")), "space typed");
    // Ctrl+A and typing replace; backspace on a selection clears it.
    key(KEY_A, MOD_PRIMARY);
    CHECK(f->all, "ctrl+a selects the field text");
    key(KEY_BACKSPACE, 0);
    CHECK(f->len == 0 && !D.find_q.len, "backspace deletes the selected text");
    // Clipboard: copy and paste in the field.
    type_text("fox");
    key(KEY_A, MOD_PRIMARY);
    fk.clipset = 0;
    key(KEY_C, MOD_PRIMARY);
    expect_str(clip(), "fox", "ctrl+c in the field copies its text");
    key(KEY_BACKSPACE, 0);
    key(KEY_V, MOD_PRIMARY);
    CHECK(equals(field_text(), S("fox")) && f->total == 9, "ctrl+v pastes");
    key(KEY_V, MOD_PRIMARY);
    CHECK(equals(field_text(), S("foxfox")), "and again, at the caret");
    key(KEY_A, MOD_PRIMARY);
    key(KEY_BACKSPACE, 0);
    // A pasted line break ends the text.
    fk_free(fk.clip);
    fk.clip = fk_alloc(10, 1);
    memcpy(fk.clip, "ab\ncd", 5);
    fk.cliplen = 5;
    key(KEY_V, MOD_PRIMARY);
    CHECK(equals(field_text(), S("ab")), "paste takes the first line");
    key(KEY_A, MOD_PRIMARY);
    key(KEY_BACKSPACE, 0);

    // Escape closes and clears the highlights; then it clears the selection.
    type_text("fox");
    key(KEY_A, MOD_PRIMARY);        // field text
    key(KEY_ESCAPE, 0);
    CHECK(!f->open && !D.find_q.len && !D.find_ok, "escape closes the bar");
    paint();
    CHECK2(count_fills(theme_light.find) + count_fills(theme_light.find_cur) == 0, "no highlights after escape", 0, 0);
    CHECK(fk.live_paras == D.nlive, "the bar's paragraphs are freed");
    // The text stays for next time.
    key(KEY_F, MOD_PRIMARY);
    CHECK(f->open && equals(field_text(), S("fox")) && f->all, "reopened with the old text selected");
    CHECK2(f->total == 9, "and searching", f->total, 0);
    key(KEY_ESCAPE, 0);
    // F3 opens it when it is closed.
    key(KEY_F3, 0);
    CHECK(f->open, "F3 opens the bar");
    key(KEY_ESCAPE, 0);

    // It starts from the reading position.
    key(KEY_ESCAPE, 0);
    Leaf *code = 0;
    for (i32 i = 0; i < D.nleaves; i++) {
        if (D.leaves[i].kind == LK_CODE) {
            code = D.leaves + i;
        }
    }
    CHECK(code != 0, "code leaf");
    scroll_to(code->y);
    paint();
    key(KEY_F, MOD_PRIMARY);
    key(KEY_A, MOD_PRIMARY);
    type_text("fox");
    CHECK2(f->n == 7, "starts at the first match on or below the top of the window", f->n, 7);
    CHECK(D.leaves[f->cur.leaf].kind == LK_CODE, "in the code block");
    // Typing more keeps the match, as long as it still matches.
    key(KEY_A, MOD_PRIMARY);
    type_text("f");
    key(KEY_END, 0);
    type_text("o");
    CHECK2(D.leaves[f->cur.leaf].kind == LK_CODE && f->cur.off == 10, "typing keeps the current match", f->cur.off, 0);
    key(KEY_BACKSPACE, 0);
    CHECK2(D.leaves[f->cur.leaf].kind == LK_CODE && f->cur.off == 10, "and so does deleting", f->cur.off, 0);
    key(KEY_ESCAPE, 0);

    // A selection of one line fills the field.
    key(KEY_HOME, 0);
    paint();
    Point p, e;
    CHECK(goto_span(S("quick"), &p, &e), "text");
    dbl_click(p);
    key(KEY_F, MOD_PRIMARY);
    CHECK(equals(field_text(), S("quick")) && f->total == 1 && f->n == 1, "selected word becomes the query");
    check_find_here("pre-filled");
    key(KEY_ESCAPE, 0);
    key(KEY_A, MOD_PRIMARY);
    key(KEY_F, MOD_PRIMARY);
    CHECK(equals(field_text(), S("quick")), "a many-line selection is not used");
    key(KEY_ESCAPE, 0);
    key(KEY_ESCAPE, 0);

    // Keeping the current match: reload, zoom, resize, theme.
    key(KEY_HOME, 0);
    paint();
    key(KEY_F, MOD_PRIMARY);
    key(KEY_A, MOD_PRIMARY);
    type_text("fox");
    for (i32 i = 0; i < 3; i++) {
        key(KEY_ENTER, 0);
    }
    CHECK2(f->n == 4, "at the fourth", f->n, 4);
    DocPos at = f->cur;
    key(KEY_PLUS, MOD_PRIMARY);
    key(KEY_PLUS, MOD_PRIMARY);
    paint();
    CHECK(f->n == 4 && f->total == 9 && pos_cmp(f->cur, at) == 0, "zoom keeps it");
    resize(400, 300);
    paint();
    CHECK(f->n == 4 && f->total == 9 && pos_cmp(f->cur, at) == 0, "resize keeps it");
    fk.dark = 1;
    app_theme_changed();
    paint();
    CHECK(f->n == 4 && f->total == 9 && pos_cmp(f->cur, at) == 0, "theme change keeps it");
    app_file_changed();
    CHECK(f->stale, "reload makes the results stale");
    paint();
    CHECK(!f->stale && f->n == 4 && f->total == 9 && pos_cmp(f->cur, at) == 0, "reload keeps it");
    check_find_here("after reload");
    // The file grows by a match at the end: the number stays.
    Buf b2 = bnew(1<<16);
    bs(&b2, doc_find_md);
    bs(&b2, "\nfox at the end\n");
    put("/docs/find.md", bstr(&b2));
    app_file_changed();
    paint();
    CHECK2(f->total == 10 && f->n == 4, "reload of a changed file", f->total, f->n);
    // Buttons with the mouse.
    FindGeo g = find_geo();
    click_at((Point){g.next.x + 5, g.next.y + 5}, 0);
    CHECK2(f->n == 5, "the next button", f->n, 5);
    click_at((Point){g.prev.x + 5, g.prev.y + 5}, 0);
    click_at((Point){g.prev.x + 5, g.prev.y + 5}, 0);
    CHECK2(f->n == 3, "the previous button", f->n, 3);
    click_at((Point){g.field.x + 3, g.field.y + 5}, 0);
    CHECK(f->caret == 0 && !f->all, "a click in the field puts the caret there");
    CHECK(!V.sel.on, "and does not select the document");
    app_mouse_move((Point){g.field.x + 3, g.field.y + 5}, 0);
    CHECK(fk.cursor == CURSOR_IBEAM, "I-beam over the field");
    app_mouse_move((Point){g.next.x + 5, g.next.y + 5}, 0);
    CHECK(fk.cursor == CURSOR_HAND, "hand over a button");
    click_at((Point){g.close.x + 5, g.close.y + 5}, 0);
    CHECK(!f->open, "the close button");
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
}

// Text offsets of leaves with images in them: the images are alt text.
static void test_sel_images(void) {
    ctx.phase = "sel-images";
    rig_reset();
    setup_files();
    put_s("/docs/imgsel.md",
          "before ![alt one](img/a.fimg) middle ![x](img/b.fimg) after\n"
          "\n"
          "| c1 | c2 |\n"
          "|---|---|\n"
          "| ![in cell](img/a.fimg) tail | plain |\n");
    start("/docs/imgsel.md", 900, 600);
    paint();
    layout_all();
    CHECK(fk.nimage_draws >= 3, "images shown");
    key(KEY_A, MOD_PRIMARY);
    expect_copy("before alt one middle x after\n\nc1\tc2\nin cell tail\tplain", "images as alt text");
    // Select from the image to the end of "middle": the alt text is whole or not there.
    Point p, e;
    Point b0, b0e;
    CHECK(goto_span(S("middle"), &p, &e), "text");
    CHECK(goto_span(S("before"), &b0, &b0e), "text");
    drag(b0, e);
    expect_copy("before alt one middle", "an image inside the selection");
    // Find sees alt text and highlights through the image.
    key(KEY_ESCAPE, 0);
    key(KEY_F, MOD_PRIMARY);
    type_text("alt one");
    CHECK2(V.find.total == 1, "alt text is searchable", V.find.total, 0);
    type_text(" middle");
    CHECK2(V.find.total == 1, "across an image", V.find.total, 0);
    key(KEY_A, MOD_PRIMARY);
    type_text("tail");
    CHECK2(V.find.total == 1 && D.leaves[V.find.cur.leaf].kind == LK_TABLE, "after an image in a cell", V.find.total, 0);
    check_find_here("in a cell with an image");
    // Offsets turn into positions and back.
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        if (!(l->flags & LF_IMAGES)) {
            continue;
        }
        Arena s = rig.scratch;
        RB rb;
        rb_init(&rb, &s);
        i32 cell = l->kind == LK_TABLE ? 2 : -1;
        Seg sg = seg_get(i, cell, &rb);
        seg_boxes(l, cell, &rb);
        for (iz o = 0; o <= sg.len; o++) {
            if (o < sg.len && (sg.p[o] & 0xc0) == 0x80) {
                continue;
            }
            iz pp = para_of_plain(&rb, o);
            CHECK2(pp >= 0, "paragraph offset", i, o);
            CHECK2(plain_of_para(&rb, pp) >= o - 20 && plain_of_para(&rb, pp) <= o + 20, "offsets round trip near", i, o);
        }
    }
    app_close();
    CHECK2(fk.live_paras == 0 && fk.live_images == 0, "leak", fk.live_paras, fk.live_images);
}

// Text and layout agree on every leaf.
static void check_texts(void) {
    if (V.state != VS_DOC) {
        return;
    }
    Arena s = rig.scratch;
    RB rb;
    rb_init(&rb, &s);
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        if (l->flags & LF_FAILED) {
            continue;
        }
        i32 n = l->kind == LK_TABLE ? l->tb->nrows*l->tb->ncols : 1;
        for (i32 k = 0; k < n; k++) {
            Paragraph *p = l->kind == LK_TABLE ? l->tb->cell[k] : l->p;
            i32 cell = l->kind == LK_TABLE ? k : -1;
            Seg sg = seg_get(i, cell, &rb);
            iz len = p ? os_para_length(p) : 0;
            if (l->flags & LF_IMAGES) {
                seg_boxes(l, cell, &rb);
                len = plain_of_para(&rb, len);
            }
            if (sg.len != len) {
                Output *o = ctx.out;
                print(o, S("kind "));
                printi64(o, l->kind);
                print(o, S(" flags "));
                printi64(o, l->flags);
                print(o, S(" plain "));
                printi64(o, sg.len);
                print(o, S(" para "));
                printi64(o, len);
                print(o, S(" "));
                print_esc(o, (Str){sg.p, sg.len});
                if (p) {
                    print(o, S(" "));
                    print_esc(o, (Str){p->text, p->len});
                }
                print(o, S("\n"));
            }
            CHECK2(sg.len == len, "plain text and paragraph text differ in length", i, k);
            CHECK2(valid_utf8((Str){sg.p, sg.len}), "plain text is not valid UTF-8", i, k);
            if (p && !(l->flags & LF_IMAGES)) {
                CHECK2(equals((Str){sg.p, sg.len}, (Str){p->text, len}), "plain text and paragraph text differ", i, k);
            }
        }
    }
}

static void test_texts(void) {
    ctx.phase = "texts";
    rig_reset();
    setup_files();
    start("/docs/all.md", 900, 700);
    paint();
    for (i32 zi = 0; zi < (i32)countof(test_zooms); zi++) {
        set_zoom(test_zooms[zi]);
        for (i32 wi = 0; wi < (i32)countof(test_widths); wi++) {
            resize(test_widths[wi], 600);
            layout_all();
            check_texts();
        }
    }
    app_close();
    CHECK(fk.live_paras == 0, "leak");
}

// Nothing to select, copy or find, and no crash.
static void test_sel_empty(void) {
    ctx.phase = "sel-empty";
    rig_reset();
    put_s("/e/empty.md", "");
    put_s("/e/comment.md", "<!-- nothing to see -->\n");
    put_s("/e/rule.md", "---\n");
    char const *const paths[] = {"/e/empty.md", "/e/comment.md", "/e/rule.md"};
    for (i32 i = 0; i < 3; i++) {
        start(paths[i], 600, 400);
        paint();
        CHECK(V.state == VS_DOC, "loaded");
        Point p = {100, 100};
        click_at(p, 0);
        click_at(p, MOD_SHIFT);
        dbl_click(p);
        fk.now += 100;
        app_mouse_down(p, BUTTON_LEFT, 0);
        app_mouse_move((Point){300, 500}, 0);
        app_timer(SEL_TIMER);
        app_mouse_up((Point){300, 500}, BUTTON_LEFT, 0);
        key(KEY_A, MOD_PRIMARY);
        fk.clipset = 0;
        key(KEY_C, MOD_PRIMARY);
        CHECK(!fk.clipset || clip().len == 0, "nothing was copied");
        key(KEY_F, MOD_PRIMARY);
        type_text("x");
        CHECK(V.find.open && V.find.total == 0, "nothing found");
        key(KEY_ENTER, 0);
        key(KEY_ESCAPE, 0);
        paint();
        check_state();
        app_close();
        CHECK(fk.live_paras == 0, "leak");
    }
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
    test_texts();
    test_copy();
    test_sel_click();
    test_sel_images();
    test_sel_scroll();
    test_find();
    test_sel_empty();
}


// Random documents

static char const *const words[] = {
    "alpha", "beta", "gamma", "delta", "lorem", "ipsum", "dolor", "sit", "amet", "the", "quick",
    "brown", "fox", "jumps", "over", "lazy", "dog", "日本語", "中文", "😀", "ünï" "cödé", "a", "of",
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
    switch (rnd(r, 50)) {
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
            check_texts();
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
    case 30: case 31: case 32: {
        // Drag a selection, sometimes out of the window, with the timer ticking
        Point a = {rndf(r, 0, w.w), rndf(r, 0, w.h)};
        app_mouse_move(a, 0);
        app_mouse_down(a, BUTTON_LEFT, chance(r, 15) ? MOD_SHIFT : 0);
        i32 moves = 1 + (i32)rnd(r, 5);
        for (i32 i = 0; i < moves; i++) {
            Point b = {rndf(r, -20, w.w + 20), rndf(r, -80, w.h + 80)};
            app_mouse_move(b, 0);
            for (i32 k = (i32)rnd(r, 4); k > 0; k--) {
                app_timer(SEL_TIMER);
            }
            if (chance(r, 20)) {
                paint();
                check_state();
            }
        }
        if (chance(r, 90)) {
            app_mouse_up((Point){rndf(r, -20, w.w + 20), rndf(r, -80, w.h + 80)}, BUTTON_LEFT, 0);
        }
        break;
    }
    case 33:
        key(KEY_A, MOD_PRIMARY);
        break;
    case 34:
        fk.clipset = 0;
        key(chance(r, 70) ? KEY_C : KEY_INSERT, MOD_PRIMARY);
        break;
    case 35:
        key(KEY_ESCAPE, 0);
        break;
    case 36: {
        Point p = {rndf(r, 0, w.w), rndf(r, 0, w.h)};
        fk.now += rnd(r, 3) ? 50 : 900;
        if (chance(r, 50)) {
            app_mouse_down(p, BUTTON_LEFT, 0);
            app_mouse_up(p, BUTTON_LEFT, 0);
        }
        app_double_click(p, 0);
        app_mouse_up(p, BUTTON_LEFT, 0);
        if (chance(r, 60)) {
            fk.now += 100;
            app_mouse_down(p, BUTTON_LEFT, 0);
            app_mouse_up(p, BUTTON_LEFT, 0);
        }
        break;
    }
    case 37: case 38:
        key(chance(r, 70) ? KEY_F : KEY_F3, chance(r, 70) ? MOD_PRIMARY : 0);
        break;
    case 39: case 40: case 41: {
        // Type: a word, or part of one, or something odd
        if (chance(r, 70)) {
            Str t = cstr(words[rnd(r, countof(words))]);
            iz  n = chance(r, 50) ? t.len : 1 + (iz)rnd(r, (u64)t.len);
            while (n > 0 && n < t.len && (t.data[n] & 0xc0) == 0x80) {
                n--;
            }
            for (Str u = {t.data, n}; u.len;) {
                Utf8 d = utf8decode(u);
                app_char(chance(r, 10) ? d.rune - 32*(d.rune >= 'a' && d.rune <= 'z') : d.rune);
                u = d.tail;
            }
        } else {
            static i32 const odd[] = {' ', 0x7f, 0x1f, 0xfffd, 0x10000, 0x1f600, 0xd800, 'A', 0xdc00, 0x110000, -1};
            app_char(odd[rnd(r, countof(odd))]);
        }
        break;
    }
    case 42: case 43: {
        static Key const fk_[] = {KEY_ENTER, KEY_BACKSPACE, KEY_DELETE, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
                                  KEY_F3, KEY_V, KEY_C, KEY_X, KEY_A, KEY_G, KEY_INSERT, KEY_SPACE, KEY_ESCAPE};
        key(fk_[rnd(r, countof(fk_))], (u32)rnd(r, 8));
        break;
    }
    case 44:
        app_timer(SEL_TIMER);
        break;
    case 45:
        fk.now += rnd(r, 1200);
        break;
    case 46:
        if (chance(r, 50)) {
            fk_free(fk.clip);
            fk.clip    = fk_alloc(40, 1);
            fk.cliplen = 0;
            for (iz n = (iz)rnd(r, 20); n > 0; n--) {
                static Str const pool[] = {S("a"), S("b"), S(" "), S("\n"), S("\r\n"), S("\xC3\xA9"), S("x")};
                Str t = pool[rnd(r, countof(pool))];
                memcpy(fk.clip + fk.cliplen, t.data, (uz)t.len);
                fk.cliplen += t.len;
            }
        }
        break;
    case 47:
        if (V.find.open && V.find.len && chance(r, 70)) {
            Point q = {rndf(r, 0, w.w), rndf(r, 0, 60)};
            click_at(q, 0);
        }
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
        check_texts();
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

    // Find on the whole document: the first key makes the text of every
    // leaf, the rest only search it.
    key(KEY_HOME, 0);
    paint();
    key(KEY_F, MOD_PRIMARY);
    i64 first = 0, rest = 0;
    char const *query = "dolor";
    for (i32 i = 0; query[i]; i++) {
        i64 a = now_us();
        app_char(query[i]);
        i64 d = now_us() - a;
        if (i == 0) {
            first = d;
        } else {
            rest = max(rest, d);
        }
    }
    CHECK2(V.find.total > 1000, "found many matches", V.find.total, 0);
    i64 step = 0;
    for (i32 i = 0; i < 20; i++) {
        i64 a = now_us();
        key(KEY_ENTER, i%3 ? 0 : MOD_SHIFT);
        step = max(step, now_us() - a);
        paint();
    }
    i64 matches = V.find.total;
    key(KEY_ESCAPE, 0);
    CHECK(fk.live_paras < 700, "find does not make paragraphs for the whole document");

    // Select everything and copy it, from a freshly loaded document.
    app_file_changed();
    paint();
    i64 s0 = now_us();
    key(KEY_A, MOD_PRIMARY);
    paint();
    i64 s1 = now_us();
    fk.clipset = 0;
    key(KEY_C, MOD_PRIMARY);
    i64 s2 = now_us();
    CHECK(fk.clipset && clip().len > doc.len/2 && clip().len < doc.len, "everything was copied");
    CHECK(valid_utf8(clip()), "copied text is valid");
    CHECK(fk.live_paras < 700, "select all and copy do not make paragraphs for the whole document");
    i64 s3 = now_us();
    key(KEY_C, MOD_PRIMARY);
    i64 s4 = now_us();
    print(o, S("time: find: first key "));
    printi64(o, first/1000);
    print(o, S(" ms (all text made), next keys worst "));
    printi64(o, rest/1000);
    print(o, S(" ms, next/previous worst "));
    printi64(o, step/1000);
    print(o, S(" ms, "));
    printi64(o, matches);
    print(o, S(" matches; select all "));
    printi64(o, (s1 - s0)/1000);
    print(o, S(" ms, copy "));
    printi64(o, (s2 - s1)/1000);
    print(o, S(" ms (then "));
    printi64(o, (s4 - s3)/1000);
    print(o, S(" ms), "));
    printi64(o, clip().len);
    print(o, S(" bytes\n"));
    flush(o);
    CHECK2(first/1000 < 1500, "first find key took too long (ms)", first/1000, 0);
    CHECK2(rest/1000 < 100, "find key took too long (ms)", rest/1000, 0);
    CHECK2(step/1000 < 100, "find next took too long (ms)", step/1000, 0);
    CHECK2((s1 - s0)/1000 < 500, "select all took too long (ms)", (s1 - s0)/1000, 0);
    CHECK2((s2 - s1)/1000 < 1500, "copy took too long (ms)", (s2 - s1)/1000, 0);
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
        print(o, S("unit: ok, "));
        printi64(o, (i64)ctx.checks);
        print(o, S(" checks\n"));
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
        print(o, S(" (seen: selections "));
        printi64(o, (i64)ctx.n_sel);
        print(o, S(", copies "));
        printi64(o, (i64)ctx.n_copy);
        print(o, S(", find matches "));
        printi64(o, (i64)ctx.n_find);
        print(o, S(", of those with more than one "));
        printi64(o, (i64)ctx.n_multi);
        print(o, S(")\n"));
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
