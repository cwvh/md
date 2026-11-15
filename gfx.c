// Windows graphics: Direct2D drawing and DirectWrite paragraphs, behind the
// os_* functions in platform.h. Included from main.c after win32.c.

enum { NBRUSH = 48 };

static struct {
    D2dFactory *d2d;
    DwFactory  *dw;
    DwLayout   *format;     // base text format that every layout starts from
    D2dRt      *rt;         // 0 until created, and after the device is lost
    u32         gen;        // bumped when rt is replaced; brushes die with it
    f32         dpi;        // render target DPI, 96 means 1 DIP per pixel

    // Solid brushes by color. Direct2D brushes belong to one render
    // target, so the cache is dropped along with it.
    u32         brushkey[NBRUSH];
    void       *brush[NBRUSH];
    i32         nbrush;
} gfx;

static void release(void *p) {
    ComObj *u = p;
    if (u) {
        COM(u, Release);
    }
}

static void *heapnew(iz count, iz size) {
    uz n = (uz)(count*size);
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1);
}

static void heapfree(void *p) {
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

static u32 colorkey(Color c) {
    return (u32)c.r | (u32)c.g<<8 | (u32)c.b<<16 | (u32)c.a<<24;
}

static D2dColor d2dcolor(Color c) {
    return (D2dColor){c.r/255.0f, c.g/255.0f, c.b/255.0f, c.a/255.0f};
}

static D2dRect d2drect(Rect r) {
    return (D2dRect){r.x, r.y, r.x+r.w, r.y+r.h};
}


// Setup

static b32 gfx_init(void) {
    gfx.dpi = 96;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 &iid_d2d_factory, 0, (void **)&gfx.d2d))) {
        return 0;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   &iid_dw_factory, (void **)&gfx.dw))) {
        return 0;
    }
    // A text format is a layout's starting style; the runs then override
    // size, weight and so on per range. Created here as the layout-capable
    // interface only through CreateTextFormat, whose object we use solely
    // as an argument to CreateTextLayout.
    void *fmt = 0;
    if (FAILED(COM(gfx.dw, CreateTextFormat, L"Segoe UI", 0, DWRITE_FONT_WEIGHT_NORMAL,
                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
                   L"en-us", &fmt))) {
        return 0;
    }
    gfx.format = fmt;
    return 1;
}

static void gfx_drop_brushes(void) {
    for (i32 i = 0; i < gfx.nbrush; i++) {
        release(gfx.brush[i]);
        gfx.brush[i] = 0;
    }
    gfx.nbrush = 0;
}

// The render target is lost with its device, and by definition all its
// resources: brushes, bitmaps. Layouts survive but their drawing effects
// are brushes of the old target, which paragraphs notice through gen.
static void gfx_discard(void) {
    gfx_drop_brushes();
    release(gfx.rt);
    gfx.rt = 0;
    gfx.gen++;
}

static b32 gfx_target(uptr hwnd, u32 pw, u32 ph, f32 dpi) {
    gfx.dpi = dpi;
    if (gfx.rt) {
        return 1;
    }
    D2dRenderTargetProps props = {0};   // default type, format and usage
    D2dHwndRenderTargetProps hp = {hwnd, {pw, ph}, 0};
    void *rt = 0;
    if (FAILED(COM(gfx.d2d, CreateHwndRenderTarget, &props, &hp, &rt))) {
        return 0;
    }
    gfx.rt = rt;
    COM(gfx.rt, SetDpi, dpi, dpi);
    gfx.gen++;
    return 1;
}

static void gfx_resize(u32 pw, u32 ph, f32 dpi) {
    gfx.dpi = dpi;
    if (gfx.rt) {
        D2dSizeU s = {pw, ph};
        COM(gfx.rt, SetDpi, dpi, dpi);
        if (FAILED(COM(gfx.rt, Resize, &s))) {
            gfx_discard();
        }
    }
}

// Returns the EndDraw result; D2DERR_RECREATE_TARGET means the target was
// discarded and the caller should repaint.
static i32 gfx_paint(void (*draw)(void)) {
    if (!gfx.rt) {
        return E_FAIL;
    }
    COM(gfx.rt, BeginDraw);
    draw();
    i32 hr = COM(gfx.rt, EndDraw, 0, 0);
    if (hr == D2DERR_RECREATE_TARGET) {
        gfx_discard();
    }
    return hr;
}

static void *brushfor(Color c) {
    u32 key = colorkey(c);
    for (i32 i = 0; i < gfx.nbrush; i++) {
        if (gfx.brushkey[i] == key) {
            return gfx.brush[i];
        }
    }
    if (gfx.nbrush == NBRUSH) {
        // Paragraphs keep their own references to brushes they use.
        gfx_drop_brushes();
    }
    D2dColor dc = d2dcolor(c);
    void *b = 0;
    if (FAILED(COM(gfx.rt, CreateSolidColorBrush, &dc, 0, &b))) {
        return 0;
    }
    gfx.brushkey[gfx.nbrush] = key;
    gfx.brush[gfx.nbrush]    = b;
    gfx.nbrush++;
    return b;
}


// Primitives

static void os_clear(Color c) {
    if (gfx.rt) {
        D2dColor dc = d2dcolor(c);
        COM(gfx.rt, Clear, &dc);
    }
}

static void os_fill_rect(Rect r, Color c) {
    void *b = gfx.rt ? brushfor(c) : 0;
    if (b) {
        D2dRect d = d2drect(r);
        COM(gfx.rt, FillRectangle, &d, b);
    }
}

static D2dRoundedRect d2drrect(Rect r, f32 radius) {
    radius = clampf(radius, 0, min(r.w, r.h)/2);
    return (D2dRoundedRect){d2drect(r), radius, radius};
}

static void os_fill_rrect(Rect r, f32 radius, Color c) {
    void *b = gfx.rt ? brushfor(c) : 0;
    if (b) {
        D2dRoundedRect d = d2drrect(r, radius);
        COM(gfx.rt, FillRoundedRectangle, &d, b);
    }
}

static void os_stroke_rrect(Rect r, f32 radius, f32 width, Color c) {
    void *b = gfx.rt ? brushfor(c) : 0;
    if (b) {
        D2dRoundedRect d = d2drrect(r, radius);
        COM(gfx.rt, DrawRoundedRectangle, &d, b, width, 0);
    }
}

static void os_stroke_line(Point a, Point z, f32 width, Color c) {
    void *b = gfx.rt ? brushfor(c) : 0;
    if (b) {
        COM(gfx.rt, DrawLine, (D2dPoint){a.x, a.y}, (D2dPoint){z.x, z.y}, b, width, 0);
    }
}

static void os_clip_push(Rect r) {
    if (gfx.rt) {
        D2dRect d = d2drect(r);
        COM(gfx.rt, PushAxisAlignedClip, &d, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
}

static void os_clip_pop(void) {
    if (gfx.rt) {
        COM(gfx.rt, PopAxisAlignedClip);
    }
}


// Inline boxes: a DirectWrite inline object that only reserves space. It
// draws nothing; the portable code draws into the rectangle it finds with
// os_para_rects.

typedef struct {
    DwInlineVtbl const *v;
    i32 refs;
    f32 w;
    f32 h;
} Box;

static i32 box_query(void *self, Guid const *iid, void **out) {
    if (guideq(iid, &iid_unknown) || guideq(iid, &iid_dw_inline)) {
        *out = self;
        Box *b = self;
        b->refs++;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}

static u32 box_addref(void *self) {
    Box *b = self;
    return (u32)++b->refs;
}

static u32 box_release(void *self) {
    Box *b = self;
    u32  r = (u32)--b->refs;
    if (!r) {
        heapfree(b);
    }
    return r;
}

static i32 box_draw(DwInline *self, void *ctx, void *renderer, f32 ox, f32 oy,
                    b32 sideways, b32 rtl, void *effect) {
    (void)self; (void)ctx; (void)renderer; (void)ox; (void)oy;
    (void)sideways; (void)rtl; (void)effect;
    return S_OK;
}

static i32 box_metrics(DwInline *self, DwInlineMetrics *m) {
    Box *b      = (Box *)self;   // Box begins with its DwInline
    m->width    = b->w;
    m->height   = b->h;
    m->baseline = b->h;     // bottom edge on the baseline
    m->sideways = 0;
    return S_OK;
}

static i32 box_overhang(DwInline *self, DwOverhangMetrics *m) {
    (void)self;
    *m = (DwOverhangMetrics){0};
    return S_OK;
}

static i32 box_breaks(DwInline *self, u32 *before, u32 *after) {
    (void)self;
    *before = DWRITE_BREAK_CONDITION_NEUTRAL;
    *after  = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
}

static const DwInlineVtbl boxvtbl = {
    {box_query, box_addref, box_release},
    box_draw, box_metrics, box_overhang, box_breaks,
};


// Paragraphs

typedef struct {
    u32   wbeg;         // UTF-16 range
    u32   wlen;
    Color color;
    i32   link;
    b32   box;          // inline box: no color
} PRun;

struct Paragraph {
    DwLayout    *layout;
    c16         *w;         // UTF-16 text
    iz           wlen;
    iz           len;       // UTF-8 length
    i32         *u2w;       // UTF-8 byte offset to UTF-16 index, len+1 entries
    i32         *w2u;       // and back, wlen+1 entries (trail surrogate maps
                            //   to the start of its code point too)
    PRun        *runs;
    iz           nruns;
    LineMetrics *lines;
    iz           nlines;
    Size         size;
    u32          gen;       // render target generation of the drawing effects
    b32          boxes;     // has inline boxes
    f32          natheight; // first line, before os_para_style
    f32          natbase;
};

// Fetch size and line metrics from the layout after (re)wrapping.
static void para_measure(Paragraph *p) {
    heapfree(p->lines);
    p->lines  = 0;
    p->nlines = 0;
    p->size   = (Size){0};

    DwTextMetrics tm = {0};
    if (!FAILED(COM(p->layout, GetMetrics, &tm))) {
        p->size = (Size){tm.width, tm.height};
    }

    u32 n = 0;
    COM(p->layout, GetLineMetrics, 0, 0, &n);   // E_NOT_SUFFICIENT_BUFFER, sets n
    if (!n) {
        return;
    }
    DwLineMetrics *dl = heapnew(n, (iz)sizeof(DwLineMetrics));
    p->lines = heapnew(n, (iz)sizeof(LineMetrics));
    if (!dl || !p->lines) {
        heapfree(dl);
        heapfree(p->lines);
        p->lines = 0;
        return;
    }
    if (FAILED(COM(p->layout, GetLineMetrics, dl, n, &n))) {
        heapfree(dl);
        heapfree(p->lines);
        p->lines = 0;
        return;
    }
    f32 top  = 0;
    iz  wpos = 0;
    for (u32 i = 0; i < n; i++) {
        iz wend = min(wpos + (iz)dl[i].length, p->wlen);
        p->lines[i] = (LineMetrics){
            p->w2u[wpos], p->w2u[wend], top, dl[i].height, dl[i].baseline,
        };
        top  += dl[i].height;
        wpos  = wend;
    }
    p->nlines = n;
    heapfree(dl);
}

static f32 wrapwidth(f32 w) {
    return w > 0 ? w : 1e6f;
}

static void os_para_free(Paragraph *p) {
    if (p) {
        release(p->layout);
        heapfree(p->w);
        heapfree(p->u2w);
        heapfree(p->w2u);
        heapfree(p->runs);
        heapfree(p->lines);
        heapfree(p);
    }
}

static Paragraph *os_para_new(Run const *runs, iz nruns, f32 max_width) {
    if (!gfx.dw) {
        return 0;
    }
    iz total = 0;       // UTF-8 bytes
    for (iz i = 0; i < nruns; i++) {
        total += runs[i].box_w>0 && runs[i].box_h>0 ? 3 : runs[i].text.len;
    }

    Paragraph *p = heapnew(1, (iz)sizeof(Paragraph));
    if (!p) {
        return 0;
    }
    p->len  = total;
    p->w    = heapnew(total + 1, (iz)sizeof(c16));   // units <= bytes
    p->u2w  = heapnew(total + 1, (iz)sizeof(i32));
    p->w2u  = heapnew(total + 2, (iz)sizeof(i32));
    p->runs = heapnew(nruns, (iz)sizeof(PRun));
    p->nruns = nruns;
    if (!p->w || !p->u2w || !p->w2u || !p->runs) {
        os_para_free(p);
        return 0;
    }

    // Convert, recording the offset maps and each run's UTF-16 range.
    iz u = 0;
    iz w = 0;
    for (iz i = 0; i < nruns; i++) {
        Run const *r  = runs + i;
        PRun      *pr = p->runs + i;
        pr->wbeg  = (u32)w;
        pr->color = r->color;
        pr->link  = r->link;
        pr->box   = r->box_w>0 && r->box_h>0;
        p->boxes |= pr->box;
        Str s = r->text;
        if (pr->box) {
            s = (Str){(u8 *)"\xEF\xBF\xBC", 3};    // U+FFFC
        }
        while (s.len) {
            Utf8 d    = utf8decode(s);
            iz   n    = s.len - d.tail.len;
            iz   wn   = d.rune >= 0x10000 ? 2 : 1;
            for (iz k = 0; k < n; k++) {
                p->u2w[u+k] = (i32)w;
            }
            for (iz k = 0; k < wn; k++) {
                p->w2u[w+k] = (i32)u;
            }
            if (wn == 2) {
                i32 v = d.rune - 0x10000;
                p->w[w]   = (c16)(0xd800 + (v >> 10));
                p->w[w+1] = (c16)(0xdc00 + (v & 0x3ff));
            } else {
                p->w[w] = (c16)d.rune;
            }
            u += n;
            w += wn;
            s  = d.tail;
        }
        pr->wlen = (u32)(w - (iz)pr->wbeg);
    }
    p->u2w[total] = (i32)w;
    p->w2u[w]     = (i32)total;
    p->wlen       = w;

    void *lay = 0;
    if (FAILED(COM(gfx.dw, CreateTextLayout, p->w, (u32)w, gfx.format,
                   wrapwidth(max_width), 1e7f, &lay))) {
        os_para_free(p);
        return 0;
    }
    p->layout = lay;

    for (iz i = 0; i < nruns; i++) {
        Run const *r  = runs + i;
        PRun      *pr = p->runs + i;
        if (!pr->wlen) {
            continue;
        }
        DwTextRange range = {pr->wbeg, pr->wlen};
        if (pr->box) {
            Box *b = heapnew(1, (iz)sizeof(Box));
            if (b) {
                b->v    = &boxvtbl;
                b->refs = 1;
                b->w    = r->box_w;
                b->h    = r->box_h;
                COM(p->layout, SetInlineObject, b, range);
                box_release(b);     // the layout holds its own reference
            }
            continue;
        }
        if (r->font == FONT_MONOSPACE) {
            COM(p->layout, SetFontFamilyName, L"Consolas", range);
        }
        COM(p->layout, SetFontSize, r->size, range);
        if (r->bold) {
            COM(p->layout, SetFontWeight, DWRITE_FONT_WEIGHT_BOLD, range);
        }
        if (r->italic) {
            COM(p->layout, SetFontStyle, DWRITE_FONT_STYLE_ITALIC, range);
        }
        if (r->underline) {
            COM(p->layout, SetUnderline, 1, range);
        }
        if (r->strike) {
            COM(p->layout, SetStrikethrough, 1, range);
        }
    }

    para_measure(p);
    if (p->nlines) {
        p->natheight = p->lines[0].height;
        p->natbase   = p->lines[0].baseline;
    }
    return p;
}

static void os_para_style(Paragraph *p, f32 line_height, Align align) {
    COM(p->layout, SetTextAlignment, align==ALIGN_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER :
        align==ALIGN_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (line_height>0 && !p->boxes && p->natheight>0) {
        // Half-leading above and below, like CSS.
        f32 base = p->natbase + (line_height - p->natheight)/2;
        COM(p->layout, SetLineSpacing, DWRITE_LINE_SPACING_METHOD_UNIFORM, line_height, base);
    }
    para_measure(p);
}

static void os_para_set_width(Paragraph *p, f32 max_width) {
    COM(p->layout, SetMaxWidth, wrapwidth(max_width));
    para_measure(p);
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
    return p->lines[index];
}

static TextHit os_para_hit(Paragraph *p, Point pt) {
    TextHit r = {0};
    b32 trailing = 0;
    b32 inside   = 0;
    DwHitTestMetrics m = {0};
    if (FAILED(COM(p->layout, HitTestPoint, pt.x, pt.y, &trailing, &inside, &m))) {
        return r;
    }
    iz wpos = (iz)m.position + (trailing ? (iz)m.length : 0);
    wpos     = min(wpos, p->wlen);
    r.offset = p->w2u[wpos];
    r.inside = inside;
    if (inside) {
        for (iz i = 0; i < p->nruns; i++) {
            PRun *pr = p->runs + i;
            if (m.position>=pr->wbeg && m.position<pr->wbeg+pr->wlen) {
                r.link = pr->link;
                break;
            }
        }
    }
    return r;
}

static iz os_para_rects(Paragraph *p, iz beg, iz end, Rect *out, iz cap) {
    beg = max(beg, (iz)0);
    end = min(end, p->len);
    if (end <= beg) {
        return 0;
    }
    iz wbeg = p->u2w[beg];
    iz wend = p->u2w[end];
    if (wend <= wbeg) {
        return 0;
    }
    u32 n = 0;
    COM(p->layout, HitTestTextRange, (u32)wbeg, (u32)(wend-wbeg), 0, 0, 0, 0, &n);
    if (!n) {
        return 0;
    }
    DwHitTestMetrics *m = heapnew(n, (iz)sizeof(DwHitTestMetrics));
    if (!m) {
        return 0;
    }
    iz count = 0;
    if (!FAILED(COM(p->layout, HitTestTextRange, (u32)wbeg, (u32)(wend-wbeg),
                    0, 0, m, n, &n))) {
        count = n;
        for (iz i = 0; i<count && i<cap; i++) {
            out[i] = (Rect){m[i].left, m[i].top, m[i].width, m[i].height};
        }
    }
    heapfree(m);
    return count;
}

static void os_para_draw(Paragraph *p, Point origin) {
    if (!gfx.rt) {
        return;
    }
    // Colors are drawing effects: the brush each range is drawn with. They
    // belong to a render target, so set them again after it was replaced.
    if (p->gen != gfx.gen) {
        for (iz i = 0; i < p->nruns; i++) {
            PRun *pr = p->runs + i;
            if (pr->wlen && !pr->box) {
                void *b = brushfor(pr->color);
                DwTextRange range = {pr->wbeg, pr->wlen};
                COM(p->layout, SetDrawingEffect, b, range);
            }
        }
        p->gen = gfx.gen;
    }
    void *def = brushfor(p->nruns ? p->runs[0].color : rgb(0, 0, 0));
    COM(gfx.rt, DrawTextLayout, (D2dPoint){origin.x, origin.y}, p->layout, def,
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}


// Images

// TODO(milestone 4): decode with WIC (IWICImagingFactory, frame decoder,
// GIF frame metadata for delay/disposal/offsets), keep composited frames,
// and draw them through ID2D1Bitmap created from the render target (cache
// per frame, dropped with the target, see gfx.gen).
static Image *os_image_decode(Str bytes) {
    (void)bytes;
    return 0;
}

static void os_image_free(Image *im) {
    (void)im;
}

static Size os_image_size(Image *im) {
    (void)im;
    return (Size){0};
}

static i32 os_image_frames(Image *im) {
    (void)im;
    return 0;
}

static i32 os_image_delay(Image *im, i32 frame) {
    (void)im; (void)frame;
    return 0;
}

static void os_image_draw(Image *im, i32 frame, Rect dst) {
    (void)im; (void)frame; (void)dst;
}
