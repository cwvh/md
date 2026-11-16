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
    SEL_TIMER = 2,          // scrolls while a selection is dragged out of the window
    SEL_MS    = 16,
    BAR_ZONE  = 20,         // width of the strip at the right that grabs the scrollbar
    FIND_MAX  = 240,        // bytes in the find field
    DRAG_PX   = 3,          // how far the mouse moves before a press is a drag
    TRIPLE_MS = 500,        // a third click this soon after a double click
};

typedef struct {
    u8     path[PATHCAP];
    iz     len;
    Anchor at;              // where the reader was when leaving
} HistEntry;

// The selection runs from anchor to focus, which may come in either order;
// on is false while they are the same place. The press that started it is
// remembered to tell a click from a drag.
typedef struct {
    DocPos    anchor, focus;
    b32       have;         // anchor is a place the user clicked
    b32       on;
    b32       down;         // the left button is down, after a press in the text
    b32       moved;        // and has dragged, or the press was a double or triple click
    Point     press;
    i32       gran;         // a drag selects by 0 characters, 1 words, 2 lines
    DocPos    unit0, unit1; // the word or line that was selected first
    b32       dbl;          // a double click happened at t_dbl and p_dbl
    u64       t_dbl;
    Point     p_dbl;
    b32       timer;        // the auto-scroll timer runs
} Sel;

// The find bar: a text field and its results. The query is searched in the
// text of the whole document, see doc_find_scan.
typedef struct {
    b32       open;
    u8        text[FIND_MAX];
    iz        len, caret;
    b32       all;          // all of the text is selected
    u8        fold[FIND_MAX];
    b32       have;         // cur is a match
    DocPos    cur;
    i32       n, total;     // which match cur is, from 1, and how many
    b32       stale;        // the document was loaded again: search anew
    i32       keep_n;       // ... and go to this match
    Paragraph *pt, *pc;     // the field's text and the count, made when drawn
    f32       sx;           // how far the text in the field is scrolled
} Find;

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

    Sel       sel;
    Find      find;

    HistEntry hist[HIST];
    i32       nhist, cur;
} V;

static f32 const zoom_levels[] = {
    0.5f, 0.67f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4,
};


// Paths and links

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
        memmove(joined, r.data, (uz)r.len);
        j = (Str){joined, r.len};
    } else {
        memcpy(joined, dir.data, (uz)dir.len);
        if (dir.len && !is_sep(dir.data[dir.len-1])) {
            joined[dir.len] = sep;
            j = (Str){joined, dir.len + 1 + r.len};
        } else {
            memmove(joined + dir.len, r.data, (uz)r.len);
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


// Selection and find

// Working memory for an event, which the layout code takes as an RB.
typedef struct {
    Arena a;
    RB    rb;
} Work;

static void work_init(Work *w) {
    w->a = V.tmp;
    rb_init(&w->rb, &w->a);
}

static void sel_bounds(DocPos *lo, DocPos *hi) {
    if (pos_cmp(V.sel.anchor, V.sel.focus) <= 0) {
        *lo = V.sel.anchor;
        *hi = V.sel.focus;
    } else {
        *lo = V.sel.focus;
        *hi = V.sel.anchor;
    }
}

static void sel_sync(void) {
    D.sel_on = V.sel.on;
    if (V.sel.on) {
        sel_bounds(&D.sel_lo, &D.sel_hi);
    }
    os_invalidate();
}

static void sel_set(DocPos anchor, DocPos focus) {
    V.sel.anchor = anchor;
    V.sel.focus  = focus;
    V.sel.have   = 1;
    V.sel.on     = pos_cmp(anchor, focus) != 0;
    sel_sync();
}

static void sel_clear(void) {
    if (V.sel.on) {
        V.sel.on = 0;
        sel_sync();
    }
}

static void select_all(void) {
    if (V.state != VS_DOC || !D.nleaves) {
        return;
    }
    Work w;
    work_init(&w);
    sel_set(pos_first(0), pos_last(D.nleaves - 1, &w.rb));
}

// The word (gran 1) or line (gran 2) around pos, as two positions.
static void sel_unit(DocPos pos, b32 right, i32 gran, DocPos *u0, DocPos *u1, Work *w) {
    Seg s = seg_get(pos.leaf, pos.cell, &w->rb);
    iz  b = pos.off, e = pos.off;
    if (gran == 1) {
        seg_word(s, pos.off, right, &b, &e);
    } else {
        seg_line(D.leaves + pos.leaf, s, pos.off, &b, &e);
    }
    *u0 = *u1 = pos;
    u0->off = b;
    u1->off = e;
}

// Put the selected text on the clipboard.
static void copy_selection(void) {
    if (V.state != VS_DOC || !V.sel.on) {
        return;
    }
    DocPos lo, hi;
    sel_bounds(&lo, &hi);
    Work w;
    work_init(&w);
    iz n = doc_copy(lo, hi, 0, &w.rb);     // this makes the text, so no more memory is needed
    if (n <= 0) {
        return;
    }
    Arena t = *D.mem;                       // free space, forgotten after use
    if (n + 3*4096 > t.end - t.beg) {
        return;
    }
    u8 *buf = new(&t, n, u8);
    doc_copy(lo, hi, buf, &w.rb);
    os_clipboard_set((Str){buf, n});
}

// Keep scrolling while the mouse is held outside the window.
static void sel_timer_update(void) {
    b32 out = V.sel.down && V.sel.moved && V.state == VS_DOC &&
              (V.mouse.y < 0 || V.mouse.y > V.size.h);
    if (out && !V.sel.timer) {
        os_timer_start(SEL_TIMER, SEL_MS);
        V.sel.timer = 1;
    } else if (!out && V.sel.timer) {
        os_timer_stop(SEL_TIMER);
        V.sel.timer = 0;
    }
}

static DocPos pos_under(Point p, Work *w, b32 *right) {
    p.x = clampf(p.x, 0, V.size.w - 1);
    p.y = clampf(p.y, 0, V.size.h - 1);
    return doc_pos_at(p, V.scroll, &w->rb, right);
}

// The mouse moved to p during a drag: the selection runs to there.
static void sel_drag_to(Point p) {
    layout_now();
    Work w;
    work_init(&w);
    b32    right;
    DocPos pos = pos_under(p, &w, &right);
    DocPos a = V.sel.anchor, f = pos;
    if (V.sel.gran) {
        DocPos u0, u1;
        sel_unit(pos, right, V.sel.gran, &u0, &u1, &w);
        if (pos_cmp(pos, V.sel.unit0) < 0) {
            a = V.sel.unit1;
            f = u0;
        } else {
            a = V.sel.unit0;
            f = u1;
        }
    }
    sel_set(a, f);
}


// Find

static Str find_query(void) {
    return (Str){V.find.fold, V.find.len};
}

// Tell the painter what to highlight.
static void find_sync(void) {
    Find *f = &V.find;
    D.find_q   = f->open && f->len ? find_query() : (Str){0};
    D.find_ok  = f->open && f->have;
    D.find_cur = f->cur;
    if (f->pc) {            // the count changed
        os_para_free(f->pc);
        f->pc = 0;
    }
    os_invalidate();
}

static void find_drop_paras(void) {
    Find *f = &V.find;
    if (f->pt) {
        os_para_free(f->pt);
        f->pt = 0;
    }
    if (f->pc) {
        os_para_free(f->pc);
        f->pc = 0;
    }
}

// Bring the current match into view: nothing if it is on screen already,
// else centered. The reading position is otherwise left alone.
static void find_reveal(void) {
    Find *f = &V.find;
    if (V.state != VS_DOC || !f->open || !f->have) {
        return;
    }
    layout_now();
    Leaf *l = D.leaves + f->cur.leaf;
    f32 top, h;
    Work w;
    work_init(&w);
    if (!doc_range_y(f->cur, f->len, &w.rb, &top, &h)) {
        // Not laid out yet: go to its leaf, then look again.
        pin_to((Anchor){f->cur.leaf, -V.size.h/3, l->line});
        layout_now();
        work_init(&w);      // layout_now used the same memory
        if (!doc_range_y(f->cur, f->len, &w.rb, &top, &h)) {
            return;
        }
    }
    f32 y = l->y + top;
    if (y >= V.scroll + 56 && y + h <= V.scroll + V.size.h - 24) {
        return;
    }
    f32 off = l->y - anchor_base(f->cur.leaf) + top + h/2 - V.size.h/2;
    pin_to((Anchor){f->cur.leaf, off, l->line});
}

// Search again, from the current match or else from the reading position, or
// for the match numbered want_n if that is not 0.
static void find_run(i32 want_n, b32 reveal) {
    Find *f = &V.find;
    f->stale = 0;
    if (V.state != VS_DOC || !f->open || !f->len || !D.nleaves) {
        f->have  = 0;
        f->total = f->n = 0;
        find_sync();
        return;
    }
    layout_now();
    DocPos from = f->cur;
    if (!f->have) {
        i32 i = leaf_at(V.scroll);
        while (i + 1 < D.nleaves && D.leaves[i].y + D.leaves[i].h <= V.scroll) {
            i++;
        }
        from = pos_first(i);
    }
    Work w;
    work_init(&w);
    FindScan r = doc_find_scan(find_query(), from, want_n, &w.rb);
    f->total = r.total;
    f->n     = r.n;
    f->have  = r.n > 0;
    f->cur   = r.pos;
    find_sync();
    if (reveal) {
        find_reveal();
    }
}

static void find_step(i32 dir) {
    Find *f = &V.find;
    if (V.state != VS_DOC || !f->open || !f->len) {
        return;
    }
    if (f->stale) {
        find_run(f->keep_n, 0);
    }
    if (!f->have) {
        return;
    }
    Work w;
    work_init(&w);
    f->cur = doc_find_step(find_query(), f->cur, dir, &w.rb);
    f->n   = dir > 0 ? f->n%f->total + 1 : f->n > 1 ? f->n - 1 : f->total;
    find_sync();
    find_reveal();
}

// The text of the field changed.
static void find_edited(void) {
    Find *f = &V.find;
    fold_text(f->fold, (Str){f->text, f->len});
    find_drop_paras();
    find_run(0, 1);
}

static void find_close(void) {
    Find *f = &V.find;
    f->open = 0;
    f->have = 0;
    find_drop_paras();
    find_sync();
}

// Open the find bar, or select its text if it is open. A selection of one
// line becomes the query.
static void find_open(void) {
    Find *f = &V.find;
    if (V.state != VS_DOC) {
        return;
    }
    if (f->open) {
        f->all = f->len > 0;
        os_invalidate();
        return;
    }
    b32 filled = 0;
    if (V.sel.on) {
        DocPos lo, hi;
        sel_bounds(&lo, &hi);
        Work w;
        work_init(&w);
        if (lo.leaf == hi.leaf && lo.cell == hi.cell && hi.off - lo.off <= FIND_MAX) {
            Seg s = seg_get(lo.leaf, lo.cell, &w.rb);
            iz  b = clampz(lo.off, 0, s.len), e = clampz(hi.off, 0, s.len);
            b32 line = e > b;
            for (iz i = b; i < e; i++) {
                line = line && s.p[i] != '\n';
            }
            if (line) {
                memcpy(f->text, s.p + b, (uz)(e - b));
                f->len = e - b;
                filled = 1;
                f->cur = lo;
            }
        }
    }
    f->open  = 1;
    f->all   = f->len > 0;
    f->caret = f->len;
    f->have  = filled;
    fold_text(f->fold, (Str){f->text, f->len});
    find_drop_paras();
    find_run(0, 1);
}

// Replace bytes [b, e) of the field's text.
static void field_replace(iz b, iz e, Str ins) {
    Find *f = &V.find;
    if (f->len - (e - b) + ins.len > FIND_MAX) {
        return;
    }
    memmove(f->text + b + ins.len, f->text + e, (uz)(f->len - e));
    if (ins.len) {
        memcpy(f->text + b, ins.data, (uz)ins.len);
    }
    f->len   += ins.len - (e - b);
    f->caret  = b + ins.len;
    f->all    = 0;
    find_edited();
}

static iz field_prev(iz i) {
    Find *f = &V.find;
    while (i > 0) {
        i--;
        if ((f->text[i] & 0xc0) != 0x80) {
            break;
        }
    }
    return i;
}

static iz field_next(iz i) {
    Find *f = &V.find;
    while (i < f->len) {
        i++;
        if (i >= f->len || (f->text[i] & 0xc0) != 0x80) {
            break;
        }
    }
    return i;
}

// The start of the word before i, for Ctrl+Backspace and Ctrl+Left.
static iz field_word_back(iz i) {
    Find *f = &V.find;
    while (i > 0 && f->text[i-1] == ' ') {
        i--;
    }
    while (i > 0 && f->text[i-1] != ' ') {
        i--;
    }
    return i;
}

static iz field_word_fwd(iz i) {
    Find *f = &V.find;
    while (i < f->len && f->text[i] != ' ') {
        i++;
    }
    while (i < f->len && f->text[i] == ' ') {
        i++;
    }
    return i;
}

static void field_paste(void) {
    Find *f = &V.find;
    Arena t = V.tmp;
    Str   s = {0};
    if (!os_clipboard_get(&t, &s)) {
        return;
    }
    iz n = 0;
    while (n < s.len && s.data[n] != '\n' && s.data[n] != '\r') {
        n++;
    }
    n = min(n, (iz)FIND_MAX);
    while (n > 0 && n < s.len && (s.data[n] & 0xc0) == 0x80) {
        n--;
    }
    iz b = f->all ? 0 : f->caret, e = f->all ? f->len : f->caret;
    field_replace(b, e, (Str){s.data, n});
}

// Keys for the find bar while it is open. Returns false for the keys that
// are not its business, which then go on to the document.
static b32 find_key(Key key, u32 mods) {
    Find *f    = &V.find;
    b32   ctrl = !!(mods & MOD_PRIMARY);
    b32   shift = !!(mods & MOD_SHIFT);
    if (mods & MOD_ALT) {
        return 0;
    }
    switch (key) {
    case KEY_ESCAPE:
        find_close();
        return 1;
    case KEY_ENTER:
    case KEY_F3:
        find_step(shift ? -1 : 1);
        return 1;
    case KEY_G:
        if (ctrl) {
            find_step(shift ? -1 : 1);
            return 1;
        }
        return 0;
    case KEY_F:
    case KEY_A:
        if (ctrl) {
            f->all = f->len > 0;
            os_invalidate();
            return 1;
        }
        return 0;
    case KEY_C:
    case KEY_X:
        if (ctrl && f->all) {
            os_clipboard_set((Str){f->text, f->len});
            if (key == KEY_X) {
                field_replace(0, f->len, (Str){0});
            }
            return 1;
        }
        return 0;
    case KEY_INSERT:
        if (shift) {
            field_paste();
            return 1;
        }
        if (ctrl && f->all) {
            os_clipboard_set((Str){f->text, f->len});
            return 1;
        }
        return 0;
    case KEY_V:
        if (ctrl) {
            field_paste();
            return 1;
        }
        return 0;
    case KEY_SPACE:
        return !ctrl;           // text: it comes as a character
    case KEY_BACKSPACE:
        if (f->all) {
            field_replace(0, f->len, (Str){0});
        } else if (f->caret > 0) {
            field_replace(ctrl ? field_word_back(f->caret) : field_prev(f->caret), f->caret, (Str){0});
        }
        return 1;
    case KEY_DELETE:
        if (f->all) {
            field_replace(0, f->len, (Str){0});
        } else if (f->caret < f->len) {
            field_replace(f->caret, ctrl ? field_word_fwd(f->caret) : field_next(f->caret), (Str){0});
        }
        return 1;
    case KEY_LEFT:
        f->caret = f->all ? 0 : ctrl ? field_word_back(f->caret) : field_prev(f->caret);
        f->all   = 0;
        os_invalidate();
        return 1;
    case KEY_RIGHT:
        f->caret = f->all ? f->len : ctrl ? field_word_fwd(f->caret) : field_next(f->caret);
        f->all   = 0;
        os_invalidate();
        return 1;
    case KEY_HOME:
        f->caret = 0;
        f->all   = 0;
        os_invalidate();
        return 1;
    case KEY_END:
        f->caret = f->len;
        f->all   = 0;
        os_invalidate();
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    Rect bar, field, count, prev, next, close;
} FindGeo;

static FindGeo find_geo(void) {
    FindGeo g;
    f32 w   = clampf(V.size.w - 2*BAR_ZONE - 8, 150, 380);
    g.bar   = (Rect){max(V.size.w - w - BAR_ZONE - 4, (f32)4), 8, w, 38};
    f32 bx  = g.bar.x + g.bar.w - 6 - 26;
    g.close = (Rect){bx, g.bar.y + 6, 26, 26};
    g.next  = (Rect){bx - 26, g.bar.y + 6, 26, 26};
    g.prev  = (Rect){bx - 52, g.bar.y + 6, 26, 26};
    f32 cw  = w < 260 ? 0 : 66;
    g.count = (Rect){g.prev.x - cw - 4, g.bar.y, cw, g.bar.h};
    f32 fx  = g.bar.x + 8;
    g.field = (Rect){fx, g.bar.y + 6, max(g.count.x - 6 - fx, (f32)40), 26};
    return g;
}

static b32 in_rect(Point p, Rect r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

static b32 find_over(Point p) {
    return V.state == VS_DOC && V.find.open && in_rect(p, find_geo().bar);
}

// A click in the bar.
static void find_press(Point p) {
    Find *f = &V.find;
    FindGeo g = find_geo();
    if (in_rect(p, g.close)) {
        find_close();
    } else if (in_rect(p, g.next)) {
        find_step(1);
    } else if (in_rect(p, g.prev)) {
        find_step(-1);
    } else if (in_rect(p, g.field)) {
        f->all = 0;
        if (f->pt) {
            TextHit h = os_para_hit(f->pt, (Point){p.x - (g.field.x + 6 - f->sx), 1});
            f->caret = clampz(h.offset, 0, f->len);
        } else {
            f->caret = f->len;
        }
        os_invalidate();
    }
}

static Paragraph *ui_para(Str text, f32 size, Color c, Align align, f32 width) {
    Run r = {0};
    r.text  = text;
    r.size  = size;
    r.color = c;
    Paragraph *p = os_para_new(&r, 1, width);
    if (p) {
        os_para_style(p, 0, align);
    }
    return p;
}

static void draw_button(Rect r, i32 kind, Color c, Color hover) {
    if (V.inside && in_rect(V.mouse, r)) {
        os_fill_rrect(r, 4, hover);
    }
    f32 cx = r.x + r.w/2, cy = r.y + r.h/2;
    if (kind == 2) {
        os_stroke_line((Point){cx - 4, cy - 4}, (Point){cx + 4, cy + 4}, 1.5f, c);
        os_stroke_line((Point){cx - 4, cy + 4}, (Point){cx + 4, cy - 4}, 1.5f, c);
    } else {
        f32 d = kind == 0 ? 1.0f : -1.0f;       // chevron up or down
        os_stroke_line((Point){cx - 5, cy + 2.5f*d}, (Point){cx, cy - 2.5f*d}, 1.5f, c);
        os_stroke_line((Point){cx, cy - 2.5f*d}, (Point){cx + 5, cy + 2.5f*d}, 1.5f, c);
    }
}

static void draw_find(void) {
    Find *f = &V.find;
    if (!f->open) {
        return;
    }
    Theme const *t = current_theme();
    FindGeo g = find_geo();
    os_fill_rrect(g.bar, 6, t->status_bg);
    os_stroke_rrect(g.bar, 6, 1, t->border);
    b32 bad = f->len && !f->total;
    os_fill_rrect(g.field, 4, t->bg);
    os_stroke_rrect(g.field, 4, 1, bad ? t->error : t->border);
    if (!f->pt && f->len) {
        f->pt = ui_para((Str){f->text, f->len}, 14, t->text, ALIGN_LEFT, 0);
    }
    Rect inner = {g.field.x + 1, g.field.y + 1, g.field.w - 2, g.field.h - 2};
    f32  cx    = 0;
    if (f->pt && f->caret > 0) {
        Rect r[8];
        iz   n = os_para_rects(f->pt, 0, f->caret, r, 8);
        if (n > 0) {
            cx = r[min(n, (iz)8) - 1].x + r[min(n, (iz)8) - 1].w;
        }
    }
    f->sx = max(cx - (inner.w - 14), (f32)0);
    os_clip_push(inner);
    f32 tx = g.field.x + 6 - f->sx;
    if (f->pt) {
        Size s = os_para_size(f->pt);
        f32  ty = g.field.y + (g.field.h - s.h)/2;
        if (f->all) {
            os_fill_rect((Rect){tx - 1, ty, s.w + 2, s.h}, t->sel);
        }
        os_para_draw(f->pt, (Point){tx, ty});
    }
    if (!f->all) {
        os_fill_rect((Rect){tx + cx, g.field.y + 5, 1, g.field.h - 10}, t->text);
    }
    os_clip_pop();
    if (g.count.w > 0 && f->len) {
        if (!f->pc) {
            u8  buf[64];
            iz  n = 0;
            if (f->total) {
                n += fmt_int(buf + n, f->n);
                memcpy(buf + n, " of ", 4);
                n += 4;
                n += fmt_int(buf + n, f->total);
            } else {
                memcpy(buf, "No results", 10);
                n = 10;
            }
            f->pc = ui_para((Str){buf, n}, 12.5f, bad ? t->error : t->muted, ALIGN_RIGHT, g.count.w);
        }
        if (f->pc) {
            Size s = os_para_size(f->pc);
            os_para_draw(f->pc, (Point){g.count.x, g.count.y + (g.count.h - s.h)/2});
        }
    }
    draw_button(g.prev, 0, t->text, t->inline_bg);
    draw_button(g.next, 1, t->text, t->inline_bg);
    draw_button(g.close, 2, t->text, t->inline_bg);
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
    b32 same = V.state==VS_DOC && equals(path, (Str){V.path, V.pathlen});
    doc_clear();
    hover_clear();
    anim_stop();
    if (V.sel.timer) {
        os_timer_stop(SEL_TIMER);
    }
    V.sel = (Sel){0};
    V.find.have   = 0;
    V.find.stale  = V.find.open;      // searched again when it is next needed
    V.find.keep_n = same ? V.find.n : 0;
    find_drop_paras();
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
        memmove(V.hist, V.hist + 1, (uz)((HIST - 1)*(iz)sizeof(HistEntry)));
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
    if (V.sel.down && V.sel.moved) {
        cur = CURSOR_IBEAM;     // selecting: wherever the mouse is
    } else if (V.state==VS_DOC && V.inside && find_over(V.mouse)) {
        FindGeo g = find_geo();
        cur = in_rect(V.mouse, g.field) ? CURSOR_IBEAM :
              in_rect(V.mouse, g.prev) || in_rect(V.mouse, g.next) || in_rect(V.mouse, g.close) ?
              CURSOR_HAND : CURSOR_ARROW;
    } else if (V.state==VS_DOC && V.inside && !V.drag && V.mouse.x < V.size.w - BAR_ZONE) {
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
    if (V.sel.down && V.state == VS_DOC) {
        if (V.sel.moved || p.x - V.sel.press.x >= DRAG_PX || V.sel.press.x - p.x >= DRAG_PX ||
            p.y - V.sel.press.y >= DRAG_PX || V.sel.press.y - p.y >= DRAG_PX) {
            V.sel.moved = 1;
            sel_drag_to(p);
            sel_timer_update();
        }
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
    if (find_over(p)) {
        find_press(p);
        return;
    }
    Hit h = doc_hit(p, V.scroll);
    if (h.leaf>=0 && h.p && h.th.inside && h.th.link) {
        V.down_leaf = h.leaf;
        V.down_cell = h.cell;
        V.down_link = h.th.link;
    }
    // Start a selection, or extend the last one, or select a line.
    Work w;
    work_init(&w);
    u64    now = os_time_ms();
    b32    right;
    DocPos pos = pos_under(p, &w, &right);
    Sel   *s   = &V.sel;
    f32    dx  = p.x - s->p_dbl.x, dy = p.y - s->p_dbl.y;
    if (s->dbl && now - s->t_dbl <= TRIPLE_MS && dx*dx + dy*dy <= 36) {
        DocPos u0, u1;
        sel_unit(pos, right, 2, &u0, &u1, &w);
        s->gran  = 2;
        s->unit0 = u0;
        s->unit1 = u1;
        sel_set(u0, u1);
        s->moved = 1;
    } else if ((mods & MOD_SHIFT) && s->have) {
        s->gran = 0;
        sel_set(s->anchor, pos);
        s->moved = 1;
    } else {
        s->gran = 0;
        sel_set(pos, pos);
        s->moved = 0;
    }
    s->dbl   = 0;
    s->down  = 1;
    s->press = p;
}

static void app_mouse_up(Point p, Button b, u32 mods) {
    (void)mods;
    V.mouse = p;
    if (V.drag) {
        V.drag = 0;
        os_invalidate();
        return;
    }
    if (b == BUTTON_LEFT && V.sel.down) {
        V.sel.down = 0;
        sel_timer_update();
        if (V.sel.moved) {
            V.down_link = 0;        // that was a drag, not a click
        }
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
    (void)mods;
    V.mouse  = p;
    V.inside = 1;
    if (V.state != VS_DOC || p.x >= V.size.w - BAR_ZONE) {
        return;
    }
    layout_now();
    if (find_over(p)) {
        V.find.all = V.find.len > 0;
        os_invalidate();
        return;
    }
    Work w;
    work_init(&w);
    b32    right;
    DocPos pos = pos_under(p, &w, &right);
    DocPos u0, u1;
    sel_unit(pos, right, 1, &u0, &u1, &w);
    Sel *s = &V.sel;
    s->gran  = 1;
    s->unit0 = u0;
    s->unit1 = u1;
    sel_set(u0, u1);
    s->down  = 1;
    s->moved = 1;
    s->press = p;
    s->dbl   = 1;
    s->t_dbl = os_time_ms();
    s->p_dbl = p;
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
    if (V.state == VS_DOC && V.find.open && find_key(key, mods)) {
        return;
    }
    f32 line = 2*line_body*V.zoom;
    f32 page = V.size.h*0.9f;
    if (mods & MOD_PRIMARY) {
        switch (key) {
        case KEY_PLUS:  zoom_step(1); break;
        case KEY_MINUS: zoom_step(-1); break;
        case KEY_0:     set_zoom(1); break;
        case KEY_O:     open_dialog(); break;
        case KEY_R:     reload(); break;
        case KEY_A:     select_all(); break;
        case KEY_C:
        case KEY_INSERT: copy_selection(); break;
        case KEY_F:     find_open(); break;
        case KEY_G:     find_step(mods & MOD_SHIFT ? -1 : 1); break;
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
    case KEY_F3:
        find_open();
        if (V.find.open && V.find.len && !V.find.have) {
            find_run(0, 1);
        }
        break;
    case KEY_ESCAPE: sel_clear(); break;
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
    Find *f = &V.find;
    if (V.state != VS_DOC || !f->open || rune < 32 || rune == 0x7f) {
        return;
    }
    u8 enc[4];
    iz n = utf8encode(enc, rune);
    field_replace(f->all ? 0 : f->caret, f->all ? f->len : f->caret, (Str){enc, n});
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
    find_drop_paras();
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
    if (id == SEL_TIMER) {
        sel_timer_update();
        if (V.sel.timer) {
            // Further out, faster.
            f32 d = V.mouse.y < 0 ? -V.mouse.y : V.mouse.y - V.size.h;
            f32 v = 4 + min(d, (f32)300)*0.25f;
            scroll_by(V.mouse.y < 0 ? -v : v);
            sel_drag_to(V.mouse);
        }
        return;
    }
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
    if (V.sel.timer) {
        os_timer_stop(SEL_TIMER);
        V.sel.timer = 0;
    }
    find_drop_paras();
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
    if (V.find.stale) {
        find_run(V.find.keep_n, 0);
    }
    doc_paint(V.scroll, size, &rb);
    hover_refresh(&rb);
    draw_find();
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
