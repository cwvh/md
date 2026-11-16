// Tests for md: a separate unity build (everything but main.c) with the
// debug checks on: UBSan traps, guard pages behind every allocation,
// poisoned arenas.
//   $ make test
//   $ md-test.exe [-s SEED] [-n ITERS] [-i START] [-d DIR] [-t WHICH] [-v]
//
// Three kinds of test:
//   spec     the CommonMark 0.31.2 examples and the GFM extension examples,
//            rendered to HTML by the test-only printer below and compared
//   random   Markdown-ish noise: no crash, a well-formed tree, and the same
//            tree for the same text with other line endings
//   path     inputs that make naive parsers quadratic, with a time limit
//
// Iteration i of seed s is self-contained: it derives its own generator from
// (s, i), so "-s S -i I -n 1 -t random" replays exactly it.
#include "base.c"
#include "win32.c"
#include "md.c"

// Built with -DDEBUG (the Makefile does) every allocation gets guard pages.
// Without it, the same tests run against the release allocator.
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

static void poison(Arena a) {
    os_protect(a.beg, a.end - a.beg, 1);
    memset(a.beg, 0xcc, (uz)(a.end - a.beg));
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

// Index chosen with the given relative weights.
static i32 weighted(u64 *s, i32 const *w, i32 n) {
    i32 total = 0;
    for (i32 i = 0; i < n; i++) {
        total += w[i];
    }
    i32 x = (i32)rnd(s, (u64)total);
    for (i32 i = 0; i < n; i++) {
        if (x < w[i]) {
            return i;
        }
        x -= w[i];
    }
    return n - 1;
}


// Reporting

typedef struct {
    u64         seed;
    i64         iter;       // -1 outside the random tests
    char const *phase;
    Str         input;      // dumped on failure
    Output     *out;
    b32         dead;
    volatile u64 progress;  // tick of the last sign of life
} Ctx;
static Ctx ctx;

static Str cstr(char const *s) {
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

// Print a string with the invisible characters spelled out.
static void dumpstr(Output *o, Str s, iz limit) {
    iz lim = min(s.len, limit);
    for (iz i = 0; i < lim; i++) {
        u8 c = s.data[i];
        if (c == '\n') {
            print(o, S("\\n\n"));
        } else if (c == '\r') {
            print(o, S("\\r"));
        } else if (c == '\t') {
            print(o, S("\\t"));
        } else if (c<0x20 || c==0x7f) {
            print(o, S("\\x"));
            printhex(o, c, 2);
        } else {
            printc(o, c);
        }
    }
    if (lim < s.len) {
        print(o, S("..."));
    }
    print(o, S("\n"));
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
        print(o, S("\n  reproduce: md-test.exe -t random -s 0x"));
        printhex(o, ctx.seed, 1);
        print(o, S(" -i "));
        printi64(o, ctx.iter);
        print(o, S(" -n 1   (gdb: gdb --args md-test.exe ...)"));
    }
    print(o, S("\n"));
    if (ctx.input.len) {
        print(o, S("  input ("));
        printi64(o, ctx.input.len);
        print(o, S(" bytes):\n"));
        dumpstr(o, ctx.input, 4000);
    }
    flush(o);
    ExitProcess(1);
}

static _Noreturn void fail(char const *check, char const *what, i64 a, i64 b) {
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

// A fault (guard page, UBSan trap, assert) gets the same report as a failed
// check, so the seed and the input are never lost.
static i32 onfault(ExceptionPointers *ep) {
    ExceptionRecord *e = ep->rec;
    if ((e->code & 0xc0000000u) != 0xc0000000u || ctx.dead) {
        return 0;  // not an error, e.g. a debugger message
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
        if (GetTickCount64() - ctx.progress > 30000) {
            ctx.dead = 1;
            print(ctx.out, S("HANG: no progress for 30 seconds\n"));
            context();
        }
    }
}

static void alive(void) {
    ctx.progress = GetTickCount64();
}


// Memory

enum {
    TOTAL_SIZE   = (iz)640<<20,
    PERM_SIZE    = (iz)256<<20,
    SCRATCH_SIZE = (iz)128<<20,
    HTML_SIZE    = (iz)64<<20,
    GEN_SIZE     = (iz)8<<20,
    SMALL_SIZE   = (iz)512<<10,       // the random tests' arenas
};

static Arena carve(Arena *a, iz size) {
    a->beg = (u8 *)(((uptr)a->beg + PAGE - 1) & ~(uptr)(PAGE - 1));
    assert(size%PAGE == 0);
    assert(size <= a->end - a->beg);
    Arena r = {a->beg, a->beg+size};
    a->beg += size;
    return r;
}

typedef struct {
    Arena perm, scratch;    // the big ones, for the spec and path tests
    Arena sperm, sscratch;  // small ones, poisoned for each random iteration
    Arena data;             // file contents and the like
    Arena gen;              // raw memory for generated inputs
    u8   *html;             // HTML_SIZE bytes
    u8   *html2;
    Output *out;
} Rig;


// A growable-looking string builder over a fixed buffer, for generating
// inputs and for the HTML printer. Overflow is sticky, not a crash.

typedef struct {
    u8 *p;
    iz  len, cap;
    b32 over;
} Buf;

static void bput(Buf *b, Str s) {
    if (s.len > b->cap - b->len) {
        b->over = 1;
        return;
    }
    memcpy(b->p + b->len, s.data, (uz)s.len);
    b->len += s.len;
}

static void bc(Buf *b, u8 c) {
    bput(b, (Str){&c, 1});
}

static void bs(Buf *b, char const *s) {
    bput(b, cstr(s));
}

static void bnum(Buf *b, i64 v) {
    u8  t[24];
    iz  n = 0;
    i64 x = v;
    do {
        t[n++] = (u8)('0' + x%10);
        x /= 10;
    } while (x);
    while (n) {
        bc(b, t[--n]);
    }
}

static Str bstr(Buf *b) {
    return (Str){b->p, b->len};
}

// Percent by which the slow-input tests scale their repeat counts (-z), to
// check that doubling the input doubles the time.
static u64 scale = 100;

static void brep(Buf *b, char const *s, iz n) {
    Str t = cstr(s);
    n = n * (iz)scale / 100;
    for (iz i = 0; i < n && !b->over; i++) {
        bput(b, t);
    }
}


// HTML, as cmark and cmark-gfm print it

static void bcr(Buf *b) {
    if (b->len && b->p[b->len-1]!='\n') {
        bc(b, '\n');
    }
}

static void besc(Buf *b, Str s) {
    for (iz i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == '&') {
            bs(b, "&amp;");
        } else if (c == '<') {
            bs(b, "&lt;");
        } else if (c == '>') {
            bs(b, "&gt;");
        } else if (c == '"') {
            bs(b, "&quot;");
        } else {
            bc(b, c);
        }
    }
}

static b32 href_safe(u8 c) {
    Str safe = S("!#$%()*+,-./:;=?@_~");
    for (iz i = 0; i < safe.len; i++) {
        if (safe.data[i] == c) {
            return 1;
        }
    }
    return md_isalnum(c);
}

// Percent-encode what a URL cannot hold, as cmark's houdini does.
static void bhref(Buf *b, Str s) {
    for (iz i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == '&') {
            bs(b, "&amp;");
        } else if (c == '\'') {
            bs(b, "&#x27;");
        } else if (href_safe(c)) {
            bc(b, c);
        } else {
            bc(b, '%');
            bc(b, (u8)"0123456789ABCDEF"[c>>4]);
            bc(b, (u8)"0123456789ABCDEF"[c&15]);
        }
    }
}

static void html_node(Buf *b, MdNode *n);

static void html_children(Buf *b, MdNode *n) {
    for (MdNode *c = n->first; c; c = c->next) {
        html_node(b, c);
    }
}

// The text of an image's alt, without tags.
static void html_plain(Buf *b, MdNode *n) {
    for (MdNode *c = n->first; c; c = c->next) {
        if (c->kind==MD_TEXT || c->kind==MD_CODE || c->kind==MD_HTML_INLINE) {
            besc(b, c->text);
        } else if (c->kind==MD_SOFTBREAK || c->kind==MD_HARDBREAK) {
            bc(b, ' ');
        } else {
            html_plain(b, c);
        }
    }
}

static void html_tag(Buf *b, char const *open, char const *name, char const *close) {
    bs(b, open);
    bs(b, name);
    bs(b, close);
}

static void html_table(Buf *b, MdNode *t) {
    bcr(b);
    bs(b, "<table>");
    b32 body = 0;
    for (MdNode *row = t->first; row; row = row->next) {
        b32 header = row->flags & MD_F_HEADER;
        bcr(b);
        if (header) {
            bs(b, "<thead>");
            bcr(b);
        } else if (!body) {
            bs(b, "<tbody>");
            bcr(b);
            body = 1;
        }
        bs(b, "<tr>");
        MdNode *cell = row->first;
        MdNode *head = t->first->first;
        for (i32 i = 0; i < t->ncols; i++) {
            bcr(b);
            char const *name = header ? "th" : "td";
            u8 align = head ? head->align : 0;
            bs(b, "<");
            bs(b, name);
            if (align == MD_ALIGN_LEFT) {
                bs(b, " align=\"left\"");
            } else if (align == MD_ALIGN_CENTER) {
                bs(b, " align=\"center\"");
            } else if (align == MD_ALIGN_RIGHT) {
                bs(b, " align=\"right\"");
            }
            bs(b, ">");
            if (cell) {
                html_children(b, cell);
            }
            html_tag(b, "</", name, ">");
            cell = cell ? cell->next : 0;
            head = head ? head->next : 0;
        }
        bcr(b);
        bs(b, "</tr>");
        if (header) {
            bcr(b);
            bs(b, "</thead>");
        }
    }
    if (body) {
        bcr(b);
        bs(b, "</tbody>");
        bcr(b);
    }
    bcr(b);
    bs(b, "</table>");
    bcr(b);
}

static void html_node(Buf *b, MdNode *n) {
    switch (n->kind) {
    case MD_DOCUMENT:
        html_children(b, n);
        break;
    case MD_QUOTE:
        bcr(b);
        bs(b, "<blockquote>\n");
        html_children(b, n);
        bcr(b);
        bs(b, "</blockquote>\n");
        break;
    case MD_LIST:
        bcr(b);
        if (!(n->flags & MD_F_ORDERED)) {
            bs(b, "<ul>\n");
        } else if (n->start == 1) {
            bs(b, "<ol>\n");
        } else {
            bs(b, "<ol start=\"");
            bnum(b, n->start);
            bs(b, "\">\n");
        }
        html_children(b, n);
        bs(b, n->flags & MD_F_ORDERED ? "</ol>\n" : "</ul>\n");
        break;
    case MD_ITEM:
        bcr(b);
        bs(b, "<li>");
        if (n->task == MD_TASK_OPEN) {
            bs(b, "<input disabled=\"\" type=\"checkbox\"> ");
        } else if (n->task == MD_TASK_DONE) {
            bs(b, "<input checked=\"\" disabled=\"\" type=\"checkbox\"> ");
        }
        html_children(b, n);
        bs(b, "</li>\n");
        break;
    case MD_PARAGRAPH: {
        MdNode *g     = n->parent ? n->parent->parent : 0;
        b32     tight = g && g->kind==MD_LIST && (g->flags & MD_F_TIGHT);
        if (!tight) {
            bcr(b);
            bs(b, "<p>");
        }
        html_children(b, n);
        if (!tight) {
            bs(b, "</p>\n");
        }
        break;
    }
    case MD_HEADING:
        bcr(b);
        bs(b, "<h");
        bnum(b, n->level);
        bs(b, ">");
        html_children(b, n);
        bs(b, "</h");
        bnum(b, n->level);
        bs(b, ">\n");
        break;
    case MD_RULE:
        bcr(b);
        bs(b, "<hr />\n");
        break;
    case MD_CODE_BLOCK: {
        bcr(b);
        iz w = 0;
        while (w<n->info.len && !md_isspace(n->info.data[w])) {
            w++;
        }
        if (!w) {
            bs(b, "<pre><code>");
        } else {
            bs(b, "<pre><code class=\"language-");
            besc(b, (Str){n->info.data, w});
            bs(b, "\">");
        }
        besc(b, n->text);
        bs(b, "</code></pre>\n");
        break;
    }
    case MD_HTML_BLOCK:
        bcr(b);
        bput(b, n->text);
        bcr(b);
        break;
    case MD_TABLE:
        html_table(b, n);
        break;
    case MD_TEXT:
        besc(b, n->text);
        break;
    case MD_SOFTBREAK:
        bs(b, "\n");
        break;
    case MD_HARDBREAK:
        bs(b, "<br />\n");
        break;
    case MD_CODE:
        bs(b, "<code>");
        besc(b, n->text);
        bs(b, "</code>");
        break;
    case MD_HTML_INLINE:
        bput(b, n->text);
        break;
    case MD_EMPH:
        bs(b, "<em>");
        html_children(b, n);
        bs(b, "</em>");
        break;
    case MD_STRONG:
        bs(b, "<strong>");
        html_children(b, n);
        bs(b, "</strong>");
        break;
    case MD_STRIKE:
        bs(b, "<del>");
        html_children(b, n);
        bs(b, "</del>");
        break;
    case MD_LINK:
        bs(b, "<a href=\"");
        bhref(b, n->dest);
        bs(b, "\"");
        if (n->title.len) {
            bs(b, " title=\"");
            besc(b, n->title);
            bs(b, "\"");
        }
        bs(b, ">");
        html_children(b, n);
        bs(b, "</a>");
        break;
    case MD_IMAGE:
        bs(b, "<img src=\"");
        bhref(b, n->dest);
        bs(b, "\" alt=\"");
        html_plain(b, n);
        bs(b, "\"");
        if (n->title.len) {
            bs(b, " title=\"");
            besc(b, n->title);
            bs(b, "\"");
        }
        bs(b, " />");
        break;
    default:
        break;
    }
}


// Checking the tree

typedef struct {
    Str    src;
    u8    *lo, *hi;     // the perm arena the tree was made in
    iz     count;
    iz     limit;
} Check;

static void ck(b32 ok, char const *what, i64 a, i64 b) {
    if (!ok) {
        fail("tree", what, a, b);
    }
}

static b32 ck_inside(Check *c, Str s) {
    if (!s.len) {
        return 1;
    }
    b32 in_src  = s.data>=c->src.data && s.data+s.len<=c->src.data+c->src.len;
    b32 in_perm = s.data>=c->lo && s.data+s.len<=c->hi;
    return in_src || in_perm;
}

static b32 is_block(u8 k) {
    return k <= MD_CELL;
}

static b32 is_inline(u8 k) {
    return k>=MD_TEXT && k<MD_KIND_COUNT;
}

static b32 child_ok(u8 parent, u8 kind) {
    switch (parent) {
    case MD_DOCUMENT:
    case MD_QUOTE:
    case MD_ITEM:
        return kind==MD_QUOTE || kind==MD_LIST || kind==MD_PARAGRAPH ||
               kind==MD_HEADING || kind==MD_RULE || kind==MD_CODE_BLOCK ||
               kind==MD_HTML_BLOCK || kind==MD_TABLE;
    case MD_LIST:
        return kind == MD_ITEM;
    case MD_TABLE:
        return kind == MD_ROW;
    case MD_ROW:
        return kind == MD_CELL;
    case MD_PARAGRAPH:
    case MD_HEADING:
    case MD_CELL:
    case MD_EMPH:
    case MD_STRONG:
    case MD_STRIKE:
    case MD_LINK:
    case MD_IMAGE:
        return is_inline(kind);
    }
    return 0;
}

static void ck_node(Check *c, MdNode *n, i32 depth, b32 in_link) {
    c->count++;
    ck(c->count <= c->limit, "too many nodes (cycle?)", c->count, c->limit);
    ck(depth < MD_MAX_DEPTH + 3*MD_MAX_INLINE + 16, "tree too deep", depth, 0);
    ck(n->kind < MD_KIND_COUNT, "bad kind", n->kind, 0);
    ck(ck_inside(c, n->text), "text outside input and arena", n->kind, 0);
    if (n->kind==MD_LINK || n->kind==MD_IMAGE) {
        ck(ck_inside(c, n->dest), "dest outside input and arena", n->kind, 0);
        ck(ck_inside(c, n->title), "title outside input and arena", n->kind, 0);
    } else if (n->kind == MD_CODE_BLOCK) {
        ck(ck_inside(c, n->info), "info outside input and arena", n->kind, 0);
    } else if (n->kind == MD_HEADING) {
        ck(ck_inside(c, n->dest), "slug outside input and arena", n->kind, 0);
    }
    if (is_block(n->kind) && n->kind!=MD_DOCUMENT) {
        ck(n->line >= 1, "block without a line", n->kind, n->line);
    }
    // Leaves have text and no children; containers have children, and no
    // text of their own once parsed.
    switch (n->kind) {
    case MD_TEXT:
    case MD_CODE:
        ck(n->text.len > 0, "empty text", n->kind, 0);
        // fall through
    case MD_SOFTBREAK:
    case MD_HARDBREAK:
    case MD_HTML_INLINE:
    case MD_RULE:
    case MD_CODE_BLOCK:
    case MD_HTML_BLOCK:
        ck(!n->first && !n->last, "leaf with children", n->kind, 0);
        break;
    case MD_PARAGRAPH:
    case MD_CELL:
        ck(!n->text.len, "unparsed text left", n->kind, n->text.len);
        break;
    case MD_LIST:
        ck(n->first != 0, "empty list", 0, 0);
        ck(n->start >= 0, "negative list start", n->start, 0);
        break;
    case MD_TABLE:
        ck(n->first && n->first->flags&MD_F_HEADER, "table without header", 0, 0);
        ck(n->ncols >= 1, "table without columns", n->ncols, 0);
        break;
    case MD_HEADING:
        ck(n->level>=1 && n->level<=6, "bad heading level", n->level, 0);
        ck(!n->text.len, "unparsed text left", n->kind, n->text.len);
        break;
    default:
        break;
    }
    if (n->kind == MD_ROW) {
        i32 cells = 0;
        for (MdNode *k = n->first; k; k = k->next) {
            cells++;
        }
        ck(cells <= n->parent->ncols, "row longer than header", cells, n->parent->ncols);
        ck(!!(n->flags&MD_F_HEADER) == (n == n->parent->first), "header row misplaced", 0, 0);
    }
    if (n->kind == MD_CELL) {
        ck(n->align <= MD_ALIGN_RIGHT, "bad alignment", n->align, 0);
    }
    // Links do not nest, except that <autolinks> can be inside a link's text
    // (cmark does the same).
    if (n->kind==MD_LINK && !(n->flags&MD_F_AUTOLINK)) {
        ck(!in_link, "link in link", 0, 0);
    }
    if (n->task) {
        ck(n->kind == MD_ITEM, "task on a non-item", n->kind, 0);
    }
    b32 now_in_link = in_link || (n->kind==MD_LINK && !(n->flags&MD_F_AUTOLINK));
    MdNode *prev = 0;
    for (MdNode *k = n->first; k; k = k->next) {
        ck(k->parent == n, "bad parent", k->kind, n->kind);
        ck(k->prev == prev, "bad prev", k->kind, n->kind);
        ck(child_ok(n->kind, k->kind), "child of the wrong kind", n->kind, k->kind);
        if (prev) {
            ck(!(prev->kind==MD_TEXT && k->kind==MD_TEXT), "adjacent text nodes", 0, 0);
        }
        ck_node(c, k, depth + 1, now_in_link);
        prev = k;
    }
    ck(n->last == prev, "bad last", n->kind, 0);
}

static void check_tree(MdNode *doc, Str src, Arena perm0) {
    Check c = {src, perm0.beg, perm0.end, 0, 64*(src.len + 16)};
    ck(doc->kind==MD_DOCUMENT && !doc->parent && !doc->next, "bad root", 0, 0);
    ck_node(&c, doc, 0, 0);
}


// Rendering a parse

// Parse and render, in the given arenas, which are reset first.
static MdNode *parse(Rig *r, Arena perm, Arena scratch, Str src, u32 ext, Arena *used) {
    Arena p0 = perm;
    MdNode *doc = md_parse(&perm, scratch, src, ext);
    if (!doc) {
        fail("parse", "out of memory", perm.end - perm.beg, src.len);
    }
    check_tree(doc, src, p0);
    if (used) {
        *used = perm;
    }
    (void)r;
    return doc;
}

static Str render(u8 *mem, MdNode *doc) {
    Buf b = {mem, 0, HTML_SIZE, 0};
    html_node(&b, doc);
    if (b.over) {
        fail("render", "HTML buffer overflow", b.len, 0);
    }
    return bstr(&b);
}


// Reading files

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

static Str readfile(Arena *a, Str dir, char const *name) {
    u8  buf[512];
    Buf b = {buf, 0, sizeof(buf), 0};
    bput(&b, dir);
    bs(&b, "/");
    bs(&b, name);
    return readpath(a, bstr(&b));
}


// The spec files: JSON, an array of {"markdown", "html", "example", "section"}

typedef struct {
    Str md, html, section;
    i32 num;
} Example;

typedef struct {
    Example *ex;
    iz       count;
} Examples;

static iz json_ws(Str s, iz i) {
    while (i<s.len && (s.data[i]==' ' || s.data[i]=='\n' || s.data[i]=='\r' ||
                       s.data[i]=='\t' || s.data[i]==',')) {
        i++;
    }
    return i;
}

static i32 hex4(Str s, iz i) {
    i32 v = 0;
    for (iz k = 0; k < 4; k++) {
        i32 c = md_peek(s, i + k);
        v = v*16 + (md_isdigit(c) ? c - '0' : (c|32) - 'a' + 10);
    }
    return v;
}

// Decode the string at s[*pos] (a quote), carving the result from pool.
static Str json_string(Str s, iz *pos, u8 **pool) {
    iz  i   = *pos + 1;
    u8 *out = *pool;
    iz  n   = 0;
    while (i < s.len && s.data[i] != '"') {
        u8 c = s.data[i++];
        if (c != '\\') {
            out[n++] = c;
            continue;
        }
        c = s.data[i++];
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'u': {
            i32 v = hex4(s, i);
            i += 4;
            if (v>=0xd800 && v<=0xdbff && md_peek(s, i)=='\\' && md_peek(s, i+1)=='u') {
                i32 lo = hex4(s, i + 2);
                v  = 0x10000 + ((v - 0xd800)<<10) + (lo - 0xdc00);
                i += 6;
            }
            n += utf8encode(out + n, v);
            break;
        }
        default: out[n++] = c; break;
        }
    }
    *pos  = i + 1;
    *pool = out + n;
    return (Str){out, n};
}

static Examples load_examples(Arena *a, Str json) {
    Examples r = {new(a, 1000, Example), 0};
    u8 *pool   = new(a, json.len, u8);
    for (iz i = 0; i < json.len;) {
        if (json.data[i] != '{') {
            i++;
            continue;
        }
        Example e = {0};
        i = json_ws(json, i + 1);
        while (i<json.len && json.data[i]!='}') {
            Str key = json_string(json, &i, &pool);
            i = json_ws(json, i);
            i++;    // colon
            i = json_ws(json, i);
            Str val = {0};
            i32 num = 0;
            if (md_peek(json, i) == '"') {
                val = json_string(json, &i, &pool);
            } else {
                for (; i<json.len && md_isdigit(json.data[i]); i++) {
                    num = num*10 + (json.data[i] - '0');
                }
            }
            if (equals(key, S("markdown"))) {
                e.md = val;
            } else if (equals(key, S("html"))) {
                e.html = val;
            } else if (equals(key, S("section"))) {
                e.section = val;
            } else if (equals(key, S("example"))) {
                e.num = num;
            }
            i = json_ws(json, i);
        }
        assert(r.count < 1000);
        r.ex[r.count++] = e;
    }
    return r;
}


// Spec tests

// Examples that fail for a known reason, by file and number.
typedef struct {
    char const *file;
    i32         num;
    char const *why;
} Known;

static Known const known[] = {
    {0, 0, 0},
};

static char const *known_why(char const *file, i32 num) {
    for (iz i = 0; known[i].file; i++) {
        if (equals(cstr(known[i].file), cstr(file)) && known[i].num==num) {
            return known[i].why;
        }
    }
    return 0;
}

typedef struct {
    Str name;
    i32 pass, total;
} Section;

// Returns the number of unexpected failures.
static i32 run_spec(Rig *r, Str dir, char const *file, u32 ext, b32 verbose) {
    Output *o = r->out;
    Arena   scratch_data = r->data;
    Str json = readfile(&scratch_data, dir, file);
    if (!json.len) {
        print(o, S("md-test: cannot read "));
        print(o, dir);
        print(o, S("/"));
        print(o, cstr(file));
        print(o, S(" (run from the repository root, or use -d)\n"));
        flush(o);
        ExitProcess(2);
    }
    Examples ex = load_examples(&scratch_data, json);
    Section  secs[64] = {0};
    i32      nsecs    = 0;
    i32      pass     = 0;
    i32      bad      = 0;
    ctx.phase = "spec";
    for (iz i = 0; i < ex.count; i++) {
        Example *e = &ex.ex[i];
        alive();
        ctx.input = e->md;
        MdNode *doc = parse(r, r->perm, r->scratch, e->md, ext, 0);
        Str got  = render(r->html, doc);
        b32 ok   = equals(got, e->html);
        i32 s    = 0;
        while (s<nsecs && !equals(secs[s].name, e->section)) {
            s++;
        }
        if (s == nsecs) {
            secs[nsecs++].name = e->section;
        }
        secs[s].total++;
        secs[s].pass += ok;
        pass += ok;
        char const *why = known_why(file, e->num);
        if (!ok || verbose) {
            print(o, ok ? S("ok   ") : why ? S("known ") : S("FAIL "));
            print(o, cstr(file));
            print(o, S(" #"));
            printi64(o, e->num);
            print(o, S(" ["));
            print(o, e->section);
            print(o, S("]"));
            if (why) {
                print(o, S(" "));
                print(o, cstr(why));
            }
            print(o, S("\n"));
            if (!ok && (verbose || !why)) {
                print(o, S("--- markdown\n"));
                dumpstr(o, e->md, 2000);
                print(o, S("--- expected\n"));
                dumpstr(o, e->html, 2000);
                print(o, S("--- got\n"));
                dumpstr(o, got, 2000);
                print(o, S("---\n"));
            }
        }
        bad += !ok && !why;
    }
    ctx.input = (Str){0};
    print(o, cstr(file));
    print(o, S(": "));
    printi64(o, pass);
    print(o, S("/"));
    printi64(o, ex.count);
    print(o, S(" passed\n"));
    for (i32 s = 0; s < nsecs; s++) {
        print(o, S("  "));
        print(o, secs[s].name);
        for (iz k = secs[s].name.len; k < 40; k++) {
            printc(o, ' ');
        }
        printi64(o, secs[s].pass);
        print(o, S("/"));
        printi64(o, secs[s].total);
        print(o, S("\n"));
    }
    flush(o);
    return bad;
}


// Unit cases: behavior the spec files do not pin down

typedef struct {
    char const *md;
    char const *html;
} Case;

static Case const cases[] = {
    {"foo\n| a | b |\n|---|---|\n| c | d |\n",
     "<p>foo</p>\n<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n"
     "<tbody>\n<tr>\n<td>c</td>\n<td>d</td>\n</tr>\n</tbody>\n</table>\n"},
    {"| a | b |\n|---|---|\n| c | d | e |\n| f |\n",
     "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n"
     "<tbody>\n<tr>\n<td>c</td>\n<td>d</td>\n</tr>\n<tr>\n<td>f</td>\n<td></td>\n"
     "</tr>\n</tbody>\n</table>\n"},
    {"|a|\n|-|\n|`x\\|y`|\n",
     "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n"
     "<td><code>x|y</code></td>\n</tr>\n</tbody>\n</table>\n"},
    {"- [ ] a\n- [X] b\n  - [x] c\n- [ ]d\n-  [ ] e\n",
     "<ul>\n<li><input disabled=\"\" type=\"checkbox\"> a</li>\n"
     "<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> b\n<ul>\n"
     "<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> c</li>\n</ul>\n</li>\n"
     "<li>[ ]d</li>\n<li><input disabled=\"\" type=\"checkbox\"> e</li>\n</ul>\n"},
    {"~one~ and ~~two~~ and ~~~three~~~ and ~a~~\n",
     "<p><del>one</del> and <del>two</del> and ~~~three~~~ and ~a~~</p>\n"},
    {"see www.a.com/b_c, or http://localhost:3000/x). me@x.org.\n",
     "<p>see <a href=\"http://www.a.com/b_c\">www.a.com/b_c</a>, or "
     "<a href=\"http://localhost:3000/x\">http://localhost:3000/x</a>). "
     "<a href=\"mailto:me@x.org\">me@x.org</a>.</p>\n"},
    {"[a [b](c) d](e) <http://x.y>\n",
     "<p>[a <a href=\"c\">b</a> d](e) <a href=\"http://x.y\">http://x.y</a></p>\n"},
    {"[Foo\n  bar]: /url\n\n[\xe1\xba\x9e] [\xce\x91\xce\x93] [foo\n bar]\n",
     "<p>[\xe1\xba\x9e] [\xce\x91\xce\x93] <a href=\"/url\">foo\nbar</a></p>\n"},
    {"a\\\nb  \nc\n", "<p>a<br />\nb<br />\nc</p>\n"},
    {"\xef\xbb\xbf" "a\r\nb\rc\n","<p>a\nb\nc</p>\n"},
    {"a\xe2\x80\xa8" "b\n", "<p>a\xe2\x80\xa8" "b</p>\n"},
    {"&nGt; &#0; &#x110000; &bogus; &amp\n",
     "<p>\xe2\x89\xab\xe2\x83\x92 \xef\xbf\xbd \xef\xbf\xbd &amp;bogus; &amp;amp</p>\n"},
    {"```\xe2\x80\x8b js x\n`\n```\n",
     "<pre><code class=\"language-\xe2\x80\x8b\">`\n</code></pre>\n"},
};

// Heading anchors, in order, for md_slugify.
static char const *const slug_md =
    "# Hello, World!\n## Hello, World!\n### hello-world\n#### a_b *C* `d`\n"
    "##### \xc3\x89t\xc3\xa9 \xe2\x82\xac\n###### !!!\n# !!!\n";
static char const *const slugs[] = {
    "hello-world", "hello-world-1", "hello-world-2", "a_b-c-d",
    "\xc3\xa9t\xc3\xa9-", "", "-1",
};

static i32 run_unit(Rig *r) {
    Output *o   = r->out;
    i32     bad = 0;
    ctx.phase   = "unit";
    for (iz i = 0; i < countof(cases); i++) {
        alive();
        Str in = cstr(cases[i].md);
        ctx.input = in;
        MdNode *doc = parse(r, r->perm, r->scratch, in, MD_GFM, 0);
        Str got  = render(r->html, doc);
        if (!equals(got, cstr(cases[i].html))) {
            print(o, S("FAIL unit #"));
            printi64(o, (i64)i);
            print(o, S("\n--- markdown\n"));
            dumpstr(o, in, 1000);
            print(o, S("--- expected\n"));
            dumpstr(o, cstr(cases[i].html), 1000);
            print(o, S("--- got\n"));
            dumpstr(o, got, 1000);
            bad++;
        }
    }
    Str in = cstr(slug_md);
    ctx.input = in;
    Arena   perm = r->perm;
    MdNode *doc  = md_parse(&perm, r->scratch, in, MD_GFM);
    ck(doc != 0, "slug parse failed", 0, 0);
    md_slugify(&perm, r->scratch, doc);
    iz k = 0;
    for (MdNode *n = doc->first; n; n = n->next, k++) {
        ck(k < countof(slugs), "too many headings", k, 0);
        if (!equals(n->dest, cstr(slugs[k]))) {
            print(o, S("FAIL unit slug "));
            printi64(o, k);
            print(o, S(": got "));
            print(o, n->dest);
            print(o, S("\n"));
            bad++;
        }
    }
    ck(k == countof(slugs), "wrong number of headings", k, countof(slugs));
    ctx.input = (Str){0};
    print(o, S("unit: "));
    printi64(o, (i64)countof(cases) + k - bad);
    print(o, S("/"));
    printi64(o, (i64)(countof(cases) + k));
    print(o, S(" passed\n"));
    flush(o);
    return bad;
}


// Random documents

static char const *const prefixes[] = {
    "", "> ", ">", "- ", "* ", "+ ", "1. ", "2) ", "10. ", "  ", "    ", "\t",
    "   - ", "# ", "## ", "###### ", "####### ", "===", "---", "***", "___",
    "- - -", "```", "```c ", "~~~", "<div>", "</div>", "<!-- ", "<?", "<pre>",
    "[a]: /u ", "[b]: <x y> \"t\" ", "| a | b |", "|---|:-:|", "| - | - |",
    "- [ ] ", "- [x] ", "* [X] ", "0. ", "1) ", "<script>", "<![CDATA[",
    "<a href=\"x\">", ":--", "a | b", "|", "||",
};

static char const *const tokens[] = {
    "foo", "bar", "a", "b", "x", " ", " ", "  ", "   ", "*", "**", "***", "_",
    "__", "~", "~~", "~~~", "`", "``", "```", "[", "]", "![", "](", "](/u)",
    "](/u \"t\")", "](<a b>)", "][", "[a]", "[b]", "[]", "(", ")", "<", ">",
    "<a>", "</a>", "<a b=c>", "<br/>", "<http://a.b>", "<a@b.c>", "<!-- c -->",
    "<!--", "-->", "<?x?>", "&", "&amp;", "&#35;", "&#x41;", "&#0;", "&bogus;",
    "&nGt;", "&", ";", "\\", "\\*", "\\\n", "\\|", "\"", "'", "|", ":", "-",
    "=", "#", "www.x.com", "www.", "http://a.b/c", "https://x", "a@b.cc",
    "foo@bar.baz.", "(www.a.b)", "\t", "\xc3\xa9", "\xc3\x9f", "\xe1\xba\x9e",
    "\xce\xa3", "\xcf\x82", "\xf0\x9f\x98\x80", "\xff", "\xc3", "\xe2\x82\xac",
    "\xc2\xa0", "\xe2\x80\x9c", "\0", "\r", "0", "1", ".", ",", "!", "?", "@",
    "{", "}", "^", "%", "$", "/", "+",
};

// One random document.
static void gen_doc(Buf *g, u64 *rng) {
    i32 lines = chance(rng, 85) ? (i32)rnd(rng, 10) + 1 : (i32)rnd(rng, 80) + 10;
    for (i32 l = 0; l < lines && !g->over; l++) {
        i32 np = chance(rng, 40) ? 0 : (i32)rnd(rng, 4);
        for (i32 p = 0; p < np; p++) {
            bs(g, prefixes[rnd(rng, (u64)countof(prefixes))]);
        }
        i32 nt = chance(rng, 10) ? 0 : (i32)rnd(rng, 8);
        for (i32 t = 0; t < nt; t++) {
            char const *tok = tokens[rnd(rng, (u64)countof(tokens))];
            if (tok[0] == 0) {
                bc(g, 0);       // a NUL: the token is "\0"
            } else {
                bs(g, tok);
            }
        }
        if (l+1<lines || chance(rng, 70)) {
            switch (weighted(rng, (i32 const[]){90, 6, 4}, 3)) {
            case 0:  bs(g, "\n");   break;
            case 1:  bs(g, "\r\n"); break;
            default: bs(g, "\r");   break;
            }
        }
        if (chance(rng, 12)) {
            bs(g, "\n");        // a blank line
        }
    }
}

// Reference normalization, written differently from md.c's.
static void ref_normalize(Str s, Buf *out) {
    iz i = 0;
    if (s.len>=3 && s.data[0]==0xef && s.data[1]==0xbb && s.data[2]==0xbf) {
        i = 3;
    }
    for (; i < s.len; i++) {
        if (s.data[i] == 0) {
            bs(out, "\xef\xbf\xbd");
        } else if (s.data[i] == '\r') {
            bc(out, '\n');
            if (i+1<s.len && s.data[i+1]=='\n') {
                i++;
            }
        } else {
            bc(out, s.data[i]);
        }
    }
}

static void with_crlf(Str s, Buf *out) {
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i] == '\n') {
            bc(out, '\r');
        }
        bc(out, s.data[i]);
    }
}

static void iteration(Rig *r, u64 seed, i64 iter) {
    u64 rng = seed ^ (u64)iter*0x9e3779b97f4a7c15ULL;
    rand64(&rng);
    rand64(&rng);
    ctx.iter = iter;
    alive();

    Buf g = {r->gen.beg, 0, 1<<20, 0};
    ctx.phase = "generate";
    gen_doc(&g, &rng);
    Str in = bstr(&g);
    ctx.input = in;

    poison(r->sperm);
    poison(r->sscratch);
    ctx.phase = "parse";
    Arena used;
    MdNode *doc = parse(r, r->sperm, r->sscratch, in, MD_GFM, &used);
    ctx.phase = "render";
    Str html = render(r->html, doc);

    // The same document with other line endings, a BOM, a final newline.
    Buf norm = {r->gen.beg + (2<<20), 0, 1<<20, 0};
    ref_normalize(in, &norm);
    Buf crlf = {r->gen.beg + (3<<20), 0, 1<<20, 0};
    with_crlf(bstr(&norm), &crlf);
    Buf bom = {r->gen.beg + (4<<20), 0, 1<<20, 0};
    bs(&bom, "\xef\xbb\xbf");
    bput(&bom, in);
    Buf nl = {r->gen.beg + (5<<20), 0, 1<<20, 0};
    bput(&nl, bstr(&norm));
    if (!norm.len || norm.p[norm.len-1]!='\n') {
        bs(&nl, "\n");
    }
    Buf *variants[] = {&norm, &crlf, &bom, &nl};
    static char const *const names[] = {
        "normalized text parses differently",
        "CRLF text parses differently",
        "text after a BOM parses differently",
        "text with a final newline parses differently",
    };
    for (iz v = 0; v < countof(variants); v++) {
        ctx.phase = names[v];
        poison(r->sperm);
        poison(r->sscratch);
        MdNode *d2 = parse(r, r->sperm, r->sscratch, bstr(variants[v]), MD_GFM, 0);
        Str h2 = render(r->html2, d2);
        if (!equals(html, h2)) {
            // The first difference tells most.
            iz k = 0;
            while (k<html.len && k<h2.len && html.data[k]==h2.data[k]) {
                k++;
            }
            print(ctx.out, S("--- HTML of the input\n"));
            dumpstr(ctx.out, html, 3000);
            print(ctx.out, S("--- HTML of the variant\n"));
            dumpstr(ctx.out, h2, 3000);
            fail("variant", names[v], k, 0);
        }
    }

    // Running out of memory is an error, not a crash.
    ctx.phase = "out of memory";
    for (i32 k = 0; k < 3; k++) {
        poison(r->sperm);
        poison(r->sscratch);
        Arena small_perm    = r->sperm;
        Arena small_scratch = r->sscratch;
        small_perm.end    = small_perm.beg + (iz)rnd(&rng, SMALL_SIZE);
        small_scratch.end = small_scratch.beg + (iz)rnd(&rng, SMALL_SIZE);
        Arena before = small_perm;
        MdNode *d3 = md_parse(&small_perm, small_scratch, in, MD_GFM);
        if (!d3) {
            ck(small_perm.beg == before.beg && small_perm.end == before.end,
               "failed parse used the arena", 0, 0);
        } else {
            check_tree(d3, in, before);
            Str h3 = render(r->html2, d3);
            ck(equals(html, h3), "small-arena parse differs", 0, 0);
        }
    }
    ctx.phase = 0;
}


// Pathological inputs

typedef struct {
    char const *name;
    void      (*gen)(Buf *);
} Path;

static void p_nested_strong(Buf *b)  { brep(b, "*a **a ", 7000); bs(b, "b"); brep(b, " a** a*", 7000); }
static void p_emph_closers(Buf *b)   { brep(b, "a_ ", 33000); }
static void p_emph_openers(Buf *b)   { brep(b, "_a ", 33000); }
static void p_link_closers(Buf *b)   { brep(b, "a]", 50000); }
static void p_link_openers(Buf *b)   { brep(b, "[a", 50000); }
static void p_mismatched(Buf *b)     { brep(b, "*a_ ", 25000); }
static void p_mult3(Buf *b)          { bs(b, "a**b"); brep(b, "c* ", 33000); }
static void p_link_emph(Buf *b)      { brep(b, "[ a_", 25000); }
static void p_paren_pattern(Buf *b)  { brep(b, "[ (](", 20000); }
static void p_nested_brackets(Buf *b) { brep(b, "[", 50000); bs(b, "a"); brep(b, "]", 50000); }
static void p_nested_brackets_ref(Buf *b) {
    bs(b, "[a]: /u\n\n");
    brep(b, "[", 40000);
    bs(b, "a");
    brep(b, "]", 40000);
}
static void p_nested_images(Buf *b)  { brep(b, "![", 30000); bs(b, "a"); brep(b, "](b)", 20000); }
static void p_nested_quotes(Buf *b)  { brep(b, "> ", 50000); bs(b, "a\n"); }
static void p_quote_run(Buf *b)      { brep(b, ">", 100000); bs(b, " a\n"); }
static void p_nested_quote_lines(Buf *b) { brep(b, "> a\n", 25000); brep(b, "> > a\n", 12000); }
static void p_nested_lists(Buf *b) {
    for (i32 i = 0; i < 350; i++) {
        brep(b, "  ", i);
        bs(b, "* a\n");
    }
}
static void p_list_markers(Buf *b)   { brep(b, "- ", 50000); bs(b, "a\n"); }
static void p_ordered_markers(Buf *b) { brep(b, "1. ", 30000); bs(b, "a\n"); }
static void p_nul(Buf *b)            { for (i32 i = 0; i < 12000; i++) { bs(b, "abc"); bc(b, 0); bs(b, "de"); bc(b, 0); } }
static void p_backticks(Buf *b)      { for (i32 i = 1; i < 300; i++) { bc(b, 'e'); brep(b, "`", i); } }
static void p_backticks_1(Buf *b)    { brep(b, "`a", 50000); }
static void p_backticks_esc(Buf *b)  { brep(b, "\\``", 30000); }
static void p_backtick_run(Buf *b)   { brep(b, "`", 100000); }
static void p_backticks_len(Buf *b)  { for (i32 i = 2; i < 400; i++) { brep(b, "`", i); bc(b, ' '); } }
static void p_unclosed_a(Buf *b)     { brep(b, "[a](<b", 15000); }
static void p_unclosed_b(Buf *b)     { brep(b, "[a](b", 20000); }
static void p_unclosed_c(Buf *b)     { brep(b, "[a](b \"c", 12000); }
static void p_unclosed_d(Buf *b)     { brep(b, "[a](b 'c", 12000); }
static void p_unclosed_e(Buf *b)     { brep(b, "[a](b (c", 12000); }
static void p_long_dest(Buf *b)      { bs(b, "[a]("); brep(b, "b", 90000); }
static void p_many_refs(Buf *b)      { brep(b, "[a]: /u\n", 12000); brep(b, "[a]", 20000); }
static void p_unique_refs(Buf *b) {
    for (i32 i = 0; i < 4000; i++) {
        bs(b, "[a"); bnum(b, i); bs(b, "]: /u \"t\"\n");
    }
    bs(b, "\n");
    for (i32 i = 0; i < 4000; i++) {
        bs(b, "[a"); bnum(b, i); bs(b, "] ");
    }
}
static void p_ref_long_label(Buf *b) {
    bs(b, "[");
    brep(b, "a ", 200);
    bs(b, "]: /u\n\n");
    for (i32 i = 0; i < 200; i++) {
        bs(b, "[");
        brep(b, "a ", 200);
        bs(b, "] ");
    }
}
static void p_ref_titles(Buf *b)     { brep(b, "[a]: /u 'x\n", 20000); }
static void p_lt(Buf *b)             { brep(b, "<", 100000); }
static void p_lt_a(Buf *b)           { brep(b, "<a ", 30000); }
static void p_lt_comment(Buf *b)     { brep(b, "<!--", 25000); }
static void p_lt_pi(Buf *b)          { brep(b, "<?", 50000); }
static void p_lt_cdata(Buf *b)       { brep(b, "<![CDATA[", 10000); }
static void p_lt_decl(Buf *b)        { brep(b, "<!A", 33000); }
static void p_lt_quote(Buf *b)       { brep(b, "<a b='", 15000); }
static void p_lt_dquote(Buf *b)      { brep(b, "<a b=\"x", 15000); }
static void p_lt_attrs(Buf *b)       { bs(b, "<a"); brep(b, " b", 40000); }
static void p_lt_autolink(Buf *b)    { brep(b, "<http://", 12000); }
static void p_lt_email(Buf *b)       { brep(b, "<a@", 33000); }
static void p_html_tags(Buf *b)      { brep(b, "<a>", 30000); brep(b, "</a>", 3000); }
static void p_entities(Buf *b)       { brep(b, "&", 100000); }
static void p_entities_hash(Buf *b)  { brep(b, "&#", 50000); }
static void p_entities_open(Buf *b)  { brep(b, "&amp", 25000); }
static void p_entities_ok(Buf *b)    { brep(b, "&amp;&nGt;&#35;", 7000); }
static void p_www(Buf *b)            { brep(b, "www.", 25000); }
static void p_www_a(Buf *b)          { brep(b, "www.a.", 16000); }
static void p_www_us(Buf *b)         { brep(b, "www.a_a_a_a_.a_ ", 6000); }
static void p_http(Buf *b)           { brep(b, "http://", 14000); }
static void p_http_a(Buf *b)         { brep(b, "http://a.b ", 9000); }
static void p_at(Buf *b)             { brep(b, "a@", 50000); }
static void p_at_dots(Buf *b)        { brep(b, "a.a@a.", 16000); }
static void p_at_long(Buf *b)        { brep(b, "a", 40000); brep(b, "@", 40000); }
static void p_www_parens(Buf *b)     { bs(b, "www.a.com/"); brep(b, ")", 50000); }
static void p_www_punct(Buf *b)      { bs(b, "www.a.com/"); brep(b, ".", 50000); }
static void p_www_semis(Buf *b)      { bs(b, "www.a.com/"); brep(b, "&a;", 20000); }
static void p_table_wide(Buf *b)     { bs(b, "|"); brep(b, "a|", 25000); bs(b, "\n|"); brep(b, "-|", 25000); bs(b, "\n"); brep(b, "|\n", 20000); }
static void p_table_rows(Buf *b)     { bs(b, "|a|b|\n|-|-|\n"); brep(b, "|c|d|\n", 15000); }
static void p_table_short(Buf *b)    { bs(b, "|a|b|\n|-|-|\n"); brep(b, "|\n", 40000); }
static void p_table_empty(Buf *b)    { bs(b, "||\n|-|\n"); brep(b, "||\n", 30000); }
static void p_table_esc(Buf *b)      { bs(b, "|a|\n|-|\n|"); brep(b, "\\|", 40000); }
static void p_table_head(Buf *b)     { brep(b, "a|b\n", 25000); bs(b, "-|-\n"); }
static void p_table_repeat(Buf *b)   { brep(b, "a|b\n-|-\n", 10000); }
static void p_table_delims(Buf *b)   { bs(b, "a|b\n"); brep(b, "|--", 30000); bs(b, "\n"); }
static void p_stars(Buf *b)          { brep(b, "*", 100000); }
static void p_unders(Buf *b)         { brep(b, "_", 100000); }
static void p_tildes(Buf *b)         { brep(b, "~", 100000); }
static void p_star_pairs(Buf *b)     { brep(b, "*a*", 30000); }
static void p_star_open(Buf *b)      { brep(b, "**a ", 25000); }
static void p_star_words(Buf *b)     { brep(b, "*a ", 33000); brep(b, " a*", 33000); }
static void p_strike_open(Buf *b)    { brep(b, "~a ", 33000); }
static void p_strike_pairs(Buf *b)   { brep(b, "~~a", 33000); }
static void p_strike_mixed(Buf *b)   { brep(b, "~a~~b~~~", 12000); }
static void p_hard_breaks(Buf *b)    { brep(b, "a  \n", 25000); }
static void p_backslash_breaks(Buf *b) { brep(b, "\\\n", 50000); }
static void p_backslashes(Buf *b)    { brep(b, "\\", 100000); }
static void p_backslash_pairs(Buf *b) { brep(b, "\\*", 50000); }
static void p_setext(Buf *b)         { brep(b, "a\n===\n", 16000); }
static void p_setext_run(Buf *b)     { brep(b, "===\n", 25000); }
static void p_setext_refs(Buf *b)    { brep(b, "[a]: /u\n", 20000); bs(b, "===\n"); }
static void p_long_para(Buf *b)      { brep(b, "a\n", 50000); }
static void p_long_para_rule(Buf *b) { brep(b, "a\n", 40000); bs(b, "---\n"); }
static void p_tiny_paras(Buf *b)     { brep(b, "a\n\n", 30000); }
static void p_bullets(Buf *b)        { brep(b, "- a\n", 25000); }
static void p_loose(Buf *b)          { brep(b, "- a\n\n", 20000); }
static void p_numbers(Buf *b)        { brep(b, "1. a\n", 20000); }
static void p_empty_items(Buf *b)    { brep(b, "-\n", 50000); }
static void p_tasks(Buf *b)          { brep(b, "- [ ] a\n- [x] b\n", 6000); }
static void p_html_divs(Buf *b)      { brep(b, "<div>\n", 16000); }
static void p_html_split(Buf *b)     { brep(b, "<div>\n\n", 14000); }
static void p_html_comments(Buf *b)  { brep(b, "<!--\n", 20000); }
static void p_html_tags7(Buf *b)     { brep(b, "<a>\n", 25000); }
static void p_fences(Buf *b)         { brep(b, "```\n", 25000); }
static void p_fences_info(Buf *b)    { brep(b, "~~~ a&amp;\\*\n", 8000); }
static void p_indented(Buf *b)       { brep(b, "    a\n", 20000); }
static void p_indented_blank(Buf *b) { bs(b, "    a\n"); brep(b, "\n", 80000); }
static void p_link_in_link(Buf *b)   { brep(b, "[a ", 30000); brep(b, "](x)", 30000); }
static void p_link_text_emph(Buf *b) { bs(b, "["); brep(b, "*a ", 20000); bs(b, "](u)"); }
static void p_ref_brackets(Buf *b)   { bs(b, "[b]: /u\n\n"); brep(b, "[a][b]", 15000); }
static void p_no_refs(Buf *b)        { brep(b, "[a]", 30000); }
static void p_collapsed(Buf *b)      { bs(b, "[a]: /u\n\n"); brep(b, "[a][]", 15000); }
static void p_item_fences(Buf *b)    { brep(b, "- ```\n  a\n  ```\n", 5000); }
static void p_tabs(Buf *b)           { brep(b, "\t- \ta\n", 10000); }
static void p_tab_code(Buf *b)       { brep(b, " \t a\n", 15000); }
static void p_combining(Buf *b)      { bs(b, "a"); brep(b, "\xcc\x81", 30000); }
static void p_invalid_utf8(Buf *b)   { brep(b, "\xff", 100000); }
static void p_unicode_emph(Buf *b)   { brep(b, "*\xe2\x82\xac*a ", 10000); }
static void p_alt_text(Buf *b)       { bs(b, "![") ; brep(b, "*a* `b` ", 8000); bs(b, "](u)"); }
static void p_mixed(Buf *b)          { brep(b, "> - *a* [b](c) `d` ![e](f) <g> &h; www.i.j\n", 2000); }
static void p_www_us_run(Buf *b)     { brep(b, "www.a_", 16000); }
static void p_www_dash_run(Buf *b)   { brep(b, "www.a-", 16000); }
static void p_www_star_run(Buf *b)   { brep(b, "www.*", 16000); }
static void p_email_run(Buf *b)      { brep(b, "a_@a_", 20000); }
static void p_close_tags(Buf *b)     { brep(b, "</a ", 25000); }
static void p_quote_list(Buf *b)     { brep(b, "> - ", 25000); bs(b, "a\n"); }
static void p_rules(Buf *b)          { brep(b, "---\n", 25000); }
static void p_rules_stars(Buf *b)    { brep(b, "* * *\n", 16000); }
static void p_setext_quotes(Buf *b)  { brep(b, "> a\n---\n", 12000); }
static void p_pipes(Buf *b)          { brep(b, "|", 50000); bs(b, "\n"); brep(b, "|-", 25000); bs(b, "\n"); }
static void p_item_tables(Buf *b)    { brep(b, "- | a |\n  |---|\n  | b |\n", 4000); }
static void p_script_end(Buf *b)     { bs(b, "<script>\n"); brep(b, " </scrip\n", 10000); }
static void p_code_newlines(Buf *b)  { bs(b, "`"); brep(b, "a\n", 50000); bs(b, "`"); }
static void p_long_info(Buf *b)      { bs(b, "```"); brep(b, "a", 100000); bs(b, "\n"); }
static void p_entity_dest(Buf *b)    { bs(b, "[a]("); brep(b, "&amp;", 20000); bs(b, ")"); }
static void p_long_ref_dest(Buf *b)  { bs(b, "[a]: "); brep(b, "b", 100000); bs(b, "\n\n[a]"); }
static void p_label_999(Buf *b) {
    bs(b, "[b]: /u\n\n");
    for (i32 i = 0; i < 60; i++) {
        bs(b, "[");
        brep(b, "x", 998);
        bs(b, "] ");
    }
    for (i32 i = 0; i < 30; i++) {
        bs(b, "[b]");
    }
}
static void p_deep_emph_link(Buf *b) { brep(b, "*[a", 20000); brep(b, "](b)*", 20000); }
static void p_alt_nest(Buf *b)       { brep(b, "![a ", 20000); brep(b, "](b)", 20000); }
static void p_titles_long(Buf *b)    { bs(b, "[a](b \""); brep(b, "x\\\"", 30000); bs(b, "\")"); }
static void p_many_cells_esc(Buf *b) { bs(b, "|a|\n|-|\n"); brep(b, "|\\|a\\|\n", 12000); }

static Path const paths[] = {
    {"nested strong emph", p_nested_strong},
    {"many emph closers with no openers", p_emph_closers},
    {"many emph openers with no closers", p_emph_openers},
    {"many link closers with no openers", p_link_closers},
    {"many link openers with no closers", p_link_openers},
    {"mismatched openers and closers", p_mismatched},
    {"openers and closers multiple of 3", p_mult3},
    {"link openers and emph closers", p_link_emph},
    {"pattern [ (]( repeated", p_paren_pattern},
    {"nested brackets", p_nested_brackets},
    {"nested brackets with a reference", p_nested_brackets_ref},
    {"nested images", p_nested_images},
    {"nested block quotes", p_nested_quotes},
    {"run of block quote markers", p_quote_run},
    {"many quoted lines", p_nested_quote_lines},
    {"deeply nested lists", p_nested_lists},
    {"many list markers on a line", p_list_markers},
    {"many ordered list markers on a line", p_ordered_markers},
    {"U+0000 in input", p_nul},
    {"backticks", p_backticks},
    {"unclosed backticks", p_backticks_1},
    {"escaped backticks", p_backticks_esc},
    {"one long backtick run", p_backtick_run},
    {"backtick runs of every length", p_backticks_len},
    {"unclosed inline links A", p_unclosed_a},
    {"unclosed inline links B", p_unclosed_b},
    {"unclosed inline links C", p_unclosed_c},
    {"unclosed inline links D", p_unclosed_d},
    {"unclosed inline links E", p_unclosed_e},
    {"very long link destination", p_long_dest},
    {"many uses of one reference", p_many_refs},
    {"many references", p_unique_refs},
    {"long reference labels", p_ref_long_label},
    {"reference definitions with unclosed titles", p_ref_titles},
    {"unclosed <", p_lt},
    {"unclosed <a", p_lt_a},
    {"unclosed <!--", p_lt_comment},
    {"unclosed <?", p_lt_pi},
    {"unclosed <![CDATA[", p_lt_cdata},
    {"unclosed <!A", p_lt_decl},
    {"unclosed attribute quote '", p_lt_quote},
    {"unclosed attribute quote \"", p_lt_dquote},
    {"endless attributes", p_lt_attrs},
    {"unclosed autolinks", p_lt_autolink},
    {"unclosed email autolinks", p_lt_email},
    {"many inline tags", p_html_tags},
    {"entities: &", p_entities},
    {"entities: &#", p_entities_hash},
    {"entities: &amp without ;", p_entities_open},
    {"entities that are valid", p_entities_ok},
    {"www.", p_www},
    {"www.a.", p_www_a},
    {"www with underscores", p_www_us},
    {"http://", p_http},
    {"http://a.b", p_http_a},
    {"a@", p_at},
    {"a.a@a.", p_at_dots},
    {"long email local part", p_at_long},
    {"autolink ending in parens", p_www_parens},
    {"autolink ending in periods", p_www_punct},
    {"autolink ending in entities", p_www_semis},
    {"wide table", p_table_wide},
    {"many table rows", p_table_rows},
    {"short table rows", p_table_short},
    {"empty table cells", p_table_empty},
    {"escaped pipes", p_table_esc},
    {"table header candidates", p_table_head},
    {"repeated tables", p_table_repeat},
    {"wide delimiter row", p_table_delims},
    {"* run", p_stars},
    {"_ run", p_unders},
    {"~ run", p_tildes},
    {"*a* repeated", p_star_pairs},
    {"**a repeated", p_star_open},
    {"*a and a* words", p_star_words},
    {"~a repeated", p_strike_open},
    {"~~a repeated", p_strike_pairs},
    {"mixed tildes", p_strike_mixed},
    {"hard breaks", p_hard_breaks},
    {"backslash breaks", p_backslash_breaks},
    {"backslashes", p_backslashes},
    {"escaped stars", p_backslash_pairs},
    {"setext headings", p_setext},
    {"setext underlines", p_setext_run},
    {"setext after definitions", p_setext_refs},
    {"one long paragraph", p_long_para},
    {"one long paragraph and a rule", p_long_para_rule},
    {"many tiny paragraphs", p_tiny_paras},
    {"bullet list", p_bullets},
    {"loose list", p_loose},
    {"ordered list", p_numbers},
    {"empty items", p_empty_items},
    {"task list", p_tasks},
    {"HTML blocks, kind 6", p_html_divs},
    {"HTML blocks, split", p_html_split},
    {"HTML blocks, comments", p_html_comments},
    {"HTML blocks, kind 7", p_html_tags7},
    {"code fences", p_fences},
    {"code fences with info", p_fences_info},
    {"indented code", p_indented},
    {"indented code, blank lines", p_indented_blank},
    {"link in link", p_link_in_link},
    {"emphasis in link text", p_link_text_emph},
    {"full references", p_ref_brackets},
    {"references without definitions", p_no_refs},
    {"collapsed references", p_collapsed},
    {"fences in list items", p_item_fences},
    {"tabs in list items", p_tabs},
    {"tabs in code", p_tab_code},
    {"combining characters", p_combining},
    {"invalid UTF-8", p_invalid_utf8},
    {"Unicode punctuation and emphasis", p_unicode_emph},
    {"image alt text", p_alt_text},
    {"a bit of everything", p_mixed},
    {"www.a_ run", p_www_us_run},
    {"www.a- run", p_www_dash_run},
    {"www.* run", p_www_star_run},
    {"a_@a_ run", p_email_run},
    {"unclosed closing tags", p_close_tags},
    {"quote and list markers", p_quote_list},
    {"thematic breaks", p_rules},
    {"thematic breaks with stars", p_rules_stars},
    {"setext underlines in quotes", p_setext_quotes},
    {"table pipes", p_pipes},
    {"tables in list items", p_item_tables},
    {"HTML block kind 1 end search", p_script_end},
    {"code span with many line endings", p_code_newlines},
    {"very long info string", p_long_info},
    {"entities in a link destination", p_entity_dest},
    {"very long definition destination", p_long_ref_dest},
    {"labels near 999 characters", p_label_999},
    {"emphasis around links", p_deep_emph_link},
    {"nested image alt text", p_alt_nest},
    {"long title with escapes", p_titles_long},
    {"escaped pipes in rows", p_many_cells_esc},
};

static b32 measure;     // -u: also find how much memory each input needs

// The least size, to 4 KB, of perm (which is 0) or scratch (1) that
// md_parse succeeds with. Running out of memory must be clean at any size.
static iz min_arena(Rig *r, Str in, b32 which) {
    iz lo = 0;
    iz hi = which ? SCRATCH_SIZE : PERM_SIZE;
    while (hi - lo > 4096) {
        iz    mid = (lo + hi) / 2;
        Arena p   = r->perm;
        Arena s   = r->scratch;
        if (which) {
            s.end = s.beg + mid;
        } else {
            p.end = p.beg + mid;
        }
        if (md_parse(&p, s, in, MD_GFM)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

static i32 run_paths(Rig *r, i32 limit_ms) {
    Output *o = r->out;
    i64 freq  = 0;
    QueryPerformanceFrequency(&freq);
    i32 bad   = 0;
    i64 worst = 0;
    char const *worst_name = "";
    ctx.phase = "path";
    for (iz i = 0; i < countof(paths); i++) {
        alive();
        Buf g = {r->gen.beg, 0, GEN_SIZE, 0};
        paths[i].gen(&g);
        ck(!g.over, "generated input too big", g.len, 0);
        Str in = bstr(&g);
        ctx.input = (Str){0};
        ctx.phase = paths[i].name;
        i64 t0 = 0, t1 = 0, t2 = 0;
        QueryPerformanceCounter(&t0);
        Arena perm = r->perm;
        MdNode *doc = md_parse(&perm, r->scratch, in, MD_GFM);
        QueryPerformanceCounter(&t1);
        if (!doc) {
            fail("path", "out of memory", in.len, 0);
        }
        check_tree(doc, in, r->perm);
        QueryPerformanceCounter(&t2);
        i64 ms    = (t1 - t0)*1000/freq;
        i64 check = (t2 - t1)*1000/freq;
        if (ms > worst) {
            worst      = ms;
            worst_name = paths[i].name;
        }
        b32 slow = ms > limit_ms;
        bad += slow;
        if (slow) {
            print(o, S("SLOW "));
        } else {
            print(o, S("ok   "));
        }
        print(o, cstr(paths[i].name));
        for (iz k = cstr(paths[i].name).len; k < 44; k++) {
            printc(o, ' ');
        }
        printi64(o, in.len);
        print(o, S(" bytes  "));
        printi64(o, ms);
        print(o, S(" ms  (perm "));
        printi64(o, (perm.beg - r->perm.beg) / (in.len ? in.len : 1));
        print(o, S("/byte, check "));
        printi64(o, check);
        print(o, S(" ms)"));
        if (measure) {
            iz denom = in.len ? in.len : 1;
            print(o, S("  least perm "));
            printi64(o, min_arena(r, in, 0) / denom);
            print(o, S(" scratch "));
            printi64(o, min_arena(r, in, 1) / denom);
            print(o, S(" bytes per byte"));
        }
        print(o, S("\n"));
        flush(o);
    }
    print(o, S("path: "));
    printi64(o, (i64)countof(paths) - bad);
    print(o, S("/"));
    printi64(o, (i64)countof(paths));
    print(o, S(" under "));
    printi64(o, limit_ms);
    print(o, S(" ms, slowest "));
    print(o, cstr(worst_name));
    print(o, S(" "));
    printi64(o, worst);
    print(o, S(" ms\n"));
    flush(o);
    return bad;
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
        "usage: md-test [-s SEED] [-n ITERS] [-i START] [-d DIR] [-t WHICH] [-v]\n"
        "  -s SEED   random seed, decimal or 0x hex (default 0x5eed)\n"
        "  -n ITERS  random iterations to run (default 1500)\n"
        "  -i START  first iteration number, to replay a failure (default 0)\n"
        "  -d DIR    where the spec files are (default test)\n"
        "  -t WHICH  only run spec, random or path\n"
        "  -v        spec: show every example, not only failures\n"
        "  -m TEXT   parse TEXT (with \\n and \\t) and print the HTML, no tests\n"
        "  -f FILE   time and measure parsing one file, no tests\n"
        "  -z PCT    path: scale the inputs to PCT percent (default 100)\n"
        "  -u        path: also measure the memory each input needs (slow)\n"
    );
}

static i32 run(Arena all) {
    Rig r = {0};
    r.perm     = carve(&all, PERM_SIZE);
    r.scratch  = carve(&all, SCRATCH_SIZE);
    r.sperm    = carve(&all, SMALL_SIZE);
    r.sscratch = carve(&all, SMALL_SIZE);
    r.data     = carve(&all, 16<<20);
    r.gen      = carve(&all, GEN_SIZE);
    Arena html = carve(&all, 2*HTML_SIZE);
    r.html     = html.beg;
    r.html2    = html.beg + HTML_SIZE;
    Arena small = carve(&all, (iz)1<<20);
    ctx.out = newoutput(&small, GetStdHandle(STD_OUTPUT_HANDLE), 0, 1<<16);
    r.out   = ctx.out;
    AddVectoredExceptionHandler(1, onfault);
    alive();
    CreateThread(0, 0, watchdog, 0, 0, 0);

    u64 seed  = 0x5eed;
    u64 iters = 1500;
    u64 start = 0;
    Str dir   = S("test");
    Str which = {0};
    b32 verbose = 0;
    Str markdown = {0};
    Str file  = {0};
    Args args = getargs(&small);
    for (i32 i = 1; i < args.argc; i++) {
        Str arg  = args.argv[i];
        u64 *dst = 0;
        if (equals(arg, S("-v"))) {
            verbose = 1;
            continue;
        } else if (equals(arg, S("-d")) && i+1<args.argc) {
            dir = args.argv[++i];
            continue;
        } else if (equals(arg, S("-t")) && i+1<args.argc) {
            which = args.argv[++i];
            continue;
        } else if (equals(arg, S("-m")) && i+1<args.argc) {
            markdown = args.argv[++i];
            continue;
        } else if (equals(arg, S("-f")) && i+1<args.argc) {
            file = args.argv[++i];
            continue;
        } else if (equals(arg, S("-u"))) {
            measure = 1;
            continue;
        } else if (equals(arg, S("-z"))) {
            dst = &scale;
        } else if (equals(arg, S("-s"))) {
            dst = &seed;
        } else if (equals(arg, S("-n"))) {
            dst = &iters;
        } else if (equals(arg, S("-i"))) {
            dst = &start;
        }
        if (!dst || i+1>=args.argc || !parseu64(args.argv[++i], dst)) {
            print(ctx.out, S("md-test: bad argument: "));
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
    if (markdown.len) {
        // Parse the argument, with \n and \t as escapes, and print the HTML.
        Buf g = {r.gen.beg, 0, 1<<20, 0};
        for (iz i = 0; i < markdown.len; i++) {
            u8 c = markdown.data[i];
            if (c=='\\' && i+1<markdown.len && markdown.data[i+1]=='n') {
                c = '\n';
                i++;
            } else if (c=='\\' && i+1<markdown.len && markdown.data[i+1]=='t') {
                c = '\t';
                i++;
            }
            bc(&g, c);
        }
        MdNode *doc = parse(&r, r.perm, r.scratch, bstr(&g), MD_GFM, 0);
        print(o, render(r.html, doc));
        flush(o);
        return 0;
    }
    if (file.len) {
        // Time one real document and measure what it needs.
        Arena d  = r.data;
        Str   in = readpath(&d, file);
        if (!in.len) {
            print(o, S("md-test: cannot read the file\n"));
            flush(o);
            return 2;
        }
        i64 freq = 0, t0 = 0, t1 = 0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        Arena   perm = r.perm;
        MdNode *doc  = md_parse(&perm, r.scratch, in, MD_GFM);
        QueryPerformanceCounter(&t1);
        if (!doc) {
            fail("parse", "out of memory", 0, 0);
        }
        check_tree(doc, in, r.perm);
        print(o, S("bytes "));
        printi64(o, in.len);
        print(o, S("  parse "));
        printi64(o, (t1 - t0)*1000000/freq);
        print(o, S(" us  perm "));
        printi64(o, min_arena(&r, in, 0));
        print(o, S("  scratch "));
        printi64(o, min_arena(&r, in, 1));
        print(o, S("  html "));
        printi64(o, render(r.html, doc).len);
        print(o, S("\n"));
        flush(o);
        return 0;
    }
    b32 all_tests = !which.len;
    i32 bad = 0;
    u64 t0  = GetTickCount64();

    if (all_tests || equals(which, S("spec"))) {
        bad += run_spec(&r, dir, "commonmark.json", 0, verbose);
        bad += run_spec(&r, dir, "gfm.json", MD_GFM, verbose);
    }
    if (all_tests || equals(which, S("unit"))) {
        bad += run_unit(&r);
    }
    if (all_tests || equals(which, S("path"))) {
        bad += run_paths(&r, 1000);
    }
    if (all_tests || equals(which, S("random"))) {
        ctx.iter = 0;
        for (u64 i = 0; i < iters; i++) {
            iteration(&r, seed, (i64)(start + i));
        }
        ctx.iter = -1;
        ctx.phase = 0;
        ctx.input = (Str){0};
        print(o, S("random: ok seed=0x"));
        printhex(o, seed, 1);
        print(o, S(" iters="));
        printi64(o, (i64)iters);
        print(o, S("\n"));
    }
    u64 ms = max(GetTickCount64() - t0, (u64)1);
    print(o, bad ? S("md-test: FAILED, ") : S("md-test: ok, "));
    printi64(o, bad);
    print(o, S(" problems, "));
    printi64(o, (i64)ms);
    print(o, S(" ms\n"));
    flush(o);
    return bad ? 1 : 0;
}

void mainCRTStartup(void) {
    u8 *mem = VirtualAlloc(0, (uz)TOTAL_SIZE, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem+TOTAL_SIZE}));
}
