// Document layout and painting. Portable: talks to the OS only through
// platform.h.
//
// The parsed tree is flattened into a list of leaves in document order: a
// paragraph, a heading, a chunk of a code block, a table, a rule. Containers
// (block quotes, list items, code blocks) become decorations that refer to a
// range of leaves, so a leaf never needs to know what surrounds it beyond
// its indent and margins. The leaves are stacked top to bottom with CSS style
// margin collapsing.
//
// A leaf's height is only known once its text is laid out, and creating a
// platform paragraph for every leaf of a large document would be slow, so
// that happens lazily: leaves outside the window keep an estimated height,
// leaves that are on screen (or were, at this width) have an exact one. When
// an estimate is corrected, the caller keeps the reading position by anchoring
// on a leaf and its offset (see doc_ensure).
//
// Everything that scales with zoom is stored unscaled in the leaf and scaled
// when the leaves are positioned, so zooming is a relayout, not a reparse.

typedef struct Leaf Leaf;
typedef struct Decor Decor;
typedef struct Table Table;
typedef struct ImgEntry ImgEntry;


// Theme

typedef struct {
    Color bg, text, muted, link, border, rule, code_bg, inline_bg, quote_bar;
    Color alt_row, accent, thumb, status_bg, error;
    Color hl[HL_COUNT];     // by HlKind; unset (alpha 0) means plain text
} Theme;

#define HEX(h)      {((h)>>16)&255, ((h)>>8)&255, (h)&255, 255}
#define HEXA(h, a)  {((h)>>16)&255, ((h)>>8)&255, (h)&255, (a)}

static Theme const theme_light = {
    .bg = HEX(0xffffff), .text = HEX(0x1f2328), .muted = HEX(0x59636e),
    .link = HEX(0x0969da), .border = HEX(0xd1d9e0), .rule = HEX(0xd1d9e0),
    .code_bg = HEX(0xf6f8fa), .inline_bg = HEXA(0x818b98, 40),
    .quote_bar = HEX(0xd1d9e0), .alt_row = HEX(0xf6f8fa), .accent = HEX(0x0969da),
    .thumb = HEXA(0x59636e, 150), .status_bg = HEX(0xf6f8fa), .error = HEX(0xcf222e),
    .hl = {
        [HL_COMMENT]  = HEX(0x59636e), [HL_KEYWORD]  = HEX(0xcf222e),
        [HL_TYPE]     = HEX(0x953800), [HL_STRING]   = HEX(0x0a3069),
        [HL_ESCAPE]   = HEX(0x0550ae), [HL_NUMBER]   = HEX(0x0550ae),
        [HL_CONSTANT] = HEX(0x0550ae), [HL_FUNCTION] = HEX(0x6639ba),
        [HL_PREPROC]  = HEX(0xcf222e), [HL_VARIABLE] = HEX(0x953800),
        [HL_PROPERTY] = HEX(0x0550ae), [HL_TAG]      = HEX(0x116329),
        [HL_INSERTED] = HEX(0x116329), [HL_DELETED]  = HEX(0x82071e),
        [HL_META]     = HEX(0x0550ae),
    },
};

static Theme const theme_dark = {
    .bg = HEX(0x0d1117), .text = HEX(0xe6edf3), .muted = HEX(0x9198a1),
    .link = HEX(0x4493f8), .border = HEX(0x3d444d), .rule = HEX(0x3d444d),
    .code_bg = HEX(0x151b23), .inline_bg = HEXA(0x6e7681, 90),
    .quote_bar = HEX(0x3d444d), .alt_row = HEX(0x151b23), .accent = HEX(0x1f6feb),
    .thumb = HEXA(0x9198a1, 150), .status_bg = HEX(0x151b23), .error = HEX(0xff7b72),
    .hl = {
        [HL_COMMENT]  = HEX(0x9198a1), [HL_KEYWORD]  = HEX(0xff7b72),
        [HL_TYPE]     = HEX(0xffa657), [HL_STRING]   = HEX(0xa5d6ff),
        [HL_ESCAPE]   = HEX(0x79c0ff), [HL_NUMBER]   = HEX(0x79c0ff),
        [HL_CONSTANT] = HEX(0x79c0ff), [HL_FUNCTION] = HEX(0xd2a8ff),
        [HL_PREPROC]  = HEX(0xff7b72), [HL_VARIABLE] = HEX(0xffa657),
        [HL_PROPERTY] = HEX(0x79c0ff), [HL_TAG]      = HEX(0x7ee787),
        [HL_INSERTED] = HEX(0x7ee787), [HL_DELETED]  = HEX(0xffa198),
        [HL_META]     = HEX(0x79c0ff),
    },
};


// Metrics, in DIPs at zoom 1. Roughly GitHub's.

enum {
    M_BODY       = 16,
    M_PAGE_MAX   = 900,         // widest column
    M_SIDE       = 32,          // minimum space either side of the column
    M_PAGE_PAD   = 32,          // above the first and below the last leaf
    M_QUOTE      = 20,          // indent inside a block quote: bar and padding
    M_LIST       = 32,          // indent inside a list item
    M_CODE_PAD   = 16,
    M_CELL_PADX  = 13,
    M_CELL_PADY  = 6,
    M_CHUNK      = 64,          // code lines per leaf
    M_MAX_LIVE   = 500,         // paragraphs kept before the far ones are freed
};

static f32 const line_body = 24;            // 1.5 em
static f32 const code_scale = 0.85f;
static f32 const heading_size[7] = {16, 32, 24, 20, 16, 14, 13.6f};


// The tree, flattened

typedef enum {
    LK_PARA,
    LK_HEAD,
    LK_CODE,        // a chunk of lines of a code block
    LK_RULE,
    LK_TABLE,
    LK_HTML,        // a raw HTML block, shown as text and images
    LK_EMPTY,       // an empty list item or block quote: takes a line
} LeafKind;

enum {
    LF_FIRST   = 1<<0,      // code: first chunk of its block
    LF_LAST    = 1<<1,      // code: last chunk of its block
    LF_IMAGES  = 1<<2,      // has inline image boxes: rebuild when the width changes
    LF_EXTRAS  = 1<<3,      // has code span backgrounds or image boxes to draw
    LF_FAILED  = 1<<4,      // the platform could not create its paragraph
    LF_MUTED   = 1<<5,      // inside a block quote
};

struct Table {
    Paragraph **cell;       // nrows*ncols, 0 for an empty cell
    f32        *colw;       // ncols: width of the text in each column
    f32        *colx;       // ncols+1: x of each column's left border
    f32        *rowy;       // nrows+1: y of each row's top border
    i32         nrows, ncols;
    f32         padx;       // cell padding left and right, shrunk in narrow tables
    b32         built;      // cell paragraphs exist and match the leaf's layout
};

struct Leaf {
    MdNode    *node;
    Paragraph *p;
    Table     *tb;
    iz         beg, end;    // code: byte range of the chunk in node->text
    f32        indent;      // unscaled, from the column's left edge
    f32        mt, mb;      // unscaled margins
    f32        x, y, w, h;  // scaled; x and y from the column's top left
    f32        padl, padt, padb;   // scaled, inside h
    u32        hepoch;      // the epoch in which h is exact
    i32        line;        // source line, for keeping the position on reload
    i32        decor;       // code: its decoration
    i32        n;           // code: lines; table: rows
    u8         kind, level, align, flags;
};

typedef enum {
    DK_QUOTE,       // a bar down the left of leaves lo to hi
    DK_ITEM,        // a list marker beside leaf lo
    DK_CODE,        // a tinted box around leaves lo to hi
} DecorKind;

struct Decor {
    Paragraph *mk;          // item: the number, made when first painted
    MdNode    *node;
    HlSpans    hl;          // code: the highlighter's spans for the whole block
    i32        lo, hi;      // leaves [lo, hi)
    i32        num;         // item: its number in an ordered list
    f32        x;           // unscaled: quote bar; item: content left of the item
    u8         kind, bullet, ordered, task, delim, hldone;
};

typedef struct {
    i32 leaf;
    f32 off;                // from the leaf's top, at the zoom it was taken
    i32 line;               // source line of the leaf, for another parse
} Anchor;


// Images, decoded once per document

struct ImgEntry {
    ImgEntry *next;
    Str       src;
    Image    *im;           // 0 if it could not be loaded
    Size      size;
    i32       frames, frame, acc;
    b32       drawn;        // painted since the last timer tick
};

// Where the image's bytes come from: view.c resolves src against the
// document. Returns 0 for what it can't fetch (remote images, so far).
static b32 img_fetch(Arena *a, Str src, Str *bytes);


static struct {
    Arena       *mem;           // what the document's data lives in
    MdNode      *root;
    Leaf        *leaves;
    Decor       *decors;
    i32          nleaves, ndecors;
    ImgEntry    *imgs[64];
    Theme const *th;
    f32          zoom, winw, colx, colw, total;
    u32          epoch;         // changes when heights are no longer exact
    i32          nlive;         // paragraphs alive
    b32          anim;          // an animated image was painted
} D;


// Small helpers

static i32 clamp32(i32 v, i32 lo, i32 hi) {
    return v<lo ? lo : v>hi ? hi : v;
}

static f32 ffloor(f32 v) {
    v = clampf(v, -1e9f, 1e9f);
    f32 t = (f32)(i64)v;
    return t > v ? t - 1 : t;
}

static f32 fceil(f32 v) {
    v = clampf(v, -1e9f, 1e9f);
    f32 t = (f32)(i64)v;
    return t < v ? t + 1 : t;
}

static iz fmt_int(u8 *out, i64 v) {
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

static b32 ci_equals(Str a, Str b) {
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        if (lower(a.data[i]) != lower(b.data[i])) {
            return 0;
        }
    }
    return 1;
}

static u32 str_hash(Str s) {
    u32 h = 2166136261u;
    for (iz i = 0; i < s.len; i++) {
        h = (h ^ s.data[i]) * 16777619u;
    }
    return h;
}

static Color hl_color(HlKind k) {
    Color c = D.th->hl[k];
    return c.a ? c : D.th->text;
}

static void para_free(Paragraph *p) {
    if (p) {
        os_para_free(p);
        D.nlive--;
    }
}

static Paragraph *para_new(Run const *runs, iz n, f32 w) {
    Paragraph *p = os_para_new(runs, n, w);
    if (p) {
        D.nlive++;
    }
    return p;
}


// HTML in Markdown: tags are found, not validated. Only what a README
// plausibly uses is understood, the rest is dropped and its text kept.

typedef struct {
    Str name;               // as written
    Str attrs;              // after the name, before '>'
    iz  end;                // index past the '>'
    b32 closing;
    b32 comment;
} HtmlTag;

// The tag at s[i], which is '<'. Returns 0 if it isn't one.
static b32 html_tag(Str s, iz i, HtmlTag *t) {
    *t = (HtmlTag){0};
    iz j = i + 1;
    if (s.len-j >= 3 && s.data[j]=='!' && s.data[j+1]=='-' && s.data[j+2]=='-') {
        j += 3;
        while (j < s.len && !(s.data[j]=='>' && j-i >= 6 &&
                              s.data[j-1]=='-' && s.data[j-2]=='-')) {
            j++;
        }
        t->end     = min(j + 1, s.len);
        t->comment = 1;
        return 1;
    }
    if (j<s.len && (s.data[j]=='!' || s.data[j]=='?')) {
        while (j<s.len && s.data[j]!='>') {
            j++;
        }
        t->end     = min(j + 1, s.len);
        t->comment = 1;
        return 1;
    }
    if (j<s.len && s.data[j]=='/') {
        t->closing = 1;
        j++;
    }
    iz nb = j;
    while (j<s.len && (md_isalnum(s.data[j]) || s.data[j]=='-')) {
        j++;
    }
    if (j==nb || !md_isalpha(s.data[nb])) {
        return 0;
    }
    t->name = (Str){s.data+nb, j-nb};
    iz ab   = j;
    u8 q    = 0;
    while (j < s.len && (q || s.data[j]!='>')) {
        if (q) {
            q = s.data[j]==q ? 0 : q;
        } else if (s.data[j]=='"' || s.data[j]=='\'') {
            q = s.data[j];
        }
        j++;
    }
    if (j >= s.len) {
        return 0;
    }
    t->attrs = (Str){s.data+ab, j-ab};
    t->end   = j + 1;
    return 1;
}

// The value of attribute name, or a Str with data 0 if absent.
static Str html_attr(Str attrs, Str name) {
    iz i = 0;
    while (i < attrs.len) {
        while (i<attrs.len && (md_isspace(attrs.data[i]) || attrs.data[i]=='/')) {
            i++;
        }
        iz nb = i;
        while (i<attrs.len && !md_isspace(attrs.data[i]) && attrs.data[i]!='=' &&
               attrs.data[i]!='/') {
            i++;
        }
        Str key = {attrs.data+nb, i-nb};
        while (i<attrs.len && md_isspace(attrs.data[i])) {
            i++;
        }
        Str val = {(u8 *)"", 0};
        if (i<attrs.len && attrs.data[i]=='=') {
            i++;
            while (i<attrs.len && md_isspace(attrs.data[i])) {
                i++;
            }
            if (i<attrs.len && (attrs.data[i]=='"' || attrs.data[i]=='\'')) {
                u8  q  = attrs.data[i++];
                iz  vb = i;
                while (i<attrs.len && attrs.data[i]!=q) {
                    i++;
                }
                val = (Str){attrs.data+vb, i-vb};
                i++;
            } else {
                iz vb = i;
                while (i<attrs.len && !md_isspace(attrs.data[i])) {
                    i++;
                }
                val = (Str){attrs.data+vb, i-vb};
            }
        }
        if (key.len && ci_equals(key, name)) {
            return val;
        }
    }
    return (Str){0};
}

// A width or height attribute: "200", "200px" or "50%" (of avail). Returns 0
// if there is none.
static f32 html_dim(Str v, f32 avail) {
    if (!v.data || !v.len) {
        return 0;
    }
    f32 n = 0;
    iz  i = 0;
    for (; i<v.len && v.data[i]>='0' && v.data[i]<='9'; i++) {
        n = n*10 + (f32)(v.data[i] - '0');
    }
    if (i == 0) {
        return 0;
    }
    if (i<v.len && v.data[i]=='%') {
        return n*avail/100;
    }
    return n * D.zoom;
}

// Is there nothing to see in this HTML block: only comments, tags without
// images, and blanks?
static b32 html_blank(Str s) {
    for (iz i = 0; i < s.len;) {
        HtmlTag t;
        if (s.data[i]=='<' && html_tag(s, i, &t)) {
            if (!t.comment && !t.closing && ci_equals(t.name, S("img"))) {
                return 0;
            }
            i = t.end;
        } else {
            if (!md_isspace(s.data[i])) {
                return 0;
            }
            i++;
        }
    }
    return 1;
}


// Building runs

typedef enum {
    SP_CODE,                // tinted background
    SP_BORDER,              // bordered: an image that could not be shown
} SpanKind;

typedef struct {
    iz  beg, end;
    u8  kind;
} Span;

typedef struct {
    iz        off;          // where its 3 bytes are in the paragraph
    ImgEntry *img;
    f32       w, h;         // the box's size; its bottom edge is on the line's baseline
} ImgBox;

// Collects the runs of one paragraph, and what the painter needs to know
// about it. Lives in scratch memory, grows when needed, and is reused from
// one paragraph to the next.
typedef struct {
    Arena  *a;
    Run    *runs;
    iz     *off;            // where each run's text starts in tbuf
    u8     *tbuf;
    Span   *spans;
    ImgBox *boxes;
    iz      n, cap;
    iz      tlen, tcap;
    iz      nspans, capspans;
    iz      nboxes, capboxes;
    iz      len;            // bytes of the paragraph so far
    iz      col;            // code: column, for tabs
    i32     nlink;          // links seen so far
    i32     find;           // look for this link
    Str     found;          // its destination
    f32     avail;          // width for images
    Align   align;          // what HTML asked for
    b32     atstart;        // nothing but breaks so far on this line
    b32     lastspace;
    b32     over;           // out of memory: the rest is dropped
} RB;

static void *rb_grow(RB *rb, void *old, iz used, iz *cap, iz want, iz size) {
    iz room = (rb->a->end - rb->a->beg) - 32768;
    if (want*size > room/2) {
        rb->over = 1;
        return old;
    }
    u8 *p = alloc(rb->a, want, size, 16);
    if (used) {
        memcpy(p, old, (uz)(used*size));
    }
    *cap = want;
    return p;
}

static void rb_reset(RB *rb) {
    rb->n        = 0;
    rb->tlen     = 0;
    rb->nspans   = 0;
    rb->nboxes   = 0;
    rb->len      = 0;
    rb->col      = 0;
    rb->nlink    = 0;
    rb->find     = 0;
    rb->found    = (Str){0};
    rb->align    = ALIGN_LEFT;
    rb->atstart  = 1;
    rb->lastspace = 0;
    rb->over     = 0;
}

static void rb_init(RB *rb, Arena *a) {
    *rb   = (RB){0};
    rb->a = a;
    rb_reset(rb);
}

static b32 run_same(Run const *a, Run const *b) {
    return a->font==b->font && a->size==b->size && a->bold==b->bold &&
           a->italic==b->italic && a->strike==b->strike && a->underline==b->underline &&
           a->link==b->link && a->color.r==b->color.r && a->color.g==b->color.g &&
           a->color.b==b->color.b && a->color.a==b->color.a;
}

static b32 rb_slot(RB *rb) {
    if (rb->over) {
        return 0;
    }
    if (rb->n == rb->cap) {
        iz want = max(rb->cap*2, (iz)32);
        iz c1 = rb->cap, c2 = rb->cap;
        Run *r = rb_grow(rb, rb->runs, rb->n, &c1, want, (iz)sizeof(Run));
        iz  *o = rb_grow(rb, rb->off, rb->n, &c2, want, (iz)sizeof(iz));
        if (rb->over) {
            return 0;
        }
        rb->runs = r;
        rb->off  = o;
        rb->cap  = want;
    }
    return 1;
}

static void rb_text(RB *rb, Str s, Run const *st) {
    if (!s.len || rb->over) {
        return;
    }
    if (rb->tlen + s.len > rb->tcap) {
        iz want = max(rb->tcap*2, rb->tlen + s.len + 4096);
        iz c    = rb->tcap;
        u8 *t   = rb_grow(rb, rb->tbuf, rb->tlen, &c, want, 1);
        if (rb->over) {
            return;
        }
        rb->tbuf = t;
        rb->tcap = want;
    }
    if (rb->n>0 && rb->runs[rb->n-1].box_w==0 && run_same(rb->runs + rb->n - 1, st)) {
        rb->runs[rb->n-1].text.len += s.len;
    } else {
        if (!rb_slot(rb)) {
            return;
        }
        Run *r = rb->runs + rb->n;
        *r        = *st;
        r->text   = (Str){0, s.len};
        r->box_w  = 0;
        r->box_h  = 0;
        rb->off[rb->n] = rb->tlen;
        rb->n++;
    }
    memcpy(rb->tbuf + rb->tlen, s.data, (uz)s.len);
    rb->tlen += s.len;
    rb->len  += s.len;
    rb->atstart = 0;
}

static void rb_box(RB *rb, Run const *st, f32 w, f32 h, ImgEntry *img) {
    if (!rb_slot(rb)) {
        return;
    }
    if (rb->nboxes == rb->capboxes) {
        iz want = max(rb->capboxes*2, (iz)8);
        ImgBox *b = rb_grow(rb, rb->boxes, rb->nboxes, &rb->capboxes, want,
                            (iz)sizeof(ImgBox));
        if (rb->over) {
            return;
        }
        rb->boxes = b;
    }
    Run *r = rb->runs + rb->n;
    *r        = *st;
    r->text   = (Str){0};
    r->box_w  = w;
    r->box_h  = h;
    rb->off[rb->n] = 0;
    rb->n++;
    rb->boxes[rb->nboxes++] = (ImgBox){rb->len, img, w, h};
    rb->len    += 3;
    rb->atstart = 0;
}

static void rb_span(RB *rb, iz beg, iz end, SpanKind kind) {
    if (end <= beg || rb->over) {
        return;
    }
    if (rb->nspans == rb->capspans) {
        iz want = max(rb->capspans*2, (iz)16);
        Span *s = rb_grow(rb, rb->spans, rb->nspans, &rb->capspans, want, (iz)sizeof(Span));
        if (rb->over) {
            return;
        }
        rb->spans = s;
    }
    rb->spans[rb->nspans++] = (Span){beg, end, (u8)kind};
}

// Point the runs at their text. After this rb_text must not be called.
static void rb_finish(RB *rb) {
    for (iz i = 0; i < rb->n; i++) {
        if (rb->runs[i].box_w == 0) {
            rb->runs[i].text.data = rb->tbuf + rb->off[i];
        }
    }
}

// Newlines at the end of what has been collected, up to two.
static i32 rb_trailing_newlines(RB *rb) {
    i32 k = 0;
    while (k<2 && k<rb->tlen && rb->tbuf[rb->tlen-1-k]=='\n') {
        k++;
    }
    return k;
}

// Make the text so far end in at least want newlines, unless it is empty.
static void rb_newlines(RB *rb, i32 want, Run const *st) {
    if (rb->len == 0) {
        return;
    }
    if (rb->n && rb->runs[rb->n-1].box_w > 0) {
        rb_text(rb, S("\n"), st);
        want--;
    }
    for (i32 k = rb_trailing_newlines(rb); k < want; k++) {
        rb_text(rb, S("\n"), st);
    }
    rb->atstart   = 1;
    rb->lastspace = 0;
}

// Drop trailing blanks and newlines.
static void rb_trim(RB *rb) {
    while (rb->n && rb->runs[rb->n-1].box_w == 0) {
        Run *r = rb->runs + rb->n - 1;
        while (r->text.len && (rb->tbuf[rb->off[rb->n-1] + r->text.len - 1]==' ' ||
                               rb->tbuf[rb->off[rb->n-1] + r->text.len - 1]=='\n')) {
            r->text.len--;
            rb->tlen--;
            rb->len--;
        }
        if (r->text.len) {
            break;
        }
        rb->n--;
    }
}

// One code point of text, collapsing white space like HTML does.
static void rb_html_char(RB *rb, i32 c, Run const *st) {
    if (c==' ' || c=='\t' || c=='\n' || c=='\r') {
        if (!rb->atstart && !rb->lastspace) {
            rb_text(rb, S(" "), st);
            rb->lastspace = 1;
        }
        return;
    }
    u8 buf[4];
    rb_text(rb, (Str){buf, utf8encode(buf, c)}, st);
    rb->lastspace = 0;
}

static void rb_html_text(RB *rb, Str s, Run const *st) {
    for (iz i = 0; i < s.len;) {
        i32 c1 = 0, c2 = 0;
        iz  e  = s.data[i]=='&' ? md_entity(s, i, &c1, &c2) : 0;
        if (e) {
            rb_html_char(rb, c1, st);
            if (c2) {
                rb_html_char(rb, c2, st);
            }
            i = e;
        } else {
            Utf8 d = utf8decode((Str){s.data+i, s.len-i});
            rb_html_char(rb, d.rune, st);
            i = s.len - d.tail.len;
        }
    }
}

// The last component of a path or URL, for an image that failed to load.
static Str url_name(Str s) {
    iz e = s.len;
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i]=='?' || s.data[i]=='#') {
            e = i;
            break;
        }
    }
    iz b = 0;
    for (iz i = 0; i < e; i++) {
        if (s.data[i]=='/' || s.data[i]=='\\') {
            b = i + 1;
        }
    }
    return (Str){s.data+b, e-b};
}

static ImgEntry *img_get(Str src) {
    u32 h = str_hash(src) & 63;
    for (ImgEntry *e = D.imgs[h]; e; e = e->next) {
        if (equals(e->src, src)) {
            return e;
        }
    }
    ImgEntry *e = new(D.mem, 1, ImgEntry);
    e->src  = src;
    e->next = D.imgs[h];
    D.imgs[h] = e;
    Arena t   = *D.mem;     // read into the free space behind, then forget it
    Str bytes = {0};
    if (img_fetch(&t, src, &bytes)) {
        e->im = os_image_decode(bytes);
    }
    if (e->im) {
        e->size   = os_image_size(e->im);
        e->frames = os_image_frames(e->im);
        if (!(e->size.w >= 1 && e->size.h >= 1)) {
            os_image_free(e->im);
            e->im = 0;
        }
    }
    return e;
}

// An image: a box the size of the picture, or its alt text in a border. reqw
// and reqh are the sizes asked for in the document, already scaled, or 0.
static void rb_image(RB *rb, Str src, Str alt, f32 reqw, f32 reqh, Run const *st) {
    ImgEntry *e = src.len && !rb->find ? img_get(src) : 0;   // finding links needs no pictures
    if (e && e->im) {
        f32 z = D.zoom;
        f32 w = e->size.w * z;
        f32 h = e->size.h * z;
        if (reqw>0 && reqh>0) {
            w = reqw;
            h = reqh;
        } else if (reqw > 0) {
            h = h*reqw/w;
            w = reqw;
        } else if (reqh > 0) {
            w = w*reqh/h;
            h = reqh;
        }
        if (w > rb->avail) {
            h = h*rb->avail/w;
            w = rb->avail;
        }
        rb_box(rb, st, max(w, (f32)1), max(h, (f32)1), e);
        return;
    }
    Str text = alt.len ? alt : url_name(src);
    if (!text.len) {
        text = S("image");
    }
    Run r = *st;
    r.color = D.th->muted;
    iz beg = rb->len;
    rb_text(rb, text, &r);
    rb_span(rb, beg, rb->len, SP_BORDER);
}

static void html_img(RB *rb, HtmlTag const *t, Run const *st) {
    Str src = html_attr(t->attrs, S("src"));
    Str alt = html_attr(t->attrs, S("alt"));
    rb_image(rb, src.data ? src : (Str){0}, alt.data ? alt : (Str){0},
             html_dim(html_attr(t->attrs, S("width")), rb->avail),
             html_dim(html_attr(t->attrs, S("height")), rb->avail), st);
}

static void html_align(RB *rb, HtmlTag const *t) {
    Str a = html_attr(t->attrs, S("align"));
    if (a.data && rb->align == ALIGN_LEFT) {
        if (ci_equals(a, S("center"))) {
            rb->align = ALIGN_CENTER;
        } else if (ci_equals(a, S("right"))) {
            rb->align = ALIGN_RIGHT;
        }
    }
}

// Apply one tag to the running style st; base is the style at the start of
// the enclosing sequence, which closing tags go back to.
static void html_apply(RB *rb, HtmlTag const *t, Run *st, Run const *base) {
    if (t->comment) {
        return;
    }
    Str n = t->name;
    b32 open = !t->closing;
    if (ci_equals(n, S("img"))) {
        if (open) {
            html_img(rb, t, st);
        }
    } else if (ci_equals(n, S("br"))) {
        if (open) {
            rb_text(rb, S("\n"), st);
            rb->atstart   = 1;
            rb->lastspace = 0;
        }
    } else if (ci_equals(n, S("a"))) {
        Str href = open ? html_attr(t->attrs, S("href")) : (Str){0};
        if (href.data) {
            i32 id = ++rb->nlink;
            if (rb->find == id) {
                rb->found = href;
            }
            st->link  = id;
            st->color = D.th->link;
        } else if (!open) {
            st->link  = base->link;
            st->color = base->color;
        }
    } else if (ci_equals(n, S("b")) || ci_equals(n, S("strong"))) {
        st->bold = open ? 1 : base->bold;
    } else if (ci_equals(n, S("i")) || ci_equals(n, S("em"))) {
        st->italic = open ? 1 : base->italic;
    } else if (ci_equals(n, S("u")) || ci_equals(n, S("ins"))) {
        st->underline = open ? 1 : base->underline;
    } else if (ci_equals(n, S("s")) || ci_equals(n, S("del")) || ci_equals(n, S("strike"))) {
        st->strike = open ? 1 : base->strike;
    } else if (ci_equals(n, S("code")) || ci_equals(n, S("kbd")) || ci_equals(n, S("tt"))) {
        st->font = open ? FONT_MONOSPACE : base->font;
        st->size = open ? base->size*code_scale : base->size;
    } else if (n.len==2 && lower(n.data[0])=='h' && n.data[1]>='1' && n.data[1]<='6') {
        rb_newlines(rb, 2, st);
        if (open) {
            html_align(rb, t);
            st->size = heading_size[n.data[1]-'0'] * D.zoom;
            st->bold = 1;
        } else {
            st->size = base->size;
            st->bold = base->bold;
        }
    } else if (ci_equals(n, S("p")) || ci_equals(n, S("blockquote")) ||
               ci_equals(n, S("table")) || ci_equals(n, S("ul")) ||
               ci_equals(n, S("ol")) || ci_equals(n, S("pre")) ||
               ci_equals(n, S("details"))) {
        rb_newlines(rb, 2, st);
        if (open) {
            html_align(rb, t);
        }
    } else if (ci_equals(n, S("div")) || ci_equals(n, S("center")) ||
               ci_equals(n, S("summary")) || ci_equals(n, S("section")) ||
               ci_equals(n, S("tr")) || ci_equals(n, S("hr"))) {
        rb_newlines(rb, 1, st);
        if (open) {
            html_align(rb, t);
            if (ci_equals(n, S("center"))) {
                rb->align = ALIGN_CENTER;
            }
        }
    } else if (ci_equals(n, S("li"))) {
        rb_newlines(rb, 1, st);
        if (open) {
            rb_text(rb, S("\xE2\x80\xA2 "), st);
            rb->lastspace = 1;
        }
    } else if (ci_equals(n, S("td")) || ci_equals(n, S("th"))) {
        if (open && rb->len && !rb->atstart) {
            rb_text(rb, S("   "), st);
        }
    }
}

// The text of an HTML block.
static void html_runs(RB *rb, Str s, Run base) {
    Run st = base;
    for (iz i = 0; i < s.len;) {
        HtmlTag t;
        if (s.data[i]=='<' && html_tag(s, i, &t)) {
            html_apply(rb, &t, &st, &base);
            i = t.end;
            continue;
        }
        iz j = i + 1;
        while (j<s.len && s.data[j]!='<') {
            j++;
        }
        rb_html_text(rb, (Str){s.data+i, j-i}, &st);
        i = j;
    }
    rb_trim(rb);
}

static void inl_walk(RB *rb, MdNode *parent, Run st);

// A raw HTML node inside a paragraph holds a single tag.
static void inl_html(RB *rb, MdNode *c, Run *st, Run const *base) {
    HtmlTag t;
    if (c->text.len && c->text.data[0]=='<' && html_tag(c->text, 0, &t)) {
        html_apply(rb, &t, st, base);
    }
}

static void inl_walk(RB *rb, MdNode *parent, Run st) {
    Run base = st;
    for (MdNode *c = parent->first; c && !rb->over; c = c->next) {
        switch (c->kind) {
        case MD_TEXT:
            rb_text(rb, c->text, &st);
            break;
        case MD_SOFTBREAK:
            rb_text(rb, S(" "), &st);
            break;
        case MD_HARDBREAK:
            rb_text(rb, S("\n"), &st);
            break;
        case MD_CODE: {
            Run r = st;
            r.font = FONT_MONOSPACE;
            r.size = st.size * code_scale;
            iz beg = rb->len;
            rb_text(rb, c->text, &r);
            rb_span(rb, beg, rb->len, SP_CODE);
            break;
        }
        case MD_EMPH: {
            Run r = st;
            r.italic = 1;
            inl_walk(rb, c, r);
            break;
        }
        case MD_STRONG: {
            Run r = st;
            r.bold = 1;
            inl_walk(rb, c, r);
            break;
        }
        case MD_STRIKE: {
            Run r = st;
            r.strike = 1;
            inl_walk(rb, c, r);
            break;
        }
        case MD_LINK: {
            Run r = st;
            r.link  = ++rb->nlink;
            r.color = D.th->link;
            if (rb->find == r.link) {
                rb->found = c->dest;
            }
            inl_walk(rb, c, r);
            break;
        }
        case MD_IMAGE: {
            Str alt = md_plain(rb->a, c);
            rb_image(rb, c->dest, alt, 0, 0, &st);
            break;
        }
        case MD_HTML_INLINE:
            inl_html(rb, c, &st, &base);
            break;
        default:
            break;
        }
    }
}


// Flattening

typedef struct {
    Leaf  *leaves;
    Decor *decors;
    i32    nleaves, ndecors;
    f32    pend;            // top margin waiting for the next leaf
    b32    emit;            // false: only count
} Flat;

typedef struct {
    b32 tight;              // directly inside a tight list item
    b32 quote;
    i32 depth;              // bullet lists around
} FlatCtx;

static Leaf flat_dummy;
static Decor flat_dummy_decor;

static Leaf *flat_leaf(Flat *f, MdNode *n, LeafKind kind, f32 indent, f32 mt, f32 mb,
                       b32 first, b32 last, FlatCtx cx) {
    Leaf *l = f->emit ? f->leaves + f->nleaves : &flat_dummy;
    f->nleaves++;
    if (f->emit) {
        l->node   = n;
        l->kind   = (u8)kind;
        l->indent = indent;
        l->mt     = max(first ? 0 : mt, f->pend);
        l->mb     = last ? 0 : mb;
        l->line   = n ? n->line : 0;
        l->decor  = -1;
        l->flags  = cx.quote ? LF_MUTED : 0;
    }
    f->pend = 0;
    return l;
}

static Decor *flat_decor(Flat *f, DecorKind kind, MdNode *n, f32 x) {
    Decor *d = f->emit ? f->decors + f->ndecors : &flat_dummy_decor;
    f->ndecors++;
    if (f->emit) {
        d->kind = (u8)kind;
        d->node = n;
        d->x    = x;
        d->lo   = f->nleaves;
    }
    return d;
}

// After a container: its bottom margin goes on its last leaf.
static void flat_close(Flat *f, i32 lo, f32 mb, b32 last) {
    if (f->emit && !last && f->nleaves > lo) {
        Leaf *l = f->leaves + f->nleaves - 1;
        l->mb = max(l->mb, mb);
    }
}

static iz code_lines(Str t) {
    iz n = 0;
    for (iz i = 0; i < t.len; i++) {
        n += t.data[i] == '\n';
    }
    return max(n + (t.len>0 && t.data[t.len-1]!='\n'), (iz)1);
}

static void flat_blocks(Flat *f, MdNode *parent, f32 indent, FlatCtx cx);

static void flat_item(Flat *f, MdNode *list, MdNode *it, i32 k, f32 indent, FlatCtx cx) {
    b32 ordered = !!(list->flags & MD_F_ORDERED);
    b32 tight   = !!(list->flags & MD_F_TIGHT);
    Decor *d = flat_decor(f, DK_ITEM, it, indent);
    i32 di   = f->ndecors - 1;
    if (f->emit) {
        d->ordered = (u8)ordered;
        d->bullet  = (u8)(cx.depth % 3);
        d->num     = list->start + k;
        d->task    = it->task;
        d->delim   = list->delim;
    }
    i32 lo = f->nleaves;
    FlatCtx in = {tight, cx.quote, cx.depth + !ordered};
    flat_blocks(f, it, indent + M_LIST, in);
    if (f->nleaves == lo) {
        flat_leaf(f, 0, LK_EMPTY, indent + M_LIST, 0, 0, 1, 1, cx);
    }
    if (f->emit) {
        f->decors[di].hi = f->nleaves;
    }
}

static void flat_blocks(Flat *f, MdNode *parent, f32 indent, FlatCtx cx) {
    for (MdNode *c = parent->first; c; c = c->next) {
        b32 first = c == parent->first;
        b32 last  = !c->next;
        f32 mb    = cx.tight ? 0 : M_BODY;
        switch (c->kind) {
        case MD_PARAGRAPH:
            flat_leaf(f, c, LK_PARA, indent, 0, mb, first, last, cx);
            break;
        case MD_HEADING: {
            Leaf *l = flat_leaf(f, c, LK_HEAD, indent, 24, M_BODY, first, last, cx);
            l->level = c->level;
            break;
        }
        case MD_RULE:
            flat_leaf(f, c, LK_RULE, indent, 24, 24, first, last, cx);
            break;
        case MD_HTML_BLOCK:
            if (!html_blank(c->text)) {
                flat_leaf(f, c, LK_HTML, indent, 0, mb, first, last, cx);
            }
            break;
        case MD_TABLE: {
            Leaf *l = flat_leaf(f, c, LK_TABLE, indent, 0, M_BODY, first, last, cx);
            i32 rows = 0;
            for (MdNode *r = c->first; r; r = r->next) {
                rows++;
            }
            l->n = rows;
            break;
        }
        case MD_CODE_BLOCK: {
            Decor *d = flat_decor(f, DK_CODE, c, indent);
            i32 di   = f->ndecors - 1;
            iz lines = code_lines(c->text);
            iz pos   = 0;
            for (iz at = 0; at < lines; at += M_CHUNK) {
                iz  cnt = min(lines - at, (iz)M_CHUNK);
                iz  e   = pos;
                for (iz k = 0; k < cnt && e < c->text.len; k++) {
                    while (e < c->text.len && c->text.data[e] != '\n') {
                        e++;
                    }
                    e += e < c->text.len;
                }
                b32   isfirst = at == 0;
                b32   islast  = at + cnt >= lines;
                Leaf *l = flat_leaf(f, c, LK_CODE, indent, 0, islast ? M_BODY : 0,
                                    first && isfirst, last && islast, cx);
                l->beg   = pos;
                l->end   = e;
                l->n     = (i32)cnt;
                l->decor = di;
                l->line  = c->line + !!(c->flags & MD_F_FENCED) + (i32)at;
                l->flags = (u8)((l->flags & LF_MUTED) | (isfirst ? LF_FIRST : 0) |
                                (islast ? LF_LAST : 0));
                if (!islast) {
                    l->mb = 0;
                }
                pos = e;
            }
            if (f->emit) {
                d->hi = f->nleaves;
            }
            (void)d;
            break;
        }
        case MD_QUOTE: {
            Decor *d = flat_decor(f, DK_QUOTE, c, indent);
            i32 di = f->ndecors - 1;
            i32 lo = f->nleaves;
            FlatCtx in = {0, 1, cx.depth};
            flat_blocks(f, c, indent + M_QUOTE, in);
            if (f->nleaves == lo) {
                flat_leaf(f, 0, LK_EMPTY, indent + M_QUOTE, 0, 0, 1, 1, in);
            }
            if (f->emit) {
                f->decors[di].hi = f->nleaves;
            }
            (void)d;
            flat_close(f, lo, M_BODY, last);
            break;
        }
        case MD_LIST: {
            i32 lo = f->nleaves;
            if (!first && cx.tight) {
                f->pend = max(f->pend, (f32)4);
            }
            b32 tight = !!(c->flags & MD_F_TIGHT);
            i32 k = 0;
            for (MdNode *it = c->first; it; it = it->next, k++) {
                if (it != c->first) {
                    f->pend = max(f->pend, tight ? (f32)4 : (f32)M_BODY);
                }
                flat_item(f, c, it, k, indent, cx);
            }
            flat_close(f, lo, M_BODY, last);
            break;
        }
        default:
            break;
        }
    }
}

// Take the document apart into leaves and decorations.
static void doc_flatten(void) {
    Flat f = {0};
    FlatCtx cx = {0};
    flat_blocks(&f, D.root, 0, cx);
    D.leaves  = new(D.mem, f.nleaves, Leaf);
    D.decors  = new(D.mem, f.ndecors, Decor);
    Flat g    = {D.leaves, D.decors, 0, 0, 0, 1};
    flat_blocks(&g, D.root, 0, cx);
    assert(g.nleaves == f.nleaves && g.ndecors == f.ndecors);
    D.nleaves = g.nleaves;
    D.ndecors = g.ndecors;
}


// Freeing

static void table_free_cells(Table *tb) {
    if (tb && tb->built) {
        for (i32 i = 0; i < tb->nrows*tb->ncols; i++) {
            para_free(tb->cell[i]);
            tb->cell[i] = 0;
        }
        tb->built = 0;
    }
}

static void leaf_free(Leaf *l) {
    para_free(l->p);
    l->p = 0;
    table_free_cells(l->tb);
}

// Free every paragraph and forget every height: sizes and colors changed.
static void doc_free_paragraphs(void) {
    for (i32 i = 0; i < D.nleaves; i++) {
        leaf_free(D.leaves + i);
        D.leaves[i].flags &= (u8)~LF_FAILED;
        D.leaves[i].hepoch = 0;
    }
    for (i32 i = 0; i < D.ndecors; i++) {
        para_free(D.decors[i].mk);
        D.decors[i].mk = 0;
    }
    D.epoch++;
}

// Free everything of the current document, platform objects included.
static void doc_clear(void) {
    for (i32 i = 0; i < D.nleaves; i++) {
        leaf_free(D.leaves + i);
    }
    for (i32 i = 0; i < D.ndecors; i++) {
        para_free(D.decors[i].mk);
    }
    for (i32 h = 0; h < 64; h++) {
        for (ImgEntry *e = D.imgs[h]; e; e = e->next) {
            if (e->im) {
                os_image_free(e->im);
            }
        }
    }
    Theme const *th = D.th;
    f32 zoom = D.zoom, winw = D.winw;
    u32 epoch = D.epoch + 1;
    memset(&D, 0, sizeof(D));
    D.th    = th;
    D.zoom  = zoom;
    D.winw  = winw;
    D.epoch = epoch;
}


// Positions

// The column for the window width at the current zoom.
static void doc_column(void) {
    f32 w = min((f32)M_PAGE_MAX*D.zoom, D.winw - 2*M_SIDE);
    D.colw = max(w, (f32)80);
    D.colx = max((D.winw - D.colw)/2, (f32)0);
}

// Set the window width, zoom and theme. Heights stay exact if the width and
// zoom did not change.
static void doc_configure(f32 winw, f32 zoom, Theme const *th) {
    f32 oldw = D.colw, oldz = D.zoom;
    b32 style = th != D.th || zoom != oldz;
    D.winw = winw;
    D.zoom = zoom;
    D.th   = th;
    doc_column();
    if (style) {
        doc_free_paragraphs();
    } else if (D.colw != oldw) {
        D.epoch++;
    }
}

static void leaf_geometry(Leaf *l) {
    f32 z   = D.zoom;
    f32 ind = min(l->indent*z, D.colw*0.6f);
    l->x = ind;
    l->w = max(D.colw - ind, (f32)8);
    l->padl = l->padt = l->padb = 0;
    if (l->kind == LK_CODE) {
        l->padl = M_CODE_PAD*z;
        l->padt = l->flags & LF_FIRST ? M_CODE_PAD*z : 0;
        l->padb = l->flags & LF_LAST ? M_CODE_PAD*z : 0;
    } else if (l->kind==LK_HEAD && l->level<=2) {
        l->padb = (0.3f*heading_size[l->level]*z + 1);
    }
}

static b32 near_f(f32 a, f32 b) {
    return a - b < 1e-6f && b - a < 1e-6f;
}

static f32 est_lines(iz chars, f32 size, f32 w) {
    f32 per = max(w/(size*0.5f), (f32)1);
    return max(fceil((f32)chars/per), (f32)1);
}

static f32 leaf_estimate(Leaf *l) {
    f32 z = D.zoom;
    f32 w = max(l->w - 2*l->padl, (f32)8);
    switch (l->kind) {
    case LK_PARA:
    case LK_HTML:
        return est_lines(l->node->text.len, M_BODY*z, w) * line_body*z;
    case LK_HEAD: {
        f32 s = heading_size[l->level]*z;
        return est_lines(l->node->text.len, s, w) * s*1.25f + l->padb;
    }
    case LK_CODE:
        return (f32)l->n * M_BODY*code_scale*1.45f*z + l->padt + l->padb;
    case LK_RULE:
        return 4*z;
    case LK_TABLE:
        return (f32)l->n * (line_body*z + 2*M_CELL_PADY*z + 1) + 1;
    default:
        return line_body*z;
    }
}

// Stack the leaves. Exact heights are kept, the rest estimated.
static void doc_relayout(void) {
    f32 z = D.zoom;
    f32 y = M_PAGE_PAD*z;
    f32 prev = 0;
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        leaf_geometry(l);
        if (i) {
            y += max(prev, l->mt*z);
        }
        l->y = y;
        if (l->hepoch != D.epoch) {
            l->h = leaf_estimate(l);
        }
        y   += l->h;
        prev = l->mb*z;
    }
    D.total = y + M_PAGE_PAD*z;
}

// The last leaf starting at or above y, or 0.
static i32 leaf_at(f32 y) {
    i32 lo = 0, hi = D.nleaves;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (D.leaves[mid].y <= y) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return max(lo - 1, 0);
}


// Making leaves

static Run base_run(f32 size, Color c) {
    Run r = {0};
    r.size  = size;
    r.color = c;
    return r;
}

// The code block's spans, highlighted when first needed and kept.
static HlSpans code_spans(Decor *d, Arena *scratch) {
    if (!d->hldone) {
        d->hldone = 1;
        MdNode *n = d->node;
        if (n->info.len && n->text.len) {
            Arena s0 = *scratch;    // free space: the spans are copied out
            HlSpans s = hl_highlight(&s0, md_lang(n), n->text);
            if (s.len) {
                d->hl.spans = new(D.mem, s.len, HlSpan);
                memcpy(d->hl.spans, s.spans, (uz)(s.len*(iz)sizeof(HlSpan)));
                d->hl.len = s.len;
            }
        }
    }
    return d->hl;
}

// Code text with tabs expanded to the next multiple of four columns.
static void rb_code(RB *rb, Str s, Run const *st) {
    while (s.len) {
        if (s.data[0] == '\t') {
            u8 sp[4] = {' ', ' ', ' ', ' '};
            iz n = 4 - rb->col%4;
            rb_text(rb, (Str){sp, n}, st);
            rb->col += n;
            s.data++;
            s.len--;
            continue;
        }
        iz e = 0;
        while (e<s.len && s.data[e]!='\t') {
            e++;
        }
        for (iz i = 0; i < e; i++) {
            rb->col = s.data[i]=='\n' ? 0 : rb->col + ((s.data[i]&0xc0) != 0x80);
        }
        rb_text(rb, (Str){s.data, e}, st);
        s.data += e;
        s.len  -= e;
    }
}

static void code_runs(RB *rb, Leaf *l) {
    f32     z  = D.zoom;
    Decor  *d  = D.decors + l->decor;
    HlSpans hs = code_spans(d, rb->a);
    Str  all   = d->node->text;
    iz   beg   = l->beg;
    iz   end   = l->end;
    if (end>beg && all.data[end-1]=='\n') {
        end--;
    }
    Run st = base_run(M_BODY*code_scale*z, D.th->text);
    st.font = FONT_MONOSPACE;
    // First span ending after beg.
    iz lo = 0, hi = hs.len;
    while (lo < hi) {
        iz mid = (lo + hi) / 2;
        if (hs.spans[mid].end <= beg) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    iz pos = beg;
    for (iz i = lo; i < hs.len && hs.spans[i].beg < end; i++) {
        HlSpan s = hs.spans[i];
        iz sb = max(s.beg, beg);
        iz se = min(s.end, end);
        if (se <= sb) {
            continue;
        }
        if (sb > pos) {
            rb_code(rb, (Str){all.data+pos, sb-pos}, &st);
        }
        Run c = st;
        c.color = hl_color(s.kind);
        rb_code(rb, (Str){all.data+sb, se-sb}, &c);
        pos = se;
    }
    if (end > pos) {
        rb_code(rb, (Str){all.data+pos, end-pos}, &st);
    }
}

// Collect the runs for a leaf that is one paragraph of text.
static void leaf_runs(RB *rb, Leaf *l) {
    f32   z  = D.zoom;
    Theme const *t = D.th;
    Color fg = l->flags & LF_MUTED ? t->muted : t->text;
    rb->avail = max(l->w - 2*l->padl, (f32)8);
    switch (l->kind) {
    case LK_HEAD: {
        Run st = base_run(heading_size[l->level]*z, l->level==6 ? t->muted : fg);
        st.bold = 1;
        inl_walk(rb, l->node, st);
        break;
    }
    case LK_HTML:
        html_runs(rb, l->node->text, base_run(M_BODY*z, fg));
        break;
    case LK_CODE:
        code_runs(rb, l);
        break;
    default:
        inl_walk(rb, l->node, base_run(M_BODY*z, fg));
        break;
    }
}

static f32 leaf_line(Leaf *l) {
    f32 z = D.zoom;
    switch (l->kind) {
    case LK_HEAD: return heading_size[l->level]*z*1.25f;
    case LK_CODE: return M_BODY*code_scale*1.45f*z;
    default:      return line_body*z;
    }
}

static void leaf_finish(Leaf *l, Paragraph *p, RB *rb) {
    l->p = p;
    if (!p) {
        l->flags |= LF_FAILED;
        l->h = leaf_estimate(l);
        return;
    }
    os_para_style(p, leaf_line(l), l->kind==LK_HTML || l->kind==LK_PARA ? rb->align : ALIGN_LEFT);
    l->flags = (u8)((l->flags & ~(LF_IMAGES|LF_EXTRAS)) | (rb->nboxes ? LF_IMAGES : 0) |
                    (rb->nspans || rb->nboxes ? LF_EXTRAS : 0));
    l->h = max(os_para_size(p).h, leaf_line(l)) + l->padt + l->padb;
}

static void para_make(Leaf *l, RB *rb) {
    if (l->p && !(l->flags & LF_IMAGES)) {
        // Only the width changed.
        os_para_set_width(l->p, max(l->w - 2*l->padl, (f32)8));
        l->h = max(os_para_size(l->p).h, leaf_line(l)) + l->padt + l->padb;
        return;
    }
    para_free(l->p);
    l->p = 0;
    rb_reset(rb);
    leaf_runs(rb, l);
    rb_finish(rb);
    Paragraph *p = para_new(rb->runs, rb->n, max(l->w - 2*l->padl, (f32)8));
    leaf_finish(l, p, rb);
}

static Align cell_align(u8 a) {
    return a==MD_ALIGN_CENTER ? ALIGN_CENTER : a==MD_ALIGN_RIGHT ? ALIGN_RIGHT : ALIGN_LEFT;
}

static void table_make(Leaf *l, RB *rb) {
    f32 z = D.zoom;
    MdNode *tn = l->node;
    Table *tb = l->tb;
    if (!tb) {
        i32 nr = l->n, nc = max(tn->ncols, (i32)1);
        tb = new(D.mem, 1, Table);
        tb->nrows = nr;
        tb->ncols = nc;
        tb->cell  = new(D.mem, (iz)nr*nc, Paragraph *);
        tb->colw  = new(D.mem, nc, f32);
        tb->colx  = new(D.mem, nc + 1, f32);
        tb->rowy  = new(D.mem, nr + 1, f32);
        l->tb = tb;
    }
    table_free_cells(tb);
    i32 nr = tb->nrows, nc = tb->ncols;
    f32 lh = line_body*z;
    f32 padx = min(M_CELL_PADX*z, max((l->w - (f32)(2*nc + 1))/(f32)(2*nc), (f32)0)), pady = M_CELL_PADY*z;
    tb->padx = padx;
    Color fg = l->flags & LF_MUTED ? D.th->muted : D.th->text;
    for (i32 c = 0; c < nc; c++) {
        tb->colw[c] = 0;
    }
    l->flags &= (u8)~(LF_IMAGES|LF_EXTRAS);
    // Natural widths.
    f32 avail = l->w - (f32)(nc + 1) - 2*padx*(f32)nc;
    rb->avail = max(avail/(f32)nc, (f32)8);
    i32 r = 0;
    for (MdNode *row = tn->first; row && r < nr; row = row->next, r++) {
        i32 c = 0;
        for (MdNode *cell = row->first; cell && c < nc; cell = cell->next, c++) {
            rb_reset(rb);
            Run st = base_run(M_BODY*z, fg);
            st.bold = !!(row->flags & MD_F_HEADER);
            inl_walk(rb, cell, st);
            rb_finish(rb);
            if (rb->n) {
                Paragraph *p = para_new(rb->runs, rb->n, 0);
                tb->cell[(iz)r*nc + c] = p;
                if (p) {
                    os_para_style(p, lh, ALIGN_LEFT);
                    tb->colw[c] = max(tb->colw[c], os_para_size(p).w);
                }
            }
            l->flags |= (u8)((rb->nboxes ? LF_IMAGES : 0) | (rb->nspans || rb->nboxes ? LF_EXTRAS : 0));
        }
    }
    tb->built = 1;
    // Column widths: natural, or shrunk to fit.
    f32 sum = 0;
    for (i32 c = 0; c < nc; c++) {
        tb->colw[c] = fceil(tb->colw[c]) + 1;
        sum += tb->colw[c];
    }
    if (sum > avail) {
        // Shrink in proportion, but not below a minimum: columns that would
        // fall under it stay there and the others share what is left.
        f32 minw  = min(24*z, max(avail/(f32)nc, (f32)1));
        f32 scale = avail/sum;
        for (i32 pass = 0; pass < nc; pass++) {
            f32 pinned = 0, flex = 0;
            for (i32 c = 0; c < nc; c++) {
                if (tb->colw[c]*scale < minw) {
                    pinned += minw;
                } else {
                    flex += tb->colw[c];
                }
            }
            f32 next = flex > 0 ? (avail - pinned)/flex : scale;
            if (near_f(next, scale)) {
                break;
            }
            scale = next;
        }
        for (i32 c = 0; c < nc; c++) {
            tb->colw[c] = max(tb->colw[c]*scale, minw);
        }
    }
    tb->colx[0] = 0;
    for (i32 c = 0; c < nc; c++) {
        tb->colx[c+1] = tb->colx[c] + 1 + 2*padx + tb->colw[c];
    }
    // Wrap to the columns and measure the rows.
    tb->rowy[0] = 0;
    r = 0;
    for (MdNode *row = tn->first; row && r < nr; row = row->next, r++) {
        f32 rh = lh;
        i32 c  = 0;
        for (MdNode *cell = row->first; cell && c < nc; cell = cell->next, c++) {
            Paragraph *p = tb->cell[(iz)r*nc + c];
            if (p) {
                os_para_set_width(p, tb->colw[c]);
                os_para_style(p, lh, cell_align(cell->align));
                rh = max(rh, os_para_size(p).h);
            }
        }
        tb->rowy[r+1] = tb->rowy[r] + 1 + rh + 2*pady;
    }
    for (; r < nr; r++) {
        tb->rowy[r+1] = tb->rowy[r] + 1 + lh + 2*pady;
    }
    l->h = tb->rowy[nr] + 1;
}

static b32 leaf_ready(Leaf *l) {
    if (l->hepoch != D.epoch) {
        return 0;
    }
    switch (l->kind) {
    case LK_RULE:
    case LK_EMPTY:
        return 1;
    case LK_TABLE:
        return l->tb && l->tb->built;
    default:
        return l->p || (l->flags & LF_FAILED);
    }
}

static void leaf_make(Leaf *l, RB *rb) {
    switch (l->kind) {
    case LK_RULE:
        l->h = 4*D.zoom;
        break;
    case LK_EMPTY:
        l->h = line_body*D.zoom;
        break;
    case LK_TABLE:
        table_make(l, rb);
        break;
    default:
        para_make(l, rb);
        break;
    }
    l->hepoch = D.epoch;
}


// Reading position

// Where positions in a leaf are measured from. The first leaf includes the
// space above it, so that the top of the document is position 0.
static f32 anchor_base(i32 leaf) {
    return leaf ? D.leaves[leaf].y : 0;
}

static Anchor doc_anchor(f32 scroll) {
    Anchor a = {0};
    if (D.nleaves) {
        a.leaf = leaf_at(scroll);
        a.off  = scroll - anchor_base(a.leaf);
        a.line = D.leaves[a.leaf].line;
    }
    return a;
}

static f32 doc_anchor_scroll(Anchor a) {
    if (!D.nleaves) {
        return 0;
    }
    return anchor_base(clamp32(a.leaf, 0, D.nleaves-1)) + a.off;
}

// The anchor for a position in a document parsed again: the last leaf that
// begins at or before that source line.
static Anchor doc_anchor_for_line(i32 line, f32 off) {
    Anchor a = {0, off, line};
    i32 lo = 0, hi = D.nleaves;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (D.leaves[mid].line <= line) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    a.leaf = max(lo - 1, 0);
    return a;
}

// The leaf of the heading with this anchor name, or -1.
static i32 doc_find_slug(Str slug) {
    for (i32 i = 0; i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        if (l->kind==LK_HEAD && equals(l->node->dest, slug)) {
            return i;
        }
    }
    return -1;
}

// Lay out what the window shows. *scroll may change: leaves above the
// reading position change height when they are made, and the position stays
// with its leaf. The position is the leaf under *scroll, or pin if pinned
// (jumps, resizes), or the end of the document if pin_end. Returns the leaf
// range on screen through lo and hi.
static void doc_ensure(RB *rb, f32 *scroll, f32 viewh, Anchor const *pin,
                       b32 pin_end) {
    Anchor a = pin ? *pin : doc_anchor(*scroll);
    f32 over = 100*D.zoom;
    for (i32 iter = 0; iter < 6; iter++) {
        doc_relayout();
        f32 maxs = max(D.total - viewh, (f32)0);
        f32 s    = pin_end ? maxs : clampf(doc_anchor_scroll(a), 0, maxs);
        *scroll  = s;
        if (!D.nleaves) {
            return;
        }
        i32 lo = leaf_at(s - over);
        i32 hi = leaf_at(s + viewh + over);
        b32 changed = 0;
        for (i32 i = lo; i <= hi; i++) {
            Leaf *l = D.leaves + i;
            if (!leaf_ready(l)) {
                leaf_make(l, rb);
                changed = 1;
            }
        }
        if (!changed) {
            return;
        }
    }
    doc_relayout();
    f32 maxs = max(D.total - viewh, (f32)0);
    *scroll  = pin_end ? maxs : clampf(doc_anchor_scroll(a), 0, maxs);
}

// Build every leaf: for tests, and for measuring.
static void doc_make_all(RB *rb) {
    doc_relayout();
    for (i32 i = 0; i < D.nleaves; i++) {
        if (!leaf_ready(D.leaves + i)) {
            leaf_make(D.leaves + i, rb);
        }
    }
    doc_relayout();
}

// Free the paragraphs far from the window, once there are many.
static void doc_evict(f32 scroll, f32 viewh) {
    if (D.nlive <= M_MAX_LIVE) {
        return;
    }
    f32 keep = viewh*3;
    for (i32 i = 0; i < D.nleaves && D.nlive > M_MAX_LIVE/2; i++) {
        Leaf *l = D.leaves + i;
        if (l->y + l->h < scroll - keep || l->y > scroll + viewh + keep) {
            leaf_free(l);
        }
    }
    for (i32 i = 0; i < D.ndecors; i++) {
        Decor *d = D.decors + i;
        if (d->mk && (d->lo >= D.nleaves ||
                      D.leaves[d->lo].y + D.leaves[d->lo].h < scroll - keep ||
                      D.leaves[d->lo].y > scroll + viewh + keep)) {
            para_free(d->mk);
            d->mk = 0;
        }
    }
}


// Painting

static void draw_spans(RB *rb, Paragraph *p, Point o) {
    f32 z = D.zoom;
    Rect r[8];
    for (iz i = 0; i < rb->nspans; i++) {
        Span s = rb->spans[i];
        iz n = os_para_rects(p, s.beg, s.end, r, 8);
        for (iz k = 0; k<n && k<8; k++) {
            Rect q = {o.x + r[k].x, o.y + r[k].y, r[k].w, r[k].h};
            if (s.kind == SP_CODE) {
                f32 h = min(q.h, M_BODY*code_scale*1.3f*z);
                q = (Rect){q.x - 1, q.y + (q.h - h)/2, q.w + 2, h};
                os_fill_rrect(q, 3*z, D.th->inline_bg);
            } else {
                q = (Rect){q.x - 3, q.y + 1, q.w + 6, q.h - 2};
                os_stroke_rrect(q, 3*z, 1, D.th->border);
            }
        }
    }
}

static void draw_boxes(RB *rb, Paragraph *p, Point o) {
    for (iz i = 0; i < rb->nboxes; i++) {
        // The platform says where the box is horizontally and on which line;
        // its size is known, and it stands on the baseline.
        ImgBox *bx = rb->boxes + i;
        Rect r = {0};
        if (os_para_rects(p, bx->off, bx->off + 3, &r, 1) >= 1) {
            f32 base = r.y + r.h;
            for (iz k = 0; k < os_para_lines(p); k++) {
                LineMetrics m = os_para_line(p, k);
                if (m.beg <= bx->off && bx->off < m.end) {
                    base = m.top + m.baseline;
                    break;
                }
            }
            ImgEntry *e = bx->img;
            e->drawn = 1;
            if (e->frames > 1) {
                D.anim = 1;
            }
            os_image_draw(e->im, e->frame, (Rect){o.x + r.x, o.y + base - bx->h, bx->w, bx->h});
        }
    }
}

static void draw_table(Leaf *l, f32 x, f32 y, RB *rb) {
    f32 z = D.zoom;
    Table *tb = l->tb;
    if (!tb || !tb->built) {
        return;
    }
    Theme const *t = D.th;
    f32 w = tb->colx[tb->ncols] + 1;
    f32 padx = tb->padx, pady = M_CELL_PADY*z;
    // Alternate rows, then the grid, then the text.
    for (i32 r = 2; r < tb->nrows; r += 2) {
        os_fill_rect((Rect){x, y + tb->rowy[r], w, tb->rowy[r+1] - tb->rowy[r]}, t->alt_row);
    }
    f32 h = tb->rowy[tb->nrows] + 1;
    for (i32 r = 0; r <= tb->nrows; r++) {
        os_fill_rect((Rect){x, y + tb->rowy[r], w, 1}, t->border);
    }
    for (i32 c = 0; c <= tb->ncols; c++) {
        os_fill_rect((Rect){x + tb->colx[c], y, 1, h}, t->border);
    }
    i32 r = 0;
    for (MdNode *row = l->node->first; row && r < tb->nrows; row = row->next, r++) {
        i32 c = 0;
        for (MdNode *cell = row->first; cell && c < tb->ncols; cell = cell->next, c++) {
            Paragraph *p = tb->cell[(iz)r*tb->ncols + c];
            if (!p) {
                continue;
            }
            Point o = {x + tb->colx[c] + 1 + padx, y + tb->rowy[r] + 1 + pady};
            if (l->flags & LF_EXTRAS) {
                rb_reset(rb);
                rb->avail = max(l->w/(f32)tb->ncols, (f32)8);
                Run st = base_run(M_BODY*z, t->text);
                st.bold = !!(row->flags & MD_F_HEADER);
                inl_walk(rb, cell, st);
                draw_spans(rb, p, o);
                os_para_draw(p, o);
                draw_boxes(rb, p, o);
            } else {
                os_para_draw(p, o);
            }
        }
    }
}

// Where the first line of a leaf's text has its baseline, and its height.
static f32 leaf_baseline(Leaf *l, f32 *lh) {
    *lh = line_body*D.zoom;
    if (l->p && os_para_lines(l->p) > 0) {
        LineMetrics m = os_para_line(l->p, 0);
        *lh = m.height;
        return l->y + l->padt + m.top + m.baseline;
    }
    return l->y + l->padt + *lh*0.7f;
}

static void draw_item(Decor *d, f32 scroll, f32 viewh) {
    if (d->lo >= D.nleaves) {
        return;
    }
    f32 z  = D.zoom;
    Leaf *l = D.leaves + d->lo;
    if (l->y - scroll > viewh || l->y + min(l->h, (f32)60*z) - scroll < 0) {
        return;
    }
    Theme const *t = D.th;
    f32 lh;
    f32 base = leaf_baseline(l, &lh) - scroll;
    f32 cl   = D.colx + (d->x + M_LIST)*z;      // where the item's content starts
    if (d->task) {
        f32 s  = 14*z;
        Rect r = {cl - 24*z, base - s + 2*z, s, s};
        if (d->task == MD_TASK_DONE) {
            os_fill_rrect(r, 3*z, t->accent);
            Point a = {r.x + 3.5f*z, r.y + 7.5f*z};
            Point b = {r.x + 6*z, r.y + 10*z};
            Point c = {r.x + 10.5f*z, r.y + 4.5f*z};
            os_stroke_line(a, b, 1.6f*z, rgb(255, 255, 255));
            os_stroke_line(b, c, 1.6f*z, rgb(255, 255, 255));
        } else {
            os_stroke_rrect((Rect){r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, 3*z, 1, t->muted);
        }
        return;
    }
    if (!d->ordered) {
        f32 cx = cl - 14*z;
        f32 cy = base - 5.5f*z;
        f32 rad = 3*z;
        if (d->bullet == 0) {
            os_fill_rrect((Rect){cx - rad, cy - rad, 2*rad, 2*rad}, rad, t->text);
        } else if (d->bullet == 1) {
            os_stroke_rrect((Rect){cx - rad + 0.5f, cy - rad + 0.5f, 2*rad - 1, 2*rad - 1}, rad, 1, t->text);
        } else {
            os_fill_rect((Rect){cx - rad + 0.5f, cy - rad + 0.5f, 2*rad - 1, 2*rad - 1}, t->text);
        }
        return;
    }
    if (!d->mk) {
        u8 buf[32];
        iz n = fmt_int(buf, d->num);
        buf[n++] = d->delim == ')' ? ')' : '.';
        Run r = base_run(M_BODY*z, l->flags & LF_MUTED ? t->muted : t->text);
        r.text = (Str){buf, n};
        d->mk  = para_new(&r, 1, 0);
        if (d->mk) {
            os_para_style(d->mk, line_body*z, ALIGN_LEFT);
        }
    }
    if (d->mk) {
        Size s = os_para_size(d->mk);
        f32 mb = 0;
        if (os_para_lines(d->mk) > 0) {
            LineMetrics m = os_para_line(d->mk, 0);
            mb = m.top + m.baseline;
        }
        os_para_draw(d->mk, (Point){cl - 6*z - s.w, base - mb});
    }
}

static void doc_paint(f32 scroll, Size size, RB *rb) {
    f32 z  = D.zoom;
    Theme const *t = D.th;
    D.anim = 0;
    for (i32 h = 0; h < 64; h++) {
        for (ImgEntry *e = D.imgs[h]; e; e = e->next) {
            e->drawn = 0;
        }
    }
    if (!D.nleaves) {
        return;
    }
    i32 lo = leaf_at(scroll);
    i32 hi = leaf_at(scroll + size.h);
    // Decorations behind the leaves.
    for (i32 i = 0; i < D.ndecors; i++) {
        Decor *d = D.decors + i;
        if (d->hi <= lo || d->lo > hi || d->lo >= d->hi) {
            continue;
        }
        Leaf *a = D.leaves + d->lo;
        Leaf *b = D.leaves + d->hi - 1;
        f32 top = a->y - scroll;
        f32 bot = b->y + b->h - scroll;
        if (d->kind == DK_QUOTE) {
            os_fill_rect((Rect){D.colx + d->x*z, top, 4*z, bot - top}, t->quote_bar);
        } else if (d->kind == DK_CODE) {
            os_fill_rrect((Rect){D.colx + a->x, top, a->w, bot - top}, 6*z, t->code_bg);
        }
    }
    for (i32 i = lo; i <= hi && i < D.nleaves; i++) {
        Leaf *l = D.leaves + i;
        f32 y = l->y - scroll;
        if (y > size.h || y + l->h < 0) {
            continue;
        }
        f32 x = D.colx + l->x;
        switch (l->kind) {
        case LK_RULE:
            os_fill_rect((Rect){x, y, l->w, 4*z}, t->rule);
            break;
        case LK_TABLE:
            draw_table(l, x, y, rb);
            break;
        case LK_EMPTY:
            break;
        default:
            if (l->p) {
                Point o = {x + l->padl, y + l->padt};
                if (l->flags & LF_EXTRAS) {
                    rb_reset(rb);
                    leaf_runs(rb, l);
                    draw_spans(rb, l->p, o);
                    os_para_draw(l->p, o);
                    draw_boxes(rb, l->p, o);
                } else {
                    os_para_draw(l->p, o);
                }
            }
            if (l->kind==LK_HEAD && l->level<=2) {
                os_fill_rect((Rect){x, y + l->h - 1, l->w, 1}, t->rule);
            }
            break;
        }
    }
    for (i32 i = 0; i < D.ndecors; i++) {
        Decor *d = D.decors + i;
        if (d->kind==DK_ITEM && d->hi > lo && d->lo <= hi) {
            draw_item(d, scroll, size.h);
        }
    }
}


// Hit testing

typedef struct {
    i32        leaf;        // -1 if the point is in no leaf
    i32        cell;        // table: row*ncols + column, else -1
    Paragraph *p;           // the paragraph under the point, or 0
    Point      origin;      // where p's top left is, in window coordinates
    TextHit    th;          // th.offset is a byte offset into p
} Hit;

// p is in window coordinates for the given scroll.
static Hit doc_hit(Point p, f32 scroll) {
    Hit h = {0};
    h.leaf = h.cell = -1;
    if (!D.nleaves) {
        return h;
    }
    f32 dy = p.y + scroll;
    i32 i  = leaf_at(dy);
    Leaf *l = D.leaves + i;
    if (dy < l->y || dy >= l->y + l->h) {
        return h;
    }
    f32 x = D.colx + l->x;
    if (p.x < x || p.x >= x + l->w) {
        return h;
    }
    h.leaf = i;
    f32 y  = l->y - scroll;
    if (l->kind == LK_TABLE) {
        Table *tb = l->tb;
        if (!tb || !tb->built) {
            return h;
        }
        f32 lx = p.x - x, ly = p.y - y;
        i32 c = -1, r = -1;
        for (i32 k = 0; k < tb->ncols; k++) {
            c = lx >= tb->colx[k] ? k : c;
        }
        for (i32 k = 0; k < tb->nrows; k++) {
            r = ly >= tb->rowy[k] ? k : r;
        }
        if (c < 0 || r < 0 || lx >= tb->colx[tb->ncols]) {
            return h;
        }
        h.cell = r*tb->ncols + c;
        h.p    = tb->cell[h.cell];
        if (h.p) {
            f32 z = D.zoom;
            h.origin = (Point){x + tb->colx[c] + 1 + tb->padx, y + tb->rowy[r] + 1 + M_CELL_PADY*z};
        }
    } else if (l->p) {
        h.p      = l->p;
        h.origin = (Point){x + l->padl, y + l->padt};
    }
    if (h.p) {
        h.th = os_para_hit(h.p, (Point){p.x - h.origin.x, p.y - h.origin.y});
    }
    return h;
}

// The destination of link number k in the leaf's text (see Run.link), or an
// empty Str.
static Str doc_link(Leaf *l, i32 cell, i32 k, RB *rb) {
    MdNode *n = l->node;
    if (l->kind == LK_TABLE) {
        i32 nc = l->tb->ncols;
        MdNode *row = n->first;
        for (i32 r = cell/nc; row && r > 0; r--) {
            row = row->next;
        }
        MdNode *c = row ? row->first : 0;
        for (i32 i = cell%nc; c && i > 0; i--) {
            c = c->next;
        }
        if (!c) {
            return (Str){0};
        }
        n = c;
    }
    rb_reset(rb);
    rb->find  = k;
    rb->avail = 100;
    if (l->kind == LK_HTML) {
        html_runs(rb, n->text, base_run(M_BODY, D.th->text));
    } else {
        inl_walk(rb, n, base_run(M_BODY, D.th->text));
    }
    return rb->found;
}
