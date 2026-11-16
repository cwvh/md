// A fake, deterministic, headless platform for view-test.exe: every os_*
// function of platform.h, with no window and no fonts.
//
// Text metrics: a code point is 0.5 em wide, a wide one (CJK, full width)
// 1 em; a line is 1.25 em tall with the baseline at 1 em. Lines break after
// spaces and after wide characters, greedily; a word longer than the line
// breaks anywhere. Trailing spaces hang past the edge and don't count in a
// line's width, like in real text engines. Inline boxes are as wide and
// tall as they say, on the baseline.
//
// Images come from a tiny format, so that decoding is testable:
//   "FIMG", width u16, height u16, frames u8, delay (ms) u16, all little
//   endian. Anything else does not decode.
//
// What the code under test did is recorded for the tests to look at: the
// cursor, title, opened URLs, timers, paragraphs and images drawn, and the
// number of live paragraphs and images, so that leaks are visible.

enum {
    FK_FILES  = 64,
    FK_URLS   = 32,
    FK_DRAWS  = 8192,
};

typedef struct {
    iz  beg, end;           // bytes of the run's text
    f32 size, box_w, box_h;
    i32 link;
} FRun;

// One character (or inline box), the unit of layout.
typedef struct {
    iz  beg, end;
    f32 adv, asc, desc;
    i32 link;
    b32 space, newline, wide, box;
} FUnit;

typedef struct {
    iz  beg, end;           // units [beg, end), and bytes of the line
    iz  ubeg, uend;
    f32 top, height, baseline;
    f32 w;                  // width of the text, trailing spaces excluded
} FLine;

struct Paragraph {
    u8    *text;
    iz     len;
    FUnit *units;
    iz     nunits;
    FLine *lines;
    iz     nlines;
    f32    width;           // wrap width, 0 for none
    f32    line_height;
    Align  align;
    b32    boxes;
    Size   size;
};

struct Image {
    f32 w, h;
    i32 frames, delay;
};

typedef struct {
    Paragraph *p;
    Point      origin;
} FDrawPara;

typedef struct {
    Image *im;
    i32    frame;
    Rect   dst;
} FDrawImage;

typedef struct {
    u8 const *path;
    iz        pathlen;
    Str       data;
} FFile;

typedef struct {
    Rect  r;
    Color c;
} FFill;

static struct {
    Size     window;
    b32      dark;
    Cursor   cursor;
    b32      invalid;       // os_invalidate since the last paint
    u8       title[300];
    iz       titlelen;
    u8       watch[1200];
    iz       watchlen;
    Str      urls[FK_URLS];
    i32      nurls;
    b32      timer_on[16];
    i32      timer_ms[16];
    b32      quit;
    Str      dialog;        // what the open dialog returns, if set
    b32      dialog_asked;
    u8      *clip;          // what os_clipboard_set was given last
    iz       cliplen;
    b32      clipset;       // it was called since the test last looked
    u64      now;           // os_time_ms: tests move it forward

    FFile    files[FK_FILES];
    i32      nfiles;

    // Objects
    i32      live_paras, live_images;
    i32      made_paras, made_images;

    // Painting
    b32      painting;
    i32      clips;
    i32      fills;
    FDrawPara  para_draws[FK_DRAWS];
    i32        npara_draws;
    FDrawImage image_draws[FK_DRAWS];
    i32        nimage_draws;
    FFill      fill_log[FK_DRAWS];       // the os_fill_rect calls of the paint
    i32        nfill_log;
    Color      clear;
} fk;


// The bits base.c wants

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

static void *fk_alloc(iz count, iz size) {
    uz n = (uz)(count*size);
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1);
}

static void fk_free(void *p) {
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

// Drawing arguments must be sane: NaN and wild values would hide bugs.
static void fk_check(f32 v) {
    assert(v == v && v > -1e7f && v < 1e7f);
}

static void fk_check_rect(Rect r) {
    fk_check(r.x);
    fk_check(r.y);
    fk_check(r.w);
    fk_check(r.h);
    assert(r.w >= 0 && r.h >= 0);
}


// Paragraphs

static b32 fk_is_wide(i32 c) {
    return (c>=0x1100 && c<=0x115f) || (c>=0x2e80 && c<=0xa4cf) || (c>=0xac00 && c<=0xd7a3) ||
           (c>=0xf900 && c<=0xfaff) || (c>=0xff00 && c<=0xff60) || (c>=0x1f300 && c<=0x1faff) ||
           (c>=0x20000 && c<=0x3ffff);
}

static void fk_relayout(Paragraph *p) {
    fk_free(p->lines);
    p->lines  = fk_alloc(p->nunits + 2, (iz)sizeof(FLine));
    p->nlines = 0;
    f32  top  = 0;
    f32  maxw = 0;
    iz   i    = 0;
    iz   nu   = p->nunits;
    f32  wrap = p->width;
    for (;;) {
        iz  start = i;
        iz  end   = nu;
        f32 x     = 0;
        iz  brk   = -1;
        for (iz j = i; j < nu; j++) {
            FUnit *c = p->units + j;
            if (c->newline) {
                end = j + 1;
                break;
            }
            if (!c->space && wrap > 0 && x + c->adv > wrap + 0.001f && j > start) {
                end = brk > start ? brk : j;
                break;
            }
            x += c->adv;
            if (c->space || c->wide) {
                brk = j + 1;
            }
        }
        FLine *l = p->lines + p->nlines++;
        l->ubeg = start;
        l->uend = end;
        f32 asc = 0, desc = 0, w = 0, trail = 0;
        for (iz j = start; j < end; j++) {
            FUnit *c = p->units + j;
            asc  = max(asc, c->asc);
            desc = max(desc, c->desc);
            if (c->newline) {
                continue;
            }
            w += c->adv;
            trail = c->space ? trail + c->adv : 0;
        }
        w -= trail;
        if (start == end) {         // empty paragraph
            asc  = 16;
            desc = 4;
        }
        f32 height = asc + desc;
        f32 base   = asc;
        if (p->line_height>0 && !p->boxes) {
            base   = asc + (p->line_height - height)/2;
            height = p->line_height;
        }
        l->top      = top;
        l->height   = height;
        l->baseline = base;
        l->w        = w;
        l->beg      = start < nu ? p->units[start].beg : p->len;
        l->end      = end > start ? p->units[end-1].end : l->beg;
        top  += height;
        maxw  = max(maxw, w);
        i     = end;
        if (i >= nu) {
            break;
        }
    }
    p->size = (Size){maxw, top};
}

static Paragraph *os_para_new(Run const *runs, iz nruns, f32 max_width) {
    Paragraph *p = fk_alloc(1, (iz)sizeof(Paragraph));
    iz total = 0;
    for (iz i = 0; i < nruns; i++) {
        total += runs[i].box_w>0 && runs[i].box_h>0 ? 3 : runs[i].text.len;
    }
    p->text  = fk_alloc(total, 1);
    p->units = fk_alloc(total + 1, (iz)sizeof(FUnit));
    p->width = max_width > 0 ? max_width : 0;
    iz at = 0;
    for (iz i = 0; i < nruns; i++) {
        Run const *r = runs + i;
        if (r->box_w>0 && r->box_h>0) {
            memcpy(p->text + at, "\xEF\xBF\xBC", 3);
            FUnit *u = p->units + p->nunits++;
            u->beg  = at;
            u->end  = at + 3;
            u->adv  = r->box_w;
            u->asc  = r->box_h;
            u->link = r->link;
            u->box  = 1;
            p->boxes = 1;
            at += 3;
            continue;
        }
        assert(r->size > 0);
        Str s = r->text;
        while (s.len) {
            Utf8 d = utf8decode(s);
            iz   n = s.len - d.tail.len;
            memcpy(p->text + at, s.data, (uz)n);
            FUnit *u = p->units + p->nunits++;
            u->beg     = at;
            u->end     = at + n;
            u->wide    = fk_is_wide(d.rune);
            u->adv     = r->size * (u->wide ? 1.0f : 0.5f);
            u->asc     = r->size;
            u->desc    = r->size * 0.25f;
            u->link    = r->link;
            u->space   = d.rune == ' ';
            u->newline = d.rune == '\n';
            at += n;
            s   = d.tail;
        }
    }
    p->len = at;
    fk_relayout(p);
    fk.live_paras++;
    fk.made_paras++;
    return p;
}

static void os_para_set_width(Paragraph *p, f32 w) {
    p->width = w > 0 ? w : 0;
    fk_relayout(p);
}

static void os_para_style(Paragraph *p, f32 line_height, Align align) {
    p->line_height = line_height;
    p->align       = align;
    fk_relayout(p);
}

static void os_para_free(Paragraph *p) {
    assert(p);
    fk.live_paras--;
    assert(fk.live_paras >= 0);
    fk_free(p->text);
    fk_free(p->units);
    fk_free(p->lines);
    fk_free(p);
}

static iz os_para_length(Paragraph *p) {
    return p->len;
}

static Size os_para_size(Paragraph *p) {
    return p->size;
}

static iz os_para_lines(Paragraph *p) {
    return p->nlines;
}

static LineMetrics os_para_line(Paragraph *p, iz index) {
    assert(index>=0 && index<p->nlines);
    FLine *l = p->lines + index;
    return (LineMetrics){l->beg, l->end, l->top, l->height, l->baseline};
}

// Where the line's text starts, for the paragraph's alignment.
static f32 fk_line_x(Paragraph *p, FLine *l) {
    if (p->width <= 0) {
        return 0;
    }
    return p->align==ALIGN_CENTER ? (p->width - l->w)/2 :
           p->align==ALIGN_RIGHT ? p->width - l->w : 0;
}

// x of the byte offset b within line l.
static f32 fk_x_of(Paragraph *p, FLine *l, iz b) {
    f32 x = fk_line_x(p, l);
    for (iz j = l->ubeg; j < l->uend; j++) {
        FUnit *u = p->units + j;
        if (u->beg >= b) {
            break;
        }
        if (!u->newline) {
            x += u->adv;
        }
    }
    return x;
}

static TextHit os_para_hit(Paragraph *p, Point pt) {
    TextHit r = {0};
    iz li = 0;
    b32 inside = pt.y>=0 && pt.y<p->size.h;
    if (pt.y >= p->size.h) {
        li = p->nlines - 1;
    } else if (pt.y > 0) {
        for (iz i = 0; i < p->nlines; i++) {
            if (pt.y >= p->lines[i].top) {
                li = i;
            }
        }
    }
    FLine *l = p->lines + li;
    f32    x = fk_line_x(p, l);
    iz     end = l->end;
    if (l->uend>l->ubeg && p->units[l->uend-1].newline) {
        end -= p->units[l->uend-1].end - p->units[l->uend-1].beg;
    }
    r.offset = pt.x < x ? l->beg : end;
    r.inside = 0;
    for (iz j = l->ubeg; j < l->uend; j++) {
        FUnit *u = p->units + j;
        if (u->newline) {
            break;
        }
        if (pt.x >= x && pt.x < x + u->adv) {
            r.offset = pt.x < x + u->adv/2 ? u->beg : u->end;
            r.inside = inside;
            r.link   = inside ? u->link : 0;
            break;
        }
        x += u->adv;
    }
    return r;
}

static iz os_para_rects(Paragraph *p, iz beg, iz end, Rect *out, iz cap) {
    iz n = 0;
    for (iz i = 0; i < p->nlines; i++) {
        FLine *l = p->lines + i;
        iz le = l->end;
        if (l->uend>l->ubeg && p->units[l->uend-1].newline) {
            le = p->units[l->uend-1].beg;
        }
        iz b = max(beg, l->beg);
        iz e = min(end, le);
        if (e <= b) {
            continue;
        }
        if (n < cap) {
            f32 x0 = fk_x_of(p, l, b);
            f32 x1 = fk_x_of(p, l, e);
            out[n] = (Rect){x0, l->top, x1 - x0, l->height};
        }
        n++;
    }
    return n;
}

static void os_para_draw(Paragraph *p, Point origin) {
    assert(fk.painting);
    fk_check(origin.x);
    fk_check(origin.y);
    if (fk.npara_draws < FK_DRAWS) {
        fk.para_draws[fk.npara_draws++] = (FDrawPara){p, origin};
    }
}


// Drawing

static void os_clear(Color c) {
    assert(fk.painting);
    fk.clear         = c;
    fk.npara_draws   = 0;
    fk.nimage_draws  = 0;
    fk.fills         = 0;
    fk.nfill_log     = 0;
}

static void os_fill_rect(Rect r, Color c) {
    assert(fk.painting);
    fk_check_rect(r);
    fk.fills++;
    if (fk.nfill_log < FK_DRAWS) {
        fk.fill_log[fk.nfill_log++] = (FFill){r, c};
    }
}

static void os_fill_rrect(Rect r, f32 radius, Color c) {
    assert(fk.painting);
    (void)c;
    fk_check_rect(r);
    fk_check(radius);
    fk.fills++;
}

static void os_stroke_rrect(Rect r, f32 radius, f32 width, Color c) {
    assert(fk.painting);
    (void)c;
    fk_check_rect(r);
    fk_check(radius);
    fk_check(width);
    fk.fills++;
}

static void os_stroke_line(Point a, Point b, f32 width, Color c) {
    assert(fk.painting);
    (void)c;
    fk_check(a.x);
    fk_check(a.y);
    fk_check(b.x);
    fk_check(b.y);
    fk_check(width);
    fk.fills++;
}

static void os_clip_push(Rect r) {
    assert(fk.painting);
    fk_check_rect(r);
    fk.clips++;
}

static void os_clip_pop(void) {
    assert(fk.painting && fk.clips > 0);
    fk.clips--;
}


// Images

static Image *os_image_decode(Str bytes) {
    if (bytes.len < 11 || !equals((Str){bytes.data, 4}, S("FIMG"))) {
        return 0;
    }
    u8 const *b = bytes.data;
    Image *im = fk_alloc(1, (iz)sizeof(Image));
    im->w      = (f32)(b[4] | b[5]<<8);
    im->h      = (f32)(b[6] | b[7]<<8);
    im->frames = max((i32)b[8], (i32)1);
    im->delay  = im->frames > 1 ? max(b[9] | b[10]<<8, 10) : 0;
    if (im->w < 1 || im->h < 1) {
        fk_free(im);
        return 0;
    }
    fk.live_images++;
    fk.made_images++;
    return im;
}

static void os_image_free(Image *im) {
    assert(im);
    fk.live_images--;
    assert(fk.live_images >= 0);
    fk_free(im);
}

static Size os_image_size(Image *im) {
    return (Size){im->w, im->h};
}

static i32 os_image_frames(Image *im) {
    return im->frames;
}

static i32 os_image_delay(Image *im, i32 frame) {
    assert(frame>=0 && frame<im->frames);
    return im->delay;
}

static void os_image_draw(Image *im, i32 frame, Rect dst) {
    assert(fk.painting);
    assert(frame>=0 && frame<im->frames);
    fk_check_rect(dst);
    if (fk.nimage_draws < FK_DRAWS) {
        fk.image_draws[fk.nimage_draws++] = (FDrawImage){im, frame, dst};
    }
}


// Window and system

static void os_set_cursor(Cursor c) {
    fk.cursor = c;
}

static void os_invalidate(void) {
    fk.invalid = 1;
}

static void os_set_title(Str title) {
    fk.titlelen = min(title.len, (iz)sizeof(fk.title));
    memcpy(fk.title, title.data, (uz)fk.titlelen);
}

static void os_timer_start(i32 id, i32 ms) {
    assert(id>0 && id<16 && ms>0);
    fk.timer_on[id] = 1;
    fk.timer_ms[id] = ms;
}

static void os_timer_stop(i32 id) {
    assert(id>0 && id<16);
    fk.timer_on[id] = 0;
}

static b32 os_dark_mode(void) {
    return fk.dark;
}

static f32 os_scale(void) {
    return 1;
}

static Size os_window_size(void) {
    return fk.window;
}

static void os_quit(void) {
    fk.quit = 1;
}

static FFile *fk_find(Str path) {
    for (i32 i = 0; i < fk.nfiles; i++) {
        if (equals((Str){(u8 *)fk.files[i].path, fk.files[i].pathlen}, path)) {
            return fk.files + i;
        }
    }
    return 0;
}

// Add or replace a file of the fake file system. The strings must stay valid.
static void fk_put(Str path, Str data) {
    FFile *f = fk_find(path);
    if (!f) {
        assert(fk.nfiles < FK_FILES);
        f = fk.files + fk.nfiles++;
    }
    f->path    = path.data;
    f->pathlen = path.len;
    f->data    = data;
}

static void fk_remove(Str path) {
    FFile *f = fk_find(path);
    if (f) {
        *f = fk.files[--fk.nfiles];
    }
}

static b32 os_read_file(Arena *a, Str path, Str *out) {
    FFile *f = fk_find(path);
    if (!f || f->data.len + 3*4096 > a->end - a->beg) {
        return 0;
    }
    u8 *d = new(a, f->data.len, u8);
    memcpy(d, f->data.data, (uz)f->data.len);
    *out = (Str){d, f->data.len};
    return 1;
}

static void os_watch_file(Str path) {
    fk.watchlen = min(path.len, (iz)sizeof(fk.watch));
    memcpy(fk.watch, path.data, (uz)fk.watchlen);
}

static b32 os_open_url(Str url) {
    b32 ok = icontains((Str){url.data, min(url.len, (iz)8)}, S("http://")) ||
             icontains((Str){url.data, min(url.len, (iz)8)}, S("https://")) ||
             icontains((Str){url.data, min(url.len, (iz)7)}, S("mailto:"));
    if (fk.nurls < FK_URLS) {
        fk.urls[fk.nurls++] = url;
    }
    return ok;
}

static b32 os_open_dialog(Arena *a, Str *path) {
    fk.dialog_asked = 1;
    if (!fk.dialog.len) {
        return 0;
    }
    u8 *d = new(a, fk.dialog.len, u8);
    memcpy(d, fk.dialog.data, (uz)fk.dialog.len);
    *path = (Str){d, fk.dialog.len};
    return 1;
}

static void os_clipboard_set(Str utf8) {
    fk_free(fk.clip);
    fk.clip    = fk_alloc(utf8.len + 1, 1);
    fk.cliplen = utf8.len;
    fk.clipset = 1;
    memcpy(fk.clip, utf8.data, (uz)utf8.len);
}

static b32 os_clipboard_get(Arena *a, Str *out) {
    if (!fk.clip || fk.cliplen + 3*4096 > a->end - a->beg) {
        return 0;
    }
    u8 *d = new(a, fk.cliplen, u8);
    memcpy(d, fk.clip, (uz)fk.cliplen);
    *out = (Str){d, fk.cliplen};
    return 1;
}

static u64 os_time_ms(void) {
    return fk.now;
}
