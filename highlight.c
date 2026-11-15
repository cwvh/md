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


// TODO(milestone 6): the real implementation. Until then nothing is
// highlighted, so the layout code can already call these.

static b32 hl_known(Str lang) {
    (void)lang;
    return 0;
}

static HlSpans hl_highlight(Arena *a, Str lang, Str code) {
    (void)a; (void)lang; (void)code;
    return (HlSpans){0};
}
