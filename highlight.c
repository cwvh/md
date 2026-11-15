// Syntax highlighting for fenced code blocks. Portable: base.c only.
//
// The highlighter classifies a code block's text into spans of a small set
// of kinds; the view maps each kind to a theme color. It is a lexer, not a
// parser: per-language rules for comments, strings, numbers, keywords and
// a few cheap heuristics (a name before "(" is a function). Wrong colors on
// odd input are acceptable; crashes, out-of-bounds reads and superlinear
// time are not.

typedef enum {
    HL_PLAIN,       // not emitted as a span: gaps between spans are plain
    HL_COMMENT,
    HL_KEYWORD,     // control flow and declarations: if, fn, def, SELECT
    HL_TYPE,        // built-in and obvious type names: int, u32, String
    HL_STRING,      // string and character literals, here-docs
    HL_ESCAPE,      // escapes and interpolation markers inside strings
    HL_NUMBER,
    HL_CONSTANT,    // true, false, null, None, nil
    HL_FUNCTION,    // a name being called or defined
    HL_PREPROC,     // #include, attributes, decorators, macros (name!)
    HL_VARIABLE,    // $x, ${x}, $(X) in shells, make, PowerShell
    HL_PROPERTY,    // keys in JSON, YAML, TOML and INI; HTML attributes
    HL_TAG,         // HTML/XML tag names; TOML/INI [section] headers
    HL_INSERTED,    // diff: added line
    HL_DELETED,     // diff: removed line
    HL_META,        // diff: file and hunk headers
    HL_COUNT,
} HlKind;

typedef struct {
    iz     beg;     // byte range in the code
    iz     end;
    HlKind kind;
} HlSpan;

typedef struct {
    HlSpan *spans;  // sorted, non-overlapping, non-empty, kind != HL_PLAIN
    iz      len;
} HlSpans;

// True if lang (a fence's first info word, e.g. "c", "py", "yaml") names a
// language we highlight. Matching is case-insensitive and knows aliases.
static b32 hl_known(Str lang);

// Highlight code as language lang. Spans are allocated from the arena;
// at most one per byte of code. An unknown language, or an arena without
// room for the worst case, gives no spans (all plain) rather than failing.
static HlSpans hl_highlight(Arena *a, Str lang, Str code);


// Implementation
//
// Every language is a lexer function that walks the text once, left to
// right, handing finished tokens to hl_emit. Constructs that can fail (a
// regex literal, ${...}) never rescan: a failed search remembers where it
// gave up (HlCtx.fail) and later attempts on that line are skipped, so
// every input takes linear time. Spans are produced in two passes: one
// counts them so the arena gets exactly as much as it needs, one fills
// them in.
//
// Keywords live in one string per language ("kw kw | type type | const |
// function"), loaded into a small hash table when a block starts.

typedef struct HlCtx  HlCtx;
typedef struct HlLang HlLang;
typedef iz (*HlLexFn)(HlCtx *, HlLang const *, iz, b32);

enum {
    HL_TABSIZE  = 512,  // hash slots; the biggest word list has ~200 words
    HL_MAXWORD  = 24,
    HL_DEPTH    = 8,    // nesting of ${...} inside strings inside ...
};

typedef struct {
    char const *word;
    u8          len;
    u8          kind;
} HlWord;

struct HlCtx {
    u8 const *p;
    iz        n;        // end of the text being lexed (<= len)
    iz        len;
    HlSpan   *out;      // 0 while counting
    iz        count;
    iz        pos;      // end of the last span
    HlKind    last;
    iz        fail;     // a bounded search failed: don't retry before this
    i32       depth;
    b32       ci;       // keywords are case-insensitive
    iz        hd_beg;   // pending here-doc delimiter
    iz        hd_len;
    b32       hd_tabs;
    HlWord    tab[HL_TABSIZE];
};

// Per-language switches of the C-like lexer.
enum {
    HL_F_PP       = 1<<0,   // #directives
    HL_F_RAWCPP   = 1<<1,   // R"x(...)x"
    HL_F_RAWRUST  = 1<<2,   // r#"..."#
    HL_F_BACKTICK = 1<<3,   // `...` is a string
    HL_F_TEMPLATE = 1<<4,   // ... with ${} and escapes
    HL_F_CSHARP   = 1<<5,   // @"..." $"..."
    HL_F_LIFETIME = 1<<6,   // 'a is a lifetime
    HL_F_MACRO    = 1<<7,   // name!(
    HL_F_ATAT     = 1<<8,   // @Annotation
    HL_F_REGEX    = 1<<9,   // /re/ after an operator
    HL_F_NEST     = 1<<10,  // nested block comments
    HL_F_SQSTR    = 1<<11,  // 'x' is a string, not a character
    HL_F_ATTRHASH = 1<<12,  // #[attribute]
    HL_F_DOLLAR   = 1<<13,  // $ in names
    HL_F_TRIPLE   = 1<<14,  // """text blocks"""
    HL_F_JSON     = 1<<15,  // "key": is a property
    HL_F_ATTRBR   = 1<<16,  // [Attribute] on a line of its own
    HL_F_SEP      = 1<<17,  // 1'000
};

// String body modes.
enum {
    HL_S_ESC    = 1<<0,     // backslash escapes
    HL_S_RAW    = 1<<1,     // backslash only protects the next byte
    HL_S_MULTI  = 1<<2,     // may span lines
    HL_S_TRIPLE = 1<<3,     // closed by three quotes
    HL_S_DOUBLE = 1<<4,     // a doubled quote is an escape
    HL_S_TMPL   = 1<<5,     // ${...} holds code
    HL_S_BRACE  = 1<<6,     // {...} holds code; {{ and }} are escapes
    HL_S_VAR    = 1<<7,     // $name, ${...}, $(...)
    HL_S_BT     = 1<<8,     // the escape character is `
};

struct HlLang {
    char const *names;      // aliases
    char const *words;
    char const *words2;     // more words, overriding the first
    HlLexFn     lex;
    u32         flags;
    b32         ci;
    i32         var;        // variable syntax: 0 shell and make, 1 PowerShell, 2 CMake
};

typedef enum {
    HL_L_C, HL_L_CPP, HL_L_CS, HL_L_JAVA, HL_L_RUST, HL_L_GO, HL_L_JS,
    HL_L_TS, HL_L_JSON, HL_L_PY, HL_L_SH, HL_L_CONSOLE, HL_L_PS,
    HL_L_MAKE, HL_L_CMAKE, HL_L_TOML, HL_L_YAML, HL_L_INI, HL_L_DIFF,
    HL_L_HTML, HL_L_SQL, HL_L_COUNT,
} HlLangId;

static HlLang const hl_langs[HL_L_COUNT];


// Words

__attribute__((noinline)) static u8 hl_peek(HlCtx *h, iz i) {
    return i>=0 && i<h->n ? h->p[i] : 0;
}

static b32 hl_isdig(u8 c)   { return c>='0' && c<='9'; }
static b32 hl_isalpha(u8 c) { return (c|32)>='a' && (c|32)<='z'; }
static b32 hl_isalnum(u8 c) { return hl_isalpha(c) || hl_isdig(c); }
static b32 hl_idstart(u8 c) { return hl_isalpha(c) || c=='_' || c>=0x80; }
static b32 hl_idchar(u8 c)  { return hl_idstart(c) || hl_isdig(c); }
static b32 hl_isblank(u8 c) { return c==' ' || c=='\t' || c=='\r'; }
static b32 hl_isws(u8 c)    { return hl_isblank(c) || c=='\n' || c==0; }
static b32 hl_ishex(u8 c)   { return hl_isdig(c) || ((c|32)>='a' && (c|32)<='f'); }

static b32 hl_memeq(u8 const *a, u8 const *b, iz n) {
    for (iz i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static u32 hl_hash(u8 const *s, iz n, b32 ci) {
    u32 x = 2166136261u;
    for (iz i = 0; i < n; i++) {
        x = (x ^ (ci ? lower(s[i]) : s[i])) * 16777619u;
    }
    return x;
}

// Add a word list to the table. Sections are separated by "|": keywords,
// types, constants, functions. Words are lower case in the case-
// insensitive languages.
static void hl_load(HlCtx *h, char const *s) {
    static u8 const kinds[] = {HL_KEYWORD, HL_TYPE, HL_CONSTANT, HL_FUNCTION};
    i32 k = 0;
    while (*s) {
        if (*s == ' ') {
            s++;
            continue;
        }
        char const *b = s;
        while (*s && *s!=' ') {
            s++;
        }
        iz len = s - b;
        if (len==1 && *b=='|') {
            k = min(k+1, 3);
            continue;
        }
        assert(len <= HL_MAXWORD);
        u32 j = hl_hash((u8 const *)b, len, 0) % HL_TABSIZE;
        while (h->tab[j].word) {
            HlWord *w = &h->tab[j];
            if (w->len==len && hl_memeq((u8 const *)w->word, (u8 const *)b, len)) {
                break;
            }
            j = (j+1) % HL_TABSIZE;
        }
        h->tab[j] = (HlWord){b, (u8)len, kinds[k]};
    }
}

static HlKind hl_lookup(HlCtx *h, iz b, iz e) {
    iz len = e - b;
    if (len<=0 || len>HL_MAXWORD || b<0 || e>h->n) {
        return HL_PLAIN;
    }
    u8 const *s = h->p + b;
    u32 j = hl_hash(s, len, h->ci) % HL_TABSIZE;
    for (; h->tab[j].word; j = (j+1) % HL_TABSIZE) {
        HlWord *w = &h->tab[j];
        if (w->len != len) {
            continue;
        }
        iz i = 0;
        for (; i<len && (h->ci ? lower(s[i]) : s[i])==(u8)w->word[i]; i++) {}
        if (i == len) {
            return (HlKind)w->kind;
        }
    }
    return HL_PLAIN;
}

static b32 hl_wordeq(HlCtx *h, iz b, iz e, char const *lit) {
    iz i = 0;
    for (; lit[i]; i++) {
        if (b+i>=e || b+i>=h->n || lower(h->p[b+i])!=(u8)lit[i]) {
            return 0;
        }
    }
    return b+i == e;
}


// Spans

__attribute__((noinline)) static void hl_emit(HlCtx *h, iz b, iz e, HlKind k) {
    if (e > h->len) {
        e = h->len;
    }
    if (b < h->pos) {
        b = h->pos;     // never overlap, whatever the lexer does
    }
    if (b>=e || k==HL_PLAIN) {
        return;
    }
    if (h->count && h->last==k && h->pos==b) {
        if (h->out) {
            h->out[h->count-1].end = e;
        }
    } else {
        if (h->out) {
            h->out[h->count] = (HlSpan){b, e, k};
        }
        h->count++;
        h->last = k;
    }
    h->pos = e;
}

static iz hl_eol(HlCtx *h, iz i) {
    while (i<h->n && h->p[i]!='\n') {
        i++;
    }
    return i;
}

// Where pat (m bytes) next starts at or after i, or h->n if nowhere.
static iz hl_findmem(HlCtx *h, iz i, u8 const *pat, iz m, b32 ci) {
    for (; i+m <= h->n; i++) {
        iz k = 0;
        for (; k<m && (ci ? lower(h->p[i+k]) : h->p[i+k])==pat[k]; k++) {}
        if (k == m) {
            return i;
        }
    }
    return h->n;
}

#define hl_find(h, i, lit) hl_findmem(h, i, (u8 const *)"" lit "", sizeof(lit)-1, 0)

static b32 hl_starts(HlCtx *h, iz i, char const *lit) {
    for (iz k = 0; lit[k]; k++) {
        if (hl_peek(h, i+k) != (u8)lit[k]) {
            return 0;
        }
    }
    return 1;
}

static iz hl_skipblank(HlCtx *h, iz i) {
    while (i<h->n && hl_isblank(h->p[i])) {
        i++;
    }
    return i;
}

static iz hl_trimend(HlCtx *h, iz b, iz e) {
    while (e>b && hl_isblank(h->p[e-1])) {
        e--;
    }
    return e;
}

static iz hl_next(HlCtx *h, iz e) {
    return e < h->n ? e+1 : h->n;
}


// Pieces shared by the lexers

// The escape at i (the introducer is p[i]) ends where? Never inside a
// UTF-8 sequence, so a span boundary never splits a character.
static iz hl_esc_end(HlCtx *h, iz i) {
    if (i+1 >= h->n) {
        return h->n;
    }
    u8 c  = h->p[i+1];
    iz j  = i + 2;
    if (h->p[i] == '\\') {
        if (c=='x' || c=='u' || c=='U') {
            iz max = c=='x' ? 2 : c=='u' ? 4 : 8;
            if (hl_peek(h, j) == '{') {
                iz k = j + 1;
                while (k<h->n && k<j+12 && hl_ishex(h->p[k])) {
                    k++;
                }
                if (hl_peek(h, k) == '}') {
                    j = k + 1;
                }
            } else {
                for (iz k = 0; k<max && hl_ishex(hl_peek(h, j)); k++) {
                    j++;
                }
            }
        } else if (c>='0' && c<='7') {
            for (iz k = 0; k<2 && hl_peek(h, j)>='0' && hl_peek(h, j)<='7'; k++) {
                j++;
            }
        }
    }
    if (c=='\r' && hl_peek(h, j)=='\n') {
        j++;
    }
    if (c >= 0x80) {
        while (j<h->n && (h->p[j]&0xc0)==0x80) {
            j++;
        }
    }
    return j;
}

// A variable reference at i (a "$"), or i if there is none. flavor is the
// language's HlLang.var.
static iz hl_var_end(HlCtx *h, iz i, i32 flavor) {
    iz j = i + 1;
    u8 c = hl_peek(h, j);
    if (flavor==2 && hl_idstart(c)) {   // $ENV{X}
        while (hl_idchar(hl_peek(h, j))) {
            j++;
        }
        c = hl_peek(h, j);
        if (c != '{') {
            return i;
        }
    }
    if (c=='{' || (c=='(' && flavor!=2)) {
        if (i < h->fail) {
            return j + 1;
        }
        u8  close = c=='{' ? '}' : ')';
        iz  open  = j;
        i32 d     = 1;
        for (j++; j<h->n && h->p[j]!='\n'; j++) {
            if (h->p[j] == c) {
                d++;
            } else if (h->p[j]==close && --d==0) {
                return j + 1;
            }
        }
        h->fail = j;
        return open + 1;
    }
    if (hl_idstart(c)) {
        for (j++; ; j++) {
            u8 d = hl_peek(h, j);
            if (!hl_idchar(d) && !(flavor==1 && d==':' && hl_idstart(hl_peek(h, j+1)))) {
                break;
            }
        }
        return j;
    }
    if (c && (hl_isdig(c) || (flavor!=2 && c!=' ' && c!='\n' && c!='\t' &&
            (c=='@' || c=='*' || c=='#' || c=='?' || c=='$' || c=='!' ||
             c=='-' || c=='<' || c=='^' || c=='+' || c=='%' || c=='&')))) {
        return i + 2;
    }
    return i;
}

// The string whose body starts at i (the opening delimiter began at b, and
// may include a prefix like r or $@). Emits the string, its escapes and
// whatever code is interpolated. Returns the index after the closing
// delimiter. An unterminated string ends at the end of the line, or of the
// text if it may span lines.
static iz hl_str(HlCtx *h, HlLang const *L, iz b, iz i, u8 q, u32 m, HlKind kind) {
    u8 const *p      = h->p;
    iz        n      = h->n;
    iz        seg    = b;
    b32       triple = (m&HL_S_TRIPLE) != 0;
    u8        esc    = (m&HL_S_BT) ? '`' : '\\';
    while (i < n) {
        u8  c    = p[i];
        iz  open = 0;
        if (c==q && (!triple || (hl_peek(h, i+1)==q && hl_peek(h, i+2)==q))) {
            if ((m&HL_S_DOUBLE) && hl_peek(h, i+1)==q) {
                hl_emit(h, seg, i, kind);
                hl_emit(h, i, i+2, HL_ESCAPE);
                i += 2;
                seg = i;
                continue;
            }
            i += triple ? 3 : 1;
            hl_emit(h, seg, i, kind);
            return i;
        }
        if (c=='\n' && !(m&HL_S_MULTI)) {
            break;
        }
        if (c==esc && (m&(HL_S_ESC|HL_S_RAW))) {
            if (m & HL_S_RAW) {
                i = hl_esc_end(h, i);
            } else {
                hl_emit(h, seg, i, kind);
                iz e = hl_esc_end(h, i);
                hl_emit(h, i, e, HL_ESCAPE);
                i = seg = e;
            }
            continue;
        }
        if ((m&HL_S_TMPL) && c=='$' && hl_peek(h, i+1)=='{') {
            open = 2;
        } else if ((m&HL_S_BRACE) && (c=='{' || c=='}')) {
            if (hl_peek(h, i+1) == c) {
                hl_emit(h, seg, i, kind);
                hl_emit(h, i, i+2, HL_ESCAPE);
                i += 2;
                seg = i;
                continue;
            }
            open = c=='{';
        } else if ((m&HL_S_VAR) && c=='$') {
            iz e = hl_var_end(h, i, L->var);
            if (e > i) {
                hl_emit(h, seg, i, kind);
                hl_emit(h, i, e, HL_VARIABLE);
                i = seg = e;
                continue;
            }
        }
        if (open && h->depth<HL_DEPTH) {
            hl_emit(h, seg, i, kind);
            hl_emit(h, i, i+open, HL_ESCAPE);
            h->depth++;
            iz e = L->lex(h, L, i+open, 1);
            h->depth--;
            if (e<n && p[e]=='}') {
                hl_emit(h, e, e+1, HL_ESCAPE);
                e++;
            }
            i = seg = e;
            continue;
        }
        i++;
    }
    i = min(i, n);
    hl_emit(h, seg, i, kind);
    return i;
}

// Does a number start at i? A leading dot only counts when it is not a
// member access or a range: x.5, a[0].5, 1..2.
static b32 hl_numstart(HlCtx *h, iz i) {
    u8 c = h->p[i];
    if (c != '.') {
        return hl_isdig(c);
    }
    u8 d = hl_peek(h, i-1);
    return hl_isdig(hl_peek(h, i+1)) && !hl_idchar(d) && d!='.' && d!=')' && d!=']';
}

// The end of the number starting at i. Swallows suffixes (u32, f, n, L).
static iz hl_num_end(HlCtx *h, iz i, b32 sep) {
    u8 const *p   = h->p;
    iz        n   = h->n;
    b32       hex = i+1<n && p[i]=='0' && (p[i+1]|32)=='x';
    b32       dot = 0;
    if (p[i] == '.') {
        dot = 1;
        i++;
    }
    while (i < n) {
        u8 c = p[i];
        if (hl_isalnum(c) || c=='_') {
            i++;
            if ((hex ? (c|32)=='p' : (c|32)=='e') && (hl_peek(h, i)=='+' || hl_peek(h, i)=='-') &&
                    hl_isdig(hl_peek(h, i+1))) {
                i++;
            }
        } else if (c=='\'' && sep && hl_isalnum(hl_peek(h, i+1))) {
            i++;
        } else if (c=='.' && !dot && !hex && hl_peek(h, i+1)!='.' && !hl_idstart(hl_peek(h, i+1))) {
            dot = 1;
            i++;
        } else {
            break;
        }
    }
    return i;
}

static b32 hl_key_follows(HlCtx *h, iz i, u8 q, u8 term) {
    iz e = i + 1;
    while (e<h->n && h->p[e]!=q && h->p[e]!='\n') {
        e += h->p[e]=='\\' ? 2 : 1;
    }
    if (hl_peek(h, e) != q) {
        return 0;
    }
    e++;
    while (hl_isblank(hl_peek(h, e))) {
        e++;
    }
    return hl_peek(h, e) == term;
}

// A C++ raw string whose opening quote is at q: its end, or 0 if the
// delimiter is malformed.
static iz hl_raw_cpp(HlCtx *h, iz q) {
    u8 const *p = h->p;
    iz        n = h->n;
    iz        d = q + 1;
    iz        k = d;
    while (k<n && k-d<=16 && p[k]!='(' && p[k]!=')' && p[k]!='"' &&
            p[k]!='\\' && !hl_isws(p[k]) && p[k]!='\t') {
        k++;
    }
    if (hl_peek(h, k) != '(') {
        return 0;
    }
    iz m = k - d;
    for (iz j = k + 1; j+m+1 < n; j++) {
        if (p[j]==')' && p[j+m+1]=='"' && hl_memeq(p+j+1, p+d, m)) {
            return j + m + 2;
        }
    }
    return n;
}

// A Rust raw string r#"..."# whose hashes start at i: its end, or 0.
static iz hl_raw_rust(HlCtx *h, iz i) {
    iz j = i;
    while (hl_peek(h, j) == '#') {
        j++;
    }
    iz hc = j - i;
    if (hl_peek(h, j) != '"') {
        return 0;
    }
    for (iz k = j + 1; k < h->n; k++) {
        if (h->p[k] == '"') {
            iz t = k + 1;
            while (t<h->n && t-(k+1)<hc && h->p[t]=='#') {
                t++;
            }
            if (t-(k+1) == hc) {
                return t;
            }
            k = t - 1;
        }
    }
    return h->n;
}


// C and friends

static iz hl_lex_c(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    u8 const *p       = h->p;
    iz        n       = h->n;
    u32       f       = L->flags;
    iz        braces  = 0;
    b32       bol     = 1;
    b32       operand = 0;
    b32       deff    = 0;
    while (i < n) {
        u8 c = p[i];
        if (c == '\n') {
            bol = 1;
            i++;
            continue;
        }
        if (c==' ' || c=='\t' || c=='\r' || c=='\f' || c=='\v') {
            i++;
            continue;
        }
        b32 first = bol;
        b32 was   = operand;
        b32 wasdef = deff;
        bol     = 0;
        operand = 0;
        deff    = 0;
        if (c=='/' && hl_peek(h, i+1)=='/') {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='/' && hl_peek(h, i+1)=='*') {
            iz e = i + 2;
            if (f & HL_F_NEST) {
                i32 d = 1;
                while (e<n && d>0) {
                    if (p[e]=='/' && hl_peek(h, e+1)=='*') {
                        d++;
                        e += 2;
                    } else if (p[e]=='*' && hl_peek(h, e+1)=='/') {
                        d--;
                        e += 2;
                    } else {
                        e++;
                    }
                }
            } else {
                e = hl_find(h, e, "*/");
                e = e<n ? e+2 : n;
            }
            e = min(e, n);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='/' && (f&HL_F_REGEX) && !was && i>=h->fail) {
            iz  j   = i + 1;
            b32 cls = 0;
            for (; j<n && p[j]!='\n'; j++) {
                if (p[j] == '\\') {
                    j++;
                } else if (p[j] == '[') {
                    cls = 1;
                } else if (p[j] == ']') {
                    cls = 0;
                } else if (p[j]=='/' && !cls) {
                    break;
                }
            }
            if (j<n && p[j]=='/') {
                j++;
                while (j<n && hl_idchar(p[j])) {
                    j++;
                }
                hl_emit(h, i, j, HL_STRING);
                i = j;
                operand = 1;
            } else {
                h->fail = j;
                i++;
            }
        } else if (c == '"') {
            u32 m = HL_S_ESC;
            iz  o = 1;
            if ((f&HL_F_TRIPLE) && hl_peek(h, i+1)=='"' && hl_peek(h, i+2)=='"') {
                m |= HL_S_TRIPLE | HL_S_MULTI;
                o  = 3;
            }
            HlKind k = (f&HL_F_JSON) && hl_key_follows(h, i, '"', ':') ? HL_PROPERTY : HL_STRING;
            i = hl_str(h, L, i, i+o, '"', m, k);
            operand = 1;
        } else if (c == '\'') {
            u8 d = hl_peek(h, i+1);
            if (f & HL_F_SQSTR) {
                HlKind k = (f&HL_F_JSON) && hl_key_follows(h, i, '\'', ':') ? HL_PROPERTY : HL_STRING;
                i = hl_str(h, L, i, i+1, '\'', HL_S_ESC, k);
                operand = 1;
            } else if (f & HL_F_LIFETIME) {
                iz clen = d>=0xf0 ? 4 : d>=0xe0 ? 3 : d>=0xc0 ? 2 : 1;
                if (d=='\\' || (d && d!='\'' && d!='\n' && hl_peek(h, i+1+clen)=='\'')) {
                    i = hl_str(h, L, i, i+1, '\'', HL_S_ESC, HL_STRING);
                    operand = 1;
                } else if (hl_idstart(d)) {
                    iz j = i + 1;
                    while (hl_idchar(hl_peek(h, j))) {
                        j++;
                    }
                    hl_emit(h, i, j, HL_TYPE);
                    i = j;
                } else {
                    i++;
                }
            } else {
                i = hl_str(h, L, i, i+1, '\'', HL_S_ESC, HL_STRING);
                operand = 1;
            }
        } else if (c=='`' && (f&HL_F_BACKTICK)) {
            u32 m = (f&HL_F_TEMPLATE) ? HL_S_ESC|HL_S_MULTI|HL_S_TMPL : HL_S_MULTI;
            i = hl_str(h, L, i, i+1, '`', m, HL_STRING);
            operand = 1;
        } else if ((f&HL_F_CSHARP) && (c=='@' || c=='$') &&
                ((hl_peek(h, i+1)=='"') || ((hl_peek(h, i+1)=='@' || hl_peek(h, i+1)=='$') &&
                  hl_peek(h, i+1)!=c && hl_peek(h, i+2)=='"'))) {
            b32 at = c=='@' || hl_peek(h, i+1)=='@';
            b32 dl = c=='$' || hl_peek(h, i+1)=='$';
            iz  j  = i + (hl_peek(h, i+1)=='"' ? 1 : 2);
            u32 m  = at ? HL_S_MULTI|HL_S_DOUBLE : HL_S_ESC;
            if (dl) {
                m |= HL_S_BRACE;
            }
            i = hl_str(h, L, i, j+1, '"', m, HL_STRING);
            operand = 1;
        } else if (c=='#' && (f&HL_F_PP) && first) {
            iz j = hl_skipblank(h, i+1);
            iz w = j;
            while (hl_isalpha(hl_peek(h, j))) {
                j++;
            }
            hl_emit(h, i, j, HL_PREPROC);
            if (hl_wordeq(h, w, j, "include") || hl_wordeq(h, w, j, "import")) {
                iz k = hl_skipblank(h, j);
                if (hl_peek(h, k) == '<') {
                    iz e = k;
                    while (e<n && p[e]!='>' && p[e]!='\n') {
                        e++;
                    }
                    e += hl_peek(h, e)=='>';
                    hl_emit(h, k, e, HL_STRING);
                    j = e;
                }
            }
            i = j;
        } else if (c=='#' && (f&HL_F_ATTRHASH) &&
                (hl_peek(h, i+1)=='[' || (hl_peek(h, i+1)=='!' && hl_peek(h, i+2)=='['))) {
            iz  j = i + (hl_peek(h, i+1)=='!' ? 3 : 2);
            i32 d = 1;
            while (j<n && d>0) {
                d += p[j]=='[';
                d -= p[j]==']';
                j++;
            }
            hl_emit(h, i, j, HL_PREPROC);
            i = j;
        } else if (c=='@' && (f&HL_F_ATAT) && hl_idstart(hl_peek(h, i+1))) {
            iz j = i + 1;
            while (hl_idchar(hl_peek(h, j)) || hl_peek(h, j)=='.') {
                j++;
            }
            hl_emit(h, i, j, HL_PREPROC);
            i = j;
        } else if (c=='[' && first && (f&HL_F_ATTRBR)) {
            iz j = i + 1;
            while (j<n && p[j]!=']' && p[j]!='\n') {
                j++;
            }
            iz k = hl_skipblank(h, j+1);
            if (hl_peek(h, j)==']' && (k>=n || p[k]=='\n')) {
                hl_emit(h, i, j+1, HL_PREPROC);
                i = j + 1;
            } else {
                i++;
            }
        } else if (hl_numstart(h, i)) {
            iz e = hl_num_end(h, i, (f&HL_F_SEP) != 0);
            hl_emit(h, i, e, HL_NUMBER);
            i = e;
            operand = 1;
        } else if (hl_idstart(c) || (c=='$' && (f&HL_F_DOLLAR))) {
            iz b = i;
            for (i++; i<n && (hl_idchar(p[i]) || (p[i]=='$' && (f&HL_F_DOLLAR))); i++) {}
            u8 q = hl_peek(h, i);
            iz e = 0;
            if ((f&HL_F_RAWCPP) && q=='"' && p[i-1]=='R' && i-b<=3) {
                b32 ok = 1;
                for (iz k = b; k < i-1; k++) {
                    ok = ok && (p[k]=='L' || p[k]=='u' || p[k]=='U' || p[k]=='8');
                }
                e = ok ? hl_raw_cpp(h, i) : 0;
            } else if ((f&HL_F_RAWRUST) && (q=='"' || q=='#') &&
                    (hl_wordeq(h, b, i, "r") || hl_wordeq(h, b, i, "br") || hl_wordeq(h, b, i, "cr"))) {
                e = hl_raw_rust(h, i);
            }
            if (e) {
                hl_emit(h, b, e, HL_STRING);
                i = e;
                operand = 1;
                continue;
            }
            HlKind k   = HL_PLAIN;
            b32    dot = (b>0 && p[b-1]=='.' && !(b>1 && p[b-2]=='.')) ||
                          (b>1 && p[b-1]=='>' && p[b-2]=='-');
            if (!dot) {
                k = hl_lookup(h, b, i);
            }
            if (k==HL_FUNCTION && q!='(') {
                k = HL_PLAIN;
            }
            if (k == HL_PLAIN) {
                if ((f&HL_F_MACRO) && q=='!' && (hl_peek(h, i+1)=='(' ||
                        hl_peek(h, i+1)=='[' || hl_peek(h, i+1)=='{')) {
                    i++;
                    k = HL_PREPROC;
                } else if (q=='(' || wasdef) {
                    k = HL_FUNCTION;
                } else if (f & HL_F_JSON) {
                    iz j = hl_skipblank(h, i);
                    k = hl_peek(h, j)==':' ? HL_PROPERTY : HL_PLAIN;
                }
            }
            hl_emit(h, b, i, k);
            deff    = k==HL_KEYWORD && (hl_wordeq(h, b, i, "fn") ||
                        hl_wordeq(h, b, i, "func") || hl_wordeq(h, b, i, "function"));
            operand = k != HL_KEYWORD;
        } else {
            if (c == '{') {
                braces++;
            } else if (c == '}') {
                if (braces==0 && nested) {
                    return i;
                }
                braces -= braces > 0;
            }
            operand = c==')' || c==']' || c=='}';
            i++;
        }
    }
    return n;
}


// Python

static iz hl_py_str(HlCtx *h, HlLang const *L, iz b, iz q) {
    u32 m = HL_S_ESC;
    for (iz k = b; k < q; k++) {
        u8 c = (u8)(h->p[k] | 32);
        if (c == 'r') {
            m = (m & ~(u32)HL_S_ESC) | HL_S_RAW;
        } else if (c == 'f') {
            m |= HL_S_BRACE;
        }
    }
    u8 d = h->p[q];
    iz o = 1;
    if (hl_peek(h, q+1)==d && hl_peek(h, q+2)==d) {
        m |= HL_S_TRIPLE | HL_S_MULTI;
        o  = 3;
    }
    return hl_str(h, L, b, q+o, d, m, HL_STRING);
}

static iz hl_lex_py(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    u8 const *p      = h->p;
    iz        n      = h->n;
    iz        braces = 0;
    b32       bol    = 1;
    HlKind    deff   = HL_PLAIN;
    while (i < n) {
        u8 c = p[i];
        if (c == '\n') {
            bol = 1;
            i++;
            continue;
        }
        if (hl_isblank(c) || c=='\f' || c=='\v') {
            i++;
            continue;
        }
        b32 first  = bol;
        HlKind wasdef = deff;
        bol  = 0;
        deff = HL_PLAIN;
        if (c == '#') {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='"' || c=='\'') {
            i = hl_py_str(h, L, i, i);
        } else if (hl_numstart(h, i)) {
            iz e = hl_num_end(h, i, 0);
            hl_emit(h, i, e, HL_NUMBER);
            i = e;
        } else if (c=='@' && first && hl_idstart(hl_peek(h, i+1))) {
            iz j = i + 1;
            while (hl_idchar(hl_peek(h, j)) || hl_peek(h, j)=='.') {
                j++;
            }
            hl_emit(h, i, j, HL_PREPROC);
            i = j;
        } else if (hl_idstart(c)) {
            iz b = i;
            for (i++; i<n && hl_idchar(p[i]); i++) {}
            u8 q = hl_peek(h, i);
            if ((q=='"' || q=='\'') && i-b<=2) {
                b32 ok = 1;
                for (iz k = b; k < i; k++) {
                    u8 d = (u8)(p[k] | 32);
                    ok = ok && (d=='r' || d=='b' || d=='u' || d=='f');
                }
                if (ok) {
                    i = hl_py_str(h, L, b, i);
                    continue;
                }
            }
            HlKind k = b>0 && p[b-1]=='.' ? HL_PLAIN : hl_lookup(h, b, i);
            if (k == HL_PLAIN) {
                k = wasdef ? wasdef : q=='(' ? HL_FUNCTION : HL_PLAIN;
            } else if (k == HL_FUNCTION) {
                k = HL_PLAIN;
            }
            hl_emit(h, b, i, k);
            deff = hl_wordeq(h, b, i, "def") ? HL_FUNCTION : hl_wordeq(h, b, i, "class") ? HL_TYPE : HL_PLAIN;
        } else {
            if (c == '{') {
                braces++;
            } else if (c == '}') {
                if (braces==0 && nested) {
                    return i;
                }
                braces -= braces > 0;
            }
            i++;
        }
    }
    return n;
}


// Shell

static b32 hl_shdelim(u8 c) {
    return hl_isws(c) || c==';' || c=='&' || c=='|' || c=='(' || c==')' ||
        c=='<' || c=='>' || c=='"' || c=='\'' || c=='$' || c=='`' || c=='\\';
}

// Does a # at i start a comment (it must start a word)?
static b32 hl_shcomment(HlCtx *h, iz i) {
    u8 c = hl_peek(h, i-1);
    return i==0 || hl_isws(c) || c==';' || c=='&' || c=='|' || c=='(' || c==')' || c=='{';
}

// The here-doc body starting at the line start i. Returns where the
// terminator line ends.
static iz hl_heredoc(HlCtx *h, iz i) {
    iz beg = i;
    while (i < h->n) {
        iz e = hl_eol(h, i);
        iz s = i;
        if (h->hd_tabs) {
            while (s<e && h->p[s]=='\t') {
                s++;
            }
        }
        iz t = e;
        if (t>s && h->p[t-1]=='\r') {
            t--;
        }
        if (t-s==h->hd_len && hl_memeq(h->p+s, h->p+h->hd_beg, h->hd_len)) {
            hl_emit(h, beg, i, HL_STRING);
            h->hd_len = 0;
            return e;
        }
        i = hl_next(h, e);
    }
    hl_emit(h, beg, h->n, HL_STRING);
    h->hd_len = 0;
    return h->n;
}

static iz hl_lex_sh(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p       = h->p;
    iz        n       = h->n;
    b32       cmd     = 1;
    b32       forcase = 0;
    b32       deff    = 0;
    b32       asg     = 0;      // the last token was NAME=
    while (i < n) {
        u8  c      = p[i];
        b32 wasasg = asg && !hl_isws(c);
        asg = 0;
        if (c == '\n') {
            i++;
            if (h->hd_len) {
                i = hl_heredoc(h, i);
            }
            cmd     = 1;
            forcase = 0;
        } else if (hl_isblank(c)) {
            i++;
        } else if (c=='#' && hl_shcomment(h, i)) {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c == '\\') {
            i = hl_esc_end(h, i);
        } else if (c == '\'') {
            i = hl_str(h, L, i, i+1, '\'', HL_S_MULTI, HL_STRING);
            cmd = 0;
        } else if (c == '"') {
            i = hl_str(h, L, i, i+1, '"', HL_S_ESC|HL_S_MULTI|HL_S_VAR, HL_STRING);
            cmd = 0;
        } else if (c == '$') {
            u8 d = hl_peek(h, i+1);
            if (d == '\'') {
                i = hl_str(h, L, i, i+2, '\'', HL_S_ESC|HL_S_MULTI, HL_STRING);
            } else if (d == '"') {
                i = hl_str(h, L, i, i+2, '"', HL_S_ESC|HL_S_MULTI|HL_S_VAR, HL_STRING);
            } else {
                iz e = hl_var_end(h, i, L->var);
                hl_emit(h, i, e, HL_VARIABLE);
                i = max(e, i+1);
            }
            cmd = 0;
        } else if (c==';' || c=='&' || c=='|' || c=='(' || c==')') {
            cmd = 1;
            if (c != ')') {
                forcase = 0;
            }
            i++;
        } else if (c=='<' && hl_peek(h, i+1)=='<' && hl_peek(h, i+2)!='<' && hl_peek(h, i-1)!='<') {
            iz  j    = i + 2;
            b32 tabs = 0;
            if (hl_peek(h, j) == '-') {
                tabs = 1;
                j++;
            }
            j = hl_skipblank(h, j);
            u8  q  = hl_peek(h, j);
            iz  db = j;
            iz  de = j;
            if (q=='\'' || q=='"') {
                db = de = j + 1;
                while (de<n && p[de]!=q && p[de]!='\n') {
                    de++;
                }
                j = de + (hl_peek(h, de)==q);
            } else if (hl_idstart(q)) {
                while (de<n && (hl_idchar(p[de]) || p[de]=='-' || p[de]=='.')) {
                    de++;
                }
                j = de;
            }
            if (de > db) {
                h->hd_beg  = db;
                h->hd_len  = de - db;
                h->hd_tabs = tabs;
                i = j;
            } else {
                i += 2;
            }
        } else if (c=='<' || c=='>' || c=='`') {
            i++;
        } else {
            iz b = i;
            if (cmd && hl_idstart(c)) {
                iz j = i;
                while (hl_idchar(hl_peek(h, j))) {
                    j++;
                }
                if (hl_peek(h, j)=='=' || (hl_peek(h, j)=='+' && hl_peek(h, j+1)=='=')) {
                    hl_emit(h, i, j, HL_VARIABLE);
                    i   = j + 1 + (p[j]=='+');
                    asg = 1;
                    continue;
                }
            }
            for (i++; i<n && !hl_shdelim(p[i]); i++) {}
            HlKind k = HL_PLAIN;
            if (deff) {
                k    = HL_FUNCTION;
                deff = 0;
                cmd  = 0;
            } else if (cmd) {
                k = hl_lookup(h, b, i);
                if (k==HL_PLAIN && hl_peek(h, i)=='(') {
                    k = HL_FUNCTION;
                }
                if (k == HL_KEYWORD) {
                    b32 loop = hl_wordeq(h, b, i, "for") || hl_wordeq(h, b, i, "case") ||
                               hl_wordeq(h, b, i, "select");
                    forcase = loop;
                    deff    = hl_wordeq(h, b, i, "function");
                    cmd     = !loop && !deff;
                } else {
                    cmd = 0;
                }
            } else if (forcase && hl_wordeq(h, b, i, "in")) {
                k = HL_KEYWORD;
            } else {
                b32 num = 1;
                for (iz j = b; j < i; j++) {
                    num = num && hl_isdig(p[j]);
                }
                if (num) {
                    k = HL_NUMBER;
                }
            }
            hl_emit(h, b, i, k);
        }
        if (wasasg) {
            cmd = 1;
        }
    }
    return n;
}

// Only the lines with a prompt are commands. A block without any prompt is
// taken as a script.
static iz hl_lex_console(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    iz n    = h->n;
    b32 any = 0;
    for (iz j = i; j < n && !any; j = hl_next(h, hl_eol(h, j))) {
        any = hl_starts(h, j, "$ ") || hl_starts(h, j, "# ") || hl_starts(h, j, "% ") ||
              hl_starts(h, j, "> ");
    }
    if (!any) {
        return hl_lex_sh(h, L, i, 0);
    }
    while (i < n) {
        iz e = hl_eol(h, i);
        if (hl_starts(h, i, "$ ") || hl_starts(h, i, "# ") || hl_starts(h, i, "% ")) {
            h->n      = e;
            h->hd_len = 0;
            hl_lex_sh(h, L, i+2, 0);
            h->n = n;
        }
        i = hl_next(h, e);
    }
    return n;
}


// PowerShell

static iz hl_lex_ps(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p    = h->p;
    iz        n    = h->n;
    b32       deff = 0;
    while (i < n) {
        u8 c = p[i];
        if (hl_isws(c)) {
            i++;
            continue;
        }
        b32 wasdef = deff;
        deff = 0;
        if (c=='<' && hl_peek(h, i+1)=='#') {
            iz e = hl_find(h, i+2, "#>");
            e = e<n ? e+2 : n;
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='#' && hl_shcomment(h, i)) {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='@' && (hl_peek(h, i+1)=='"' || hl_peek(h, i+1)=='\'') &&
                hl_peek(h, hl_skipblank(h, i+2))=='\n') {
            char t[4] = {'\n', (char)hl_peek(h, i+1), '@', 0};
            iz   e    = hl_findmem(h, i+2, (u8 const *)t, 3, 0);
            e = e<n ? e+3 : n;
            hl_emit(h, i, e, HL_STRING);
            i = e;
        } else if (c == '\'') {
            i = hl_str(h, L, i, i+1, '\'', HL_S_MULTI|HL_S_DOUBLE, HL_STRING);
        } else if (c == '"') {
            i = hl_str(h, L, i, i+1, '"', HL_S_ESC|HL_S_BT|HL_S_MULTI|HL_S_DOUBLE|HL_S_VAR, HL_STRING);
        } else if (c == '$') {
            iz e = hl_var_end(h, i, 1);
            if (e > i) {
                HlKind k = e-i>1 && p[i+1]!='{' && p[i+1]!='(' ? hl_lookup(h, i+1, e) : HL_PLAIN;
                hl_emit(h, i, e, k==HL_CONSTANT ? HL_CONSTANT : HL_VARIABLE);
                i = e;
            } else {
                i++;
            }
        } else if (c == '`') {
            i = hl_esc_end(h, i);
        } else if (c=='[' && hl_isalpha(hl_peek(h, i+1))) {
            iz j = i + 1;
            while (hl_idchar(hl_peek(h, j)) || hl_peek(h, j)=='.') {
                j++;
            }
            if (hl_peek(h, j)=='[' && hl_peek(h, j+1)==']') {
                j += 2;
            }
            if (hl_peek(h, j) == ']') {
                hl_emit(h, i, j+1, HL_TYPE);
                i = j + 1;
            } else {
                i++;
            }
        } else if (c=='-' && hl_isalpha(hl_peek(h, i+1)) && hl_isws(hl_peek(h, i-1))) {
            iz j = i + 1;
            while (hl_isalpha(hl_peek(h, j))) {
                j++;
            }
            hl_emit(h, i, j, hl_lookup(h, i, j)==HL_KEYWORD ? HL_KEYWORD : HL_PLAIN);
            i = j;
        } else if (hl_isdig(c)) {
            iz e = hl_num_end(h, i, 0);
            hl_emit(h, i, e, HL_NUMBER);
            i = e;
        } else if (hl_idstart(c)) {
            iz b = i;
            for (i++; i<n && hl_idchar(p[i]); i++) {}
            if (wasdef) {
                while (i<n && (hl_idchar(p[i]) || p[i]=='-' || p[i]==':')) {
                    i++;
                }
                hl_emit(h, b, i, HL_FUNCTION);
                continue;
            }
            if (hl_peek(h, i)=='-' && hl_idstart(hl_peek(h, i+1)) &&
                    (hl_lookup(h, b, i)==HL_FUNCTION || hl_wordeq(h, b, i, "foreach"))) {
                iz j = i + 1;
                while (hl_idchar(hl_peek(h, j))) {
                    j++;
                }
                hl_emit(h, b, j, HL_FUNCTION);
                i = j;
                continue;
            }
            HlKind k = hl_lookup(h, b, i);
            if (k == HL_FUNCTION) {
                k = HL_PLAIN;
            }
            hl_emit(h, b, i, k);
            deff = hl_wordeq(h, b, i, "function") || hl_wordeq(h, b, i, "filter");
        } else {
            i++;
        }
    }
    return n;
}


// Make

// The first : = or # that is not inside $( ) or ${ }, or e.
static iz hl_mk_scan(HlCtx *h, iz s, iz e) {
    iz d = 0;
    for (iz j = s; j < e; j++) {
        u8 c = h->p[j];
        if (c=='$' && (hl_peek(h, j+1)=='(' || hl_peek(h, j+1)=='{')) {
            d++;
            j++;
        } else if ((c==')' || c=='}') && d>0) {
            d--;
        } else if (d==0 && (c==':' || c=='=' || c=='#')) {
            return j;
        }
    }
    return e;
}

// Variables, comments, and (in recipes) quoted strings in [i, e); other
// text gets kind dflt.
static void hl_mk_expr(HlCtx *h, HlLang const *L, iz i, iz e, HlKind dflt, b32 recipe) {
    u8 const *p = h->p;
    while (i < e) {
        u8 c = p[i];
        if (hl_isblank(c)) {
            i++;
        } else if (c == '$') {
            iz j = hl_var_end(h, i, 0);
            hl_emit(h, i, j, HL_VARIABLE);
            i = max(j, i+1);
        } else if (c == '#') {
            hl_emit(h, i, e, HL_COMMENT);
            return;
        } else if (recipe && (c=='"' || c=='\'')) {
            i = hl_str(h, L, i, i+1, c, c=='"' ? HL_S_ESC|HL_S_VAR : 0, HL_STRING);
        } else {
            iz j = i + 1;
            while (j<e && !hl_isblank(p[j]) && p[j]!='$' && p[j]!='#' &&
                    !(recipe && (p[j]=='"' || p[j]=='\''))) {
                j++;
            }
            hl_emit(h, i, j, dflt);
            i = j;
        }
    }
}

static void hl_mk_line(HlCtx *h, HlLang const *L, iz b, iz e, b32 cont) {
    u8 const *p = h->p;
    if (cont) {
        hl_mk_expr(h, L, b, e, HL_PLAIN, 0);
        return;
    }
    if (p[b] == '\t') {
        hl_mk_expr(h, L, b, e, HL_PLAIN, 1);
        return;
    }
    iz s = hl_skipblank(h, b);
    if (s >= e) {
        return;
    }
    if (p[s] == '#') {
        hl_emit(h, s, e, HL_COMMENT);
        return;
    }
    iz w = s;
    while (w<e && (hl_isalpha(p[w]) || p[w]=='-')) {
        w++;
    }
    if (hl_lookup(h, s, w)==HL_KEYWORD && (w==e || hl_isblank(p[w]))) {
        hl_emit(h, s, w, HL_KEYWORD);
        if (hl_starts(h, s, "if") || hl_starts(h, s, "else")) {
            hl_mk_expr(h, L, w, e, HL_PLAIN, 0);
            return;
        }
        s = hl_skipblank(h, w);
    }
    iz j = hl_mk_scan(h, s, e);
    if (j<e && p[j]!='#') {
        b32 assign = p[j] == '=';
        if (p[j]==':' && (hl_peek(h, j+1)=='=' || (hl_peek(h, j+1)==':' && hl_peek(h, j+2)=='='))) {
            assign = 1;
        }
        if (assign) {
            iz ne = j;
            while (ne>s && (p[ne-1]=='+' || p[ne-1]=='?' || p[ne-1]=='!' || p[ne-1]==':')) {
                ne--;
            }
            hl_mk_expr(h, L, s, hl_trimend(h, s, ne), HL_VARIABLE, 0);
            hl_mk_expr(h, L, ne, e, HL_PLAIN, 0);
            return;
        }
        HlKind k = p[s]=='.' && hl_isalpha(hl_peek(h, s+1)) && hl_peek(h, s+1)<='Z' ?
            HL_PREPROC : HL_FUNCTION;
        hl_mk_expr(h, L, s, j, k, 0);
        hl_mk_expr(h, L, j+1, e, HL_PLAIN, 0);
        return;
    }
    hl_mk_expr(h, L, s, e, HL_PLAIN, 0);
}

static iz hl_lex_make(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    b32 cont = 0;
    while (i < h->n) {
        iz e = hl_eol(h, i);
        hl_mk_line(h, L, i, e, cont);
        iz t = hl_trimend(h, i, e);
        cont = t>i && h->p[t-1]=='\\';
        i = hl_next(h, e);
    }
    return h->n;
}


// CMake

static iz hl_lex_cmake(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p = h->p;
    iz        n = h->n;
    while (i < n) {
        u8 c = p[i];
        if (hl_isws(c)) {
            i++;
            continue;
        }
        // A bracket opener [==[ at j: its level, or -1.
        iz lvl = -1;
        iz j   = c=='#' ? i+1 : i;
        if ((c=='#' || c=='[') && hl_peek(h, j)=='[') {
            iz k = j + 1;
            while (hl_peek(h, k) == '=') {
                k++;
            }
            if (hl_peek(h, k) == '[') {
                lvl = k - j - 1;
                j   = k + 1;
            }
        }
        if (lvl >= 0) {
            iz e = n;
            for (iz k = j; k < n; k++) {
                if (p[k] == ']') {
                    iz t = k + 1;
                    while (t<n && p[t]=='=') {
                        t++;
                    }
                    if (t-(k+1)==lvl && hl_peek(h, t)==']') {
                        e = t + 1;
                        break;
                    }
                    k = t - 1;
                }
            }
            hl_emit(h, i, e, c=='#' ? HL_COMMENT : HL_STRING);
            i = e;
        } else if (c == '#') {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c == '"') {
            i = hl_str(h, L, i, i+1, '"', HL_S_ESC|HL_S_MULTI|HL_S_VAR, HL_STRING);
        } else if (c == '$') {
            iz e = hl_var_end(h, i, 2);
            hl_emit(h, i, e, HL_VARIABLE);
            i = max(e, i+1);
        } else if (c == '\\') {
            i = hl_esc_end(h, i);
        } else if (hl_isalnum(c) || c=='_') {
            iz b = i;
            while (i<n && (hl_isalnum(p[i]) || p[i]=='_' || p[i]=='.' || p[i]=='+' ||
                    p[i]=='-' || p[i]=='/' || p[i]==':')) {
                i++;
            }
            iz k = hl_skipblank(h, i);
            HlKind kind = hl_lookup(h, b, i);
            if (hl_peek(h, k) == '(') {
                kind = kind==HL_KEYWORD ? HL_KEYWORD : HL_FUNCTION;
            } else if (kind == HL_FUNCTION) {
                kind = HL_PLAIN;
            }
            if (kind==HL_PLAIN && hl_isdig(p[b])) {
                kind = HL_NUMBER;
                for (iz t = b; t < i; t++) {
                    if (!hl_isdig(p[t]) && p[t]!='.') {
                        kind = HL_PLAIN;
                    }
                }
            }
            hl_emit(h, b, i, kind);
        } else {
            i++;
        }
    }
    return n;
}


// TOML

// The [table] or [[array]] header at i, or i if it is no header.
static iz hl_toml_header(HlCtx *h, iz i) {
    b32 dbl = hl_peek(h, i+1) == '[';
    for (iz k = i + 1 + dbl; k < h->n; k++) {
        u8 c = h->p[k];
        if (c=='\n' || c==',' || c=='=') {
            return i;
        }
        if (c == ']') {
            iz m = k + 1;
            if (dbl && hl_peek(h, m)==']') {
                m++;
            }
            iz t = hl_skipblank(h, m);
            return t>=h->n || h->p[t]=='\n' || h->p[t]=='#' ? m : i;
        }
    }
    return i;
}

static iz hl_lex_toml(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p   = h->p;
    iz        n   = h->n;
    b32       bol = 1;
    while (i < n) {
        u8 c = p[i];
        if (c == '\n') {
            bol = 1;
            i++;
            continue;
        }
        if (hl_isblank(c)) {
            i++;
            continue;
        }
        b32 first = bol;
        bol = 0;
        if (c == '#') {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='[' && first && hl_toml_header(h, i)>i) {
            iz e = hl_toml_header(h, i);
            hl_emit(h, i, e, HL_TAG);
            i = e;
        } else if (c=='"' || c=='\'') {
            u32 m = c=='"' ? HL_S_ESC : 0;
            iz  o = 1;
            if (hl_peek(h, i+1)==c && hl_peek(h, i+2)==c) {
                m |= HL_S_TRIPLE | HL_S_MULTI;
                o  = 3;
            }
            HlKind k = o==1 && hl_key_follows(h, i, c, '=') ? HL_PROPERTY : HL_STRING;
            i = hl_str(h, L, i, i+o, c, m, k);
            if (k == HL_PROPERTY) {
                continue;
            }
        } else if (hl_isalnum(c) || c=='_' || c=='-' || c=='+' || c=='.') {
            iz b = i;
            for (i++; i<n && (hl_isalnum(p[i]) || p[i]=='_' || p[i]=='-' || p[i]=='+' ||
                    p[i]=='.' || p[i]==':'); i++) {}
            iz     k    = hl_skipblank(h, i);
            HlKind kind = hl_lookup(h, b, i);
            if (hl_peek(h, k) == '=') {
                kind = HL_PROPERTY;
            } else if (kind==HL_PLAIN && (hl_isdig(p[b]) || (b+1<i && hl_isdig(p[b+1])))) {
                kind = HL_NUMBER;
            }
            hl_emit(h, b, i, kind);
        } else {
            i++;
        }
    }
    return n;
}


// YAML

static HlKind hl_yaml_word(HlCtx *h, iz b, iz e) {
    HlKind k = hl_lookup(h, b, e);
    if (k != HL_PLAIN) {
        return k;
    }
    u8 c = h->p[b];
    if (c=='~' && e-b==1) {
        return HL_CONSTANT;
    }
    iz d = b + ((c=='-' || c=='+' || c=='.') && b+1<e);
    if (!hl_isdig(h->p[d])) {
        return HL_PLAIN;
    }
    for (iz j = d; j < e; j++) {
        u8 x = h->p[j];
        if (!hl_isalnum(x) && x!='.' && x!='_' && x!='-' && x!='+' && x!=':') {
            return HL_PLAIN;
        }
    }
    return HL_NUMBER;
}

// The colon ending a block key that starts at s, or -1.
static iz hl_yaml_key(HlCtx *h, iz s, iz e) {
    u8 const *p = h->p;
    u8 c = p[s];
    if (c=='[' || c=='{' || c=='|' || c=='>' || c=='&' || c=='*' || c=='!' || c=='#') {
        return -1;
    }
    iz k = s;
    if (c=='"' || c=='\'') {
        for (k++; k<e && p[k]!=c; k += (c=='"' && p[k]=='\\') ? 2 : 1) {}
        if (k >= e) {
            return -1;
        }
        k = hl_skipblank(h, k+1);
        return k<e && p[k]==':' && hl_isws(hl_peek(h, k+1)) ? k : -1;
    }
    for (; k < e; k++) {
        if (p[k]==':' && hl_isws(hl_peek(h, k+1))) {
            return k;
        }
        if (p[k]=='#' && k>s && hl_isblank(p[k-1])) {
            return -1;
        }
    }
    return -1;
}

// The rest of a line after its key, a "- ", or "---". col is the indent
// of the node, for block scalars. Returns the start of the next line to
// lex.
static iz hl_yaml_value(HlCtx *h, HlLang const *L, iz s, iz e, iz col) {
    u8 const *p     = h->p;
    b32       block = 0;
    while (s < e) {
        u8 c = p[s];
        if (hl_isblank(c)) {
            s++;
        } else if (c=='#' && hl_isws(hl_peek(h, s-1))) {
            hl_emit(h, s, e, HL_COMMENT);
            s = e;
        } else if (c == '"') {
            s = hl_str(h, L, s, s+1, '"', HL_S_ESC, HL_STRING);
        } else if (c == '\'') {
            s = hl_str(h, L, s, s+1, '\'', HL_S_DOUBLE, HL_STRING);
        } else if (c=='&' || c=='*' || c=='!') {
            iz j = s + 1;
            while (j<e && !hl_isws(p[j]) && p[j]!=',' && p[j]!=']' && p[j]!='}') {
                j++;
            }
            hl_emit(h, s, j, c=='!' ? HL_TYPE : HL_VARIABLE);
            s = j;
        } else if (c=='|' || c=='>') {
            iz j = s + 1;
            while (j<e && (p[j]=='+' || p[j]=='-' || hl_isdig(p[j]))) {
                j++;
            }
            if (j==e || hl_isws(p[j])) {
                hl_emit(h, s, j, HL_KEYWORD);
                block = 1;
                s = j;
            } else {
                hl_emit(h, s, j, HL_PLAIN);
                s = j;
            }
        } else if (c==',' || c=='[' || c==']' || c=='{' || c=='}') {
            s++;
        } else {
            iz  j   = s;
            b32 key = 0;
            for (; j < e; j++) {
                u8 d = p[j];
                if (hl_isblank(d) || d==',' || d==']' || d=='}') {
                    break;
                }
                if (d==':' && hl_isws(hl_peek(h, j+1))) {
                    key = 1;
                    break;
                }
            }
            if (j == s) {
                s++;
                continue;
            }
            hl_emit(h, s, j, key ? HL_PROPERTY : hl_yaml_word(h, s, j));
            s = j + key;
        }
    }
    if (!block) {
        return hl_next(h, e);
    }
    iz first = hl_next(h, e);
    iz last  = -1;
    iz j     = first;
    while (j < h->n) {
        iz le = hl_eol(h, j);
        iz t  = j;
        while (t<le && p[t]==' ') {
            t++;
        }
        iz u = hl_skipblank(h, t);
        if (u < le) {
            if (t-j <= col) {
                break;
            }
            last = le;
        }
        j = hl_next(h, le);
    }
    if (last < 0) {
        return first;
    }
    hl_emit(h, first, last, HL_STRING);
    return hl_next(h, last);
}

static iz hl_lex_yaml(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p = h->p;
    iz        n = h->n;
    while (i < n) {
        iz e = hl_eol(h, i);
        if ((hl_starts(h, i, "---") || hl_starts(h, i, "...")) && hl_isws(hl_peek(h, i+3))) {
            hl_emit(h, i, i+3, HL_PREPROC);
            i = hl_yaml_value(h, L, i+3, e, 0);
            continue;
        }
        if (p[i] == '%') {
            hl_emit(h, i, e, HL_PREPROC);
            i = hl_next(h, e);
            continue;
        }
        iz s   = hl_skipblank(h, i);
        iz col = s - i;
        while (s<e && p[s]=='-' && hl_isws(hl_peek(h, s+1))) {
            col = s - i;
            s   = hl_skipblank(h, s+1);
        }
        if (s >= e) {
            i = hl_next(h, e);
            continue;
        }
        iz k = hl_yaml_key(h, s, e);
        if (k >= 0) {
            hl_emit(h, s, hl_trimend(h, s, k), HL_PROPERTY);
            col = s - i;
            s   = k + 1;
        }
        i = hl_yaml_value(h, L, s, e, col);
    }
    return n;
}


// INI

static iz hl_lex_ini(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)L;
    (void)nested;
    u8 const *p = h->p;
    iz        n = h->n;
    while (i < n) {
        iz e = hl_eol(h, i);
        iz s = hl_skipblank(h, i);
        iz t = hl_trimend(h, s, e);
        if (s < t) {
            if (p[s]==';' || p[s]=='#') {
                hl_emit(h, s, t, HL_COMMENT);
            } else if (p[s] == '[') {
                iz k = s + 1;
                while (k<t && p[k]!=']') {
                    k++;
                }
                if (k < t) {
                    hl_emit(h, s, k+1, HL_TAG);
                }
            } else {
                iz k = s;
                while (k<t && p[k]!='=' && p[k]!=':') {
                    k++;
                }
                if (k < t) {
                    hl_emit(h, s, hl_trimend(h, s, k), HL_PROPERTY);
                    iz v = hl_skipblank(h, k+1);
                    if (v < t) {
                        HlKind kind = hl_lookup(h, v, t);
                        if (p[v]=='"' || p[v]=='\'') {
                            kind = HL_STRING;
                        } else if (kind==HL_PLAIN && hl_isdig(p[v])) {
                            kind = HL_NUMBER;
                            for (iz j = v; j < t; j++) {
                                if (!hl_isdig(p[j]) && p[j]!='.') {
                                    kind = HL_PLAIN;
                                }
                            }
                        }
                        hl_emit(h, v, t, kind);
                    }
                }
            }
        }
        i = hl_next(h, e);
    }
    return n;
}


// diff

static iz hl_lex_diff(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)L;
    (void)nested;
    static char const *const heads[] = {
        "diff ", "index ", "new file", "deleted file", "old mode", "new mode",
        "similarity", "dissimilarity", "rename ", "copy ", "Binary files",
        "Index: ", "====", "*** ",
    };
    u8 const *p = h->p;
    iz        n = h->n;
    b32       hdr = 0;      // the previous line was a "--- " header
    while (i < n) {
        iz     e = hl_eol(h, i);
        iz     t = e - (e>i && p[e-1]=='\r');
        HlKind k = HL_PLAIN;
        b32    minus3 = 0;
        u8     c = p[i];
        if (c == '+') {
            k = hl_starts(h, i, "+++ ") && hdr ? HL_META : HL_INSERTED;
        } else if (c == '-') {
            minus3 = hl_starts(h, i, "--- ") && hl_starts(h, hl_next(h, e), "+++ ");
            k = minus3 ? HL_META : HL_DELETED;
        } else if (c=='@' && hl_peek(h, i+1)=='@') {
            k = HL_META;
        } else if (c == '\\') {
            k = HL_COMMENT;
        } else if (hl_isalpha(c) || c=='=' || c=='*') {
            for (i32 j = 0; j < (i32)countof(heads); j++) {
                if (hl_starts(h, i, heads[j])) {
                    k = HL_META;
                }
            }
        }
        hl_emit(h, i, t, k);
        hdr = minus3;
        i = hl_next(h, e);
    }
    return n;
}


// HTML and XML

static iz hl_lex_html(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p = h->p;
    iz        n = h->n;
    while (i < n) {
        if (p[i] == '&') {
            iz j = i + 1 + (hl_peek(h, i+1)=='#');
            iz k = j;
            while (k<n && k<j+10 && hl_isalnum(p[k])) {
                k++;
            }
            if (k>j && hl_peek(h, k)==';') {
                hl_emit(h, i, k+1, HL_ESCAPE);
                i = k + 1;
            } else {
                i++;
            }
            continue;
        }
        if (p[i] != '<') {
            i++;
            continue;
        }
        if (hl_starts(h, i, "<!--")) {
            iz e = hl_find(h, i+4, "-->");
            e = e<n ? e+3 : n;
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (hl_starts(h, i, "<![CDATA[")) {
            iz e = hl_find(h, i+9, "]]>");
            hl_emit(h, i, i+9, HL_PREPROC);
            hl_emit(h, i+9, e, HL_STRING);
            iz t = e<n ? e+3 : n;
            hl_emit(h, e, t, HL_PREPROC);
            i = t;
        } else if (hl_peek(h, i+1)=='!' || hl_peek(h, i+1)=='?') {
            iz e = i + 2;
            while (e<n && p[e]!='>') {
                e++;
            }
            e = min(e+1, n);
            hl_emit(h, i, e, HL_PREPROC);
            i = e;
        } else {
            iz  j     = i + 1;
            b32 close = hl_peek(h, j) == '/';
            j += close;
            if (!hl_idstart(hl_peek(h, j))) {
                i++;
                continue;
            }
            iz nb = j;
            while (j<n && (hl_idchar(p[j]) || p[j]==':' || p[j]=='-' || p[j]=='.')) {
                j++;
            }
            iz  ne   = j;
            b32 self = 0;
            hl_emit(h, i, j, HL_TAG);
            for (;;) {
                while (j<n && hl_isws(p[j])) {
                    j++;
                }
                if (j >= n || p[j]=='<') {
                    break;
                }
                u8 c = p[j];
                if (c == '>') {
                    hl_emit(h, j, j+1, HL_TAG);
                    j++;
                    break;
                }
                if (c=='/' && hl_peek(h, j+1)=='>') {
                    hl_emit(h, j, j+2, HL_TAG);
                    self = 1;
                    j += 2;
                    break;
                }
                iz ab = j;
                while (j<n && !hl_isws(p[j]) && p[j]!='=' && p[j]!='>' && p[j]!='<' &&
                        p[j]!='"' && p[j]!='\'' && !(p[j]=='/' && hl_peek(h, j+1)=='>')) {
                    j++;
                }
                hl_emit(h, ab, j, HL_PROPERTY);
                iz k = j;
                while (k<n && hl_isws(p[k])) {
                    k++;
                }
                if (hl_peek(h, k) == '=') {
                    j = k + 1;
                    while (j<n && hl_isws(p[j])) {
                        j++;
                    }
                    u8 q = hl_peek(h, j);
                    if (q=='"' || q=='\'') {
                        j = hl_str(h, L, j, j+1, q, HL_S_MULTI, HL_STRING);
                    } else {
                        iz vb = j;
                        while (j<n && !hl_isws(p[j]) && p[j]!='>') {
                            j++;
                        }
                        hl_emit(h, vb, j, HL_STRING);
                    }
                } else if (j == ab) {
                    j++;
                }
            }
            i = j;
            b32 script = hl_wordeq(h, nb, ne, "script");
            if (!close && !self && (script || hl_wordeq(h, nb, ne, "style"))) {
                iz e = hl_findmem(h, i, (u8 const *)(script ? "</script" : "</style"),
                        script ? 8 : 7, 1);
                if (script) {
                    h->n = e;
                    hl_lex_c(h, &hl_langs[HL_L_JS], i, 0);
                    h->n = n;
                }
                i = e;
            }
        }
    }
    return n;
}


// SQL

static iz hl_lex_sql(HlCtx *h, HlLang const *L, iz i, b32 nested) {
    (void)nested;
    u8 const *p = h->p;
    iz        n = h->n;
    while (i < n) {
        u8 c = p[i];
        if (hl_isws(c)) {
            i++;
        } else if (c=='-' && hl_peek(h, i+1)=='-') {
            iz e = hl_eol(h, i);
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='/' && hl_peek(h, i+1)=='*') {
            iz e = hl_find(h, i+2, "*/");
            e = e<n ? e+2 : n;
            hl_emit(h, i, e, HL_COMMENT);
            i = e;
        } else if (c=='\'' || c=='"') {
            i = hl_str(h, L, i, i+1, c, HL_S_MULTI|HL_S_DOUBLE, HL_STRING);
        } else if (c == '$') {
            iz j = i + 1;
            while (j<n && j-i<=16 && hl_idchar(p[j])) {
                j++;
            }
            if (hl_peek(h, j)=='$' && j-i<=16) {
                iz e = hl_findmem(h, j+1, p+i, j+1-i, 0);
                e = e<n ? e+(j+1-i) : n;
                hl_emit(h, i, e, HL_STRING);
                i = e;
            } else {
                i++;
            }
        } else if ((c=='@' || c==':') && hl_idstart(hl_peek(h, i+1)) &&
                !hl_idchar(hl_peek(h, i-1)) && hl_peek(h, i-1)!=':') {
            iz j = i + 1;
            while (hl_idchar(hl_peek(h, j))) {
                j++;
            }
            hl_emit(h, i, j, HL_VARIABLE);
            i = j;
        } else if (hl_numstart(h, i)) {
            iz e = hl_num_end(h, i, 0);
            hl_emit(h, i, e, HL_NUMBER);
            i = e;
        } else if (hl_idstart(c)) {
            iz b = i;
            for (i++; i<n && hl_idchar(p[i]); i++) {}
            HlKind k = hl_lookup(h, b, i);
            if (k==HL_FUNCTION && hl_peek(h, hl_skipblank(h, i))!='(') {
                k = HL_PLAIN;
            }
            if (k==HL_PLAIN && hl_peek(h, i)=='(') {
                k = HL_FUNCTION;
            }
            hl_emit(h, b, i, k);
        } else {
            i++;
        }
    }
    return n;
}


// Word lists

#define HL_C_WORDS \
    "auto break case const continue default do else enum extern for goto if " \
    "inline register restrict return sizeof static struct switch typedef " \
    "union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn " \
    "_Static_assert _Thread_local alignas alignof constexpr static_assert " \
    "thread_local typeof " \
    "| void char short int long float double signed unsigned bool _Bool " \
    "_Complex size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t " \
    "int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t FILE va_list " \
    "| NULL true false nullptr EOF"

#define HL_CPP_WORDS \
    "and and_eq asm bitand bitor catch class compl concept consteval " \
    "constinit const_cast co_await co_return co_yield decltype delete " \
    "dynamic_cast explicit export final friend mutable namespace new " \
    "noexcept not not_eq operator or or_eq override private protected " \
    "public reinterpret_cast requires static_cast template this throw try " \
    "typeid typename using virtual xor xor_eq " \
    "| char8_t char16_t char32_t auto string wstring vector nullptr_t"

#define HL_CS_WORDS \
    "abstract as base break case catch checked class const continue " \
    "default delegate do else enum event explicit extern finally fixed " \
    "for foreach goto if implicit in interface internal is lock namespace " \
    "new operator out override params private protected public readonly " \
    "ref return sealed sizeof stackalloc static struct switch this throw " \
    "try typeof unchecked unsafe using virtual volatile while var async " \
    "await yield record partial nameof where when with init " \
    "| bool byte char decimal double float int long object sbyte short " \
    "string uint ulong ushort void dynamic String Object Int32 Int64 List " \
    "Dictionary Task Action Func " \
    "| true false null"

#define HL_JAVA_WORDS \
    "abstract assert break case catch class const continue default do " \
    "else enum extends final finally for goto if implements import " \
    "instanceof interface native new package private protected public " \
    "return static strictfp super switch synchronized this throw throws " \
    "transient try volatile while var record sealed permits yield " \
    "| boolean byte char double float int long short void String Object " \
    "Integer Long Double Boolean Character List Map Set " \
    "| true false null"

#define HL_RUST_WORDS \
    "as async await break const continue crate dyn else enum extern fn " \
    "for if impl in let loop match mod move mut pub ref return self Self " \
    "static struct super trait type unsafe use where while union " \
    "| i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool " \
    "char str String Vec Option Result Box Rc Arc " \
    "| true false None"

#define HL_GO_WORDS \
    "break case chan const continue default defer else fallthrough for " \
    "func go goto if import interface map package range return select " \
    "struct switch type var " \
    "| bool byte complex64 complex128 error float32 float64 int int8 int16 " \
    "int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any " \
    "comparable " \
    "| true false nil iota " \
    "| make new len cap append copy delete panic recover print println " \
    "close clear min max"

#define HL_JS_WORDS \
    "async await break case catch class const continue debugger default " \
    "delete do else export extends finally for function if import in " \
    "instanceof let new of return static super switch throw try typeof " \
    "var void while with yield " \
    "| Array Boolean Date Error JSON Map Math Number Object Promise " \
    "RegExp Set String Symbol " \
    "| true false null undefined NaN Infinity this"

#define HL_TS_WORDS \
    "abstract as asserts declare enum implements infer interface is keyof " \
    "module namespace override private protected public readonly " \
    "satisfies type " \
    "| any bigint never number object string symbol unknown boolean"

#define HL_PY_WORDS \
    "and as assert async await break class continue def del elif else " \
    "except finally for from global if import in is lambda nonlocal not " \
    "or pass raise return try while with yield " \
    "| int float str bool bytes list dict set tuple object complex " \
    "bytearray frozenset type " \
    "| True False None"

#define HL_SH_WORDS \
    "if then else elif fi for while until do done case esac in function " \
    "select time { } " \
    "| | | alias bg cd command declare echo eval exec exit export fg " \
    "getopts hash jobs kill let local popd printf pushd pwd read " \
    "readonly return set shift source test trap type typeset ulimit " \
    "umask unalias unset wait break continue"

#define HL_PS_WORDS \
    "begin break catch class continue data do dynamicparam else elseif " \
    "end enum exit filter finally for foreach from function hidden if in " \
    "param process return static switch throw trap try until using while " \
    "-eq -ne -gt -ge -lt -le -like -notlike -match -notmatch -contains " \
    "-notcontains -in -notin -replace -is -isnot -as -and -or -xor -not " \
    "-band -bor -bxor -bnot -shl -shr -split -join -f " \
    "| | true false null " \
    "| add clear compare convert copy disable enable enter export find " \
    "format get grant group import invoke join measure move new out pop " \
    "publish push read register remove rename resolve restart resume " \
    "revoke save select send set show sort split start stop suspend test " \
    "unregister update use wait where write"

#define HL_MAKE_WORDS \
    "include -include sinclude ifdef ifndef ifeq ifneq else endif define " \
    "endef export unexport override vpath undefine private"

#define HL_CMAKE_WORDS \
    "if elseif else endif foreach endforeach while endwhile function " \
    "endfunction macro endmacro return break continue block endblock and " \
    "or not equal less greater less_equal greater_equal strless " \
    "strgreater strequal strless_equal strgreater_equal version_less " \
    "version_greater version_equal version_less_equal " \
    "version_greater_equal matches defined exists is_directory " \
    "is_absolute is_newer_than is_symlink command policy target test " \
    "in_list " \
    "| | on off true false yes no ignore notfound"

#define HL_SQL_WORDS \
    "select from where and or not in is like between exists case when " \
    "then else end as on join inner outer left right full cross natural " \
    "using group by order having limit offset union all distinct " \
    "intersect except insert into values update set delete create alter " \
    "drop table view index database schema truncate primary key foreign " \
    "references unique check default constraint add column rename to " \
    "begin commit rollback transaction grant revoke with recursive over " \
    "partition asc desc if cascade temporary temp replace returning " \
    "explain analyze vacuum pragma trigger procedure function return " \
    "declare cursor fetch for each row engine auto_increment " \
    "autoincrement ilike glob " \
    "| int integer bigint smallint tinyint serial boolean bool bit float " \
    "double real decimal numeric char varchar text blob date time " \
    "timestamp datetime interval json jsonb uuid bytea money binary " \
    "varbinary enum " \
    "| null true false unknown " \
    "| count sum avg min max coalesce nullif cast convert concat " \
    "substring length lower upper trim round abs now ifnull isnull " \
    "row_number rank dense_rank lag lead extract date_trunc"


static HlLang const hl_langs[HL_L_COUNT] = {
    [HL_L_C] = {"c h", HL_C_WORDS, 0, hl_lex_c, HL_F_PP, 0, 0},
    [HL_L_CPP] = {"cpp c++ cc cxx hpp hh", HL_C_WORDS, HL_CPP_WORDS, hl_lex_c,
        HL_F_PP|HL_F_RAWCPP|HL_F_SEP, 0, 0},
    [HL_L_CS] = {"cs csharp", HL_CS_WORDS, 0, hl_lex_c,
        HL_F_PP|HL_F_CSHARP|HL_F_ATTRBR|HL_F_TRIPLE, 0, 0},
    [HL_L_JAVA] = {"java", HL_JAVA_WORDS, 0, hl_lex_c,
        HL_F_ATAT|HL_F_TRIPLE|HL_F_DOLLAR, 0, 0},
    [HL_L_RUST] = {"rust rs", HL_RUST_WORDS, 0, hl_lex_c,
        HL_F_RAWRUST|HL_F_LIFETIME|HL_F_MACRO|HL_F_NEST|HL_F_ATTRHASH, 0, 0},
    [HL_L_GO] = {"go golang", HL_GO_WORDS, 0, hl_lex_c, HL_F_BACKTICK, 0, 0},
    [HL_L_JS] = {"js javascript jsx mjs cjs", HL_JS_WORDS, 0, hl_lex_c,
        HL_F_BACKTICK|HL_F_TEMPLATE|HL_F_REGEX|HL_F_SQSTR|HL_F_ATAT|HL_F_DOLLAR, 0, 0},
    [HL_L_TS] = {"ts typescript tsx", HL_JS_WORDS, HL_TS_WORDS, hl_lex_c,
        HL_F_BACKTICK|HL_F_TEMPLATE|HL_F_REGEX|HL_F_SQSTR|HL_F_ATAT|HL_F_DOLLAR, 0, 0},
    [HL_L_JSON] = {"json jsonc json5", "| | true false null NaN Infinity", 0, hl_lex_c,
        HL_F_JSON|HL_F_SQSTR, 0, 0},
    [HL_L_PY] = {"python py python3", HL_PY_WORDS, 0, hl_lex_py, 0, 0, 0},
    [HL_L_SH] = {"sh bash shell zsh", HL_SH_WORDS, 0, hl_lex_sh, 0, 0, 0},
    [HL_L_CONSOLE] = {"console shell-session", HL_SH_WORDS, 0, hl_lex_console, 0, 0, 0},
    [HL_L_PS] = {"powershell ps1 pwsh ps", HL_PS_WORDS, 0, hl_lex_ps, 0, 1, 1},
    [HL_L_MAKE] = {"make makefile mk", HL_MAKE_WORDS, 0, hl_lex_make, 0, 0, 0},
    [HL_L_CMAKE] = {"cmake", HL_CMAKE_WORDS, 0, hl_lex_cmake, 0, 1, 2},
    [HL_L_TOML] = {"toml", "| | true false inf nan", 0, hl_lex_toml, 0, 0, 0},
    [HL_L_YAML] = {"yaml yml", "| | true false null", 0, hl_lex_yaml, 0, 1, 0},
    [HL_L_INI] = {"ini cfg conf properties", "| | true false yes no on off null none", 0,
        hl_lex_ini, 0, 1, 0},
    [HL_L_DIFF] = {"diff patch", 0, 0, hl_lex_diff, 0, 0, 0},
    [HL_L_HTML] = {"html htm xml svg xhtml", HL_JS_WORDS, 0, hl_lex_html, 0, 0, 0},
    [HL_L_SQL] = {"sql mysql postgresql postgres pgsql plpgsql sqlite sqlite3 tsql mariadb",
        HL_SQL_WORDS, 0, hl_lex_sql, 0, 1, 0},
};


// API

static HlLang const *hl_findlang(Str s) {
    if (s.len<=0 || s.len>16) {
        return 0;
    }
    char t[17];
    for (iz i = 0; i < s.len; i++) {
        t[i] = (char)lower(s.data[i]);
    }
    t[s.len] = 0;
    for (i32 k = 0; k < HL_L_COUNT; k++) {
        char const *a = hl_langs[k].names;
        while (*a) {
            while (*a == ' ') {
                a++;
            }
            iz i = 0;
            while (a[i] && a[i]!=' ' && a[i]==t[i]) {
                i++;
            }
            if (i==s.len && (!a[i] || a[i]==' ')) {
                return &hl_langs[k];
            }
            while (*a && *a!=' ') {
                a++;
            }
        }
    }
    return 0;
}

static b32 hl_known(Str lang) {
    return hl_findlang(lang) != 0;
}

static void hl_run(HlCtx *h, HlLang const *L) {
    h->count   = 0;
    h->pos     = 0;
    h->last    = HL_PLAIN;
    h->fail    = 0;
    h->depth   = 0;
    h->hd_len  = 0;
    h->n       = h->len;
    L->lex(h, L, 0, 0);
}

static HlSpans hl_highlight(Arena *a, Str lang, Str code) {
    HlLang const *L = hl_findlang(lang);
    if (!L || code.len<=0) {
        return (HlSpans){0};
    }
    // Room for the worst case, one span per byte, plus the arena's padding.
    iz slack = 16;
#ifdef DEBUG
    slack = 4*PAGE;
#endif
    if (code.len > ((a->end - a->beg) - slack)/(iz)sizeof(HlSpan)) {
        return (HlSpans){0};
    }
    HlCtx h = {0};
    h.p   = code.data;
    h.len = code.len;
    h.ci  = L->ci;
    if (L->words) {
        hl_load(&h, L->words);
    }
    if (L->words2) {
        hl_load(&h, L->words2);
    }
    hl_run(&h, L);
    HlSpans r = {0};
    if (h.count == 0) {
        return r;
    }
    iz count = h.count;
    r.spans  = new(a, count, HlSpan);
    h.out    = r.spans;
    hl_run(&h, L);
    assert(h.count == count);
    r.len = count;
    return r;
}
