// The viewer: implements the app_* callbacks of platform.h on top of layout.c.
// Portable; the only way out is platform.h.
//
// One document is open at a time. Loading it starts from fresh arenas, so
// everything the previous document owned (tree, leaves, images) goes at
// once, and its paragraphs and images are freed first. What is on screen
// is laid out lazily, see layout.c; this file keeps the reading position
// (scroll) and the things around it: scrollbar, links, history, zoom.

typedef enum {
    VS_EMPTY,               // no document yet
    VS_DOC,
    VS_ERROR,               // the file could not be opened; V.msg says why
} ViewState;

enum {
    PATHCAP   = 1100,
    MSGCAP    = 1800,
    HIST      = 32,
    ANIM_TIMER = 1,
    ANIM_MS   = 16,
    BAR_ZONE  = 20,         // width of the strip at the right that grabs the scrollbar
};

typedef struct {
    u8     path[PATHCAP];
    iz     len;
    Anchor at;              // where the reader was when leaving
} HistEntry;

static struct {
    Arena     mem0;         // all the memory documents may use
    Arena     arena;        // the open document's allocations, D.mem points here
    Arena     tmp;          // working memory for events, by value
    Size      size;
    f32       zoom, scroll, zoom_acc;
    b32       dark;
    ViewState state;
    u8        path[PATHCAP];
    iz        pathlen;
    u8        msg[MSGCAP];
    iz        msglen;

    // The reading position is V.scroll, unless one of these says otherwise:
    // after something changes the layout, the position is a leaf and an
    // offset, or the end of the document, until the next layout.
    b32       dirty;        // the layout must be redone before it is trusted
    b32       pinned;
    Anchor    pin;
    b32       pin_end;

    Point     mouse;
    b32       inside;       // the pointer is in the window
    Str       hover;        // destination of the link under it
    Paragraph *status;      // the bubble showing hover
    i32       down_leaf, down_cell, down_link;
    b32       bar_hot;      // the pointer is over the scrollbar's strip
    b32       drag;         // dragging the scrollbar thumb
    f32       drag_off;
    b32       timer;        // the animation timer runs

    HistEntry hist[HIST];
    i32       nhist, cur;
} V;

static f32 const zoom_levels[] = {
    0.5f, 0.67f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4,
};


// Paths and links

// memcpy for ranges that may overlap. Not allowed to turn itself into a
// call to memmove, which there is no C runtime to provide.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void bytes_move(void *to, void const *from, iz n) {
    u8       *dst = to;
    u8 const *src = from;
    if (dst < src) {
        for (iz i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    } else {
        for (iz i = n; i > 0; i--) {
            dst[i-1] = src[i-1];
        }
    }
}

static b32 is_sep(u8 c) {
    return c=='/' || c=='\\';
}

static Str path_name(Str p) {
    iz b = 0;
    for (iz i = 0; i < p.len; i++) {
        if (is_sep(p.data[i])) {
            b = i + 1;
        }
    }
    return (Str){p.data+b, p.len-b};
}

static b32 has_ext(Str p, Str ext) {
    Str n = path_name(p);
    return n.len > ext.len && ci_equals((Str){n.data + n.len - ext.len, ext.len}, ext);
}

static b32 is_markdown_path(Str p) {
    return has_ext(p, S(".md")) || has_ext(p, S(".markdown"));
}

// "http:", "mailto:", "C:" and the like.
static b32 has_scheme(Str s) {
    iz i = 0;
    while (i<s.len && (md_isalnum(s.data[i]) || s.data[i]=='+' || s.data[i]=='-' || s.data[i]=='.')) {
        i++;
    }
    return i>=2 && i<s.len && s.data[i]==':' && md_isalpha(s.data[0]);
}

static i32 hexval(u8 c) {
    return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : -1;
}

// Undo percent-encoding into out (which needs s.len bytes).
static Str unescape(u8 *out, Str s) {
    iz n = 0;
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i]=='%' && i+2<s.len+0 && hexval(s.data[i+1])>=0 && hexval(s.data[i+2])>=0) {
            out[n++] = (u8)(hexval(s.data[i+1])*16 + hexval(s.data[i+2]));
            i += 2;
        } else {
            out[n++] = s.data[i];
        }
    }
    return (Str){out, n};
}

// A file path for a relative reference found in the document at dir:
// the reference is cut at '?' and '#', unescaped, joined to dir, and
// cleaned of "." and ".." segments. Separators come out as dir's, or '/'.
// Returns an empty Str if it doesn't fit.
static Str resolve_path(u8 *out, iz cap, Str dir, Str rel) {
    for (iz i = 0; i < rel.len; i++) {
        if (rel.data[i]=='?' || rel.data[i]=='#') {
            rel.len = i;
            break;
        }
    }
    u8 sep = '/';
    for (iz i = 0; i < dir.len; i++) {
        if (is_sep(dir.data[i])) {
            sep = dir.data[i];
            break;
        }
    }
    if (rel.len + dir.len + 2 > cap) {
        return (Str){0};
    }
    u8 *joined = out + cap - (rel.len + dir.len + 2);   // built at the end of out
    Str r = unescape(joined + dir.len + 1, rel);
    b32 abs = (r.len && is_sep(r.data[0])) || (r.len>=2 && md_isalpha(r.data[0]) && r.data[1]==':');
    // r sits at joined+dir.len+1; put the pieces in order in front of it.
    Str j;
    if (abs) {
        bytes_move(joined, r.data, r.len);
        j = (Str){joined, r.len};
    } else {
        memcpy(joined, dir.data, (uz)dir.len);
        if (dir.len && !is_sep(dir.data[dir.len-1])) {
            joined[dir.len] = sep;
            j = (Str){joined, dir.len + 1 + r.len};
        } else {
            bytes_move(joined + dir.len, r.data, r.len);
            j = (Str){joined, dir.len + r.len};
        }
    }
    // Prefix: a drive, a root, or a UNC root.
    iz  pos = 0;
    u8  prefix[4];
    iz  np  = 0;
    if (j.len>=2 && md_isalpha(j.data[0]) && j.data[1]==':') {
        prefix[np++] = j.data[0];
        prefix[np++] = ':';
        pos = 2;
    }
    b32 root = pos<j.len && is_sep(j.data[pos]);
    if (root) {
        prefix[np++] = sep;
        pos++;
        if (np == 1 && pos<j.len && is_sep(j.data[pos])) {
            prefix[np++] = sep;     // \\server
            pos++;
        }
    }
    struct { iz beg, len; } seg[64];
    i32 ns = 0;
    while (pos <= j.len) {
        iz e = pos;
        while (e<j.len && !is_sep(j.data[e])) {
            e++;
        }
        Str s = {j.data+pos, e-pos};
        if (s.len==0 || equals(s, S("."))) {
            // nothing
        } else if (equals(s, S(".."))) {
            if (ns>0 && !equals((Str){j.data+seg[ns-1].beg, seg[ns-1].len}, S(".."))) {
                ns--;
            } else if (!root) {
                if (ns == 64) {
                    return (Str){0};
                }
                seg[ns].beg = pos;
                seg[ns].len = 2;
                ns++;
            }
        } else {
            if (ns == 64) {
                return (Str){0};
            }
            seg[ns].beg = pos;
            seg[ns].len = s.len;
            ns++;
        }
        pos = e + 1;
    }
    // Write the result over the front of out; segments live at the back, and
    // the result is never longer than what it was made from.
    u8 res[PATHCAP + 8];
    iz n = 0;
    if (np + 2 + j.len > (iz)sizeof(res)) {
        return (Str){0};
    }
    for (iz i = 0; i < np; i++) {
        res[n++] = prefix[i];
    }
    for (i32 i = 0; i < ns; i++) {
        if (i) {
            res[n++] = sep;
        }
        for (iz k = 0; k < seg[i].len; k++) {
            res[n++] = j.data[seg[i].beg + k];
        }
    }
    if (n > cap) {
        return (Str){0};
    }
    memcpy(out, res, (uz)n);
    return (Str){out, n};
}

static Str current_dir(void) {
    iz e = 0;
    for (iz i = 0; i < V.pathlen; i++) {
        if (is_sep(V.path[i])) {
            e = i + 1;
        }
    }
    return (Str){V.path, e};
}

// Where an image's bytes come from. Local files only, for now: a remote
// source is where a download will go.
static b32 img_fetch(Arena *a, Str src, Str *bytes) {
    if (!src.len || has_scheme(src)) {
        return 0;
    }
    u8  buf[PATHCAP];
    Str p = resolve_path(buf, sizeof(buf), current_dir(), src);
    return p.len && os_read_file(a, p, bytes);
}


// Messages

static void msg_add(Str s) {
    iz n = min(s.len, (iz)MSGCAP - V.msglen);
    memcpy(V.msg + V.msglen, s.data, (uz)n);
    V.msglen += n;
}

static void set_title(void) {
    u8  buf[PATHCAP + 16];
    Str name = path_name((Str){V.path, V.pathlen});
    if (V.state != VS_DOC || !name.len) {
        os_set_title(S("md"));
        return;
    }
    iz n = min(name.len, (iz)PATHCAP);
    memcpy(buf, name.data, (uz)n);
    memcpy(buf + n, " - md", 5);
    os_set_title((Str){buf, n + 5});
}

static Theme const *current_theme(void) {
    return V.dark ? &theme_dark : &theme_light;
}


// Reading position

static Anchor current_anchor(void) {
    return V.pinned ? V.pin : doc_anchor(V.scroll);
}

static void pin_to(Anchor a) {
    V.pin     = a;
    V.pinned  = 1;
    V.dirty   = 1;
    os_invalidate();
}

// Make the layout trustworthy: build what is on screen at the current
// position. Paint does this too; events that look at what is on screen
// call it first.
static void layout_now(void) {
    if (V.state != VS_DOC || !V.dirty) {
        return;
    }
    Arena tmp = V.tmp;
    RB rb;
    rb_init(&rb, &tmp);
    doc_ensure(&rb, &V.scroll, V.size.h, V.pinned ? &V.pin : 0, V.pin_end);
    V.pinned = 0;
    V.dirty  = 0;
}

static f32 max_scroll(void) {
    return max(D.total - V.size.h, (f32)0);
}

static void scroll_to(f32 y) {
    layout_now();
    V.pin_end = 0;
    V.scroll  = clampf(y, 0, max_scroll());
    V.dirty   = 1;      // new leaves may need making, and corrections
    os_invalidate();
}

static void scroll_by(f32 dy) {
    layout_now();
    scroll_to(V.scroll + dy);
}


// Loading documents

static void hover_clear(void) {
    if (V.status) {
        os_para_free(V.status);
        V.status = 0;
    }
    V.hover = (Str){0};
}

static void anim_stop(void) {
    if (V.timer) {
        os_timer_stop(ANIM_TIMER);
        V.timer = 0;
    }
}

static void show_error(Str what) {
    V.state   = VS_ERROR;
    V.msglen  = 0;
    msg_add(what);
    msg_add(S("\n"));
    msg_add((Str){V.path, V.pathlen});
}

// Open the file at path, positioning the view at restore (if not 0) or at
// the heading named frag (if not empty) or at the top. path may point into
// V.path.
static void load_document(Str path, Anchor const *restore, Str frag) {
    doc_clear();
    hover_clear();
    anim_stop();
    V.down_link = 0;
    V.drag      = 0;
    iz n = min(path.len, (iz)PATHCAP);
    if (path.data != V.path) {
        memcpy(V.path, path.data, (uz)n);
    }
    V.pathlen = n;
    V.state   = VS_ERROR;
    V.scroll  = 0;
    V.pinned  = V.pin_end = 0;
    V.dirty   = 0;
    V.arena   = V.mem0;
    os_watch_file((Str){V.path, V.pathlen});

    Arena a   = V.arena;
    Str   src = {0};
    if (!os_read_file(&a, (Str){V.path, V.pathlen}, &src)) {
        show_error(S("Could not open the file. It may be missing, unreadable, or too large."));
        set_title();
        return;
    }
    // The parser's worst case, from its documentation, is 130 N for the tree
    // and 35 N of working memory; typical text needs a twentieth of that.
    iz need = max((iz)256<<10, 40*src.len);
    need = (need + 4095) & ~(iz)4095;
    if (need + 65536 > a.end - a.beg) {
        show_error(S("The file is too large to open."));
        set_title();
        return;
    }
    Arena scr = {a.end - need, a.end};
    a.end     = scr.beg;
    MdNode *root = md_parse(&a, scr, src, MD_GFM);
    if (!root) {
        show_error(S("The file is too large to open."));
        set_title();
        return;
    }
    md_slugify(&a, scr, root);
    a.end     = scr.end;
    V.arena   = a;
    D.mem     = &V.arena;
    D.root    = root;
    doc_configure(V.size.w, V.zoom, current_theme());
    doc_flatten();
    V.state = VS_DOC;
    set_title();

    Anchor at = {0};
    if (restore) {
        at = doc_anchor_for_line(restore->line, restore->off);
    } else if (frag.len) {
        u8  buf[PATHCAP];
        Str slug = unescape(buf, frag);
        i32 i = doc_find_slug(slug);
        at.leaf = max(i, 0);
    }
    pin_to(at);
}


// History

static void hist_save(void) {
    if (V.cur >= 0 && V.cur < V.nhist && V.state == VS_DOC) {
        V.hist[V.cur].at = current_anchor();
    }
}

// Record a new place after the current one, dropping what was forward.
static void hist_push(Str path, Anchor at) {
    hist_save();
    V.nhist = V.nhist ? V.cur + 1 : 0;
    if (V.nhist == HIST) {
        bytes_move(V.hist, V.hist + 1, (HIST - 1)*(iz)sizeof(HistEntry));
        V.nhist--;
    }
    HistEntry *h = V.hist + V.nhist++;
    h->len = min(path.len, (iz)PATHCAP);
    memcpy(h->path, path.data, (uz)h->len);
    h->at  = at;
    V.cur  = V.nhist - 1;
}

static void go_history(i32 i) {
    if (i < 0 || i >= V.nhist) {
        return;
    }
    hist_save();
    V.cur = i;
    HistEntry *h = V.hist + i;
    Str p = {h->path, h->len};
    if (V.state==VS_DOC && equals(p, (Str){V.path, V.pathlen})) {
        pin_to(h->at);
    } else {
        Anchor at = h->at;
        load_document(p, &at, (Str){0});
    }
}

// Open a file as a new place in the history.
static void open_path(Str path, Str frag) {
    hist_push(path, (Anchor){0});
    load_document(path, 0, frag);
}

static void open_dialog(void) {
    Arena tmp = V.tmp;
    Str   p   = {0};
    if (os_open_dialog(&tmp, &p) && p.len) {
        open_path(p, (Str){0});
    }
}

// Follow a link of the document.
static void open_link(Str dest) {
    if (!dest.len) {
        return;
    }
    if (dest.data[0] == '#') {
        layout_now();
        u8  buf[PATHCAP];
        Str slug = unescape(buf, (Str){dest.data+1, dest.len-1});
        i32 i = doc_find_slug(slug);
        if (i < 0) {
            u8 low[PATHCAP];
            iz n = min(slug.len, (iz)PATHCAP);
            for (iz k = 0; k < n; k++) {
                low[k] = lower(slug.data[k]);
            }
            i = doc_find_slug((Str){low, n});
        }
        if (i >= 0) {
            Anchor at = {i, 0, D.leaves[i].line};
            hist_push((Str){V.path, V.pathlen}, at);
            pin_to(at);
        }
        return;
    }
    if (has_scheme(dest)) {
        os_open_url(dest);
        return;
    }
    u8  buf[PATHCAP];
    Str p = resolve_path(buf, sizeof(buf), current_dir(), dest);
    if (!p.len || !is_markdown_path(p)) {
        return;
    }
    Str frag = {0};
    for (iz i = 0; i < dest.len; i++) {
        if (dest.data[i] == '#') {
            frag = (Str){dest.data+i+1, dest.len-i-1};
            break;
        }
    }
    open_path(p, frag);
}


// Mouse

typedef struct {
    b32  show;
    Rect thumb;
    f32  track;             // length the thumb moves along
} Bar;

static Bar bar_geometry(void) {
    Bar b = {0};
    f32 h = V.size.h;
    if (V.state!=VS_DOC || D.total <= h + 0.5f || h < 40) {
        return b;
    }
    b.show = 1;
    f32 len   = clampf(h*h/D.total, 32, h);
    f32 maxs  = max_scroll();
    f32 w     = V.drag || (V.inside && V.mouse.x >= V.size.w - BAR_ZONE) ? 10.0f : 6.0f;
    b.track   = h - len;
    b.thumb   = (Rect){V.size.w - w - 3, maxs > 0 ? b.track*V.scroll/maxs : 0, w, len};
    return b;
}

// What the pointer is over: the cursor, and the link's destination if any.
static void hover_refresh(RB *rb) {
    Str    url = {0};
    Cursor cur = CURSOR_ARROW;
    if (V.state==VS_DOC && V.inside && !V.drag && V.mouse.x < V.size.w - BAR_ZONE) {
        Hit h = doc_hit(V.mouse, V.scroll);
        if (h.leaf>=0 && h.p && h.th.inside) {
            cur = CURSOR_IBEAM;
            if (h.th.link) {
                url = doc_link(D.leaves + h.leaf, h.cell, h.th.link, rb);
                if (url.len) {
                    cur = CURSOR_HAND;
                }
            }
        }
    }
    os_set_cursor(cur);
    if (url.len!=V.hover.len || url.data!=V.hover.data) {
        if (V.status) {
            os_para_free(V.status);
            V.status = 0;
        }
        V.hover = url;
        os_invalidate();
    }
}

static void hover_now(void) {
    layout_now();
    Arena tmp = V.tmp;
    RB rb;
    rb_init(&rb, &tmp);
    hover_refresh(&rb);
}

static void drag_to(f32 y) {
    Bar b = bar_geometry();
    if (b.show && b.track > 0) {
        scroll_to((y - V.drag_off)/b.track * max_scroll());
    }
}

static void app_mouse_move(Point p, u32 mods) {
    (void)mods;
    V.mouse  = p;
    V.inside = 1;
    if (V.drag) {
        drag_to(p.y);
        return;
    }
    hover_now();
    // The bar widens when approached.
    b32 hot = p.x >= V.size.w - BAR_ZONE;
    if (hot != V.bar_hot) {
        V.bar_hot = hot;
        os_invalidate();
    }
}

static void app_mouse_leave(void) {
    V.inside  = 0;
    V.bar_hot = 0;
    hover_now();
    os_invalidate();
}

static void app_mouse_down(Point p, Button b, u32 mods) {
    (void)mods;
    V.mouse  = p;
    V.inside = 1;
    V.down_link = 0;
    if (b != BUTTON_LEFT || V.state != VS_DOC) {
        return;
    }
    layout_now();
    if (p.x >= V.size.w - BAR_ZONE) {
        Bar bar = bar_geometry();
        if (bar.show) {
            if (p.y>=bar.thumb.y && p.y<bar.thumb.y+bar.thumb.h) {
                V.drag     = 1;
                V.drag_off = p.y - bar.thumb.y;
            } else {
                scroll_by(p.y < bar.thumb.y ? -V.size.h*0.9f : V.size.h*0.9f);
            }
            return;
        }
    }
    Hit h = doc_hit(p, V.scroll);
    if (h.leaf>=0 && h.p && h.th.inside && h.th.link) {
        V.down_leaf = h.leaf;
        V.down_cell = h.cell;
        V.down_link = h.th.link;
    }
}

static void app_mouse_up(Point p, Button b, u32 mods) {
    (void)mods;
    V.mouse = p;
    if (V.drag) {
        V.drag = 0;
        os_invalidate();
        return;
    }
    if (b != BUTTON_LEFT || !V.down_link || V.state != VS_DOC) {
        return;
    }
    i32 link = V.down_link, leaf = V.down_leaf, cell = V.down_cell;
    V.down_link = 0;
    layout_now();
    Hit h = doc_hit(p, V.scroll);
    if (h.leaf==leaf && h.cell==cell && h.p && h.th.inside && h.th.link==link) {
        Arena tmp = V.tmp;
        RB rb;
        rb_init(&rb, &tmp);
        Str dest = doc_link(D.leaves + leaf, cell, link, &rb);
        open_link(dest);
    }
}

static void app_double_click(Point p, u32 mods) {
    (void)p; (void)mods;
}


// Zoom

static void set_zoom(f32 z) {
    z = clampf(z, zoom_levels[0], zoom_levels[countof(zoom_levels) - 1]);
    if (z == V.zoom || V.state != VS_DOC) {
        V.zoom = z;
        return;
    }
    Anchor a = current_anchor();
    a.off   *= z / V.zoom;
    V.zoom   = z;
    doc_configure(V.size.w, z, current_theme());
    pin_to(a);
}

// Step through the zoom levels, from the nearest to the current one.
static void zoom_step(i32 dir) {
    i32 n = (i32)countof(zoom_levels);
    i32 at = 0;
    for (i32 i = 0; i < n; i++) {
        if (zoom_levels[i] <= V.zoom*1.001f) {
            at = i;
        }
    }
    i32 to = at + dir;
    if (dir < 0 && zoom_levels[at] < V.zoom*0.999f) {
        to = at;    // between two levels: going down lands on the one below
    }
    set_zoom(zoom_levels[clamp32(to, 0, n - 1)]);
}

static void app_wheel(Point p, f32 dx, f32 dy, u32 mods) {
    (void)p; (void)dx;
    if (mods & MOD_PRIMARY) {
        V.zoom_acc += dy;
        while (V.zoom_acc >= 0.5f) {
            zoom_step(1);
            V.zoom_acc -= 1;
        }
        while (V.zoom_acc <= -0.5f) {
            zoom_step(-1);
            V.zoom_acc += 1;
        }
        return;
    }
    scroll_by(-dy * 3 * line_body * V.zoom);
}


// Keyboard

static void reload(void) {
    if (!V.pathlen) {
        return;
    }
    Anchor a = current_anchor();
    Str p    = {V.path, V.pathlen};
    load_document(p, V.state == VS_DOC ? &a : 0, (Str){0});
}

static void app_key(Key key, u32 mods) {
    f32 line = 2*line_body*V.zoom;
    f32 page = V.size.h*0.9f;
    if (mods & MOD_PRIMARY) {
        switch (key) {
        case KEY_PLUS:  zoom_step(1); break;
        case KEY_MINUS: zoom_step(-1); break;
        case KEY_0:     set_zoom(1); break;
        case KEY_O:     open_dialog(); break;
        case KEY_R:     reload(); break;
        default: break;
        }
        return;
    }
    if (mods & MOD_ALT) {
        if (key == KEY_LEFT) {
            go_history(V.cur - 1);
        } else if (key == KEY_RIGHT) {
            go_history(V.cur + 1);
        }
        return;
    }
    switch (key) {
    case KEY_BACKSPACE: go_history(V.cur - 1); break;
    case KEY_F5:        reload(); break;
    default: break;
    }
    if (V.state != VS_DOC) {
        return;
    }
    switch (key) {
    case KEY_UP:    scroll_by(-line); break;
    case KEY_DOWN:  scroll_by(line); break;
    case KEY_PGUP:  scroll_by(-page); break;
    case KEY_PGDN:  scroll_by(page); break;
    case KEY_SPACE: scroll_by(mods & MOD_SHIFT ? -page : page); break;
    case KEY_HOME:  scroll_to(0); break;
    case KEY_END:
        layout_now();
        V.pin_end = 1;
        V.dirty   = 1;
        os_invalidate();
        break;
    default: break;
    }
}

static void app_char(i32 rune) {
    (void)rune;
}


// Window and files

static void app_resize(Size size) {
    if (size.w==V.size.w && size.h==V.size.h) {
        return;
    }
    b32 wider = size.w != V.size.w;
    if (V.state == VS_DOC && wider) {
        Anchor a = current_anchor();
        V.size = size;
        doc_configure(size.w, V.zoom, current_theme());
        pin_to(a);
    } else {
        V.size  = size;
        V.dirty = 1;
    }
    os_invalidate();
}

static void app_theme_changed(void) {
    b32 dark = os_dark_mode();
    if (dark == V.dark) {
        return;
    }
    V.dark = dark;
    if (V.state == VS_DOC) {
        Anchor a = current_anchor();
        doc_configure(V.size.w, V.zoom, current_theme());
        pin_to(a);
    }
    os_invalidate();
}

static void app_file_changed(void) {
    reload();
}

static void app_file_dropped(Str path) {
    if (path.len) {
        open_path(path, (Str){0});
    }
}

static void app_timer(i32 id) {
    if (id != ANIM_TIMER) {
        return;
    }
    if (!D.anim) {
        anim_stop();
        return;
    }
    b32 changed = 0;
    for (i32 h = 0; h < 64; h++) {
        for (ImgEntry *e = D.imgs[h]; e; e = e->next) {
            if (!e->im || e->frames < 2 || !e->drawn) {
                continue;
            }
            e->acc += ANIM_MS;
            i32 d = os_image_delay(e->im, e->frame);
            while (e->acc >= max(d, (i32)10)) {
                e->acc  -= max(d, (i32)10);
                e->frame = (e->frame + 1) % e->frames;
                d        = os_image_delay(e->im, e->frame);
                changed  = 1;
            }
        }
    }
    if (changed) {
        os_invalidate();
    }
}

static void app_close(void) {
    anim_stop();
    hover_clear();
    doc_clear();
    os_watch_file((Str){0});
}


// Painting

// A paragraph of plain text, drawn once and freed.
static void draw_message(Str text, f32 size, b32 bold, Color color, f32 width, f32 *y, f32 gap) {
    Run r = {0};
    r.text  = text;
    r.size  = size;
    r.bold  = bold;
    r.color = color;
    Paragraph *p = os_para_new(&r, 1, width);
    if (!p) {
        return;
    }
    os_para_style(p, 0, ALIGN_CENTER);
    Size s = os_para_size(p);
    os_para_draw(p, (Point){(V.size.w - width)/2, *y});
    *y += s.h + gap;
    os_para_free(p);
}

static void draw_empty(void) {
    Theme const *t = current_theme();
    f32 w = clampf(V.size.w - 64, 100, 600);
    f32 y = V.size.h/2 - 20;
    if (V.state == VS_ERROR) {
        y -= 40;
        draw_message(S("Cannot show this file"), 20, 1, t->error, w, &y, 12);
        Str m = {V.msg, V.msglen};
        draw_message(m, 14, 0, t->muted, w, &y, 0);
    } else {
        draw_message(S("Open a Markdown file with Ctrl+O, or drop one here"), 18, 0, t->muted, w, &y, 0);
    }
}

static void draw_status(void) {
    if (!V.hover.len) {
        return;
    }
    Theme const *t = current_theme();
    if (!V.status) {
        Run r = {0};
        r.text  = V.hover;
        r.size  = 12;
        r.color = t->text;
        V.status = os_para_new(&r, 1, max(min(V.size.w*0.7f, (f32)700), (f32)100));
    }
    if (!V.status) {
        return;
    }
    Size s = os_para_size(V.status);
    Rect r = {0, V.size.h - s.h - 10, s.w + 16, s.h + 10};
    if (V.inside && V.mouse.x < r.w + 24 && V.mouse.y > r.y - 12) {
        r.x = V.size.w - r.w;       // out of the pointer's way
    }
    os_fill_rrect(r, 4, t->status_bg);
    os_stroke_rrect(r, 4, 1, t->border);
    os_para_draw(V.status, (Point){r.x + 8, r.y + 5});
}

static void draw_bar(void) {
    Bar b = bar_geometry();
    if (b.show) {
        os_fill_rrect(b.thumb, b.thumb.w/2, current_theme()->thumb);
    }
}

static void app_paint(Arena scratch, Size size) {
    if (size.w!=V.size.w || size.h!=V.size.h) {
        app_resize(size);
    }
    os_clear(current_theme()->bg);
    if (V.state != VS_DOC) {
        draw_empty();
        return;
    }
    RB rb;
    rb_init(&rb, &scratch);
    doc_ensure(&rb, &V.scroll, size.h, V.pinned ? &V.pin : 0, V.pin_end);
    V.pinned  = 0;
    V.pin_end = 0;
    V.dirty   = 0;
    doc_paint(V.scroll, size, &rb);
    hover_refresh(&rb);
    draw_status();
    draw_bar();
    doc_evict(V.scroll, size.h);
    if (D.anim && !V.timer) {
        os_timer_start(ANIM_TIMER, ANIM_MS);
        V.timer = 1;
    }
}


// Start

static void app_init(Arena *perm, Str path, Size size) {
    memset(&V, 0, sizeof(V));
    V.tmp = carve(perm, (iz)4<<20);
    iz room = (perm->end - perm->beg - 2*4096) & ~(iz)4095;
    V.mem0  = carve(perm, room);
    V.size  = size;
    V.zoom  = 1;
    V.dark  = os_dark_mode();
    V.cur   = -1;
    D.th    = current_theme();
    D.zoom  = 1;
    doc_configure(size.w, 1, D.th);
    if (path.len) {
        open_path(path, (Str){0});
    } else {
        set_title();
    }
}
