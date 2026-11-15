// Temporary demo of the platform interface: stands in for view.c until the
// layout engine exists. Exercises text runs, wrapping, hit testing, an
// inline box, drawing primitives, scrolling, zoom and theme changes.
// TODO(milestone 3): delete this file, and its include in main.c.

enum { DLINK = 1 };

enum {
    DF_B = 1,    // bold
    DF_I = 2,    // italic
    DF_S = 4,    // strikethrough
    DF_U = 8,    // underline
    DF_M = 16,   // monospace
    DF_L = 32,   // link
};

typedef enum { DBLK_TEXT, DBLK_CODE, DBLK_RULE } DKind;

typedef struct {
    DKind  kind;
    Paragraph *p;
    iz         box_beg;     // byte offset of an inline box, or -1
    f32        y;           // top, relative to the document
    f32        h;           // height including the space after
    f32        pad;         // padding inside a code block
} DBlock;

typedef struct {
    Color bg, text, link, codebg, border, rule, box;
} DTheme;

typedef struct {
    Run runs[24];
    i32 n;
    f32 size;
    Color color;
    DTheme const *theme;
} DRuns;

enum { DMAX_BLOCKS = 32 };

static struct {
    Arena  mem;             // for reading the file
    Str    path;
    u8     info[1100];
    iz     infolen;
    Size   size;
    DBlock  blocks[DMAX_BLOCKS];
    i32    nblocks;
    f32    scroll;
    f32    zoom;
    f32    total;           // document height
    DTheme  theme;
    i32    downlink;
} demo;

static DTheme const dlight = {
    {255, 255, 255, 255}, {31, 35, 40, 255}, {9, 105, 218, 255},
    {246, 248, 250, 255}, {208, 215, 222, 255}, {208, 215, 222, 255},
    {255, 129, 0, 255},
};
static DTheme const ddark = {
    {13, 17, 23, 255}, {230, 237, 243, 255}, {68, 147, 248, 255},
    {22, 27, 34, 255}, {48, 54, 61, 255}, {48, 54, 61, 255},
    {255, 166, 0, 255},
};

static void dadd(DRuns *rb, Str s, u32 flags) {
    assert(rb->n < countof(rb->runs));
    Run *r = rb->runs + rb->n++;
    *r = (Run){0};
    r->text      = s;
    r->font      = flags & DF_M ? FONT_MONOSPACE : FONT_PROPORTIONAL;
    r->size      = rb->size * (flags & DF_M ? 0.92f : 1.0f);
    r->bold      = !!(flags & DF_B);
    r->italic    = !!(flags & DF_I);
    r->strike    = !!(flags & DF_S);
    r->underline = !!(flags & (DF_U|DF_L));
    r->color     = flags & DF_L ? rb->theme->link : rb->color;
    r->link      = flags & DF_L ? DLINK : 0;
}

static void daddbox(DRuns *rb, f32 w, f32 h) {
    assert(rb->n < countof(rb->runs));
    Run *r = rb->runs + rb->n++;
    *r = (Run){0};
    r->size  = rb->size;
    r->color = rb->color;
    r->box_w = w;
    r->box_h = h;
}

static DRuns dnewbuf(f32 size, DTheme const *t) {
    DRuns rb = {0};
    rb.size  = size * demo.zoom;
    rb.color = t->text;
    rb.theme = &t[0];
    return rb;
}

static void dblock(DKind kind, Paragraph *p, iz box_beg, f32 pad) {
    assert(demo.nblocks < DMAX_BLOCKS);
    demo.blocks[demo.nblocks++] = (DBlock){kind, p, box_beg, 0, 0, pad};
}

static void demo_free(void) {
    for (i32 i = 0; i < demo.nblocks; i++) {
        os_para_free(demo.blocks[i].p);
    }
    demo.nblocks = 0;
}

static iz dfmtint(u8 *out, i64 v) {
    u8 tmp[24];
    iz n = 0;
    do {
        tmp[n++] = (u8)('0' + v%10);
        v /= 10;
    } while (v);
    for (iz i = 0; i < n; i++) {
        out[i] = tmp[n-1-i];
    }
    return n;
}

// Position blocks for the current width and compute the document height.
static void demo_layout(void) {
    f32 width = min(demo.size.w - 48, 760.0f);
    f32 y     = 24;
    for (i32 i = 0; i < demo.nblocks; i++) {
        DBlock *b = demo.blocks + i;
        b->y = y;
        if (b->kind == DBLK_RULE) {
            b->h = 25;
        } else {
            f32 w = b->kind==DBLK_CODE ? width - 2*b->pad : width;
            os_para_set_width(b->p, w);
            b->h = os_para_size(b->p).h + (b->kind==DBLK_CODE ? 2*b->pad + 14 : 14);
        }
        y += b->h;
    }
    demo.total = y + 24;
    f32 maxscroll = max(demo.total - demo.size.h, (f32)0);
    demo.scroll   = clampf(demo.scroll, 0, maxscroll);
}

static void demo_title(void) {
    Str name = demo.path;
    for (iz i = 0; i < demo.path.len; i++) {
        u8 c = demo.path.data[i];
        if (c=='\\' || c=='/') {
            name = (Str){demo.path.data+i+1, demo.path.len-i-1};
        }
    }
    u8  buf[600];
    iz  n   = min(name.len, (iz)512);
    memcpy(buf, name.data, (uz)n);
    Str suffix = name.len ? S(" - md") : S("md demo");
    memcpy(buf+n, suffix.data, (uz)suffix.len);
    os_set_title((Str){buf, n + suffix.len});
}

static void demo_info(void) {
    Str  head = S("File: ");
    u8  *out  = demo.info;
    memcpy(out, head.data, (uz)head.len);
    iz n = head.len;
    if (demo.path.len) {
        iz pl = min(demo.path.len, (iz)512);
        memcpy(out+n, demo.path.data, (uz)pl);
        n += pl;
        Str data = {0};
        Arena a  = demo.mem;
        if (os_read_file(&a, demo.path, &data)) {
            Str mid = S(", ");
            memcpy(out+n, mid.data, (uz)mid.len);
            n += mid.len;
            n += dfmtint(out+n, data.len);
            Str tail = S(" bytes");
            memcpy(out+n, tail.data, (uz)tail.len);
            n += tail.len;
        } else {
            Str bad = S(" (cannot read)");
            memcpy(out+n, bad.data, (uz)bad.len);
            n += bad.len;
        }
    } else {
        Str none = S("(none given on the command line)");
        memcpy(out+n, none.data, (uz)none.len);
        n += none.len;
    }
    demo.infolen = n;
}

static void demo_build(void) {
    demo_free();
    demo.theme = os_dark_mode() ? ddark : dlight;
    DTheme const *t = &demo.theme;

    DRuns h1 = dnewbuf(32, t);
    dadd(&h1, S("md platform demo"), DF_B);
    dblock(DBLK_TEXT, os_para_new(h1.runs, h1.n, 0), -1, 0);

    DRuns info = dnewbuf(13, t);
    dadd(&info, (Str){demo.info, demo.infolen}, DF_M);
    dblock(DBLK_TEXT, os_para_new(info.runs, info.n, 0), -1, 0);

    DRuns p1 = dnewbuf(16, t);
    dadd(&p1, S("This paragraph mixes "), 0);
    dadd(&p1, S("bold"), DF_B);
    dadd(&p1, S(", "), 0);
    dadd(&p1, S("italic"), DF_I);
    dadd(&p1, S(", "), 0);
    dadd(&p1, S("bold italic"), DF_B|DF_I);
    dadd(&p1, S(", "), 0);
    dadd(&p1, S("strikethrough"), DF_S);
    dadd(&p1, S(", "), 0);
    dadd(&p1, S("underline"), DF_U);
    dadd(&p1, S(", "), 0);
    dadd(&p1, S("inline code"), DF_M);
    dadd(&p1, S(", and a "), 0);
    dadd(&p1, S("link to example.com"), DF_L);
    dadd(&p1, S(". It is long enough to wrap over several lines, so narrow the window "
               "to watch it re-wrap, and hover the link to get a hand cursor. "
               "Click it to open the page in your browser."), 0);
    dblock(DBLK_TEXT, os_para_new(p1.runs, p1.n, 0), -1, 0);

    DRuns p2 = dnewbuf(16, t);
    dadd(&p2, S("Shaping and fallback: Привет, мир! Ελληνικά. 日本語のテキスト。中文文本。"
               "العربية (right to left). עברית. हिन्दी. "), 0);
    dadd(&p2, S("Emoji: 😀 🎉 👨‍👩‍👧 🇩🇪 ❤️ 🚀"), 0);
    dblock(DBLK_TEXT, os_para_new(p2.runs, p2.n, 0), -1, 0);

    DRuns h2 = dnewbuf(24, t);
    dadd(&h2, S("Code and boxes"), DF_B);
    dblock(DBLK_TEXT, os_para_new(h2.runs, h2.n, 0), -1, 0);

    DRuns code = dnewbuf(14, t);
    dadd(&code, S("static void os_set_cursor(Cursor c) {\n"
                 "    g.cursor = c;   // sticky\n"
                 "}\n"
                 "if (a < b && c != d) return \"mono\";"), DF_M);
    dblock(DBLK_CODE, os_para_new(code.runs, code.n, 0), -1, 14);

    DRuns p3 = dnewbuf(16, t);
    Str lead = S("An inline box is reserved space for something portable code draws, such as an image: ");
    dadd(&p3, lead, 0);
    daddbox(&p3, 28, 28);
    dadd(&p3, S(" and the text carries on after it, baseline-aligned, wrapping like any other word."), 0);
    dblock(DBLK_TEXT, os_para_new(p3.runs, p3.n, 0), lead.len, 0);

    dblock(DBLK_RULE, 0, -1, 0);

    DRuns p4 = dnewbuf(16, t);
    dadd(&p4, S("Scroll with the wheel, arrow keys, Page Up/Down, Home and End. Ctrl+wheel or "
               "Ctrl+Plus/Minus/0 zooms. Ctrl+O opens a file, Ctrl+C copies a line to the "
               "clipboard. Everything below is filler to give the scrollbar-less window "
               "something to scroll through."), 0);
    dblock(DBLK_TEXT, os_para_new(p4.runs, p4.n, 0), -1, 0);

    for (i32 i = 0; i < 6; i++) {
        DRuns f = dnewbuf(16, t);
        dadd(&f, S("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
                  "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, "
                  "quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo "
                  "consequat. "), 0);
        dadd(&f, S("Duis aute irure dolor"), DF_B);
        dadd(&f, S(" in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
                  "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa "
                  "qui officia deserunt mollit anim id est laborum."), 0);
        dblock(DBLK_TEXT, os_para_new(f.runs, f.n, 0), -1, 0);
    }
    demo_layout();
}

static void app_init(Arena *perm, Str path, Size size) {
    demo.mem  = carve(perm, (iz)16<<20);
    demo.path = path;
    demo.size = size;
    demo.zoom = 1;
    demo_title();
    demo_info();
    demo_build();
    os_watch_file(path);
}

static void app_paint(Arena scratch, Size size) {
    (void)scratch; (void)size;
    DTheme const *t = &demo.theme;
    f32 width = min(demo.size.w - 48, 760.0f);
    f32 x0    = (demo.size.w - width)/2;
    os_clear(t->bg);
    for (i32 i = 0; i < demo.nblocks; i++) {
        DBlock *b = demo.blocks + i;
        f32    y = b->y - demo.scroll;
        if (y+b->h < 0 || y > demo.size.h) {
            continue;
        }
        switch (b->kind) {
        case DBLK_RULE:
            os_stroke_line((Point){x0, y+12}, (Point){x0+width, y+12}, 1, t->rule);
            break;
        case DBLK_CODE: {
            Rect r = {x0, y, width, os_para_size(b->p).h + 2*b->pad};
            os_fill_rrect(r, 8, t->codebg);
            os_stroke_rrect(r, 8, 1, t->border);
            os_clip_push(r);
            os_para_draw(b->p, (Point){x0 + b->pad, y + b->pad});
            os_clip_pop();
            break;
        }
        case DBLK_TEXT:
            os_para_draw(b->p, (Point){x0, y});
            if (b->box_beg >= 0) {
                Rect r = {0};
                if (os_para_rects(b->p, b->box_beg, b->box_beg+3, &r, 1) >= 1) {
                    r.x += x0;
                    r.y += y;
                    os_fill_rrect(r, 6, t->box);
                }
            }
            break;
        }
    }
}

static void app_resize(Size size) {
    demo.size = size;
    demo_layout();
    os_invalidate();
}

static void dscrollby(f32 dy) {
    f32 maxscroll = max(demo.total - demo.size.h, (f32)0);
    demo.scroll   = clampf(demo.scroll + dy, 0, maxscroll);
    os_invalidate();
}

static void dzoomto(f32 z) {
    demo.zoom = clampf(z, 0.5f, 3.0f);
    demo_build();
    os_invalidate();
}

static void app_key(Key key, u32 mods) {
    f32 page = demo.size.h * 0.9f;
    if (mods & MOD_PRIMARY) {
        switch (key) {
        case KEY_PLUS:  dzoomto(demo.zoom * 1.1f); break;
        case KEY_MINUS: dzoomto(demo.zoom / 1.1f); break;
        case KEY_0:     dzoomto(1); break;
        case KEY_C:
            os_clipboard_set(S("md platform demo: copied from the clipboard test"));
            break;
        case KEY_O: {
            Str p = {0};
            Arena a = demo.mem;
            if (os_open_dialog(&a, &p)) {
                // Keep the path in the info buffer's neighbor: copy out.
                static u8 pathbuf[1100];
                iz n = min(p.len, (iz)sizeof(pathbuf));
                memcpy(pathbuf, p.data, (uz)n);
                demo.path = (Str){pathbuf, n};
                demo_title();
                demo_info();
                demo_build();
                os_watch_file(demo.path);
                os_invalidate();
            }
            break;
        }
        default: break;
        }
        return;
    }
    switch (key) {
    case KEY_UP:    dscrollby(-48); break;
    case KEY_DOWN:  dscrollby(48); break;
    case KEY_PGUP:  dscrollby(-page); break;
    case KEY_PGDN:
    case KEY_SPACE: dscrollby(page); break;
    case KEY_HOME:  dscrollby(-1e9f); break;
    case KEY_END:   dscrollby(1e9f); break;
    default: break;
    }
}

static void app_char(i32 rune) {
    (void)rune;
}

// Which link, if any, is under p.
static i32 dlinkat(Point p) {
    f32 width = min(demo.size.w - 48, 760.0f);
    f32 x0    = (demo.size.w - width)/2;
    for (i32 i = 0; i < demo.nblocks; i++) {
        DBlock *b = demo.blocks + i;
        if (b->kind != DBLK_TEXT) {
            continue;
        }
        f32 y = b->y - demo.scroll;
        Size s = os_para_size(b->p);
        if (p.y>=y && p.y<y+s.h && p.x>=x0 && p.x<x0+width) {
            TextHit hit = os_para_hit(b->p, (Point){p.x - x0, p.y - y});
            return hit.link;
        }
    }
    return 0;
}

static void app_mouse_move(Point p, u32 mods) {
    (void)mods;
    os_set_cursor(dlinkat(p) ? CURSOR_HAND : CURSOR_ARROW);
}

static void app_mouse_down(Point p, Button b, u32 mods) {
    (void)mods;
    demo.downlink = b==BUTTON_LEFT ? dlinkat(p) : 0;
}

static void app_mouse_up(Point p, Button b, u32 mods) {
    (void)mods;
    i32 link = dlinkat(p);
    if (b==BUTTON_LEFT && link && link==demo.downlink) {
        os_open_url(S("https://example.com"));
    }
    demo.downlink = 0;
}

static void app_double_click(Point p, u32 mods) {
    (void)p; (void)mods;
}

static void app_wheel(Point p, f32 dx, f32 dy, u32 mods) {
    (void)p; (void)dx;
    if (mods & MOD_PRIMARY) {
        dzoomto(demo.zoom * (dy > 0 ? 1.1f : 1/1.1f));
    } else {
        dscrollby(-dy * 3 * 16 * demo.zoom);
    }
}

static void app_timer(i32 id) {
    (void)id;
}

static void app_file_changed(void) {
    demo_info();
    demo_build();
    os_invalidate();
}

static void app_file_dropped(Str path) {
    static u8 pathbuf[1100];
    iz n = min(path.len, (iz)sizeof(pathbuf));
    memcpy(pathbuf, path.data, (uz)n);
    demo.path = (Str){pathbuf, n};
    demo_title();
    demo_info();
    demo_build();
    os_watch_file(demo.path);
    os_invalidate();
}

static void app_theme_changed(void) {
    demo_build();
    os_invalidate();
}

static void app_close(void) {
    demo_free();
}
