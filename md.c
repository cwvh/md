// Markdown parser: CommonMark 0.31.2 plus the GitHub extensions (tables,
// strikethrough, task list items and extended autolinks). Portable: only
// base.c is needed, no platform calls.
//
//   MdNode *doc = md_parse(&perm, scratch, source, MD_GFM);
//
// The result is a tree of MdNode in perm. The caller decides how much memory
// to give the two arenas. Roughly, for an input of N bytes:
//
//   perm     ~ 5 N typical (prose, code, lists), up to ~ 130 N for hostile
//              input made only of tiny nodes (`a|b` rows, `*a*a*a`, ...)
//   scratch  ~ 1 N typical, up to ~ 35 N hostile (`[a[a[a`, `*a*a*a`)
//
// plus 32 KB each for the first chunk, which is the floor for tiny
// inputs, and up to 1 MB of unused chunk tail that is not given back in
// debug builds (it is in release builds). Scratch is passed
// by value and is dead once md_parse returns, so the caller may reuse it.
// When either arena is too small md_parse returns 0 and leaves perm
// untouched, so out-of-memory is an error rather than a crash. Strs in the
// tree point into the source (which must outlive the tree) or into perm.
//
// Nesting limits, past which the construct is parsed as plain text:
//   MD_MAX_DEPTH    100  open containers (block quotes, lists, list items)
//   MD_MAX_INLINE    32  nested emphasis, strikethrough, links and images
//
// Parsing is linear in the input size. The usual quadratic traps (unclosed
// brackets, backticks, tags, comments, emphasis runs) are defeated by
// remembering failed scans.

#include "mdtables.c"


// The tree

typedef enum {
    // Blocks
    MD_DOCUMENT,    // root; children are blocks
    MD_QUOTE,       // block quote; children are blocks
    MD_LIST,        // children are MD_ITEM. flags MD_F_ORDERED, MD_F_TIGHT;
                    //   start: first number; delim: bullet char or '.' / ')'
    MD_ITEM,        // list item; children are blocks. task: MD_TASK_*
    MD_PARAGRAPH,   // children are inlines
    MD_HEADING,     // level 1-6; children are inlines. After md_slugify, dest
                    //   holds the anchor slug without the '#'
    MD_RULE,        // thematic break
    MD_CODE_BLOCK,  // text: contents (each line ends in '\n'). info: info
                    //   string, flags MD_F_FENCED. See md_lang
    MD_HTML_BLOCK,  // text: raw HTML, without the final newline
    MD_TABLE,       // children are MD_ROW; ncols: columns in the header row
    MD_ROW,         // flags MD_F_HEADER for the header row; children are
                    //   MD_CELL. A body row may have fewer than ncols cells
                    //   (the rest are empty) but never more
    MD_CELL,        // align: MD_ALIGN_*; children are inlines
    // Inlines
    MD_TEXT,        // text: never empty. Adjacent texts are merged
    MD_SOFTBREAK,
    MD_HARDBREAK,
    MD_CODE,        // code span; text: the code, line endings as spaces
    MD_EMPH,        // children are inlines
    MD_STRONG,
    MD_STRIKE,
    MD_LINK,        // dest, title (decoded, not percent-encoded); children are
                    //   the link text. MD_F_AUTOLINK: <url>, or an extended
                    //   autolink such as www.example.com (dest has the scheme)
    MD_IMAGE,       // dest, title; children are the alt text
    MD_HTML_INLINE, // text: one raw tag, comment, declaration or CDATA section
    MD_KIND_COUNT,
} MdKind;

enum { MD_ALIGN_NONE, MD_ALIGN_LEFT, MD_ALIGN_CENTER, MD_ALIGN_RIGHT };
enum { MD_TASK_NONE, MD_TASK_OPEN, MD_TASK_DONE };

enum {
    MD_F_ORDERED  = 1<<0,   // list
    MD_F_TIGHT    = 1<<1,   // list
    MD_F_FENCED   = 1<<2,   // code block
    MD_F_HEADER   = 1<<3,   // table row
    MD_F_AUTOLINK = 1<<4,   // link
    MD_F_BLANK    = 1<<5,   // internal: block ended with a blank line
};

enum {
    MD_MAX_DEPTH  = 100,
    MD_MAX_INLINE = 32,
};

// Extensions, for md_parse. Without any, the parser is plain CommonMark.
enum {
    MD_EXT_TABLES   = 1<<0,
    MD_EXT_STRIKE   = 1<<1,     // ~one~ and ~~two~~ tildes
    MD_EXT_AUTOLINK = 1<<2,     // www.example.com, https://..., a@b.cd
    MD_EXT_TASKS    = 1<<3,     // - [ ] and - [x]
    MD_GFM          = 15,
};

// Children are a doubly linked list: parent->first .. parent->last, via
// next and prev. A walk needs no recursion: see md_next.
typedef struct MdNode MdNode;
struct MdNode {
    MdNode *parent, *first, *last, *next, *prev;
    Str     text;       // see MdKind
    union {
        struct { Str dest, title; };    // link, image
        Str  info;                      // code block
        struct { i32 start, ncols; };   // list, table
    };
    i32 line;           // block nodes: 1-based first source line, else 0
    u8  kind;           // MdKind
    u8  level;          // heading
    u8  align;          // cell
    u8  task;           // list item
    u8  flags;          // MD_F_*
    u8  delim;          // list
    u8  depth;          // internal: inline nesting depth
    u8  pad;
};

// Next node in a depth-first walk below root, skipping nothing, or 0.
static MdNode *md_next(MdNode *n, MdNode *root) {
    if (n->first) {
        return n->first;
    }
    while (n != root) {
        if (n->next) {
            return n->next;
        }
        n = n->parent;
    }
    return 0;
}

// First word of a code block's info string, the language.
static Str md_lang(MdNode *n) {
    Str s = n->info;
    iz  i = 0;
    while (i<s.len && s.data[i]!=' ' && s.data[i]!='\t') {
        i++;
    }
    return (Str){s.data, i};
}


// Memory
//
// Parsing makes millions of tiny allocations and the base arena is built for
// few large ones (in debug builds every allocation costs two guard pages), so
// parsing carves its own bump allocations out of big chunks. Scratch memory
// can be rewound to a mark; chunks stay linked and are reused.

typedef struct MdChunk MdChunk;
struct MdChunk {
    MdChunk *next;
    iz       size;      // bytes, header included
    iz       hw;        // bytes at or past this offset are still zero
};

enum {
    MD_HDR       = 32,
    MD_CHUNK_MIN = 1<<15,
    MD_CHUNK_MAX = 1<<20,
#ifdef DEBUG
    MD_SLACK     = 3*PAGE + 64,     // alignment, and a guard page behind it
#else
    MD_SLACK     = 64,
#endif
};

typedef struct {
    Arena     arena;
    MdChunk  *first, *cur;
    iz        off;      // next free offset in cur
    iz        next;     // size of the next new chunk
    void    **oom;      // __builtin_longjmp target
} MdMem;

typedef struct {
    MdChunk *cur;
    iz       off;
} MdMark;

static _Noreturn void md_oom(MdMem *m) {
    __builtin_longjmp(m->oom, 1);
}

static MdChunk *md_newchunk(MdMem *m, iz need) {
    iz size = max(m->next, need + MD_HDR);
    size    = (size + 15) & ~(iz)15;
    if (size > m->arena.end - m->arena.beg - MD_SLACK) {
        md_oom(m);
    }
    m->next = min(m->next*2, (iz)MD_CHUNK_MAX);
    MdChunk *c = alloc(&m->arena, size, 1, 16);
    c->size    = size;
    c->hw      = MD_HDR;
    return c;
}

// Zeroed, aligned to align (a power of two up to 16).
static void *md_alloc(MdMem *m, iz size, iz align) {
    assert(size >= 0);
    for (;;) {
        MdChunk *c = m->cur;
        if (c) {
            iz off = (m->off + align - 1) & ~(align - 1);
            if (size <= c->size - off) {
                u8 *r = (u8 *)c + off;
                iz  z = min(off + size, c->hw) - off;
                if (z > 0) {
                    memset(r, 0, (uz)z);
                }
                c->hw  = max(c->hw, off + size);
                m->off = off + size;
                return r;
            }
        }
        MdChunk *n = c ? c->next : m->first;
        if (!n) {
            n = md_newchunk(m, size + 16);
            if (c) {
                c->next = n;
            } else {
                m->first = n;
            }
        }
        m->cur = n;
        m->off = MD_HDR;
    }
}
#define md_new(m, t, n) md_alloc(m, (n)*(iz)sizeof(t), (iz)_Alignof(t))

static MdMark md_mark(MdMem *m) {
    return (MdMark){m->cur, m->off};
}

static void md_reset(MdMem *m, MdMark k) {
    m->cur = k.cur;
    m->off = k.off;
}

// Give the unused tail of the last chunk back. Not in debug builds, where
// the guard page behind an allocation must stay where it is.
static void md_trim(MdMem *m) {
#ifndef DEBUG
    MdChunk *c = m->cur;
    if (c && (u8 *)c + c->size == m->arena.beg) {
        iz used  = (m->off + 15) & ~(iz)15;
        m->arena.beg -= c->size - used;
        c->size = used;
    }
#else
    (void)m;
#endif
}


// Character classes

// Unicode whitespace: Zs, tab, line feed, form feed, carriage return.
static b32 md_isuspace(i32 c) {
    return c==' ' || c=='\t' || c=='\n' || c=='\f' || c=='\r' ||
           c==0xa0 || c==0x1680 || (c>=0x2000 && c<=0x200a) ||
           c==0x202f || c==0x205f || c==0x3000;
}

// Unicode punctuation: general categories P* and S*.
static b32 md_isupunct(i32 c) {
    if (c < 0x80) {
        return (c>='!' && c<='/') || (c>=':' && c<='@') ||
               (c>='[' && c<='`') || (c>='{' && c<='~');
    }
    iz lo = 0;
    iz hi = countof(md_punct);
    while (lo < hi) {
        iz  mid   = (lo + hi) / 2;
        u32 v     = md_punct[mid];
        i32 first = (i32)(v >> 11);
        i32 last  = first + (i32)(v & 0x7ff);
        if (c < first) {
            hi = mid;
        } else if (c > last) {
            lo = mid + 1;
        } else {
            return 1;
        }
    }
    return 0;
}

static b32 md_ispunct(i32 c) {
    return c>0 && c<0x80 && md_isupunct(c);
}

static b32 md_isalpha(i32 c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z');
}

static b32 md_isdigit(i32 c) {
    return c>='0' && c<='9';
}

static b32 md_isalnum(i32 c) {
    return md_isalpha(c) || md_isdigit(c);
}

static b32 md_isxdigit(i32 c) {
    return md_isdigit(c) || (c>='a' && c<='f') || (c>='A' && c<='F');
}

static b32 md_isblank(i32 c) {
    return c==' ' || c=='\t';
}

// Space, tab, newline, vertical tab, form feed, carriage return.
static b32 md_isspace(i32 c) {
    return c==' ' || (c>=9 && c<=13);
}

static i32 md_peek(Str s, iz i) {
    return i>=0 && i<s.len ? s.data[i] : -1;
}

static Str md_sub(Str s, iz beg, iz end) {
    assert(beg>=0 && beg<=end && end<=s.len);
    return (Str){s.data + beg, end - beg};
}

static Str md_rtrim(Str s) {
    while (s.len && md_isspace(s.data[s.len-1])) {
        s.len--;
    }
    return s;
}

static b32 md_blank(Str s) {
    for (iz i = 0; i < s.len; i++) {
        if (!md_isspace(s.data[i])) {
            return 0;
        }
    }
    return 1;
}

// Code point at s[i] and its byte length.
static i32 md_cp(Str s, iz i, iz *n) {
    Utf8 r = utf8decode(md_sub(s, i, s.len));
    *n = s.len - i - r.tail.len;
    return r.rune;
}

// Code point ending just before s[i], or '\n' at the start.
static i32 md_cp_before(Str s, iz i) {
    if (i <= 0) {
        return '\n';
    }
    iz j = i - 1;
    while (j>0 && i-j<4 && (s.data[j]&0xc0)==0x80) {
        j--;
    }
    iz n;
    i32 c = md_cp(s, j, &n);
    return j+n==i ? c : 0xfffd;
}

static i32 md_cp_after(Str s, iz i) {
    if (i >= s.len) {
        return '\n';
    }
    iz n;
    return md_cp(s, i, &n);
}

// Case folding for reference labels: ASCII, Latin-1, Latin Extended-A and
// Additional, Greek, Cyrillic, Armenian and fullwidth Latin. Writes one to
// three code points. Other scripts compare case-sensitively.
static i32 md_fold(i32 c, i32 *o) {
    o[0] = c;
    if (c < 0x80) {
        if (c>='A' && c<='Z') {
            o[0] = c + 32;
        }
    } else if (c < 0x100) {
        if (c == 0xb5) {
            o[0] = 0x3bc;
        } else if (c == 0xdf) {
            o[0] = 's'; o[1] = 's';
            return 2;
        } else if (c>=0xc0 && c<=0xde && c!=0xd7) {
            o[0] = c + 32;
        }
    } else if (c < 0x180) {
        if (c == 0x130) {
            o[0] = 'i'; o[1] = 0x307;
            return 2;
        } else if (c == 0x149) {
            o[0] = 0x2bc; o[1] = 'n';
            return 2;
        } else if (c == 0x178) {
            o[0] = 0xff;
        } else if (c == 0x17f) {
            o[0] = 's';
        } else if ((c>=0x100 && c<=0x137) || (c>=0x14a && c<=0x177)) {
            o[0] = c | 1;
        } else if (((c>=0x139 && c<=0x148) || (c>=0x179 && c<=0x17e)) && (c&1)) {
            o[0] = c + 1;
        }
    } else if (c>=0x370 && c<0x400) {
        if (c == 0x386) {
            o[0] = 0x3ac;
        } else if (c>=0x388 && c<=0x38a) {
            o[0] = c + 37;
        } else if (c == 0x38c) {
            o[0] = 0x3cc;
        } else if (c==0x38e || c==0x38f) {
            o[0] = c + 63;
        } else if ((c>=0x391 && c<=0x3a1) || (c>=0x3a3 && c<=0x3ab)) {
            o[0] = c + 32;
        } else if (c == 0x3c2) {
            o[0] = 0x3c3;
        } else if (c>=0x3da && c<=0x3ef) {
            o[0] = c | 1;
        }
    } else if (c>=0x400 && c<0x530) {
        if (c <= 0x40f) {
            o[0] = c + 80;
        } else if (c <= 0x42f) {
            o[0] = c + 32;
        } else if ((c>=0x460 && c<=0x481) || (c>=0x48a && c<=0x4bf) || (c>=0x4d0 && c<=0x52f)) {
            o[0] = c | 1;
        } else if (c == 0x4c0) {
            o[0] = 0x4cf;
        } else if (c>=0x4c1 && c<=0x4ce && (c&1)) {
            o[0] = c + 1;
        }
    } else if (c>=0x531 && c<=0x556) {
        o[0] = c + 48;
    } else if (c>=0x1e00 && c<=0x1eff) {
        if (c == 0x1e9e) {
            o[0] = 's'; o[1] = 's';
            return 2;
        } else if ((c>=0x1e00 && c<=0x1e95) || (c>=0x1ea0 && c<=0x1eff)) {
            o[0] = c | 1;
        }
    } else if (c>=0xff21 && c<=0xff3a) {
        o[0] = c + 32;
    }
    return 1;
}



// Parser state

// A run of lines of a leaf block's content, kept apart from the source only
// when the lines are not contiguous in it (a block quote's "> " or a list
// item's indent sit in between). pad is a count of spaces in front, for tabs
// that were only partly consumed.
typedef struct MdSeg MdSeg;
struct MdSeg {
    MdSeg *next;
    u8    *data;
    iz     len;
    iz     pad;
};

// An open block: the chain of these is the path from the document to the
// block the last line ended in.
typedef struct {
    MdNode *node;
    MdSeg  *head, *tail;
    u8     *aligns;         // table: alignment of each column
    i32     start_line;
    i32     marker_offset;  // list item: indent of the marker
    i32     padding;        // list item: indent of the content
    i32     fence_len;
    i32     fence_offset;
    u8      kind;
    u8      fence_char;
    u8      html_type;
} MdOpen;

typedef struct {
    u32 hash;
    Str key;                // normalized label, empty data: free slot
    Str dest, title;
} MdRef;

enum { MD_LBL = 8192 };

typedef struct {
    MdMem   perm, scratch;
    void   *jb[5];
    Str     src;
    MdNode *free_node;
    MdSeg  *free_seg;
    u32     ext;

    // Block structure
    MdOpen  st[MD_MAX_DEPTH + 4];
    i32     nopen;
    i32     ci;             // deepest container that matched this line
    i32     last_matched;
    b32     all_closed;
    Str     line;           // the line, without its line ending
    iz      offset, column;
    iz      next, next_col; // first nonspace at or after offset
    iz      indent;         // next_col - column
    b32     indented, blank, partial_tab;
    i32     lineno;

    // Link reference definitions
    MdRef  *refs;
    iz      nrefs, cap_refs;
    u8     *lbl;            // normalized label, MD_LBL bytes
    i32    *bt_last;        // inline: last backtick run of each length
} MdParser;


// Nodes

static MdNode *md_node(MdParser *p, u8 kind) {
    MdNode *n = p->free_node;
    if (n) {
        p->free_node = n->next;
        memset(n, 0, sizeof(*n));
    } else {
        n = md_new(&p->perm, MdNode, 1);
    }
    n->kind = kind;
    return n;
}

static void md_free_node(MdParser *p, MdNode *n) {
    n->next      = p->free_node;
    p->free_node = n;
}

static void md_append(MdNode *parent, MdNode *n) {
    n->parent = parent;
    n->prev   = parent->last;
    n->next   = 0;
    if (parent->last) {
        parent->last->next = n;
    } else {
        parent->first = n;
    }
    parent->last = n;
}

static void md_unlink(MdNode *n) {
    MdNode *par = n->parent;
    if (n->prev) {
        n->prev->next = n->next;
    } else if (par) {
        par->first = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    } else if (par) {
        par->last = n->prev;
    }
    n->parent = n->prev = n->next = 0;
}

static void md_insert_after(MdNode *ref, MdNode *n) {
    n->parent = ref->parent;
    n->prev   = ref;
    n->next   = ref->next;
    if (ref->next) {
        ref->next->prev = n;
    } else if (ref->parent) {
        ref->parent->last = n;
    }
    ref->next = n;
}

static void md_insert_before(MdNode *ref, MdNode *n) {
    n->parent = ref->parent;
    n->next   = ref;
    n->prev   = ref->prev;
    if (ref->prev) {
        ref->prev->next = n;
    } else if (ref->parent) {
        ref->parent->first = n;
    }
    ref->prev = n;
}

static MdNode *md_text_node(MdParser *p, Str s) {
    MdNode *n = md_node(p, MD_TEXT);
    n->text   = s;
    return n;
}


// Content segments

static b32 md_in_src(MdParser *p, u8 *ptr) {
    return ptr>=p->src.data && ptr<p->src.data+p->src.len;
}

static void md_append_seg(MdParser *p, MdOpen *o, u8 *data, iz len, iz pad) {
    MdSeg *t = o->tail;
    // Lines that are neighbors in the source extend the previous run.
    if (t && pad==0 && md_in_src(p, t->data) && md_in_src(p, t->data+t->len) &&
            t->data[t->len]=='\n' && data==t->data+t->len+1) {
        t->len = data + len - t->data;
        return;
    }
    MdSeg *s = p->free_seg;
    if (s) {
        p->free_seg = s->next;
        memset(s, 0, sizeof(*s));
    } else {
        s = md_new(&p->scratch, MdSeg, 1);
    }
    s->data = data;
    s->len  = len;
    s->pad  = pad;
    if (t) {
        t->next = s;
    } else {
        o->head = s;
    }
    o->tail = s;
}

// The lines of a leaf block as one string, separated by "\n", and ended by
// one if final_nl. Uses the source in place when it can. Releases the
// segments.
static Str md_join(MdParser *p, MdOpen *o, b32 final_nl) {
    MdSeg *h = o->head;
    Str    r = {0};
    if (!h) {
        return r;
    }
    iz total = 0;
    iz count = 0;
    for (MdSeg *s = h; s; s = s->next) {
        total += s->pad + s->len;
        count++;
    }
    total += count - 1 + (final_nl ? 1 : 0);
    if (count==1 && h->pad==0 && !final_nl) {
        r = (Str){h->data, h->len};
    } else if (count==1 && h->pad==0 && md_in_src(p, h->data) &&
               md_in_src(p, h->data+h->len) && h->data[h->len]=='\n') {
        r = (Str){h->data, h->len + 1};
    } else {
        u8 *d = md_alloc(&p->perm, total, 1);
        iz  n = 0;
        for (MdSeg *s = h; s; s = s->next) {
            memset(d+n, ' ', (uz)s->pad);
            n += s->pad;
            memcpy(d+n, s->data, (uz)s->len);
            n += s->len;
            if (s->next || final_nl) {
                d[n++] = '\n';
            }
        }
        assert(n == total);
        r = (Str){d, total};
    }
    o->tail->next = p->free_seg;
    p->free_seg   = h;
    o->head = o->tail = 0;
    return r;
}


// Entities and escapes

// The entity reference at s[i], which is '&': "&name;", "&#123;" or
// "&#x1F;". Returns its end, or 0. A second code point is 0 when absent.
static iz md_entity(Str s, iz i, i32 *c1, i32 *c2) {
    *c2 = 0;
    iz j = i + 1;
    if (md_peek(s, j) == '#') {
        j++;
        b32 hex = md_peek(s, j)=='x' || md_peek(s, j)=='X';
        j += hex;
        iz  beg = j;
        i64 v   = 0;
        while (j<s.len && (hex ? md_isxdigit(s.data[j]) : md_isdigit(s.data[j])) && j-beg<8) {
            i32 d = s.data[j];
            v = v*(hex ? 16 : 10) + (d<='9' ? d-'0' : (d|32)-'a'+10);
            j++;
        }
        if (j==beg || j-beg>(hex ? 6 : 7) || md_peek(s, j)!=';') {
            return 0;
        }
        if (v==0 || v>0x10ffff || (v>=0xd800 && v<=0xdfff)) {
            v = 0xfffd;
        }
        *c1 = (i32)v;
        return j + 1;
    }
    iz beg = j;
    while (j<s.len && md_isalnum(s.data[j]) && j-beg<32) {
        j++;
    }
    iz len = j - beg;
    if (len<2 || len>31 || md_peek(s, j)!=';') {
        return 0;
    }
    iz rec = len + 3;
    iz lo  = 0;
    iz hi  = (md_ent_off[len+1] - md_ent_off[len]) / rec;
    while (lo < hi) {
        iz  mid = (lo + hi) / 2;
        const u8 *r = md_ent_data + md_ent_off[len] + mid*rec;
        iz  cmp = 0;
        for (iz k = 0; k<len && !cmp; k++) {
            cmp = s.data[beg+k] - r[k];
        }
        if (cmp < 0) {
            hi = mid;
        } else if (cmp > 0) {
            lo = mid + 1;
        } else {
            u32 v = (u32)r[len]<<16 | (u32)r[len+1]<<8 | r[len+2];
            *c1   = (i32)(v & 0x1ffff);
            if (v >> 17) {
                *c2 = (i32)md_ent_second[(v>>17) - 1];
            }
            return j + 1;
        }
    }
    return 0;
}

// Resolve backslash escapes and entity references, as in link destinations
// and titles and code block info strings.
static Str md_unescape(MdParser *p, Str s) {
    iz i = 0;
    while (i<s.len && s.data[i]!='\\' && s.data[i]!='&') {
        i++;
    }
    if (i == s.len) {
        return s;
    }
    // Only &nGt; and &nLt; grow (5 bytes become 6), so a fifth more is plenty.
    iz  cap = s.len + s.len/4 + 4;
    u8 *out = md_alloc(&p->perm, cap, 1);
    iz  n   = i;
    memcpy(out, s.data, (uz)i);
    while (i < s.len) {
        u8 c = s.data[i];
        if (c=='\\' && i+1<s.len && md_ispunct(s.data[i+1])) {
            out[n++] = s.data[i+1];
            i += 2;
        } else if (c == '&') {
            i32 c1, c2;
            iz  e = md_entity(s, i, &c1, &c2);
            if (e) {
                n += utf8encode(out+n, c1);
                if (c2) {
                    n += utf8encode(out+n, c2);
                }
                i = e;
            } else {
                out[n++] = c;
                i++;
            }
        } else {
            out[n++] = c;
            i++;
        }
        assert(n <= cap);
    }
    return (Str){out, n};
}


// Link syntax shared by inline links and reference definitions

static iz md_skip_blanks(Str s, iz i) {
    while (i<s.len && md_isblank(s.data[i])) {
        i++;
    }
    return i;
}

// Spaces and at most one line ending.
static iz md_spnl(Str s, iz i) {
    i = md_skip_blanks(s, i);
    if (i<s.len && s.data[i]=='\n') {
        i = md_skip_blanks(s, i + 1);
    }
    return i;
}

enum { MD_MAX_LABEL = 999, MD_MAX_PARENS = 32 };

// s[i] is '['. Returns the index after the matching ']' or -1. A label
// holds no unescaped brackets and is at most 999 characters.
static iz md_scan_label(Str s, iz i) {
    if (md_peek(s, i) != '[') {
        return -1;
    }
    iz n = 0;
    for (iz j = i + 1; j < s.len; j++) {
        u8 c = s.data[j];
        if (c == ']') {
            return j + 1;
        } else if (c == '[') {
            return -1;
        } else if (c=='\\' && j+1<s.len) {
            j++;
            n++;
        }
        n += (c&0xc0) != 0x80;
        if (n > MD_MAX_LABEL) {
            return -1;
        }
    }
    return -1;
}

// A link destination, undecoded and without any <>.
static b32 md_scan_dest(Str s, iz *pos, Str *raw) {
    iz i = *pos;
    if (md_peek(s, i) == '<') {
        for (iz j = i + 1; j < s.len; j++) {
            u8 c = s.data[j];
            if (c == '>') {
                *raw = md_sub(s, i+1, j);
                *pos = j + 1;
                return 1;
            } else if (c=='<' || c=='\n') {
                return 0;
            } else if (c=='\\' && j+1<s.len && s.data[j+1]!='\n') {
                j++;
            }
        }
        return 0;
    }
    iz depth = 0;
    iz j     = i;
    for (; j < s.len; j++) {
        u8 c = s.data[j];
        if (c=='\\' && j+1<s.len && md_ispunct(s.data[j+1])) {
            j++;
        } else if (c == '(') {
            if (++depth > MD_MAX_PARENS) {
                return 0;
            }
        } else if (c == ')') {
            if (!depth) {
                break;
            }
            depth--;
        } else if (c<=' ' || c==0x7f) {
            break;
        }
    }
    if ((j==i && md_peek(s, j)!=')') || depth) {
        return 0;
    }
    *raw = md_sub(s, i, j);
    *pos = j;
    return 1;
}

// A link title, undecoded and without its quotes.
static b32 md_scan_title(Str s, iz *pos, Str *raw) {
    iz  i    = *pos;
    i32 open = md_peek(s, i);
    u8  close;
    if (open == '"') {
        close = '"';
    } else if (open == '\'') {
        close = '\'';
    } else if (open == '(') {
        close = ')';
    } else {
        return 0;
    }
    for (iz j = i + 1; j < s.len; j++) {
        u8 c = s.data[j];
        if (c == close) {
            *raw = md_sub(s, i+1, j);
            *pos = j + 1;
            return 1;
        } else if (open=='(' && c=='(') {
            return 0;
        } else if (c=='\\' && j+1<s.len) {
            j++;
        }
    }
    return 0;
}


// Link reference definitions

// Lowercase (fold), collapse whitespace and trim, into p->lbl. Returns the
// length: 0 for a blank label, -1 if it does not fit.
static iz md_norm_label(MdParser *p, Str raw) {
    iz  n   = 0;
    b32 gap = 0;
    for (iz i = 0; i < raw.len;) {
        u8 c = raw.data[i];
        if (c==' ' || c=='\t' || c=='\n') {
            gap = n > 0;
            i++;
            continue;
        }
        if (n + 8 > MD_LBL) {
            return -1;
        }
        if (gap) {
            p->lbl[n++] = ' ';
            gap = 0;
        }
        iz  len;
        i32 cp = md_cp(raw, i, &len);
        if (cp==0xfffd && len==1) {
            p->lbl[n++] = c;    // invalid UTF-8: keep the byte
        } else {
            i32 f[3];
            i32 k = md_fold(cp, f);
            for (i32 j = 0; j < k; j++) {
                n += utf8encode(p->lbl+n, f[j]);
            }
        }
        i += len;
    }
    return n;
}

static u32 md_hash(const u8 *s, iz n) {
    u32 h = 2166136261u;
    for (iz i = 0; i < n; i++) {
        h = (h ^ s[i]) * 16777619u;
    }
    return h;
}

// Slot for a normalized label: its entry, or the free slot for it.
static MdRef *md_ref_slot(MdRef *refs, iz cap, const u8 *key, iz n, u32 h) {
    for (iz i = h & (u32)(cap - 1);; i = (i + 1) & (cap - 1)) {
        MdRef *r = &refs[i];
        if (!r->key.data) {
            return r;
        }
        if (r->hash==h && r->key.len==n) {
            iz k = 0;
            while (k<n && r->key.data[k]==key[k]) {
                k++;
            }
            if (k == n) {
                return r;
            }
        }
    }
}

static MdRef *md_ref_find(MdParser *p, Str raw) {
    if (!p->nrefs) {
        return 0;
    }
    iz n = md_norm_label(p, raw);
    if (n <= 0) {
        return 0;
    }
    MdRef *r = md_ref_slot(p->refs, p->cap_refs, p->lbl, n, md_hash(p->lbl, n));
    return r->key.data ? r : 0;
}

// Define the label held in p->lbl[0..n), unless it is already defined.
static void md_ref_add(MdParser *p, iz n, Str rdest, Str rtitle) {
    u32 h = md_hash(p->lbl, n);
    if (p->cap_refs) {
        if (md_ref_slot(p->refs, p->cap_refs, p->lbl, n, h)->key.data) {
            return;
        }
    }
    if ((p->nrefs + 1)*2 > p->cap_refs) {
        iz     cap  = p->cap_refs ? p->cap_refs*2 : 64;
        MdRef *refs = md_new(&p->scratch, MdRef, cap);
        for (iz i = 0; i < p->cap_refs; i++) {
            MdRef *o = &p->refs[i];
            if (o->key.data) {
                *md_ref_slot(refs, cap, o->key.data, o->key.len, o->hash) = *o;
            }
        }
        p->refs     = refs;
        p->cap_refs = cap;
    }
    u8 *key = md_alloc(&p->scratch, n, 1);
    memcpy(key, p->lbl, (uz)n);
    MdRef *r = md_ref_slot(p->refs, p->cap_refs, key, n, h);
    r->hash  = h;
    r->key   = (Str){key, n};
    r->dest  = md_unescape(p, rdest);
    r->title = md_unescape(p, rtitle);
    p->nrefs++;
}

// If s starts with a link reference definition, record it and return the
// number of bytes it takes, line ending included. Otherwise 0.
static iz md_refdef(MdParser *p, Str s) {
    iz i = md_scan_label(s, 0);
    if (i<0 || md_peek(s, i)!=':') {
        return 0;
    }
    Str label = md_sub(s, 1, i - 1);
    i = md_spnl(s, i + 1);
    Str rdest;
    if (!md_scan_dest(s, &i, &rdest)) {
        return 0;
    }
    iz  before = i;
    Str rtitle = {0};
    b32 titled = 0;
    i = md_spnl(s, i);
    if (i!=before && md_scan_title(s, &i, &rtitle)) {
        titled = 1;
    } else {
        i = before;
    }
    iz e = md_skip_blanks(s, i);
    if (e<s.len && s.data[e]!='\n') {
        if (!titled) {
            return 0;
        }
        // Not a title after all: the destination must end its line.
        titled = 0;
        rtitle = (Str){0};
        e = md_skip_blanks(s, before);
        if (e<s.len && s.data[e]!='\n') {
            return 0;
        }
    }
    iz n = md_norm_label(p, label);
    if (n > 0) {
        md_ref_add(p, n, rdest, rtitle);
        return e<s.len ? e + 1 : e;
    }
    return 0;
}


// Raw HTML syntax, shared by HTML blocks and inline HTML

// Where scans for a terminator last failed: a scan from there on fails too,
// so an unclosed "<!--" or quote is a one-time cost, not a quadratic one.
typedef struct {
    iz dq, sq, comment, pi, cdata, decl;
} MdMemo;

static MdMemo md_memo(void) {
    iz big = (iz)1 << 60;
    return (MdMemo){big, big, big, big, big, big};
}

// Index of the first needle at or after from, or -1.
static iz md_find(Str s, iz from, Str needle, iz *memo) {
    if (from >= *memo) {
        return -1;
    }
    for (iz i = from; i + needle.len <= s.len; i++) {
        iz k = 0;
        while (k<needle.len && s.data[i+k]==needle.data[k]) {
            k++;
        }
        if (k == needle.len) {
            return i;
        }
    }
    *memo = from;
    return -1;
}

static b32 md_starts(Str s, iz i, Str t) {
    return i>=0 && i+t.len<=s.len && equals(md_sub(s, i, i+t.len), t);
}

// s[i] is '<' and s[i+1] a letter. Returns the end of an open tag, or -1.
static iz md_scan_open_tag(Str s, iz i, MdMemo *m) {
    iz j = i + 2;
    while (j<s.len && (md_isalnum(s.data[j]) || s.data[j]=='-')) {
        j++;
    }
    for (;;) {
        iz k = j;
        while (k<s.len && md_isspace(s.data[k])) {
            k++;
        }
        if (md_peek(s, k) == '>') {
            return k + 1;
        } else if (md_peek(s, k)=='/' && md_peek(s, k+1)=='>') {
            return k + 2;
        } else if (k == j) {
            return -1;      // attributes need space in front
        }
        i32 c = md_peek(s, k);
        if (!(md_isalpha(c) || c=='_' || c==':')) {
            return -1;
        }
        k++;
        while (k<s.len && (md_isalnum(s.data[k]) || s.data[k]=='_' ||
                           s.data[k]=='.' || s.data[k]==':' || s.data[k]=='-')) {
            k++;
        }
        j = k;
        iz v = k;
        while (v<s.len && md_isspace(s.data[v])) {
            v++;
        }
        if (md_peek(s, v) == '=') {
            v++;
            while (v<s.len && md_isspace(s.data[v])) {
                v++;
            }
            i32 q = md_peek(s, v);
            if (q=='"' || q=='\'') {
                iz e = md_find(s, v+1, q=='"' ? S("\"") : S("'"), q=='"' ? &m->dq : &m->sq);
                if (e < 0) {
                    return -1;
                }
                j = e + 1;
            } else {
                iz e = v;
                while (e<s.len && !md_isspace(s.data[e]) && s.data[e]!='"' &&
                       s.data[e]!='\'' && s.data[e]!='=' && s.data[e]!='<' &&
                       s.data[e]!='>' && s.data[e]!='`') {
                    e++;
                }
                if (e == v) {
                    return -1;
                }
                j = e;
            }
        }
    }
}

// s[i] is '<' and s[i+1] is '/'. Returns the end of a closing tag, or -1.
static iz md_scan_close_tag(Str s, iz i) {
    iz j = i + 2;
    if (!md_isalpha(md_peek(s, j))) {
        return -1;
    }
    while (j<s.len && (md_isalnum(s.data[j]) || s.data[j]=='-')) {
        j++;
    }
    while (j<s.len && md_isspace(s.data[j])) {
        j++;
    }
    return md_peek(s, j)=='>' ? j + 1 : -1;
}

// s[i] is '<'. Returns the end of the tag, comment, processing instruction,
// declaration or CDATA section that starts there, or -1.
static iz md_scan_html(Str s, iz i, MdMemo *m) {
    i32 c = md_peek(s, i + 1);
    if (md_isalpha(c)) {
        return md_scan_open_tag(s, i, m);
    } else if (c == '/') {
        return md_scan_close_tag(s, i);
    } else if (c == '?') {
        iz e = md_find(s, i+2, S("?>"), &m->pi);
        return e<0 ? -1 : e + 2;
    } else if (c != '!') {
        return -1;
    }
    if (md_starts(s, i+2, S("--"))) {
        iz j = i + 4;
        if (md_peek(s, j) == '>') {
            return j + 1;
        } else if (md_peek(s, j)=='-' && md_peek(s, j+1)=='>') {
            return j + 2;
        }
        iz e = md_find(s, j, S("-->"), &m->comment);
        return e<0 ? -1 : e + 3;
    } else if (md_starts(s, i+2, S("[CDATA["))) {
        iz e = md_find(s, i+9, S("]]>"), &m->cdata);
        return e<0 ? -1 : e + 3;
    } else if (md_isalpha(md_peek(s, i+2))) {
        iz e = md_find(s, i+3, S(">"), &m->decl);
        return e<0 ? -1 : e + 1;
    }
    return -1;
}


// Blocks

static b32 md_ieq(Str s, const char *t) {
    iz i = 0;
    for (; t[i]; i++) {
        if (i>=s.len || lower(s.data[i])!=(u8)t[i]) {
            return 0;
        }
    }
    return i == s.len;
}

static const char *const md_block_tags[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body",
    "caption", "center", "col", "colgroup", "dd", "details", "dialog", "dir",
    "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
    "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header",
    "hr", "html", "iframe", "legend", "li", "link", "main", "menu", "menuitem",
    "nav", "noframes", "ol", "optgroup", "option", "p", "param", "search",
    "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead",
    "title", "tr", "track", "ul",
};

// The kind (1-7) of HTML block that r, a line starting with '<', starts, or 0.
// Kind 7 cannot interrupt a paragraph.
static i32 md_html_start(Str r, b32 allow7) {
    if (r.len < 2) {
        return 0;
    }
    if (r.data[1] == '!') {
        if (md_starts(r, 2, S("--"))) {
            return 2;
        } else if (md_starts(r, 2, S("[CDATA["))) {
            return 5;
        }
        return md_isalpha(md_peek(r, 2)) ? 4 : 0;
    } else if (r.data[1] == '?') {
        return 3;
    }
    b32 closing = r.data[1] == '/';
    iz  b       = 1 + closing;
    iz  e       = b;
    while (e<r.len && md_isalnum(r.data[e])) {
        e++;
    }
    Str name = md_sub(r, b, e);
    i32 c    = md_peek(r, e);
    b32 term = c==-1 || md_isspace(c) || c=='>';    // ends the name
    if (!closing && term && (md_ieq(name, "script") || md_ieq(name, "pre") ||
                             md_ieq(name, "style") || md_ieq(name, "textarea"))) {
        return 1;
    }
    if (term || (c=='/' && md_peek(r, e+1)=='>')) {
        for (iz i = 0; i < countof(md_block_tags); i++) {
            if (md_ieq(name, md_block_tags[i])) {
                return 6;
            }
        }
    }
    if (allow7) {
        MdMemo m = md_memo();
        iz     t = closing ? md_scan_close_tag(r, 0)
                           : md_isalpha(md_peek(r, 1)) ? md_scan_open_tag(r, 0, &m) : -1;
        if (t>0 && md_blank(md_sub(r, t, r.len))) {
            return 7;
        }
    }
    return 0;
}

// Does the rest of a line of an HTML block of kind 1-5 end it?
static b32 md_html_end(i32 kind, Str r) {
    iz memo = (iz)1 << 60;
    switch (kind) {
    case 1:
        for (iz i = 0; i+1 < r.len; i++) {
            if (r.data[i]=='<' && r.data[i+1]=='/') {
                iz  j = i + 2;
                Str w = md_sub(r, j, min(j + 9, r.len));
                for (iz k = 0; k < w.len; k++) {
                    if (w.data[k] == '>') {
                        Str name = md_sub(w, 0, k);
                        if (md_ieq(name, "script") || md_ieq(name, "pre") ||
                            md_ieq(name, "style") || md_ieq(name, "textarea")) {
                            return 1;
                        }
                        break;
                    }
                }
            }
        }
        return 0;
    case 2: return md_find(r, 0, S("-->"), &memo) >= 0;
    case 3: return md_find(r, 0, S("?>"), &memo) >= 0;
    case 4: return md_find(r, 0, S(">"), &memo) >= 0;
    case 5: return md_find(r, 0, S("]]>"), &memo) >= 0;
    }
    return 0;
}

static b32 md_accepts_lines(u8 kind) {
    return kind==MD_PARAGRAPH || kind==MD_CODE_BLOCK || kind==MD_HTML_BLOCK;
}

static b32 md_can_contain(u8 parent, u8 kind) {
    if (parent==MD_DOCUMENT || parent==MD_QUOTE || parent==MD_ITEM) {
        return kind != MD_ITEM;
    }
    return parent==MD_LIST && kind==MD_ITEM;
}

// Move to the next nonspace, remembering whether the line is blank.
static void md_find_nonspace(MdParser *p) {
    Str s   = p->line;
    iz  i   = p->offset;
    iz  col = p->column;
    while (i < s.len) {
        if (s.data[i] == ' ') {
            i++;
            col++;
        } else if (s.data[i] == '\t') {
            i++;
            col += 4 - col%4;
        } else {
            break;
        }
    }
    p->blank    = i >= s.len;
    p->next     = i;
    p->next_col = col;
    p->indent   = col - p->column;
    p->indented = p->indent >= 4;
}

// Advance count characters, or with columns, count columns, which can end
// in the middle of a tab.
static void md_advance(MdParser *p, iz count, b32 columns) {
    Str s = p->line;
    while (count>0 && p->offset<s.len) {
        if (s.data[p->offset] == '\t') {
            iz to_tab = 4 - p->column%4;
            if (columns) {
                p->partial_tab = to_tab > count;
                iz adv = min(to_tab, count);
                p->column += adv;
                p->offset += !p->partial_tab;
                count     -= adv;
            } else {
                p->partial_tab = 0;
                p->column += to_tab;
                p->offset++;
                count--;
            }
        } else {
            p->partial_tab = 0;
            p->offset++;
            p->column++;
            count--;
        }
    }
}

static void md_advance_nonspace(MdParser *p) {
    p->offset      = p->next;
    p->column      = p->next_col;
    p->partial_tab = 0;
}

static Str md_rest(MdParser *p, iz from) {
    return md_sub(p->line, from, p->line.len);
}

// Content of a paragraph with its link reference definitions removed.
static Str md_para_content(MdParser *p, MdOpen *o) {
    Str c = md_rtrim(md_join(p, o, 0));
    while (c.len && c.data[0]=='[') {
        iz n = md_refdef(p, c);
        if (!n) {
            break;
        }
        c = md_sub(c, n, c.len);
    }
    return c;
}

static b32 md_ends_blank(MdNode *n) {
    while (n) {
        if (n->flags & MD_F_BLANK) {
            return 1;
        }
        if (n->kind!=MD_LIST && n->kind!=MD_ITEM) {
            break;
        }
        n = n->last;
    }
    return 0;
}

// A list is loose if any items are separated by blank lines, or any item has
// blank lines between its direct children.
static void md_finish_list(MdNode *list) {
    for (MdNode *item = list->first; item; item = item->next) {
        if (md_ends_blank(item) && item->next) {
            return;
        }
        for (MdNode *sub = item->first; sub; sub = sub->next) {
            if (md_ends_blank(sub) && (item->next || sub->next)) {
                return;
            }
        }
    }
    list->flags |= MD_F_TIGHT;
}

static void md_finish_code(MdParser *p, MdOpen *o) {
    MdNode *n = o->node;
    Str     s = md_join(p, o, 1);
    if (n->flags & MD_F_FENCED) {
        // The first line is the info string.
        iz e = 0;
        while (e<s.len && s.data[e]!='\n') {
            e++;
        }
        iz b = md_skip_blanks(s, 0);
        iz f = e;
        while (f>b && md_isblank(s.data[f-1])) {
            f--;
        }
        n->info = md_unescape(p, md_sub(s, b, f));
        n->text = md_sub(s, min(e + 1, s.len), s.len);
        return;
    }
    // Indented: drop trailing blank lines.
    iz e = s.len;
    for (;;) {
        iz j = e;
        while (j>0 && s.data[j-1]==' ') {
            j--;
        }
        if (j>0 && s.data[j-1]=='\n') {
            e = j - 1;
        } else {
            break;
        }
    }
    n->text = md_sub(s, 0, min(e + 1, s.len));
}

static void md_finalize(MdParser *p) {
    MdOpen *o = &p->st[p->nopen - 1];
    MdNode *n = o->node;
    switch (n->kind) {
    case MD_PARAGRAPH: {
        Str c = md_para_content(p, o);
        if (c.len) {
            n->text = c;
        } else {
            md_unlink(n);
            md_free_node(p, n);
        }
        break;
    }
    case MD_CODE_BLOCK:
        md_finish_code(p, o);
        break;
    case MD_HTML_BLOCK: {
        Str s = md_join(p, o, 0);
        iz  e = s.len;
        for (;;) {
            iz j = e;
            while (j>0 && s.data[j-1]==' ') {
                j--;
            }
            if (j>0 && s.data[j-1]=='\n') {
                e = j - 1;
            } else {
                break;
            }
        }
        n->text = md_sub(s, 0, e);
        break;
    }
    case MD_LIST:
        md_finish_list(n);
        break;
    }
    p->nopen--;
}

static MdOpen *md_add_child(MdParser *p, u8 kind) {
    while (!md_can_contain(p->st[p->nopen-1].kind, kind)) {
        md_finalize(p);
    }
    assert(p->nopen < countof(p->st));
    MdNode *n = md_node(p, kind);
    n->line   = p->lineno;
    md_append(p->st[p->nopen-1].node, n);
    MdOpen *o = &p->st[p->nopen++];
    memset(o, 0, sizeof(*o));
    o->node       = n;
    o->kind       = kind;
    o->start_line = p->lineno;
    return o;
}

// Finalize the blocks that did not match this line, now that the line is
// known not to continue them.
static void md_close_unmatched(MdParser *p) {
    if (!p->all_closed) {
        while (p->nopen - 1 > p->last_matched) {
            md_finalize(p);
        }
        p->all_closed = 1;
    }
}

static void md_add_line(MdParser *p) {
    MdOpen *o   = &p->st[p->nopen - 1];
    iz      pad = 0;
    if (p->partial_tab && p->offset<p->line.len && p->line.data[p->offset]=='\t') {
        p->offset++;
        pad = 4 - p->column%4;
    }
    md_append_seg(p, o, p->line.data + p->offset, p->line.len - p->offset, pad);
}

// Does the line continue the open block? 0: yes. 1: no. 2: yes, and it was a
// closing fence, so there is nothing more to do with the line.
static i32 md_continue(MdParser *p, MdOpen *o) {
    switch (o->kind) {
    case MD_QUOTE:
        if (!p->indented && md_peek(p->line, p->next)=='>') {
            md_advance_nonspace(p);
            md_advance(p, 1, 0);
            if (md_isblank(md_peek(p->line, p->offset))) {
                md_advance(p, 1, 1);
            }
            return 0;
        }
        return 1;
    case MD_ITEM:
        if (p->blank) {
            if (!o->node->first) {
                return 1;       // blank line after an empty item
            }
            md_advance_nonspace(p);
        } else if (p->indent >= o->marker_offset + o->padding) {
            md_advance(p, o->marker_offset + o->padding, 1);
        } else {
            return 1;
        }
        return 0;
    case MD_CODE_BLOCK:
        if (o->node->flags & MD_F_FENCED) {
            Str r = md_rest(p, p->next);
            if (p->indent<=3 && r.len && r.data[0]==o->fence_char) {
                iz n = 0;
                while (n<r.len && r.data[n]==o->fence_char) {
                    n++;
                }
                if (n>=o->fence_len && md_skip_blanks(r, n)==r.len) {
                    md_finalize(p);
                    return 2;
                }
            }
            for (i32 i = o->fence_offset; i>0 && md_isblank(md_peek(p->line, p->offset)); i--) {
                md_advance(p, 1, 1);
            }
        } else if (p->indent >= 4) {
            md_advance(p, 4, 1);
        } else if (p->blank) {
            md_advance_nonspace(p);
        } else {
            return 1;
        }
        return 0;
    case MD_HTML_BLOCK:
        return p->blank && (o->html_type==6 || o->html_type==7);
    case MD_PARAGRAPH:
    case MD_TABLE:
        return p->blank;
    case MD_LIST:
        return 0;
    }
    return 1;   // headings and rules end with their line
}


// Block starts. Each returns 0 for no match, 1 for a container and 2 for a
// leaf, after consuming the marker.

static i32 md_start_quote(MdParser *p) {
    if (p->indented || md_peek(p->line, p->next)!='>' || p->nopen>=MD_MAX_DEPTH) {
        return 0;
    }
    md_advance_nonspace(p);
    md_advance(p, 1, 0);
    if (md_isblank(md_peek(p->line, p->offset))) {
        md_advance(p, 1, 1);
    }
    md_close_unmatched(p);
    md_add_child(p, MD_QUOTE);
    return 1;
}

// The text of an ATX heading without its closing sequence of #s.
static Str md_atx_content(Str s) {
    iz e = s.len;
    while (e>0 && md_isblank(s.data[e-1])) {
        e--;
    }
    iz f = e;
    while (f>0 && s.data[f-1]=='#') {
        f--;
    }
    if (f < e) {
        if (f == 0) {
            return md_sub(s, 0, 0);
        } else if (md_isblank(s.data[f-1])) {
            e = f;
            while (e>0 && md_isblank(s.data[e-1])) {
                e--;
            }
        }
    }
    return md_sub(s, 0, e);
}

static i32 md_start_atx(MdParser *p) {
    if (p->indented || md_peek(p->line, p->next)!='#') {
        return 0;
    }
    Str r = md_rest(p, p->next);
    iz  n = 0;
    while (n<r.len && r.data[n]=='#') {
        n++;
    }
    if (n>6 || (n<r.len && !md_isblank(r.data[n]))) {
        return 0;
    }
    iz m = md_skip_blanks(r, n);
    md_advance_nonspace(p);
    md_advance(p, m, 0);
    md_close_unmatched(p);
    MdOpen *o = md_add_child(p, MD_HEADING);
    o->node->level = (u8)n;
    o->node->text  = md_atx_content(md_rest(p, p->offset));
    p->offset      = p->line.len;
    return 2;
}

static i32 md_start_fence(MdParser *p) {
    i32 ch = md_peek(p->line, p->next);
    if (p->indented || (ch!='`' && ch!='~')) {
        return 0;
    }
    Str r = md_rest(p, p->next);
    iz  n = 0;
    while (n<r.len && r.data[n]==ch) {
        n++;
    }
    if (n < 3) {
        return 0;
    }
    if (ch == '`') {
        for (iz i = n; i < r.len; i++) {
            if (r.data[i] == '`') {
                return 0;
            }
        }
    }
    md_close_unmatched(p);
    MdOpen *o = md_add_child(p, MD_CODE_BLOCK);
    o->node->flags |= MD_F_FENCED;
    o->fence_char   = (u8)ch;
    o->fence_len    = (i32)n;
    o->fence_offset = (i32)p->indent;
    md_advance_nonspace(p);
    md_advance(p, n, 0);
    return 2;
}

static i32 md_start_html(MdParser *p) {
    if (p->indented || md_peek(p->line, p->next)!='<') {
        return 0;
    }
    // Kind 7 may not start a paragraph's continuation line.
    b32 allow7 = p->st[p->ci].kind != MD_PARAGRAPH &&
                 !(!p->all_closed && !p->blank && p->st[p->nopen-1].kind==MD_PARAGRAPH);
    i32 t = md_html_start(md_rest(p, p->next), allow7);
    if (!t) {
        return 0;
    }
    md_close_unmatched(p);
    md_add_child(p, MD_HTML_BLOCK)->html_type = (u8)t;
    return 2;
}

static i32 md_start_setext(MdParser *p) {
    MdOpen *c = &p->st[p->ci];
    if (p->indented || c->kind!=MD_PARAGRAPH) {
        return 0;
    }
    Str r = md_rest(p, p->next);
    if (!r.len || (r.data[0]!='=' && r.data[0]!='-')) {
        return 0;
    }
    iz n = 0;
    while (n<r.len && r.data[n]==r.data[0]) {
        n++;
    }
    if (md_skip_blanks(r, n) != r.len) {
        return 0;
    }
    md_close_unmatched(p);
    Str content = md_para_content(p, c);
    if (!content.len) {
        return 0;   // all link reference definitions; this line starts afresh
    }
    c->kind          = MD_HEADING;
    c->node->kind    = MD_HEADING;
    c->node->level   = r.data[0]=='=' ? 1 : 2;
    c->node->text    = content;
    p->offset        = p->line.len;
    return 2;
}

static i32 md_start_rule(MdParser *p) {
    if (p->indented) {
        return 0;
    }
    Str r = md_rest(p, p->next);
    if (!r.len || (r.data[0]!='*' && r.data[0]!='_' && r.data[0]!='-')) {
        return 0;
    }
    iz n = 0;
    for (iz i = 0; i < r.len; i++) {
        if (r.data[i] == r.data[0]) {
            n++;
        } else if (!md_isblank(r.data[i])) {
            return 0;
        }
    }
    if (n < 3) {
        return 0;
    }
    md_close_unmatched(p);
    md_add_child(p, MD_RULE);
    p->offset = p->line.len;
    return 2;
}

typedef struct {
    u8  ordered, delim;
    i32 start, marker_offset, padding;
} MdListData;

static b32 md_list_marker(MdParser *p, b32 in_paragraph, MdListData *d) {
    if (p->indent >= 4) {
        return 0;
    }
    Str r = md_rest(p, p->next);
    iz  n = 0;
    memset(d, 0, sizeof(*d));
    d->marker_offset = (i32)p->indent;
    if (r.len && (r.data[0]=='*' || r.data[0]=='+' || r.data[0]=='-')) {
        d->delim = r.data[0];
        n        = 1;
    } else {
        i64 v = 0;
        iz  k = 0;
        for (; k<r.len && k<9 && md_isdigit(r.data[k]); k++) {
            v = v*10 + (r.data[k] - '0');
        }
        if (!k || k>=r.len || (r.data[k]!='.' && r.data[k]!=')')) {
            return 0;
        }
        if (in_paragraph && v!=1) {
            return 0;
        }
        d->ordered = 1;
        d->start   = (i32)v;
        d->delim   = r.data[k];
        n          = k + 1;
    }
    i32 nc = md_peek(p->line, p->next + n);
    if (!(nc==-1 || nc==' ' || nc=='\t')) {
        return 0;
    }
    // A list that interrupts a paragraph may not start with a blank line.
    if (in_paragraph && md_blank(md_sub(r, n, r.len))) {
        return 0;
    }
    md_advance_nonspace(p);
    md_advance(p, n, 1);
    iz start_col = p->column;
    iz start_off = p->offset;
    do {
        md_advance(p, 1, 1);
        nc = md_peek(p->line, p->offset);
    } while (p->column-start_col<5 && md_isblank(nc));
    b32 blank_item = md_peek(p->line, p->offset) == -1;
    iz  spaces     = p->column - start_col;
    if (spaces>=5 || spaces<1 || blank_item) {
        d->padding     = (i32)n + 1;
        p->column      = start_col;
        p->offset      = start_off;
        p->partial_tab = 0;
        if (md_isblank(md_peek(p->line, p->offset))) {
            md_advance(p, 1, 1);
        }
    } else {
        d->padding = (i32)(n + spaces);
    }
    return 1;
}

static i32 md_start_item(MdParser *p) {
    MdOpen *c = &p->st[p->ci];
    if (p->nopen+2 >= MD_MAX_DEPTH) {
        return 0;
    }
    MdListData d;
    if (!md_list_marker(p, c->kind==MD_PARAGRAPH, &d)) {
        return 0;
    }
    md_close_unmatched(p);
    MdOpen *t = &p->st[p->nopen - 1];
    if (t->kind!=MD_LIST || !!(t->node->flags & MD_F_ORDERED)!=!!d.ordered ||
            t->node->delim!=d.delim) {
        MdNode *l = md_add_child(p, MD_LIST)->node;
        l->start  = d.start;
        l->delim  = d.delim;
        l->flags |= d.ordered ? MD_F_ORDERED : 0;
    }
    MdOpen *i       = md_add_child(p, MD_ITEM);
    i->marker_offset = d.marker_offset;
    i->padding       = d.padding;
    // A task list item: "[ ]" or "[x]" and a space, first on its line.
    Str r = md_rest(p, p->offset);
    if ((p->ext & MD_EXT_TASKS) && r.len>=4 && r.data[0]=='[' && r.data[2]==']' && md_isblank(r.data[3]) &&
            (r.data[1]==' ' || r.data[1]=='x' || r.data[1]=='X')) {
        i->node->task = r.data[1]==' ' ? MD_TASK_OPEN : MD_TASK_DONE;
        md_advance(p, 3, 0);
    }
    return 1;
}

static i32 md_start_code(MdParser *p) {
    if (!p->indented || p->blank || p->st[p->nopen-1].kind==MD_PARAGRAPH) {
        return 0;
    }
    md_advance(p, 4, 1);
    md_close_unmatched(p);
    md_add_child(p, MD_CODE_BLOCK);
    return 2;
}


// Tables

// Iterates over the cells of a table row.
typedef struct {
    Str s;
    iz  pos;
    b32 done;
} MdRow;

// Skip the optional leading and trailing pipes.
static void md_row_begin(MdRow *it, Str row) {
    iz b = md_skip_blanks(row, 0);
    iz e = row.len;
    while (e>b && md_isblank(row.data[e-1])) {
        e--;
    }
    if (b<e && row.data[b]=='|') {
        b++;
    }
    if (e>b && row.data[e-1]=='|') {
        iz k = 0;
        for (iz j = e - 1; j>b && row.data[j-1]=='\\'; j--) {
            k++;
        }
        if (k%2 == 0) {
            e--;
        }
    }
    it->s    = md_sub(row, b, e);
    it->pos  = 0;
    it->done = 0;
}

// The next cell, trimmed. Pipes split cells unless escaped.
static b32 md_row_next(MdRow *it, Str *cell) {
    if (it->done) {
        return 0;
    }
    Str s = it->s;
    iz  i = it->pos;
    for (iz j = i;; j++) {
        if (j>=s.len || s.data[j]=='|') {
            iz b = md_skip_blanks(s, i);
            iz e = j;
            while (e>b && md_isblank(s.data[e-1])) {
                e--;
            }
            *cell = md_sub(s, b, e);
            if (j >= s.len) {
                it->done = 1;
            } else {
                it->pos = j + 1;
            }
            return 1;
        }
        if (s.data[j]=='\\' && j+1<s.len) {
            j++;
        }
    }
}

// The number of columns if r is a delimiter row ("| --- | :-: |"), else 0.
static iz md_delim_row(Str r, u8 *aligns) {
    MdRow it;
    Str   cell;
    iz    n = 0;
    md_row_begin(&it, r);
    while (md_row_next(&it, &cell)) {
        iz  i = 0;
        b32 l = 0;
        b32 rt = 0;
        if (md_peek(cell, i) == ':') {
            l = 1;
            i++;
        }
        iz d = i;
        while (i<cell.len && cell.data[i]=='-') {
            i++;
        }
        if (i == d) {
            return 0;
        }
        if (md_peek(cell, i) == ':') {
            rt = 1;
            i++;
        }
        if (i != cell.len) {
            return 0;
        }
        if (aligns) {
            aligns[n] = l&&rt ? MD_ALIGN_CENTER : l ? MD_ALIGN_LEFT : rt ? MD_ALIGN_RIGHT : MD_ALIGN_NONE;
        }
        n++;
    }
    return n;
}

static iz md_row_count(Str r) {
    MdRow it;
    Str   cell;
    iz    n = 0;
    md_row_begin(&it, r);
    while (md_row_next(&it, &cell)) {
        n++;
    }
    return n;
}

static b32 md_has_pipe(Str s) {
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i] == '|') {
            return 1;
        }
    }
    return 0;
}

// Cell content, with "\|" unescaped.
static Str md_cell_text(MdParser *p, Str s) {
    iz i = 0;
    for (; i+1<s.len; i++) {
        if (s.data[i]=='\\' && s.data[i+1]=='|') {
            break;
        }
    }
    if (i+1 >= s.len) {
        return s;
    }
    u8 *out = md_alloc(&p->perm, s.len, 1);
    iz  n   = 0;
    for (i = 0; i < s.len; i++) {
        if (s.data[i]=='\\' && i+1<s.len) {
            i++;
            if (s.data[i] != '|') {
                out[n++] = '\\';
            }
        }
        out[n++] = s.data[i];
    }
    return (Str){out, n};
}

// Add a row of up to ncols cells. Missing cells are left out; the consumer
// treats them as empty.
static void md_table_row(MdParser *p, MdOpen *t, Str raw, b32 header) {
    MdNode *row = md_node(p, MD_ROW);
    row->line   = p->lineno;
    row->flags |= header ? MD_F_HEADER : 0;
    md_append(t->node, row);
    MdRow it;
    Str   cell;
    md_row_begin(&it, raw);
    for (i32 i = 0; i<t->node->ncols && md_row_next(&it, &cell); i++) {
        MdNode *c = md_node(p, MD_CELL);
        c->line   = p->lineno;
        c->align  = t->aligns[i];
        c->text   = md_cell_text(p, cell);
        md_append(row, c);
    }
}

// A delimiter row under the last line of a paragraph, which holds as many
// cells, turns that line into a table header.
static i32 md_start_table(MdParser *p) {
    MdOpen *c = &p->st[p->ci];
    if (!(p->ext & MD_EXT_TABLES) || p->indented || c->kind!=MD_PARAGRAPH || !c->tail) {
        return 0;
    }
    Str r = md_rest(p, p->next);
    iz  n = md_delim_row(r, 0);
    if (!n) {
        return 0;
    }
    MdSeg *t  = c->tail;
    iz     nl = t->len;
    while (nl>0 && t->data[nl-1]!='\n') {
        nl--;
    }
    Str header = {t->data + nl, t->len - nl};
    if (md_row_count(header)!=n || !(md_has_pipe(header) || md_has_pipe(r))) {
        return 0;
    }
    md_close_unmatched(p);
    if (nl > 0) {
        t->len = nl - 1;        // the lines above stay a paragraph
        md_finalize(p);
    } else if (c->head == t) {
        md_unlink(c->node);     // the header was all of it
        md_free_node(p, c->node);
        t->next     = p->free_seg;
        p->free_seg = t;
        c->head = c->tail = 0;
        p->nopen--;
    } else {
        MdSeg *q = c->head;
        while (q->next != t) {
            q = q->next;
        }
        q->next = 0;
        c->tail = q;
        t->next = p->free_seg;
        p->free_seg = t;
        md_finalize(p);
    }
    MdOpen *o = md_add_child(p, MD_TABLE);
    o->node->ncols = (i32)n;
    o->aligns      = md_alloc(&p->scratch, n, 1);
    md_delim_row(r, o->aligns);
    md_table_row(p, o, header, 1);
    p->offset = p->line.len;
    return 2;
}


static b32 md_maybe_special(MdParser *p) {
    i32 c = md_peek(p->line, p->next);
    return c=='#' || c=='`' || c=='~' || c=='*' || c=='+' || c=='_' ||
           c=='=' || c=='<' || c=='>' || c=='-' || c=='|' || c==':' ||
           md_isdigit(c);
}

static i32 md_try_starts(MdParser *p) {
    i32 r = md_start_quote(p);
    if (!r) { r = md_start_atx(p); }
    if (!r) { r = md_start_fence(p); }
    if (!r) { r = md_start_html(p); }
    if (!r) { r = md_start_setext(p); }
    if (!r) { r = md_start_rule(p); }
    if (!r) { r = md_start_item(p); }
    if (!r) { r = md_start_code(p); }
    if (!r) { r = md_start_table(p); }
    return r;
}

static void md_incorporate_line(MdParser *p, Str line) {
    p->line        = line;
    p->offset      = 0;
    p->column      = 0;
    p->blank       = 0;
    p->partial_tab = 0;
    p->lineno++;

    // Which of the open blocks does the line continue?
    i32 oldtip = p->nopen - 1;
    i32 ci     = 0;
    for (i32 i = 1; i < p->nopen; i++) {
        md_find_nonspace(p);
        i32 r = md_continue(p, &p->st[i]);
        if (r == 2) {
            return;
        } else if (r == 1) {
            break;
        }
        ci = i;
    }
    p->ci           = ci;
    p->last_matched = ci;
    p->all_closed   = ci == oldtip;

    // Unless in code or HTML, look for new blocks.
    b32 matched_leaf = p->st[ci].kind!=MD_PARAGRAPH && md_accepts_lines(p->st[ci].kind);
    while (!matched_leaf) {
        md_find_nonspace(p);
        if (!p->indented && !md_maybe_special(p)) {
            md_advance_nonspace(p);
            break;
        }
        i32 r = md_try_starts(p);
        if (!r) {
            md_advance_nonspace(p);
            break;
        }
        p->ci = p->nopen - 1;
        if (r == 2) {
            matched_leaf = 1;
        }
    }

    // What remains is text for the innermost block.
    if (!p->all_closed && !p->blank && p->st[p->nopen-1].kind==MD_PARAGRAPH) {
        md_add_line(p);     // lazy continuation
        return;
    }
    md_close_unmatched(p);
    MdOpen *c = &p->st[p->ci];
    if (p->blank && c->node->last) {
        c->node->last->flags |= MD_F_BLANK;
    }
    // Block quote lines are never blank (they have a '>'), and blank lines in
    // fenced code or right after an empty item's marker do not count.
    b32 blank = p->blank && !(c->kind==MD_QUOTE ||
                              (c->kind==MD_CODE_BLOCK && (c->node->flags&MD_F_FENCED)) ||
                              (c->kind==MD_ITEM && !c->node->first && c->start_line==p->lineno));
    for (i32 k = p->ci; k >= 0; k--) {
        MdNode *n = p->st[k].node;
        n->flags  = (u8)(blank ? n->flags | MD_F_BLANK : n->flags & ~MD_F_BLANK);
    }
    if (c->kind == MD_TABLE) {
        if (c->start_line != p->lineno) {   // not the delimiter row
            md_table_row(p, c, md_rest(p, p->offset), 0);
        }
        p->offset = p->line.len;
    } else if (md_accepts_lines(c->kind)) {
        md_add_line(p);
        if (c->kind==MD_HTML_BLOCK && c->html_type>=1 && c->html_type<=5 &&
                md_html_end(c->html_type, md_rest(p, p->offset))) {
            md_finalize(p);
        }
    } else if (p->offset<p->line.len && !p->blank) {
        md_add_child(p, MD_PARAGRAPH);
        md_advance_nonspace(p);
        md_add_line(p);
    }
}


// Inlines
//
// Inline parsing follows the CommonMark reference implementation: one pass
// that makes text nodes for delimiter runs and brackets and pushes them on
// two stacks, then resolves emphasis and links against the stacks.

typedef struct MdDelim MdDelim;
struct MdDelim {
    MdDelim *prev, *next;
    MdNode  *node;          // the text node holding the run
    i32      orig, num;     // delimiters in the run, and how many are left
    u8       ch, can_open, can_close;
};

typedef struct MdBracket MdBracket;
struct MdBracket {
    MdBracket *prev;
    MdNode    *node;        // the text node "[" or "!["
    MdDelim   *delim;       // top of the delimiter stack when pushed
    iz         pos;         // index of the '['
    i32        idx;         // increases with each push
    i32        maxd;        // deepest nesting among the nodes made after it
    u8         image;
    u8         after;       // another bracket was pushed above this one
};

// A run of backticks. next is the following run of the same length, and
// next_m the following run of one less, for a run whose first backtick was
// escaped.
typedef struct {
    iz  pos;
    i32 len, next, next_m;
} MdRun;

enum { MD_RUN_TABLE = 1024 };

typedef struct {
    MdParser  *p;
    MdNode    *block;       // the leaf block whose children are being made
    Str        s;
    iz         pos;
    iz         ts;          // start of the pending plain text
    MdDelim   *delims;      // top of the delimiter stack
    MdBracket *brackets;    // top of the bracket stack
    i32        nbrackets;
    i32        floor;       // links may not nest: brackets below are inactive
    MdMemo     memo;
    MdRun     *runs;
    iz         nruns;
    b32        have_runs;
} MdInl;

static b32 md_special(u8 c) {
    return c=='\n' || c=='\\' || c=='`' || c=='*' || c=='_' || c=='~' ||
           c=='[' || c==']' || c=='!' || c=='<' || c=='&';
}

// Make a text node of the pending text up to end.
static void md_flush(MdInl *in, iz end) {
    if (end > in->ts) {
        md_append(in->block, md_text_node(in->p, md_sub(in->s, in->ts, end)));
    }
}

static MdNode *md_add(MdInl *in, u8 kind) {
    MdNode *n = md_node(in->p, kind);
    md_append(in->block, n);
    return n;
}

static b32 md_i_newline(MdInl *in) {
    Str s  = in->s;
    iz  at = in->pos;
    iz  e  = at;
    while (e>in->ts && md_isblank(s.data[e-1])) {
        e--;
    }
    b32 hard = at-e>=2 && s.data[at-1]==' ' && s.data[at-2]==' ';
    md_flush(in, e);
    md_add(in, hard ? MD_HARDBREAK : MD_SOFTBREAK);
    in->pos = in->ts = md_skip_blanks(s, at + 1);
    return 1;
}

static b32 md_i_backslash(MdInl *in) {
    iz  at = in->pos;
    i32 c  = md_peek(in->s, at + 1);
    if (c == '\n') {
        md_flush(in, at);
        md_add(in, MD_HARDBREAK);
        in->pos = in->ts = md_skip_blanks(in->s, at + 2);
        return 1;
    } else if (md_ispunct(c)) {
        md_flush(in, at);
        in->ts  = at + 1;   // the escaped character starts the next text
        in->pos = at + 2;
        return 1;
    }
    return 0;
}

// Find every run of backticks, and link each to the next run of the same
// length, so a closer is found in constant time.
static void md_build_runs(MdInl *in) {
    MdParser *p = in->p;
    Str       s = in->s;
    iz        n = 0;
    for (iz i = 0; i < s.len;) {
        if (s.data[i] == '`') {
            while (i<s.len && s.data[i]=='`') {
                i++;
            }
            n++;
        } else {
            i++;
        }
    }
    in->runs  = md_new(&p->scratch, MdRun, n);
    in->nruns = n;
    iz k = 0;
    for (iz i = 0; i < s.len;) {
        if (s.data[i] == '`') {
            iz b = i;
            while (i<s.len && s.data[i]=='`') {
                i++;
            }
            in->runs[k].pos = b;
            in->runs[k].len = (i32)(i - b);
            k++;
        } else {
            i++;
        }
    }
    // Backward, so that last[] holds the next run of each length. Longer
    // runs than the table holds are rare, and kept in a list.
    iz *longs  = md_new(&p->scratch, iz, n);
    iz  nlongs = 0;
    for (iz r = n - 1; r >= 0; r--) {
        MdRun *u = &in->runs[r];
        for (i32 w = 0; w < 2; w++) {
            i32 want = u->len - w;
            i32 next = -1;
            if (want >= 1) {
                if (want < MD_RUN_TABLE) {
                    next = p->bt_last[want];
                } else {
                    for (iz j = nlongs - 1; j >= 0 && next < 0; j--) {
                        if (in->runs[longs[j]].len == want) {
                            next = (i32)longs[j];
                        }
                    }
                }
            }
            if (w == 0) {
                u->next = next;
            } else {
                u->next_m = next;
            }
        }
        if (u->len < MD_RUN_TABLE) {
            p->bt_last[u->len] = (i32)r;
        } else {
            longs[nlongs++] = r;
        }
    }
    for (iz r = 0; r < n; r++) {
        if (in->runs[r].len < MD_RUN_TABLE) {
            p->bt_last[in->runs[r].len] = -1;
        }
    }
    in->have_runs = 1;
}

static b32 md_i_backticks(MdInl *in) {
    MdParser *p  = in->p;
    Str       s  = in->s;
    iz        at = in->pos;
    if (!in->have_runs) {
        md_build_runs(in);
    }
    iz lo = 0;
    iz hi = in->nruns - 1;
    while (lo < hi) {       // the last run starting at or before at
        iz mid = (lo + hi + 1) / 2;
        if (in->runs[mid].pos <= at) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    MdRun *u = &in->runs[lo];
    iz     n = u->pos + u->len - at;
    iz     j = -1;
    if (at == u->pos) {
        j = u->next;
    } else if (at == u->pos + 1) {
        j = u->next_m;
    } else {
        for (iz k = lo + 1; k<in->nruns && j<0; k++) {
            if (in->runs[k].len == n) {
                j = k;
            }
        }
    }
    if (j < 0) {
        in->pos = at + n;   // no closer: the backticks are plain text
        return 1;
    }
    MdRun *c    = &in->runs[j];
    Str    body = md_sub(s, at + n, c->pos);
    // Line endings become spaces, and one space is dropped on each side
    // unless the code is nothing but spaces.
    iz  b       = 0;
    iz  e       = body.len;
    b32 content = 0;
    b32 nl      = 0;
    for (iz i = 0; i < body.len; i++) {
        content |= body.data[i]!=' ' && body.data[i]!='\n';
        nl      |= body.data[i]=='\n';
    }
    if (content && (body.data[0]==' ' || body.data[0]=='\n') &&
            (body.data[e-1]==' ' || body.data[e-1]=='\n')) {
        b++;
        e--;
    }
    md_flush(in, at);
    MdNode *node = md_add(in, MD_CODE);
    node->text   = md_sub(body, b, e);
    if (nl) {
        u8 *d = md_alloc(&p->perm, e - b, 1);
        for (iz i = b; i < e; i++) {
            d[i-b] = body.data[i]=='\n' ? ' ' : body.data[i];
        }
        node->text = (Str){d, e - b};
    }
    in->pos = in->ts = c->pos + c->len;
    return 1;
}

// A run of * _ or ~. The run is a text node that emphasis processing later
// consumes from, or leaves as it is.
static b32 md_i_delim(MdInl *in) {
    MdParser *p  = in->p;
    Str       s  = in->s;
    iz        at = in->pos;
    u8        c  = s.data[at];
    iz        e  = at;
    while (e<s.len && s.data[e]==c) {
        e++;
    }
    i32 before = md_cp_before(s, at);
    i32 after  = md_cp_after(s, e);
    b32 b_ws   = md_isuspace(before);
    b32 a_ws   = md_isuspace(after);
    b32 b_p    = md_isupunct(before);
    b32 a_p    = md_isupunct(after);
    b32 left   = !a_ws && (!a_p || b_ws || b_p);
    b32 right  = !b_ws && (!b_p || a_ws || a_p);
    b32 open   = left;
    b32 close  = right;
    if (c == '_') {
        open  = left && (!right || b_p);
        close = right && (!left || a_p);
    }
    md_flush(in, at);
    MdNode *t = md_text_node(p, md_sub(s, at, e));
    md_append(in->block, t);
    if ((open || close) && (c!='~' || (e-at<=2 && (p->ext & MD_EXT_STRIKE)))) {
        MdDelim *d  = md_new(&p->scratch, MdDelim, 1);
        d->node     = t;
        d->orig     = d->num = (i32)(e - at);
        d->ch       = c;
        d->can_open = (u8)open;
        d->can_close = (u8)close;
        d->prev     = in->delims;
        if (in->delims) {
            in->delims->next = d;
        }
        in->delims = d;
    }
    in->pos = in->ts = e;
    return 1;
}

static void md_remove_delim(MdInl *in, MdDelim *d) {
    if (d->prev) {
        d->prev->next = d->next;
    }
    if (d->next) {
        d->next->prev = d->prev;
    } else {
        in->delims = d->prev;
    }
}

// Move the nodes between a and b into a new node of the given kind, placed
// after a. Returns 0 if that would nest too deep.
static MdNode *md_wrap(MdInl *in, u8 kind, MdNode *a, MdNode *b) {
    i32 h = 0;
    for (MdNode *n = a->next; n != b; n = n->next) {
        h = max(h, (i32)n->depth);
    }
    if (h + 1 > MD_MAX_INLINE) {
        return 0;
    }
    MdNode *w = md_node(in->p, kind);
    w->depth  = (u8)(h + 1);
    for (MdNode *n = a->next; n != b;) {
        MdNode *next = n->next;
        md_unlink(n);
        md_append(w, n);
        n = next;
    }
    md_insert_after(a, w);
    return w;
}

// Match closers with openers above bottom, in order. The openers_bottom
// bounds keep this linear: when a closer of some kind finds no opener, no
// later closer of that kind will find one below that point either.
static void md_process_emphasis(MdInl *in, MdDelim *bottom) {
    MdDelim *ob[16];
    for (i32 i = 0; i < 16; i++) {
        ob[i] = bottom;
    }
    MdDelim *closer = in->delims;
    while (closer && closer->prev!=bottom) {
        closer = closer->prev;
    }
    while (closer) {
        if (!closer->can_close) {
            closer = closer->next;
            continue;
        }
        i32 bi;
        if (closer->ch == '_') {
            bi = (closer->can_open ? 3 : 0) + closer->orig%3;
        } else if (closer->ch == '*') {
            bi = 6 + (closer->can_open ? 3 : 0) + closer->orig%3;
        } else {
            bi = 12 + (closer->can_open ? 2 : 0) + closer->orig - 1;
        }
        MdDelim *opener = closer->prev;
        b32      found  = 0;
        while (opener && opener!=bottom && opener!=ob[bi]) {
            if (opener->ch==closer->ch && opener->can_open) {
                if (closer->ch == '~') {
                    found = opener->orig == closer->orig;
                } else {
                    // The rule of three.
                    b32 odd = (closer->can_open || opener->can_close) &&
                              closer->orig%3 != 0 &&
                              (opener->orig + closer->orig)%3 == 0;
                    found = !odd;
                }
                if (found) {
                    break;
                }
            }
            opener = opener->prev;
        }
        MdDelim *old = closer;
        if (found) {
            MdNode *on  = opener->node;
            MdNode *cn  = closer->node;
            i32     use = closer->ch=='~' ? closer->num :
                          closer->num>=2 && opener->num>=2 ? 2 : 1;
            u8      kind = closer->ch=='~' ? MD_STRIKE : use==2 ? MD_STRONG : MD_EMPH;
            if (md_wrap(in, kind, on, cn)) {
                opener->num -= use;
                closer->num -= use;
                on->text.len -= use;
                cn->text.data += use;
                cn->text.len  -= use;
                for (MdDelim *d = closer->prev; d && d!=opener;) {
                    MdDelim *prev = d->prev;
                    md_remove_delim(in, d);
                    d = prev;
                }
                if (opener->num == 0) {
                    md_unlink(on);
                    md_free_node(in->p, on);
                    md_remove_delim(in, opener);
                }
                if (closer->num == 0) {
                    MdDelim *next = closer->next;
                    md_unlink(cn);
                    md_free_node(in->p, cn);
                    md_remove_delim(in, closer);
                    closer = next;
                }
            } else {
                found = 0;
            }
        }
        if (!found) {
            closer = closer->next;
            ob[bi] = old->prev;
            if (!old->can_open) {
                md_remove_delim(in, old);
            }
        }
    }
    while (in->delims && in->delims!=bottom) {
        md_remove_delim(in, in->delims);
    }
}

static b32 md_i_open(MdInl *in, b32 image) {
    MdParser *p  = in->p;
    iz        at = in->pos;
    iz        e  = at + 1 + image;
    md_flush(in, at);
    MdNode *t = md_text_node(p, md_sub(in->s, at, e));
    md_append(in->block, t);
    MdBracket *b = md_new(&p->scratch, MdBracket, 1);
    b->node  = t;
    b->delim = in->delims;
    b->pos   = e - 1;
    b->idx   = in->nbrackets++;
    b->image = (u8)image;
    b->prev  = in->brackets;
    if (in->brackets) {
        in->brackets->after = 1;
    }
    in->brackets = b;
    in->pos = in->ts = e;
    return 1;
}

// Pop o. The nodes made since it was pushed stay, so the bracket below
// inherits their depth, and that of any node that replaced them.
static void md_pop_bracket(MdInl *in, MdBracket *o, i32 depth) {
    in->brackets = o->prev;
    if (o->prev) {
        o->prev->maxd = max(o->prev->maxd, max(o->maxd, depth));
    }
}

// A ']': try to close the innermost open bracket into a link or image.
static b32 md_i_close(MdInl *in) {
    MdParser  *p     = in->p;
    Str        s     = in->s;
    iz         at    = in->pos;
    iz         after = at + 1;
    MdBracket *o     = in->brackets;
    if (!o) {
        return 0;
    }
    if (!o->image && o->idx<in->floor) {
        md_pop_bracket(in, o, 0);   // inactive: it would be a link in a link
        return 0;
    }
    Str dest  = {0};
    Str title = {0};
    iz  end   = after;
    b32 found = 0;

    // An inline link: (destination "title")
    if (md_peek(s, after) == '(') {
        Str rdest, rtitle = {0};
        iz  q = md_spnl(s, after + 1);
        if (md_scan_dest(s, &q, &rdest)) {
            iz t = md_spnl(s, q);
            if (t>q && md_scan_title(s, &t, &rtitle)) {
                q = t;
            } else {
                q = t;
                rtitle = (Str){0};
            }
            q = md_spnl(s, q);
            if (md_peek(s, q) == ')') {
                found = 1;
                end   = q + 1;
                dest  = md_unescape(p, rdest);
                title = md_unescape(p, rtitle);
            }
        }
    }

    // A reference: [text][label], [text][] or [text]
    if (!found && p->nrefs) {
        iz  lend  = md_scan_label(s, after);
        Str label = {0};
        iz  used  = after;
        b32 have  = 0;
        if (lend>=0 && lend-after>2) {
            label = md_sub(s, after + 1, lend - 1);
            used  = lend;
            have  = 1;
        } else if (!o->after && at-o->pos<=4*MD_MAX_LABEL) {
            label = md_sub(s, o->pos + 1, at);
            used  = lend>=0 ? lend : after;
            have  = 1;
        }
        MdRef *r = have ? md_ref_find(p, label) : 0;
        if (r) {
            found = 1;
            end   = used;
            dest  = r->dest;
            title = r->title;
        }
    }

    MdNode *w = 0;
    if (found && o->maxd+1 <= MD_MAX_INLINE) {
        w        = md_node(p, o->image ? MD_IMAGE : MD_LINK);
        w->dest  = dest;
        w->title = title;
    }
    if (!w) {
        md_pop_bracket(in, o, 0);
        return 0;
    }
    md_flush(in, at);
    for (MdNode *n = o->node->next; n;) {
        MdNode *next = n->next;
        md_unlink(n);
        md_append(w, n);
        n = next;
    }
    md_append(in->block, w);
    md_process_emphasis(in, o->delim);
    // Emphasis inside may have deepened it, by up to the limit.
    i32 h = 0;
    for (MdNode *n = w->first; n; n = n->next) {
        h = max(h, (i32)n->depth);
    }
    w->depth = (u8)(h + 1);
    md_pop_bracket(in, o, w->depth);
    md_unlink(o->node);
    md_free_node(p, o->node);
    if (!o->image) {
        in->floor = o->idx;
    }
    in->pos = in->ts = end;
    return 1;
}

// The end of an autolink "<scheme:...>" or "<user@host>" at s[i], or -1.
static iz md_scan_autolink(Str s, iz i, b32 *email) {
    iz j = i + 1;
    *email = 0;
    if (md_isalpha(md_peek(s, j))) {
        iz k = j + 1;
        while (k<s.len && k-j<32 && (md_isalnum(s.data[k]) || s.data[k]=='+' ||
                                     s.data[k]=='.' || s.data[k]=='-')) {
            k++;
        }
        if (k-j>=2 && md_peek(s, k)==':') {
            k++;
            while (k<s.len && s.data[k]>' ' && s.data[k]!=0x7f &&
                   s.data[k]!='<' && s.data[k]!='>') {
                k++;
            }
            if (md_peek(s, k) == '>') {
                return k + 1;
            }
        }
    }
    iz k = j;
    while (k<s.len && (md_isalnum(s.data[k]) || md_ispunct(s.data[k])) &&
           s.data[k]!='<' && s.data[k]!='>' && s.data[k]!='@' &&
           s.data[k]!='(' && s.data[k]!=')' && s.data[k]!='[' && s.data[k]!=']' &&
           s.data[k]!='\\' && s.data[k]!=',' && s.data[k]!=';' && s.data[k]!=':' &&
           s.data[k]!='"') {
        k++;
    }
    if (k==j || md_peek(s, k)!='@') {
        return -1;
    }
    for (k++;;) {
        iz b = k;
        while (k<s.len && (md_isalnum(s.data[k]) || s.data[k]=='-')) {
            k++;
        }
        if (k==b || k-b>63 || s.data[b]=='-' || s.data[k-1]=='-') {
            return -1;
        }
        if (md_peek(s, k) == '.') {
            k++;
        } else {
            break;
        }
    }
    if (md_peek(s, k) != '>') {
        return -1;
    }
    *email = 1;
    return k + 1;
}

static b32 md_i_lt(MdInl *in) {
    MdParser *p  = in->p;
    Str       s  = in->s;
    iz        at = in->pos;
    b32       email;
    iz        e  = md_scan_autolink(s, at, &email);
    if (e > 0) {
        Str body = md_sub(s, at + 1, e - 1);
        md_flush(in, at);
        MdNode *l = md_add(in, MD_LINK);
        l->flags |= MD_F_AUTOLINK;
        l->depth  = 1;
        l->dest   = body;
        if (in->brackets) {
            in->brackets->maxd = max(in->brackets->maxd, 1);
        }
        if (email) {
            u8 *d = md_alloc(&p->perm, body.len + 7, 1);
            memcpy(d, "mailto:", 7);
            memcpy(d + 7, body.data, (uz)body.len);
            l->dest = (Str){d, body.len + 7};
        }
        md_append(l, md_text_node(p, body));
        in->pos = in->ts = e;
        return 1;
    }
    e = md_scan_html(s, at, &in->memo);
    if (e > 0) {
        md_flush(in, at);
        md_add(in, MD_HTML_INLINE)->text = md_sub(s, at, e);
        in->pos = in->ts = e;
        return 1;
    }
    return 0;
}

static b32 md_i_entity(MdInl *in) {
    iz  at = in->pos;
    i32 c1, c2;
    iz  e = md_entity(in->s, at, &c1, &c2);
    if (!e) {
        return 0;
    }
    u8 tmp[8];
    iz n = utf8encode(tmp, c1);
    if (c2) {
        n += utf8encode(tmp + n, c2);
    }
    md_flush(in, at);
    u8 *d = md_alloc(&in->p->perm, n, 1);
    memcpy(d, tmp, (uz)n);
    md_append(in->block, md_text_node(in->p, (Str){d, n}));
    in->pos = in->ts = e;
    return 1;
}


// Text clean-up: merge adjacent text nodes and find extended autolinks.

// Merge the text nodes that follow n into it.
static void md_merge_text(MdParser *p, MdNode *n) {
    if (!n->next || n->next->kind!=MD_TEXT) {
        return;
    }
    iz  total  = n->text.len;
    b32 contig = 1;
    u8 *end    = n->text.data + n->text.len;
    for (MdNode *k = n->next; k && k->kind==MD_TEXT; k = k->next) {
        contig &= k->text.data == end;
        end     = k->text.data + k->text.len;
        total  += k->text.len;
    }
    if (contig) {
        n->text.len = total;
    } else {
        u8 *d = md_alloc(&p->perm, total, 1);
        iz  m = 0;
        for (MdNode *k = n; k && k->kind==MD_TEXT; k = k->next) {
            memcpy(d + m, k->text.data, (uz)k->text.len);
            m += k->text.len;
        }
        n->text = (Str){d, total};
    }
    while (n->next && n->next->kind==MD_TEXT) {
        MdNode *k = n->next;
        md_unlink(k);
        md_free_node(p, k);
    }
}

// Trim an extended autolink that has run too far: trailing punctuation, an
// unbalanced ')' and anything that looks like an entity reference.
static iz md_autolink_trim(Str s, iz end) {
    for (iz i = 0; i < end; i++) {
        if (s.data[i] == '<') {
            end = i;
            break;
        }
    }
    iz open  = -1;
    iz close = 0;
    while (end > 0) {
        u8 c = s.data[end-1];
        if (c=='?' || c=='!' || c=='.' || c==',' || c==':' || c=='*' ||
                c=='_' || c=='~' || c=='\'' || c=='"') {
            end--;
        } else if (c == ';') {
            iz e = end - 2;
            while (e>0 && md_isalpha(s.data[e])) {
                e--;
            }
            if (e>=0 && e<end-2 && s.data[e]=='&') {
                end = e;
            } else {
                end--;
            }
        } else if (c == ')') {
            if (open < 0) {
                open = close = 0;
                for (iz i = 0; i < end; i++) {
                    open  += s.data[i] == '(';
                    close += s.data[i] == ')';
                }
            }
            if (close <= open) {
                break;
            }
            close--;
            end--;
        } else {
            break;
        }
    }
    return end;
}

// The end of the domain at the start of s, or 0 if it is not valid. A valid
// domain has a period and no underscore in its last two segments, unless
// allow_short. A domain is at most 255 bytes; this bounds the work wasted on
// every "www." in a long run of domain characters.
enum { MD_MAX_DOMAIN = 255 };
static iz md_domain(Str s, b32 allow_short) {
    if (!s.len || !md_isalnum(s.data[0])) {
        return 0;
    }
    iz i  = 1;
    iz np = 0;
    iz u1 = 0;
    iz u2 = 0;
    for (; i<s.len && i<=MD_MAX_DOMAIN; i++) {
        u8 c = s.data[i];
        if (c == '_') {
            u2++;
        } else if (c == '.') {
            u1 = u2;
            u2 = 0;
            np++;
        } else if (!md_isalnum(c) && c!='-') {
            break;
        }
    }
    if (u1>0 || u2>0 || i>MD_MAX_DOMAIN) {
        return 0;
    }
    return allow_short || np>0 ? i : 0;
}

// Look for www. URLs, scheme:// URLs and email addresses in a text node,
// and split them out as links.
static void md_autolink(MdParser *p, MdNode *n) {
    Str t    = n->text;
    iz  base = 0;
    for (iz i = 0; i < t.len; i++) {
        u8  c    = t.data[i];
        iz  beg  = -1;
        iz  end  = 0;
        Str pre  = {0};
        if (c=='w' && md_starts(t, i, S("www.")) &&
                (i==0 || md_isspace(t.data[i-1]) || t.data[i-1]=='*' ||
                 t.data[i-1]=='_' || t.data[i-1]=='~' || t.data[i-1]=='(')) {
            iz d = md_domain(md_sub(t, i, t.len), 0);
            if (d) {
                end = i + d;
                while (end<t.len && !md_isspace(t.data[end])) {
                    end++;
                }
                end = i + md_autolink_trim(md_sub(t, i, end), end - i);
                if (end > i + 4) {
                    beg = i;
                    pre = S("http://");
                }
            }
        } else if (c==':' && md_starts(t, i, S("://"))) {
            iz b = i;
            while (b>base && md_isalpha(t.data[b-1])) {
                b--;
            }
            Str scheme = md_sub(t, b, i);
            iz  d      = md_domain(md_sub(t, i + 3, t.len), 1);
            if (d && (md_ieq(scheme, "http") || md_ieq(scheme, "https") || md_ieq(scheme, "ftp"))) {
                end = i + 3 + d;
                while (end<t.len && !md_isspace(t.data[end])) {
                    end++;
                }
                end = b + md_autolink_trim(md_sub(t, b, end), end - b);
                if (end > i + 3) {
                    beg = b;
                }
            }
        } else if (c == '@') {
            iz b = i;
            while (b>base && (md_isalnum(t.data[b-1]) || t.data[b-1]=='.' ||
                              t.data[b-1]=='-' || t.data[b-1]=='_' || t.data[b-1]=='+')) {
                b--;
            }
            iz e  = i + 1;
            iz np = 0;
            for (; e < t.len; e++) {
                u8 d = t.data[e];
                if (md_isalnum(d) || d=='-' || d=='_') {
                    continue;
                } else if (d=='.' && e+1<t.len && md_isalnum(t.data[e+1])) {
                    np++;
                } else {
                    break;
                }
            }
            if (b<i && np>0 && t.data[e-1]!='-' && t.data[e-1]!='_' && md_peek(t, e)!='@') {
                beg = b;
                end = e;
                pre = S("mailto:");
            }
        }
        if (beg < 0) {
            continue;
        }
        if (beg > base) {
            md_insert_before(n, md_text_node(p, md_sub(t, base, beg)));
        }
        MdNode *l = md_node(p, MD_LINK);
        l->flags |= MD_F_AUTOLINK;
        l->depth  = 1;
        l->dest   = md_sub(t, beg, end);
        if (pre.len) {
            u8 *d = md_alloc(&p->perm, pre.len + end - beg, 1);
            memcpy(d, pre.data, (uz)pre.len);
            memcpy(d + pre.len, t.data + beg, (uz)(end - beg));
            l->dest = (Str){d, pre.len + end - beg};
        }
        md_append(l, md_text_node(p, md_sub(t, beg, end)));
        md_insert_before(n, l);
        base = end;
        i    = end - 1;
    }
    if (base > 0) {
        if (base < t.len) {
            n->text = md_sub(t, base, t.len);
        } else {
            md_unlink(n);
            md_free_node(p, n);
        }
    }
}

static void md_post(MdParser *p, MdNode *root) {
    MdNode *par    = root;
    MdNode *n      = root->first;
    i32     inlink = 0;
    for (;;) {
        while (!n) {
            if (par == root) {
                return;
            }
            inlink -= par->kind==MD_LINK || par->kind==MD_IMAGE;
            n   = par->next;
            par = par->parent;
        }
        if (n->kind == MD_TEXT) {
            md_merge_text(p, n);
            MdNode *next = n->next;
            if (!inlink && (p->ext & MD_EXT_AUTOLINK)) {
                md_autolink(p, n);
            }
            n = next;
        } else if (n->first) {
            inlink += n->kind==MD_LINK || n->kind==MD_IMAGE;
            par = n;
            n   = n->first;
        } else {
            n = n->next;
        }
    }
}

// Parse the inline content of a paragraph, heading or table cell.
static void md_inline_block(MdParser *p, MdNode *block) {
    MdInl in = {0};
    in.p     = p;
    in.block = block;
    in.s     = block->text;
    in.memo  = md_memo();
    block->text = (Str){0};
    Str s = in.s;
    while (in.pos < s.len) {
        u8  c  = s.data[in.pos];
        b32 ok = 0;
        if (!md_special(c)) {
            in.pos++;
            continue;
        }
        switch (c) {
        case '\n': ok = md_i_newline(&in);   break;
        case '\\': ok = md_i_backslash(&in); break;
        case '`':  ok = md_i_backticks(&in); break;
        case '*':
        case '_':
        case '~':  ok = md_i_delim(&in);     break;
        case '[':  ok = md_i_open(&in, 0);   break;
        case '!':  ok = md_peek(s, in.pos+1)=='[' && md_i_open(&in, 1); break;
        case ']':  ok = md_i_close(&in);     break;
        case '<':  ok = md_i_lt(&in);        break;
        case '&':  ok = md_i_entity(&in);    break;
        }
        if (!ok) {
            in.pos++;
        }
    }
    md_flush(&in, s.len);
    md_process_emphasis(&in, 0);
    md_post(p, block);
}


// Entry points

// Strip a byte order mark, turn NUL into U+FFFD, and CR and CRLF into LF.
// Uses s in place when nothing needs changing.
static Str md_normalize(MdParser *p, Str s) {
    if (s.len>=3 && s.data[0]==0xef && s.data[1]==0xbb && s.data[2]==0xbf) {
        s = md_sub(s, 3, s.len);
    }
    iz size = s.len;
    for (iz i = 0; i < s.len; i++) {
        if (s.data[i] == 0) {
            size += 2;
        } else if (s.data[i]=='\r' && i+1<s.len && s.data[i+1]=='\n') {
            size--;
        }
    }
    b32 clean = size == s.len;
    for (iz i = 0; clean && i < s.len; i++) {
        clean = s.data[i]!=0 && s.data[i]!='\r';
    }
    if (clean) {
        return s;
    }
    u8 *d = md_alloc(&p->perm, size, 1);
    iz  n = 0;
    for (iz i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == 0) {
            d[n++] = 0xef;
            d[n++] = 0xbf;
            d[n++] = 0xbd;
        } else if (c == '\r') {
            d[n++] = '\n';
            i += i+1<s.len && s.data[i+1]=='\n';
        } else {
            d[n++] = c;
        }
    }
    assert(n == size);
    return (Str){d, n};
}

// Parse src, UTF-8 Markdown, into a tree allocated from perm, with the given
// MD_EXT_* extensions. scratch is working memory, dead on return. Returns 0, leaving perm as it was, if
// either arena is too small. The tree points into src, which must stay
// alive as long as it.
static MdNode *md_parse(Arena *perm, Arena scratch, Str src, u32 ext) {
    MdParser p = {0};
    p.ext           = ext;
    p.perm.arena    = *perm;
    p.perm.next     = MD_CHUNK_MIN;
    p.perm.oom      = p.jb;
    p.scratch.arena = scratch;
    p.scratch.next  = MD_CHUNK_MIN;
    p.scratch.oom   = p.jb;
    if (__builtin_setjmp(p.jb)) {
        return 0;
    }
    if (src.len > 0x3fffffff) {
        src.len = 0x3fffffff;
    }
    p.lbl     = md_alloc(&p.scratch, MD_LBL, 1);
    p.bt_last = md_new(&p.scratch, i32, MD_RUN_TABLE);
    for (i32 i = 0; i < MD_RUN_TABLE; i++) {
        p.bt_last[i] = -1;
    }
    p.src = md_normalize(&p, src);

    // Blocks, which also finds the link reference definitions.
    MdNode *doc = md_node(&p, MD_DOCUMENT);
    p.st[0].node = doc;
    p.st[0].kind = MD_DOCUMENT;
    p.nopen      = 1;
    for (iz i = 0; i < p.src.len;) {
        iz e = i;
        while (e<p.src.len && p.src.data[e]!='\n') {
            e++;
        }
        md_incorporate_line(&p, md_sub(p.src, i, e));
        i = e + 1;
    }
    while (p.nopen) {
        md_finalize(&p);
    }

    // Then the inlines, now that every reference is known.
    for (MdNode *n = doc; n; n = md_next(n, doc)) {
        if (n->kind==MD_PARAGRAPH || n->kind==MD_HEADING || n->kind==MD_CELL) {
            if (n->text.len) {
                MdMark mark = md_mark(&p.scratch);
                md_inline_block(&p, n);
                md_reset(&p.scratch, mark);
            }
        }
    }
    md_trim(&p.perm);
    *perm = p.perm.arena;
    return doc;
}

// The text of a node and everything below it, as a reader would see it: text
// and code, with breaks as spaces. Appended to out if not 0. Returns the
// length.
static iz md_plain_walk(MdNode *root, u8 *out) {
    iz n = 0;
    for (MdNode *k = md_next(root, root); k; k = md_next(k, root)) {
        Str s = {0};
        if (k->kind==MD_TEXT || k->kind==MD_CODE) {
            s = k->text;
        } else if (k->kind==MD_SOFTBREAK || k->kind==MD_HARDBREAK) {
            s = S(" ");
        }
        if (out) {
            memcpy(out + n, s.data, (uz)s.len);
        }
        n += s.len;
    }
    return n;
}

static Str md_plain(Arena *a, MdNode *root) {
    iz  n = md_plain_walk(root, 0);
    u8 *d = new(a, n, u8);
    md_plain_walk(root, d);
    return (Str){d, n};
}

// GitHub's anchor for a heading: lowercase, spaces to hyphens, everything
// but letters, digits, hyphens and underscores dropped. Returns the length.
static iz md_slug_base(Str text, u8 *out) {
    iz n = 0;
    for (iz i = 0; i < text.len;) {
        iz  len;
        i32 c = md_cp(text, i, &len);
        i += len;
        if (c==' ' || c=='\t' || c=='\n') {
            out[n++] = '-';
        } else if (c<0x80) {
            if (md_isalnum(c) || c=='-' || c=='_') {
                out[n++] = lower((u8)c);
            }
        } else if (!md_isuspace(c) && !md_isupunct(c)) {
            i32 f[3];
            md_fold(c, f);
            n += utf8encode(out + n, f[0]);
        }
    }
    return n;
}

// Set dest of every heading to its anchor, "foo", "foo-1", "foo-2" for
// repeats, without the '#'. Allocates the slugs from a.
static void md_slugify(Arena *a, Arena scratch, MdNode *doc) {
    iz count = 0;
    iz bytes = 0;
    for (MdNode *n = doc; n; n = md_next(n, doc)) {
        if (n->kind == MD_HEADING) {
            count++;
            bytes += 4*md_plain_walk(n, 0) + 16;
        }
    }
    if (!count) {
        return;
    }
    iz     cap   = 16;
    while (cap < 2*count) {
        cap *= 2;
    }
    typedef struct { Str slug; i32 suffix; } Entry;
    Entry *tab  = new(&scratch, cap, Entry);
    u8    *tmp  = new(&scratch, bytes, u8);
    u8    *out  = new(a, bytes, u8);
    iz     used = 0;
    for (MdNode *n = doc; n; n = md_next(n, doc)) {
        if (n->kind != MD_HEADING) {
            continue;
        }
        iz  plain = md_plain_walk(n, tmp);
        Str text  = {tmp, plain};
        u8 *base  = out + used;
        iz  len   = md_slug_base(text, base);
        Str slug  = {base, len};
        for (;;) {
            // Find the slug, or the free slot for it.
            u32 h = md_hash(slug.data, slug.len);
            iz  i = h & (u32)(cap - 1);
            while (tab[i].slug.data && !equals(tab[i].slug, slug)) {
                i = (i + 1) & (cap - 1);
            }
            if (!tab[i].slug.data) {
                tab[i].slug = slug;
                break;
            }
            // Taken: the base slug counts repeats; try base-N.
            iz  b  = md_hash(base, len) & (u32)(cap - 1);
            while (!equals(tab[b].slug, (Str){base, len})) {
                b = (b + 1) & (cap - 1);
            }
            i32 k = ++tab[b].suffix;
            u8  digits[12];
            iz  nd = 0;
            for (i32 v = k; v; v /= 10) {
                digits[nd++] = (u8)('0' + v%10);
            }
            slug.data = base;
            slug.len  = len;
            base[slug.len++] = '-';
            while (nd) {
                base[slug.len++] = digits[--nd];
            }
            // base[0..len) is still the base for the next try.
        }
        n->dest = slug;
        used   += slug.len;
    }
}
