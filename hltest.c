// Tests for the syntax highlighter: a separate unity build with the debug
// checks on (UBSan traps, guard pages behind every allocation).
//   $ make hl-test.exe
//   $ hl-test.exe [-s SEED] [-n ITERS] [-i START] [-t WHICH] [-z PCT]
//
// Four kinds of test:
//   known   every alias, in any case, and some non-languages
//   golden  small snippets with the expected kind of every byte
//   random  noise and mutated snippets: the spans keep the documented
//           contract and nothing reads out of bounds
//   path    1 MB inputs that make naive lexers quadratic, with a time limit
//
// Iteration i of seed s is self-contained: "-t random -s S -i I -n 1"
// replays exactly it.
#include "base.c"
#include "win32.c"
#include "highlight.c"

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


// Reporting

typedef struct {
    u64          seed;
    i64          iter;      // -1 outside the random tests
    char const  *phase;
    Str          input;     // dumped on failure
    Output      *out;
    b32          dead;
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
        print(o, S("\n  reproduce: hl-test.exe -t random -s 0x"));
        printhex(o, ctx.seed, 1);
        print(o, S(" -i "));
        printi64(o, ctx.iter);
        print(o, S(" -n 1   (gdb: gdb --args hl-test.exe ...)"));
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

static i64 now_us(void) {
    static i64 freq;
    i64 t = 0;
    if (!freq) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&t);
    return t*1000000/freq;
}


// The languages: one row each, with every alias and a sample that makes
// all the aliases comparable.

typedef struct {
    char const *aliases;    // the first one is the canonical name
    char const *sample;
} LangRow;

static LangRow const langrows[] = {
    {"c h", "#include <a.h>\nint main(void) { return 0; }\n"},
    {"cpp c++ cc cxx hpp hh", "#include <a>\nclass A { int x = 1; };\n"},
    {"cs csharp", "class A { int x = 1; string s = \"a\"; }\n"},
    {"java", "@Override\npublic class A { int x = 1; }\n"},
    {"rust rs", "fn main() { let x = 1; println!(\"a\"); }\n"},
    {"go golang", "func main() { x := 1 }\n"},
    {"js javascript jsx mjs cjs", "function f() { return `a${1}`; }\n"},
    {"ts typescript tsx", "interface A { x: number }\n"},
    {"json jsonc json5", "{\"a\": [1, true, null]}\n"},
    {"python py python3", "def f(x):\n    return 1\n"},
    {"sh bash shell zsh", "if [ -f x ]; then echo $y; fi\n"},
    {"console shell-session", "$ echo $x\nout\n"},
    {"powershell ps1 pwsh ps", "Get-Item $env:Path -Force\n"},
    {"make makefile mk", "all: main.o\n\t$(CC) -o $@ $<\n"},
    {"cmake", "if(WIN32)\n  set(X ${Y})\nendif()\n"},
    {"toml", "[a]\nb = 1\n"},
    {"yaml yml", "a:\n  - b: true\n"},
    {"ini cfg conf properties", "[s]\nk = v\n"},
    {"diff patch", "--- a\n+++ b\n@@ -1 +1 @@\n-x\n+y\n"},
    {"html htm xml svg xhtml", "<a href=\"x\">t &amp; u</a>\n"},
    {"sql mysql postgresql sqlite", "SELECT 1 FROM t WHERE a = 'x';\n"},
};

// Not languages, or not ones we claim.
static char const *const unknown_langs[] = {
    "", "text", "txt", "plaintext", "c#", "cpp1", "kotlin", "swift", "markdown", "md",
    "javascriptjavascript", "c ", " c", "sh\n", "diffs", "\xff", "c\0c",
};

static Str word_at(char const *s, iz *pos) {
    while (s[*pos] == ' ') {
        (*pos)++;
    }
    iz b = *pos;
    while (s[*pos] && s[*pos]!=' ') {
        (*pos)++;
    }
    return (Str){(u8 *)s + b, *pos - b};
}

static void spans_of(Arena *a, Str lang, Str code, HlSpans *r) {
    *r = hl_highlight(a, lang, code);
}

static b32 same_spans(HlSpans a, HlSpans b) {
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        if (a.spans[i].beg!=b.spans[i].beg || a.spans[i].end!=b.spans[i].end ||
                a.spans[i].kind!=b.spans[i].kind) {
            return 0;
        }
    }
    return 1;
}

static i32 run_known(Arena scratch) {
    ctx.phase = "known";
    i32 bad = 0;
    for (iz r = 0; r < countof(langrows); r++) {
        ctx.iter = (i64)r;
        iz  pos  = 0;
        Str canon = word_at(langrows[r].aliases, &pos);
        Str code  = cstr(langrows[r].sample);
        ctx.input = code;
        Arena a0 = scratch;
        HlSpans base = hl_highlight(&a0, canon, code);
        if (base.len == 0) {
            fail("known", "sample gives no spans", r, 0);
        }
        for (;;) {
            Str w = word_at(langrows[r].aliases, &pos);
            if (!w.len) {
                break;
            }
            // lower, UPPER, and Capitalized
            for (i32 v = 0; v < 3; v++) {
                u8 t[32];
                for (iz i = 0; i < w.len; i++) {
                    u8 c = w.data[i];
                    t[i] = v==1 && c>='a' && c<='z' ? (u8)(c - 32) :
                           v==2 && i==0 && c>='a' && c<='z' ? (u8)(c - 32) : c;
                }
                Str s = {t, w.len};
                if (!hl_known(s)) {
                    fail("known", "alias not known", r, v);
                }
                Arena a1 = scratch;
                HlSpans sp = hl_highlight(&a1, s, code);
                if (!same_spans(sp, base)) {
                    fail("known", "alias highlights differently", r, v);
                }
            }
        }
        // the canonical name too
        if (!hl_known(canon)) {
            fail("known", "canonical name not known", r, 0);
        }
    }
    iz n = 0;
    for (iz i = 0; i < countof(unknown_langs); i++) {
        Str s = cstr(unknown_langs[i]);
        if (i == countof(unknown_langs)-1) {
            s = (Str){(u8 *)"c\0c", 3};
        }
        if (hl_known(s)) {
            fail("known", "non-language is known", (i64)i, 0);
        }
        Arena a1 = scratch;
        if (hl_highlight(&a1, s, S("int x;")).len) {
            fail("known", "non-language gives spans", (i64)i, 0);
        }
        n++;
    }
    // Empty code gives nothing, and the arena is not touched for nothing.
    Arena a2 = scratch;
    if (hl_highlight(&a2, S("c"), (Str){0}).len || hl_highlight(&a2, S("c"), S("")).len) {
        fail("known", "empty code gives spans", 0, 0);
    }
    (void)n;
    ctx.input = (Str){0};
    return bad;
}


// Rendering spans as a letter per byte, for the golden tests

static u8 kind_letter(HlKind k) {
    return (u8)".ckts" "enKfpvrg+-m"[k];
}

// One letter per byte; newlines stay newlines whatever covers them.
static Str render(Arena *a, Str code, HlSpans sp) {
    Str r = {new(a, code.len, u8), code.len};
    memset(r.data, '.', (uz)r.len);
    for (iz i = 0; i < sp.len; i++) {
        for (iz j = sp.spans[i].beg; j < sp.spans[i].end; j++) {
            r.data[j] = kind_letter(sp.spans[i].kind);
        }
    }
    for (iz i = 0; i < code.len; i++) {
        if (code.data[i] == '\n') {
            r.data[i] = '\n';
        }
    }
    return r;
}


// The contract: sorted, non-overlapping, non-empty, inside the code, no
// plain, adjacent spans of one kind merged, at most one per byte, and no
// span edge inside a UTF-8 sequence of valid text.
static void check_spans(Str code, HlSpans sp, char const *what) {
    if (sp.len < 0 || sp.len > code.len) {
        fail(what, "more spans than bytes", sp.len, code.len);
    }
    iz end = 0;
    for (iz i = 0; i < sp.len; i++) {
        HlSpan s = sp.spans[i];
        if (s.beg < 0 || s.end > code.len) {
            fail(what, "span outside the code", s.beg, s.end);
        }
        if (s.beg >= s.end) {
            fail(what, "empty span", s.beg, s.end);
        }
        if (s.beg < end) {
            fail(what, "overlap or unsorted", s.beg, end);
        }
        if (s.kind<=HL_PLAIN || s.kind>=HL_COUNT) {
            fail(what, "bad kind", s.kind, i);
        }
        if (i>0 && sp.spans[i-1].end==s.beg && sp.spans[i-1].kind==s.kind) {
            fail(what, "adjacent spans not merged", s.beg, s.kind);
        }
        end = s.end;
        iz edges[2] = {s.beg, s.end};
        for (i32 e = 0; e < 2; e++) {
            iz k = edges[e];
            if (k>0 && k<code.len && (code.data[k]&0xc0)==0x80 && code.data[k-1]>=0xc0) {
                for (iz j = max(k-12, 0); j < min(k+12, code.len); j++) {
                    printhex(ctx.out, code.data[j], 2);
                    printc(ctx.out, j==k ? '|' : ' ');
                }
                print(ctx.out, S("\n"));
                fail(what, "span edge splits a UTF-8 sequence", k, i);
            }
        }
    }
}


// Golden tests: code, and one letter per byte.
//   . plain   c comment   k keyword   t type      s string    e escape
//   n number  K constant  f function  p preproc   v variable  r property
//   g tag     + inserted  - deleted   m meta
// Newlines in the marks are only placeholders. Both strings must have the
// same length, which makes a miscounted line a test error, not a mystery.

typedef struct {
    char const *lang;
    char const *code;
    char const *marks;
} Golden;

static Golden const golden[] = {
    {"c",
     "#include <stdio.h>\n"
     "#include \"my.h\"\n",
     "pppppppp.sssssssss\n"
     "pppppppp.ssssss\n"},
    {"c",
     "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n",
     "ppppppp.fff..............................\n"},
    {"c",
     "int main(void) { return 0; }\n",
     "ttt.ffff.tttt....kkkkkk.n...\n"},
    {"c",
     "unsigned long x = 0x1Fu + 10UL + 1.5e-3f + .5;\n",
     "tttttttt.tttt.....nnnnn...nnnn...nnnnnnn...nn.\n"},
    {"c",
     "char c = '\\n', d = '\"'; /* c */ // line\n",
     "tttt.....sees......sss..ccccccc.ccccccc\n"},
    {"c",
     "const char *s = \"a\\tb\\x41\\0 %d\";\n",
     "kkkkk.tttt......sseeseeeeeessss.\n"},
    {"c",
     "x = foo(1) + bar (2);\n",
     "....fff.n.........n..\n"},
    {"c",
     "/* unterminated\n"
     "comment\n",
     "ccccccccccccccc\n"
     "ccccccc\n"},
    {"c",
     "char *s = \"unterminated\n"
     "int y;\n",
     "tttt......sssssssssssss\n"
     "ttt...\n"},
    {"c",
     "/* a */ /* b */ x\n",
     "ccccccc.ccccccc..\n"},
    {"c",
     "struct S { size_t n; uint8_t *p; } s = { NULL, 0 };\n",
     "kkkkkk.....tttttt....ttttttt.............KKKK..n...\n"},
    {"c",
     "#if 0\n"
     "  x\n"
     "#endif\n",
     "ppp.n\n"
     "...\n"
     "pppppp\n"},
    {"c",
     "  # define X 1 // c\n",
     "..pppppppp...n.cccc\n"},
    {"c",
     "a.default = b.int + c->if;\n",
     "..........................\n"},
    {"cpp",
     "auto s = R\"x(a \"quoted\" )\" b)x\";\n",
     "tttt.....ssssssssssssssssssssss.\n"},
    {"cpp",
     "auto s = R\"(raw\n"
     "more)\"; int y;\n",
     "tttt.....ssssss\n"
     "ssssss..ttt...\n"},
    {"cpp",
     "auto s = u8R\"d(x)d\" + L\"w\" + U'c';\n",
     "tttt.....ssssssssss....sss....sss.\n"},
    {"cpp",
     "int n = 1'000'000 + 0b1010'1010;\n",
     "ttt.....nnnnnnnnn...nnnnnnnnnnn.\n"},
    {"cpp",
     "template <typename T> class A : public B<T> { virtual void f() override; };\n",
     "kkkkkkkk..kkkkkkkk....kkkkk.....kkkkkk........kkkkkkk.tttt.f...kkkkkkkk....\n"},
    {"cpp",
     "std::string s = \"a\" \"b\"; auto p = nullptr; bool t = true;\n",
     ".....tttttt.....sss.sss..tttt.....KKKKKKK..tttt.....KKKK.\n"},
    {"cpp",
     "auto x = R\"abcdefghijklmnopqrstuvwxyz(a)abcdefghijklmnopqrstuvwxyz\";\n",
     "tttt......sssssssssssssssssssssssssssssssssssssssssssssssssssssssss.\n"},
    {"cs",
     "using System;\n"
     "[Serializable]\n"
     "public class A { [Obsolete] int x; }\n",
     "kkkkk........\n"
     "pppppppppppppp\n"
     "kkkkkk.kkkkk................ttt.....\n"},
    {"cs",
     "var s = @\"a \"\"quoted\"\" \\ b\";\n",
     "kkk.....sssseesssssseesssss.\n"},
    {"cs",
     "var s = $\"x {y} {{z}} \\n\";\n",
     "kkk.....sssse.eseeseesees.\n"},
    {"cs",
     "var s = $@\"a {b + \"c\"} \"\"d\"\"\";\n",
     "kkk.....ssssse....ssseseesees.\n"},
    {"cs",
     "string s = \"a\\u0041\\\"b\"; char c = 'x';\n",
     "tttttt.....sseeeeeeeess..tttt.....sss.\n"},
    {"cs",
     "#region Foo\n"
     "int x = 0x1F;\n"
     "#endregion\n",
     "ppppppp....\n"
     "ttt.....nnnn.\n"
     "pppppppppp\n"},
    {"cs",
     "public async Task<int> Run() { await Foo(); return default; }\n",
     "kkkkkk.kkkkk.tttt.ttt..fff.....kkkkk.fff....kkkkkk.kkkkkkk...\n"},
    {"cs",
     "var s = \"\"\"\n"
     "  raw \"text\"\n"
     "  \"\"\";\n",
     "kkk.....sss\n"
     "ssssssssssss\n"
     "sssss.\n"},
    {"java",
     "@Override\n"
     "public String toString() { return \"a\\n\" + 1L; }\n",
     "ppppppppp\n"
     "kkkkkk.tttttt.ffffffff.....kkkkkk.ssees...nn...\n"},
    {"java",
     "@SuppressWarnings(\"unchecked\") class A<T> extends B implements C {}\n",
     "ppppppppppppppppp.sssssssssss..kkkkk......kkkkkkk...kkkkkkkkkk.....\n"},
    {"java",
     "String t = \"\"\"\n"
     "    text \"block\"\n"
     "    \"\"\";\n",
     "tttttt.....sss\n"
     "ssssssssssssssss\n"
     "sssssss.\n"},
    {"java",
     "int x = 1_000; double d = 1e10; char c = '\\u0041'; boolean b = true;\n",
     "ttt.....nnnnn..tttttt.....nnnn..tttt.....seeeeees..ttttttt.....KKKK.\n"},
    {"java",
     "import java.util.List; // comment\n"
     "final int y = null;\n",
     "kkkkkk.................cccccccccc\n"
     "kkkkk.ttt.....KKKK.\n"},
    {"rust",
     "fn main<'a>(x: &'a str) -> &'static str { x }\n",
     "kk.ffff.tt......tt.ttt......ttttttt.ttt......\n"},
    {"rust",
     "let c = 'x'; let n = '\\n'; let u = '\\u{1F600}'; let e = '\303\251';\n",
     "kkk.....sss..kkk.....sees..kkk.....seeeeeeeees..kkk.....ssss.\n"},
    {"rust",
     "let r = r#\"a \"q\" b\"#; let b = br\"raw\"; let s = \"a\\n\";\n",
     "kkk.....ssssssssssss..kkk.....sssssss..kkk.....ssees.\n"},
    {"rust",
     "println!(\"{}\", 1); vec![1, 2]; x != y; a!=b;\n",
     "pppppppp.ssss..n...pppp.n..n................\n"},
    {"rust",
     "#[derive(Debug)]\n"
     "#![allow(unused)]\n"
     "struct S;\n",
     "pppppppppppppppp\n"
     "ppppppppppppppppp\n"
     "kkkkkk...\n"},
    {"rust",
     "/* a /* nested */ still */ x\n",
     "cccccccccccccccccccccccccc..\n"},
    {"rust",
     "let x: u32 = 0xFF_u32 + 1_000i64 + 2.5e3f64 + 1..2;\n",
     "kkk....ttt...nnnnnnnn...nnnnnnnn...nnnnnnnn...n..n.\n"},
    {"rust",
     "pub fn new() -> Self { Self {} } impl<T> A<T> { fn go(&self) {} }\n",
     "kkk.kk.fff......kkkk...kkkk......kkkk...........kk.ff..kkkk......\n"},
    {"rust",
     "let t = (1, 2); t.0; 'label: loop { break 'label; }\n",
     "kkk......n..n.....n..tttttt..kkkk...kkkkk.tttttt...\n"},
    {"rust",
     "let s = r##\"a\"#b\"##; x\n",
     "kkk.....sssssssssss...\n"},
    {"rust",
     "fn f() { let v: Vec<String> = None; Some(1); }\n",
     "kk.f.....kkk....ttt.tttttt....KKKK..ffff.n....\n"},
    {"go",
     "package main\n"
     "\n"
     "import \"fmt\"\n"
     "\n"
     "func main() {\n"
     "\tfmt.Println(\"hi\")\n"
     "}\n",
     "kkkkkkk.....\n"
     "\n"
     "kkkkkk.sssss\n"
     "\n"
     "kkkk.ffff....\n"
     ".....fffffff.ssss.\n"
     ".\n"},
    {"go",
     "var s = `raw\\n\n"
     "more`; c := 'x'; n := 0x1F + 1_000\n",
     "kkk.....ssssss\n"
     "sssss.......sss.......nnnn...nnnnn\n"},
    {"go",
     "func (r *T) Name() error { return nil }\n",
     "kkkk........ffff...ttttt...kkkkkk.KKK..\n"},
    {"go",
     "x := make([]int, 0); y := len(x) + cap(x); var z = nil\n",
     ".....ffff...ttt..n........fff......fff.....kkk.....KKK\n"},
    {"go",
     "type S struct{ a int }; go f(); defer g()\n",
     "kkkk...kkkkkk....ttt....kk.f....kkkkk.f..\n"},
    {"js",
     "const f = (a, b) => a + b; // c\n",
     "kkkkk......................cccc\n"},
    {"js",
     "let s = `a ${b + `c ${d}`} e\\n`;\n",
     "kkk.....sssee....sssee.esessees.\n"},
    {"js",
     "let r = /ab+c/gi.test(x); let q = a / b / c;\n",
     "kkk.....ssssssss.ffff.....kkk...............\n"},
    {"js",
     "return /[/]\\//.test(s)\n",
     "kkkkkk.sssssss.ffff...\n"},
    {"js",
     "x = y / 2; z = (a + b) / 3; w = a[0] / 4;\n",
     "........n................n........n....n.\n"},
    {"js",
     "var s = 'it\\'s' + \"q\\\"\";\n",
     "kkk.....ssseess...ssees.\n"},
    {"js",
     "function foo() { return new Map(); }\n",
     "kkkkkkkk.fff.....kkkkkk.kkk.ttt.....\n"},
    {"js",
     "class A extends B { static x = 1; async *g() { yield await 1; } }\n",
     "kkkkk...kkkkkkk.....kkkkkk.....n..kkkkk..f.....kkkkk.kkkkk.n.....\n"},
    {"js",
     "@Component({a: 1})\n"
     "class A {}\n",
     "pppppppppp.....n..\n"
     "kkkkk.....\n"},
    {"js",
     "let a = 0x1F + 1_000n + .5 + 1e3;\n",
     "kkk.....nnnn...nnnnnn...nn...nnn.\n"},
    {"js",
     "x.class = obj.default; y.function(1);\n",
     ".........................ffffffff.n..\n"},
    {"js",
     "let s = `unterminated ${a\n"
     "more",
     "kkk.....ssssssssssssssee.\n"
     "...."},
    {"js",
     "let o = { $key: 1, _k: `a` }; undefined; NaN; this;\n",
     "kkk.............n......sss....KKKKKKKKK..KKK..KKKK.\n"},
    {"js",
     "let s = `${`${`${`${`${`${`${`${`${`x`}`}`}`}`}`}`}`}`}`;\n",
     "kkk.....seeseeseeseeseeseeseeseessss.ssseseseseseseseses.\n"},
    {"ts",
     "interface A { x: number; y?: string }\n"
     "type B = A | null;\n",
     "kkkkkkkkk........tttttt......tttttt..\n"
     "kkkk.........KKKK.\n"},
    {"ts",
     "export class C<T> implements I { private readonly x: string = \"a\"; }\n",
     "kkkkkk.kkkkk......kkkkkkkkkk.....kkkkkkk.kkkkkkkk....tttttt...sss...\n"},
    {"ts",
     "@Injectable()\n"
     "export class S { constructor(private a: A) {} }\n",
     "ppppppppppp..\n"
     "kkkkkk.kkkkk.....fffffffffff.kkkkkkk...........\n"},
    {"ts",
     "let x: any = <unknown>y as boolean;\n",
     "kkk....ttt....ttttttt...kk.ttttttt.\n"},
    {"json",
     "{\"a\": 1, \"b\": [true, false, null], \"c\": {\"d\": \"e\\n\\u0041\"}, \"f\": -1.5e3}\n",
     ".rrr..n..rrr...KKKK..KKKKK..KKKK...rrr...rrr..sseeeeeeees...rrr...nnnnn.\n"},
    {"json",
     "// c\n"
     "{\n"
     "  /* b */ \"k\" : \"v\",\n"
     "  's': 't',\n"
     "  unq: 1\n"
     "}\n",
     "cccc\n"
     ".\n"
     "..ccccccc.rrr...sss.\n"
     "..rrr..sss.\n"
     "..rrr..n\n"
     ".\n"},
    {"json",
     "{\"unterminated\": \"abc\n",
     ".rrrrrrrrrrrrrr..ssss\n"},
    {"json",
     "[ \"a\" , \"b\" ]\n",
     "..sss...sss..\n"},
    {"python",
     "def f(x, *args):\n"
     "    \"\"\"doc\n"
     "    string\"\"\"\n"
     "    return x if x else None\n",
     "kkk.f...........\n"
     "....ssssss\n"
     "sssssssssssss\n"
     "....kkkkkk...kk...kkkk.KKKK\n"},
    {"python",
     "@decorator.attr(1)\n"
     "class A(B):\n"
     "    @staticmethod\n"
     "    def g(self): pass\n",
     "ppppppppppppppp.n.\n"
     "kkkkk.t....\n"
     "....ppppppppppppp\n"
     "....kkk.f........kkkk\n"},
    {"python",
     "s = r\"\\d+\" + b'x' + rb\"\\n\" + u\"\303\251\" + \"a\\tb\"\n",
     "....ssssss...ssss...ssssss...sssss...sseess\n"},
    {"python",
     "s = f\"x {a + 1:>{w}} {{lit}} {b!r}\" + f'{x}'\n",
     "....sssse....n.....eseessseese...es...sse.es\n"},
    {"python",
     "s = '''triple\n"
     "lines''' # c\n",
     "....sssssssss\n"
     "ssssssss.ccc\n"},
    {"python",
     "x = 0x1F + 0b101 + 1_000 + 1.5e-3 + 3j\n",
     "....nnnn...nnnnn...nnnnn...nnnnnn...nn\n"},
    {"python",
     "a = True and not False or None; import os\n",
     "....KKKK.kkk.kkk.KKKKK.kk.KKKK..kkkkkk...\n"},
    {"python",
     "x.match(1); print(len(y)); re.class\n",
     "..fffff.n...fffff.fff..............\n"},
    {"python",
     "if a:  # comment\n"
     "    b = 1 @ c\n",
     "kk.....ccccccccc\n"
     "........n....\n"},
    {"python",
     "s = \"unterminated\n"
     "x = 1\n",
     "....sssssssssssss\n"
     "....n\n"},
    {"python",
     "async def g(): await x; lambda y: y; from a import b as c\n",
     "kkkkk.kkk.f....kkkkk....kkkkkk.......kkkk...kkkkkk...kk..\n"},
    {"python",
     "x = \"\"\"never closed\n"
     "still\n",
     "....sssssssssssssss\n"
     "sssss\n"},
    {"sh",
     "#!/bin/sh\n"
     "# comment\n"
     "echo \"hi $USER ${HOME} $(date) \\n\" # trailing\n",
     "ccccccccc\n"
     "ccccccccc\n"
     "ffff.ssssvvvvvsvvvvvvvsvvvvvvvsees.cccccccccc\n"},
    {"sh",
     "if [ -f \"$f\" ]; then\n"
     "  echo $1 $@ $# $? $$\n"
     "fi\n",
     "kk......svvs....kkkk\n"
     "..ffff.vv.vv.vv.vv.vv\n"
     "kk\n"},
    {"sh",
     "x='single $notvar' y=\"dq 'x'\" z=$'a\\tb'\n",
     "v.ssssssssssssssss.v.ssssssss.v.ssseess\n"},
    {"sh",
     "FOO=bar BAZ+=1 cmd --flag=1 -x\n",
     "vvv.....vvv...................\n"},
    {"sh",
     "for i in 1 2 3; do echo $i; done\n",
     "kkk...kk.n.n.n..kk.ffff.vv..kkkk\n"},
    {"sh",
     "case $x in a) echo a;; *) echo b;; esac\n",
     "kkkk.vv.kk....ffff........ffff.....kkkk\n"},
    {"sh",
     "function foo { echo; }\n"
     "bar() { echo; }\n",
     "kkkkkkkk.fff.........k\n"
     "fff...k.ffff..k\n"},
    {"sh",
     "echo a#b ;#c\n"
     "echo # d\n",
     "ffff......cc\n"
     "ffff.ccc\n"},
    {"sh",
     "cat <<EOF\n"
     "hello $x\n"
     "EOF\n"
     "echo done\n",
     ".........\n"
     "ssssssss\n"
     "...\n"
     "ffff.....\n"},
    {"sh",
     "cat <<-\"END\" | sort\n"
     "\tbody\n"
     "\tEND\n"
     "echo after\n",
     "...................\n"
     "sssss\n"
     "....\n"
     "ffff......\n"},
    {"sh",
     "cat <<EOF\n"
     "never ends\n",
     ".........\n"
     "ssssssssss\n"},
    {"sh",
     "x=$(echo $(echo a) b) y=${a:-${b}}\n",
     "v.vvvvvvvvvvvvvvvvvvv.v.vvvvvvvvvv\n"},
    {"sh",
     "echo $((1 << 3)) <<< \"here\"\n",
     "ffff.vvvvvvvvvvv.....ssssss\n"},
    {"sh",
     "echo \"unterminated $(\n",
     "ffff.ssssssssssssssvv\n"},
    {"sh",
     "echo foo if then\n"
     "if x; then y; else z; fi\n",
     "ffff............\n"
     "kk....kkkk....kkkk....kk\n"},
    {"sh",
     "echo ${unterminated\n",
     "ffff.vv............\n"},
    {"console",
     "$ ls -la\n"
     "total 0\n"
     "$ echo \"hi\" # c\n"
     "# make install\n",
     "........\n"
     ".......\n"
     "..ffff.ssss.ccc\n"
     "..............\n"},
    {"console",
     "echo plain script $x\n",
     "ffff..............vv\n"},
    {"powershell",
     "Get-ChildItem -Path $env:TEMP -Recurse | Where-Object { $_.Length -gt 1 }\n",
     "fffffffffffff.......vvvvvvvvv............ffffffffffff...vv........kkk.n..\n"},
    {"powershell",
     "<# block\n"
     "comment #> $x = \"a`n $y $($z.p) `\"q`\" \"\"d\"\"\"\n",
     "cccccccc\n"
     "cccccccccc.vv...sseesvvsvvvvvvvseeseeseesees\n"},
    {"powershell",
     "$s = 'it''s'\n"
     "$h = @\"\n"
     "here $x\n"
     "\"@\n"
     "$g = @'\n"
     "raw\n"
     "'@\n",
     "vv...ssseess\n"
     "vv...ss\n"
     "sssssss\n"
     "ss\n"
     "vv...ss\n"
     "sss\n"
     "ss\n"},
    {"powershell",
     "function Get-Foo { param([string]$Name, [int[]]$N) if ($true -and $null) { return $false } }\n",
     "kkkkkkkk.fffffff...kkkkk.ttttttttvvvvv..tttttttvv..kk..KKKKK.kkkk.KKKKK....kkkkkk.KKKKKK....\n"},
    {"powershell",
     "FOREACH ($i in 1..3) { Write-Host $i } # c\n",
     "kkkkkkk..vv.kk.n..n....ffffffffff.vv...ccc\n"},
    {"powershell",
     "$a = 0x1F + 1.5 + 10kb; $b = ${weird var}; $c = $args[0]\n",
     "vv...nnnn...nnn...nnnn..vv...vvvvvvvvvvvv..vv...vvvvv.n.\n"},
    {"powershell",
     "@\"\n"
     "unterminated\n",
     "ss\n"
     "ssssssssssss\n"},
    {"powershell",
     "if ($a -eq 1 -or $b -like \"x*\") { } elseif ($c) { } else { }\n",
     "kk..vv.kkk.n.kkk.vv.kkkkk.ssss......kkkkkk..vv......kkkk....\n"},
    {"make",
     "CC = gcc\n"
     "CFLAGS ?= -O2 -Wall\n"
     "OBJS := a.o b.o\n",
     "vv......\n"
     "vvvvvv.............\n"
     "vvvv...........\n"},
    {"make",
     "all: main.o util.o\n"
     "\t$(CC) -o $@ $^ ${LDFLAGS}\n",
     "fff...............\n"
     ".vvvvv....vv.vv.vvvvvvvvvv\n"},
    {"make",
     ".PHONY: clean all\n"
     "clean:\n"
     "\t@rm -f *.o # remove\n"
     "\t@echo \"done $$HOME\"\n",
     "pppppp...........\n"
     "fffff.\n"
     "............cccccccc\n"
     ".......ssssssvvsssss\n"},
    {"make",
     "include config.mk\n"
     "ifeq ($(OS),Windows_NT)\n"
     "X = 1\n"
     "else\n"
     "X = 2\n"
     "endif\n",
     "kkkkkkk..........\n"
     "kkkk..vvvvv............\n"
     "v....\n"
     "kkkk\n"
     "v....\n"
     "kkkkk\n"},
    {"make",
     "FILES = a.c \\\n"
     "  b.c \\\n"
     "  -DFOO=1\n",
     "vvvvv........\n"
     ".......\n"
     ".........\n"},
    {"make",
     "%.o: %.c\n"
     "\t$(CC) -c $< -o $@\n",
     "fff.....\n"
     ".vvvvv....vv....vv\n"},
    {"make",
     "define RULE\n"
     "$(1): $(2)\n"
     "endef\n"
     "# comment\n"
     "export FOO := bar\n",
     "kkkkkk.....\n"
     "vvvv..vvvv\n"
     "kkkkk\n"
     "ccccccccc\n"
     "kkkkkk.vvv.......\n"},
    {"make",
     "a b c: d e\n"
     "\ttouch $@\n"
     "VAR=$(shell echo $(X))\n",
     "f.f.f.....\n"
     ".......vv\n"
     "vvv.vvvvvvvvvvvvvvvvvv\n"},
    {"cmake",
     "cmake_minimum_required(VERSION 3.10)\n"
     "project(Foo CXX)\n",
     "ffffffffffffffffffffff.........nnnn.\n"
     "fffffff.........\n"},
    {"cmake",
     "if(WIN32 AND NOT MSVC)\n"
     "  set(X \"a ${Y} \\\" $ENV{Z}\")\n"
     "elseif (UNIX)\n"
     "endif()\n",
     "kk.......kkk.kkk......\n"
     "..fff...sssvvvvseesvvvvvvvs.\n"
     "kkkkkk.......\n"
     "kkkkk..\n"},
    {"cmake",
     "# line\n"
     "#[[ block\n"
     "comment ]] add_executable(a b.c)\n"
     "#[==[ x ]] y ]==]\n",
     "cccccc\n"
     "ccccccccc\n"
     "cccccccccc.ffffffffffffff.......\n"
     "ccccccccccccccccc\n"},
    {"cmake",
     "set(S [[raw ${not}]] [=[x]=])\n",
     "fff...ssssssssssssss.sssssss.\n"},
    {"cmake",
     "FOREACH(f ${FILES})\n"
     "  message(STATUS \"x\" ON TRUE 12 3.4)\n"
     "ENDFOREACH()\n",
     "kkkkkkk...vvvvvvvv.\n"
     "..fffffff........sss.KK.KKKK.nn.nnn.\n"
     "kkkkkkkkkk..\n"},
    {"cmake",
     "target_link_libraries(a PRIVATE b::c -lm)\n",
     "fffffffffffffffffffff....................\n"},
    {"cmake",
     "set(X \"unterminated\n",
     "fff...sssssssssssss\n"},
    {"toml",
     "# comment\n"
     "[package]\n"
     "name = \"md\"\n"
     "version = \"0.1.0\"\n",
     "ccccccccc\n"
     "ggggggggg\n"
     "rrrr...ssss\n"
     "rrrrrrr...sssssss\n"},
    {"toml",
     "[[bin]]\n"
     "[a.b.\"c d\"]\n",
     "ggggggg\n"
     "ggggggggggg\n"},
    {"toml",
     "a = 1\n"
     "b = 1.5e3\n"
     "c = true\n"
     "d = inf\n"
     "e = 0xDEAD\n"
     "f = 1_000\n",
     "r...n\n"
     "r...nnnnn\n"
     "r...KKKK\n"
     "r...KKK\n"
     "r...nnnnnn\n"
     "r...nnnnn\n"},
    {"toml",
     "d1 = 1979-05-27T07:32:00Z\n"
     "d2 = 1979-05-27 07:32:00\n"
     "d3 = 07:32:00\n",
     "rr...nnnnnnnnnnnnnnnnnnnn\n"
     "rr...nnnnnnnnnn.nnnnnnnn\n"
     "rr...nnnnnnnn\n"},
    {"toml",
     "a = \"bas\\ic \\n\"\n"
     "b = 'lit\\eral'\n"
     "c = \"\"\"\n"
     "multi \"q\" \\n\n"
     "\"\"\"\n"
     "d = '''\n"
     "raw \\n\n"
     "'''\n",
     "r...sssseessees\n"
     "r...ssssssssss\n"
     "r...sss\n"
     "ssssssssssee\n"
     "sss\n"
     "r...sss\n"
     "ssssss\n"
     "sss\n"},
    {"toml",
     "a.b.c = 1\n"
     "\"quoted key\" = 2\n"
     "'lit key' = 3\n"
     "inline = { x = 1, y = \"s\" }\n"
     "arr = [ 1, 2 ]\n",
     "rrrrr...n\n"
     "rrrrrrrrrrrr...n\n"
     "rrrrrrrrr...n\n"
     "rrrrrr.....r...n..r...sss..\n"
     "rrr.....n..n..\n"},
    {"toml",
     "arr = [\n"
     "  [1, 2],\n"
     "  [3],\n"
     "]\n",
     "rrr....\n"
     "...n..n..\n"
     "...n..\n"
     ".\n"},
    {"toml",
     "a = \"unterminated\n"
     "b = 1\n",
     "r...sssssssssssss\n"
     "r...n\n"},
    {"yaml",
     "# comment\n"
     "---\n"
     "name: md\n"
     "version: 1.2\n"
     "enabled: true\n"
     "nothing: null\n"
     "empty: ~\n"
     "...\n",
     "ccccccccc\n"
     "ppp\n"
     "rrrr....\n"
     "rrrrrrr..nnn\n"
     "rrrrrrr..KKKK\n"
     "rrrrrrr..KKKK\n"
     "rrrrr..K\n"
     "ppp\n"},
    {"yaml",
     "list:\n"
     "  - a\n"
     "  - b: 1\n"
     "    c: \"str\" # trailing\n"
     "  - 'single'\n",
     "rrrr.\n"
     ".....\n"
     "....r..n\n"
     "....r..sssss.cccccccccc\n"
     "....ssssssss\n"},
    {"yaml",
     "base: &anchor\n"
     "  x: 1\n"
     "derived:\n"
     "  <<: *anchor\n"
     "  y: !!str 5\n",
     "rrrr..vvvvvvv\n"
     "..r..n\n"
     "rrrrrrr.\n"
     "..rr..vvvvvvv\n"
     "..r..ttttt.n\n"},
    {"yaml",
     "script: |\n"
     "  echo hi\n"
     "  echo: not key\n"
     "after: >-\n"
     "  folded\n"
     "  text\n"
     "\n"
     "last: 1\n",
     "rrrrrr..k\n"
     "sssssssss\n"
     "sssssssssssssss\n"
     "rrrrr..kk\n"
     "ssssssss\n"
     "ssssss\n"
     "\n"
     "rrrr..n\n"},
    {"yaml",
     "flow: {a: 1, b: [x, y], c: \"z\"}\n"
     "\"quoted key\": value\n"
     "'sq': v\n",
     "rrrr...r..n..r..........r..sss.\n"
     "rrrrrrrrrrrr.......\n"
     "rrrr...\n"},
    {"yaml",
     "url: http://example.com:80/x\n"
     "time: 12:30\n"
     "- item\n",
     "rrr.........................\n"
     "rrrr..nnnnn\n"
     "......\n"},
    {"yaml",
     "- name: x\n"
     "  run: |\n"
     "    multi\n"
     "    line\n"
     "- name: y\n",
     "..rrrr...\n"
     "..rrr..k\n"
     "sssssssss\n"
     "ssssssss\n"
     "..rrrr...\n"},
    {"yaml",
     "a: 1 # c\n"
     "b:#notcomment\n"
     "key with spaces: value\n",
     "r..n.ccc\n"
     ".............\n"
     "rrrrrrrrrrrrrrr.......\n"},
    {"yaml",
     "%YAML 1.2\n"
     "--- !tag\n"
     "a: b\n",
     "ppppppppp\n"
     "ppp.tttt\n"
     "r...\n"},
    {"ini",
     "; comment\n"
     "# other\n"
     "[section]\n"
     "key = value\n"
     "name: other\n"
     "num = 42\n"
     "flag = true\n"
     "str = \"quoted\"\n",
     "ccccccccc\n"
     "ccccccc\n"
     "ggggggggg\n"
     "rrr........\n"
     "rrrr.......\n"
     "rrr...nn\n"
     "rrrr...KKKK\n"
     "rrr...ssssssss\n"},
    {"ini",
     "[a b]\n"
     "empty =\n"
     "url = http://x/y?z=1\n"
     "novalue\n"
     "  indented = 1\n",
     "ggggg\n"
     "rrrrr..\n"
     "rrr.................\n"
     ".......\n"
     "..rrrrrrrr...n\n"},
    {"ini",
     "key.sub=1.5\n"
     "version=1.2.3\n",
     "rrrrrrr.nnn\n"
     "rrrrrrr.nnnnn\n"},
    {"diff",
     "diff --git a/x b/x\n"
     "index 1..2 100644\n"
     "--- a/x\n"
     "+++ b/x\n"
     "@@ -1,3 +1,3 @@ fn\n"
     " context\n"
     "-old\n"
     "+new\n"
     "\\ No newline at end of file\n",
     "mmmmmmmmmmmmmmmmmm\n"
     "mmmmmmmmmmmmmmmmm\n"
     "mmmmmmm\n"
     "mmmmmmm\n"
     "mmmmmmmmmmmmmmmmmm\n"
     "........\n"
     "----\n"
     "++++\n"
     "ccccccccccccccccccccccccccc\n"},
    {"diff",
     "--- a\n"
     "+++ b\n"
     "@@ -1 +1 @@\n"
     "-- gone\n"
     "+++ added\n",
     "mmmmm\n"
     "mmmmm\n"
     "mmmmmmmmmmm\n"
     "-------\n"
     "+++++++++\n"},
    {"diff",
     "-removed\r\n"
     "+added\r\n"
     " kept\r\n",
     "--------.\n"
     "++++++.\n"
     "......\n"},
    {"diff",
     "new file mode 100644\n"
     "Binary files a and b differ\n"
     "rename from a\n"
     "rename to b\n",
     "mmmmmmmmmmmmmmmmmmmm\n"
     "mmmmmmmmmmmmmmmmmmmmmmmmmmm\n"
     "mmmmmmmmmmmmm\n"
     "mmmmmmmmmmm\n"},
    {"html",
     "<!DOCTYPE html>\n"
     "<html lang=\"en\">\n"
     "<head><title>T &amp; U &#169; &#xA9; &bogus</title></head>\n"
     "</html>\n",
     "ppppppppppppppp\n"
     "ggggg.rrrr.ssssg\n"
     "ggggggggggggg..eeeee...eeeeee.eeeeee.......ggggggggggggggg\n"
     "ggggggg\n"},
    {"html",
     "<a href=\"x\" class=foo disabled data-x='y'>link</a><br/>\n",
     "gg.rrrr.sss.rrrrr.sss.rrrrrrrr.rrrrrr.sssg....ggggggggg\n"},
    {"html",
     "<!-- comment <b> -->text<!-- unterminated\n",
     "cccccccccccccccccccc....ccccccccccccccccc\n"},
    {"html",
     "<![CDATA[ x < y ]]> <?xml version=\"1.0\"?>\n",
     "pppppppppsssssssppp.ppppppppppppppppppppp\n"},
    {"html",
     "<script>var s = \"a</b>\"; // c\n"
     "if (a < b) x = `t`;</script><p>after</p>\n",
     "ggggggggkkk.....sssssss..cccc\n"
     "kk.............sss.gggggggggggg.....gggg\n"},
    {"html",
     "<style>a > b { color: red }</style><i>x</i>\n",
     "ggggggg....................ggggggggggg.gggg\n"},
    {"xml",
     "<ns:tag xmlns:ns=\"u\" ns:attr=\"v\"><child/></ns:tag>\n",
     "ggggggg.rrrrrrrr.sss.rrrrrrr.sssgggggggggggggggggg\n"},
    {"html",
     "a < b && c > d <3 </ <a <b>\n",
     ".....................gg.ggg\n"},
    {"html",
     "<div\n"
     "  id=\"a\"\n"
     "  class=\"b\n"
     "  c\">\n"
     "</div\n"
     ">\n",
     "gggg\n"
     "..rr.sss\n"
     "..rrrrr.ss\n"
     "ssssg\n"
     "ggggg\n"
     "g\n"},
    {"html",
     "<a href=\"unterminated>text\n",
     "gg.rrrr.ssssssssssssssssss\n"},
    {"html",
     "<SCRIPT>x = 1</SCRIPT>\n",
     "gggggggg....nggggggggg\n"},
    {"sql",
     "SELECT a, COUNT(*) AS n FROM t WHERE b = 'x''y' AND c > 10 GROUP BY a;\n",
     "kkkkkk....fffff....kk...kkkk...kkkkk.....sseess.kkk.....nn.kkkkk.kk...\n"},
    {"sql",
     "select * from t -- comment\n"
     "/* block\n"
     "comment */ where x is null;\n",
     "kkkkkk...kkkk...cccccccccc\n"
     "cccccccc\n"
     "cccccccccc.kkkkk...kk.KKKK.\n"},
    {"sql",
     "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL DEFAULT 'a', ok BOOLEAN);\n",
     "kkkkkk.kkkkk.......ttt.kkkkkkk.kkk.......ttttttt.nn..kkk.KKKK.kkkkkkk.sss.....ttttttt..\n"},
    {"sql",
     "INSERT INTO t (a, b) VALUES (1, 2.5), (:x, @y);\n",
     "kkkkkk.kkkk..........kkkkkk..n..nnn....vv..vv..\n"},
    {"sql",
     "SELECT \"quoted\", 'unterminated\n"
     "more\n",
     "kkkkkk.ssssssss..sssssssssssss\n"
     "ssss\n"},
    {"sql",
     "CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql;\n",
     "kkkkkk.kkkkkkkk.f...........ttt.kk.ssssssssssssssssssssssssss..................\n"},
    {"sql",
     "SELECT $tag$ body $tag$, $1, x::int FROM t;\n",
     "kkkkkk.ssssssssssssssss...n.....ttt.kkkk...\n"},
    {"sql",
     "select now(), length(x), my_func(1) from dual;\n",
     "kkkkkk.fff....ffffff.....fffffff.n..kkkk......\n"},
    {"sql",
     "/* unterminated\n"
     "select 1\n",
     "ccccccccccccccc\n"
     "cccccccc\n"},
    {"c",
     "char *s = \"\303\251\342\202\254\360\237\230\200\"; int \303\251 = 1; // \303\251\n",
     "tttt......sssssssssss..ttt......n..ccccc\n"},
    {"c",
     "x = \"\\\n"
     " continued\";\n",
     "....se\n"
     "sssssssssss.\n"},
    {"c",
     "/* \303\251 */ x\n",
     "cccccccc..\n"},
    {"python",
     "\303\251 = \"\303\274\"\n",
     ".....ssss\n"},
};

static i32 run_golden(Arena scratch, b32 verbose) {
    ctx.phase = "golden";
    i32 bad = 0;
    for (iz i = 0; i < countof(golden); i++) {
        Golden g    = golden[i];
        Str    code = cstr(g.code);
        Str    want = cstr(g.marks);
        Arena  a    = scratch;
        ctx.iter = (i64)i;
        ctx.input = code;
        if (code.len != want.len) {
            fail("golden", "marks and code differ in length (test error)", code.len, want.len);
        }
        HlSpans sp = hl_highlight(&a, cstr(g.lang), code);
        check_spans(code, sp, "golden");
        Str got = render(&a, code, sp);
        b32 ok  = equals(got, want);
        // newlines compare as equal whatever the marks have there
        if (!ok) {
            ok = 1;
            for (iz j = 0; j < code.len; j++) {
                if (code.data[j]!='\n' && got.data[j]!=want.data[j]) {
                    ok = 0;
                }
            }
        }
        if (!ok) {
            bad++;
            Output *o = ctx.out;
            print(o, S("golden "));
            printi64(o, (i64)i);
            print(o, S(" ("));
            print(o, cstr(g.lang));
            print(o, S(") differs\n"));
            // line by line: code, wanted, got
            iz b = 0;
            while (b < code.len) {
                iz e = b;
                while (e<code.len && code.data[e]!='\n') {
                    e++;
                }
                Str cl = {code.data+b, e-b};
                Str wl = {want.data+b, e-b};
                Str gl = {got.data+b, e-b};
                if (!equals(wl, gl)) {
                    print(o, S("  code  "));
                    print(o, cl);
                    print(o, S("\n  want  "));
                    print(o, wl);
                    print(o, S("\n  got   "));
                    print(o, gl);
                    print(o, S("\n"));
                }
                b = e + 1;
            }
            flush(o);
        } else if (verbose) {
            print(ctx.out, S("golden ok: "));
            print(ctx.out, cstr(g.lang));
            print(ctx.out, S("\n"));
        }
        alive();
    }
    ctx.input = (Str){0};
    return bad;
}


// Arena handling

static i32 run_arena(Arena scratch) {
    ctx.phase = "arena";
    Str code = S("int x = 1; // c\nchar *s = \"a\\n\";\n");
    ctx.input = code;
    // Too small for the worst case: no spans, and the arena is untouched.
    {
        Arena a = scratch;
        a.end   = a.beg + 3*PAGE;
        u8 *beg = a.beg;
        HlSpans sp = hl_highlight(&a, S("c"), code);
        if (sp.len || sp.spans || a.beg!=beg) {
            fail("arena", "tiny arena gave spans or was used", sp.len, 0);
        }
    }
    // Plenty: spans, and no more of the arena used than the spans need.
    {
        Arena a = scratch;
        HlSpans sp = hl_highlight(&a, S("c"), code);
        if (sp.len == 0) {
            fail("arena", "no spans", 0, 0);
        }
        check_spans(code, sp, "arena");
        if (a.beg - scratch.beg > sp.len*(iz)sizeof(HlSpan) + 2*PAGE) {
            fail("arena", "used much more than the spans", a.beg - scratch.beg, sp.len);
        }
    }
    ctx.input = (Str){0};
    return 0;
}


// Generators

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

static void bs(Buf *b, char const *s) {
    bput(b, cstr(s));
}

static void bc(Buf *b, u8 c) {
    bput(b, (Str){&c, 1});
}

static Str bstr(Buf *b) {
    return (Str){b->p, b->len};
}

static u64 scale = 100;

static void brep(Buf *b, char const *s, iz n) {
    Str t = cstr(s);
    n = n * (iz)scale / 100;
    for (iz i = 0; i < n && !b->over; i++) {
        bput(b, t);
    }
}

// Pieces that start or end things in some language.
static char const *const frags[] = {
    "\"", "'", "`", "/*", "*/", "//", "#", "${", "$(", "$", "{", "}", "(", ")",
    "[", "]", "<<", "<<EOF", "EOF", "\\", "\n", " ", "  ", "\t", "r#\"", "R\"x(",
    ")x\"", "\"\"\"", "'''", "<!--", "-->", "<", ">", "</", "/>", "=", ":", ": ",
    "- ", "---", "@@", "+++ ", "--- ", "-- ", "$$", "@\"", "$\"", "<#", "#>",
    "@'", "'@", "#[", "#[[", "]]", "|", ">-", "if", "fn", "def", "function",
    "true", "0x1f", "1.5e3", "foo", "Foo_bar", "\xc3\xa9", "\xe2\x82\xac",
    "\xf0\x9f\x98\x80", "\xff", "\xc3", "\x80", "\0", "&amp;", "&", "%", "@",
    "SELECT", "from", "<script>", "</script>", "<![CDATA[", "]]>", "<?", "?>",
    "#[=[", "]=]", "$ENV{", "!", ";", ",", ".", "\r\n", "+", "-", "*", "<<-",
    "\\\n", "R\"(", ")\"", "r\"", "f\"", "$'", "{{", "}}", "0b1'0", "1..2",
    "'a", "'\\n'", "/", "/[", "=>", "->", "::", "?", "~", "^", "%%",
};

static void gen_soup(u64 *s, Buf *b, iz max) {
    iz n = (iz)rnd(s, (u64)max) + 1;
    while (b->len < n && !b->over) {
        i32 k = (i32)rnd(s, 10);
        if (k < 7) {
            bs(b, frags[rnd(s, (u64)countof(frags))]);
        } else if (k == 7) {
            bc(b, (u8)rnd(s, 256));
        } else if (k == 8) {
            bc(b, (u8)(32 + rnd(s, 95)));
        } else {
            bs(b, "word");
        }
    }
}

static void gen_bytes(u64 *s, Buf *b, iz max) {
    iz n = (iz)rnd(s, (u64)max) + 1;
    for (iz i = 0; i < n; i++) {
        bc(b, (u8)rnd(s, 256));
    }
}

static void gen_mutant(u64 *s, Buf *b, iz max) {
    Golden g    = golden[rnd(s, (u64)countof(golden))];
    Str    code = cstr(g.code);
    bput(b, code);
    iz muts = (iz)rnd(s, 6);
    for (iz m = 0; m < muts && b->len > 0; m++) {
        iz at = (iz)rnd(s, (u64)b->len);
        i32 k = (i32)rnd(s, 4);
        if (k == 0) {                   // truncate
            b->len = at;
        } else if (k == 1) {            // flip a byte
            b->p[at] = (u8)rnd(s, 256);
        } else if (k == 2) {            // delete a byte
            memcpy(b->p+at, b->p+at+1, (uz)(b->len-at-1));
            b->len--;
        } else {                        // insert a fragment
            Str f = cstr(frags[rnd(s, (u64)countof(frags))]);
            if (f.len==0 && frags[0]) {
                f = S("\0");
            }
            if (b->len + f.len <= max) {
                memcpy(b->p+at+f.len, b->p+at, (uz)(b->len-at));
                memcpy(b->p+at, f.data, (uz)f.len);
                b->len += f.len;
            }
        }
    }
}

static void iteration(Arena perm, Arena scratch, u64 seed, i64 iter) {
    u64 s = seed ^ ((u64)iter * 0x9e3779b97f4a7c15ULL);
    rand64(&s);
    rand64(&s);
    ctx.iter  = iter;
    ctx.phase = "random";
    iz max = chance(&s, 10) ? 3000 : 300;
    u8 *mem = new(&perm, max + 64, u8);
    Buf b = {mem, 0, max + 64, 0};
    i32 kind = (i32)rnd(&s, 6);
    if (kind == 0) {
        gen_bytes(&s, &b, max);
    } else if (kind <= 2) {
        gen_mutant(&s, &b, max);
    } else {
        gen_soup(&s, &b, max);
    }
    // Put the input at the very end of its pages so that reading past it
    // faults: copy into an exactly-sized allocation.
    Str code = {new(&perm, b.len, u8), b.len};
    memcpy(code.data, b.p, (uz)b.len);
    ctx.input = code;
    iz lang_index = (iz)(iter % countof(langrows));
    iz pos = 0;
    Str lang = word_at(langrows[lang_index].aliases, &pos);
    // Sometimes use another alias.
    if (chance(&s, 30)) {
        iz p2 = pos;
        Str w = word_at(langrows[lang_index].aliases, &p2);
        if (w.len) {
            lang = w;
        }
    }
    poison(scratch);
    Arena a = scratch;
    HlSpans sp = hl_highlight(&a, lang, code);
    check_spans(code, sp, "random");
    // Same input, same spans: nothing depends on stale memory.
    poison(scratch);
    Arena a2 = scratch;
    HlSpans sp2 = hl_highlight(&a2, lang, code);
    if (!same_spans(sp, sp2)) {
        fail("random", "not deterministic", sp.len, sp2.len);
    }
    alive();
}


// Slow inputs

typedef struct {
    char const *name;
    char const *unit;
    char const *langs;      // space-separated, or "*" for all
} Path;

static Path const paths[] = {
    // everywhere
    {"quotes",       "\"",                     "*"},
    {"apostrophes",  "'",                      "*"},
    {"backticks",    "`",                      "*"},
    {"backslashes",  "\\",                     "*"},
    {"newlines",     "\n",                     "*"},
    {"one word",     "a",                      "*"},
    {"one number",   "1",                      "*"},
    {"high bytes",   "\xc3\xa9",               "*"},
    {"nul",          "\0",                     "*"},
    {"brackets",     "[",                      "*"},
    {"parens",       "(",                      "*"},
    {"braces",       "{",                      "*"},
    {"dollars",      "$",                      "*"},
    {"angles",       "<",                      "*"},
    // C family
    {"block comment openers", "/*",            "c cpp cs java rust go js ts json html sql"},
    {"template openers", "`${",                "js ts"},
    {"nested templates", "`${`${`${",          "js ts"},
    {"regex openers", "(/[",                   "js ts"},
    {"regex slashes", "= /a\n",                "js ts"},
    {"raw strings",  "r#\"",                   "rust"},
    {"raw hashes",   "r######",                "rust"},
    {"raw cpp",      "R\"x(",                  "cpp"},
    {"raw cpp close", "R\"(a)\"",              "cpp"},
    {"lifetimes",    "'a ",                    "rust"},
    {"rust chars",   "'\\u{1}'",               "rust"},
    {"attributes",   "#[",                     "rust"},
    {"nested comment", "/*/*",                 "rust"},
    {"verbatim",     "@\"",                    "cs"},
    {"interpolated", "$\"{",                   "cs"},
    {"text blocks",  "\"\"\"",                 "java cs"},
    {"annotations",  "@A.",                    "java ts js"},
    {"preprocessor", "#if\n#include <a\n",     "c cpp cs"},
    {"separators",   "0x1'1.e+",               "cpp c"},
    {"json keys",    "\"k\" ",                 "json"},
    {"json key colons", "\"k\":",              "json"},
    {"cs attributes", "[A\n[B]\n",             "cs"},
    // hash family
    {"command subst", "$(",                    "sh console make powershell cmake"},
    {"command subst 2", "$(a) $(b",            "sh make powershell"},
    {"param subst",  "${",                     "sh console make powershell cmake"},
    {"heredocs",     "<<A\n",                  "sh console"},
    {"heredoc lines", "cat <<-E\n\tE \n",      "sh console"},
    {"ansi quotes",  "$'",                     "sh console"},
    {"prompts",      "$ echo \"\n",            "console"},
    {"comments",     "#",                      "python sh powershell make cmake toml yaml ini"},
    {"comment lines", "# x\n",                 "python sh powershell make cmake toml yaml ini"},
    {"py triple",    "\"\"\"",                 "python toml"},
    {"py fstrings",  "f\"{",                   "python"},
    {"py nested fstrings", "f'{f\"{",          "python"},
    {"py prefixes",  "rb\"",                   "python"},
    {"decorators",   "@a.",                    "python"},
    {"ps here-strings", "@\"\n",               "powershell"},
    {"ps block comments", "<#",                "powershell"},
    {"ps types",     "[a",                     "powershell"},
    {"ps cmdlets",   "Get-",                   "powershell"},
    {"ps ops",       " -eq",                   "powershell"},
    {"make targets", "a b: c\n",               "make"},
    {"make assigns", "A += $(B)\n",            "make"},
    {"make recipes", "\t@echo \"$@\n",         "make"},
    {"make continuations", "a \\\n",           "make"},
    {"make directives", "ifeq (a,b)\n",        "make"},
    {"cmake brackets", "#[=[",                 "cmake"},
    {"cmake brackets 2", "[==[ ]=]",           "cmake"},
    {"cmake commands", "set(a ",               "cmake"},
    {"toml tables",  "[a.b]\n",                "toml"},
    {"toml arrays",  "[[a]]\n",                "toml ini"},
    {"toml keys",    "a.b = 1\n",              "toml ini"},
    {"toml multi",   "'''",                    "toml"},
    {"yaml lists",   "- a: &b *c\n",           "yaml"},
    {"yaml blocks",  "a: |\n  x\n",            "yaml"},
    {"yaml deep",    "  - ",                   "yaml"},
    {"yaml flow",    "{a: [b, ",               "yaml"},
    {"yaml markers", "---\n",                  "yaml"},
    {"yaml anchors", "&a ",                    "yaml"},
    {"ini sections", "[s]\nk=v\n",             "ini"},
    {"diff lines",   "+a\n-b\n c\n",           "diff"},
    {"diff headers", "--- a\n+++ b\n@@ @@\n",  "diff"},
    {"tags",         "<a ",                    "html"},
    {"tags 2",       "<a b=\"",                "html"},
    {"tags 3",       "<a b=c ",                "html"},
    {"comment openers", "<!--",                "html"},
    {"cdata",        "<![CDATA[",              "html"},
    {"doctype",      "<!",                     "html"},
    {"scripts",      "<script>a</script>",     "html"},
    {"open scripts", "<script>",               "html"},
    {"entities",     "&a",                     "html"},
    {"dollar quotes", "$a$",                   "sql"},
    {"dollar quotes 2", "$$ ",                 "sql"},
    {"sql params",   "@a :b ",                 "sql"},
    {"sql names",    "select ",                "sql"},
};

static b32 lang_listed(char const *list, Str lang) {
    if (list[0]=='*' && !list[1]) {
        return 1;
    }
    iz pos = 0;
    for (;;) {
        Str w = word_at(list, &pos);
        if (!w.len) {
            return 0;
        }
        if (equals(w, lang)) {
            return 1;
        }
    }
}

static i32 run_paths(Arena perm, Arena big, i64 limit_ms) {
    ctx.phase = "path";
    ctx.iter  = -1;
    iz const SIZE = (iz)1<<20;
    Arena a  = perm;
    u8 *mem  = new(&a, SIZE + 16, u8);
    Output *o = ctx.out;
    i32 bad = 0;
    i64 worst = 0;
    char const *worst_name = "";
    i64 total = 0;
    i64 runs = 0;
    i64 slow = 0;
    for (iz p = 0; p < countof(paths); p++) {
        Buf b = {mem, 0, SIZE + 16, 0};
        iz  reps = 1;
        Str unit = cstr(paths[p].unit);
        if (!unit.len) {
            unit = (Str){(u8 *)"\0", 1};    // the "nul" row
        }
        reps = SIZE * (iz)scale / 100 / unit.len;
        for (iz i = 0; i < reps && b.len + unit.len <= b.cap; i++) {
            bput(&b, unit);
        }
        Str code = bstr(&b);
        ctx.input = (Str){code.data, min(code.len, (iz)200)};
        for (iz r = 0; r < countof(langrows); r++) {
            iz pos = 0;
            Str lang = word_at(langrows[r].aliases, &pos);
            if (!lang_listed(paths[p].langs, lang)) {
                continue;
            }
            Arena ba = big;
            ctx.phase = paths[p].name;
            i64 t0 = now_us();
            HlSpans sp = hl_highlight(&ba, lang, code);
            i64 us = now_us() - t0;
            check_spans(code, sp, "path");
            alive();
            i64 ms = us / 1000;
            total += us;
            runs++;
            if (us > worst) {
                worst = us;
                worst_name = paths[p].name;
            }
            if (ms > limit_ms) {
                bad++;
                slow++;
                print(o, S("path "));
                print(o, cstr(paths[p].name));
                print(o, S(" ("));
                print(o, lang);
                print(o, S("): "));
                printi64(o, ms);
                print(o, S(" ms, limit "));
                printi64(o, limit_ms);
                print(o, S(" ms\n"));
                flush(o);
            }
        }
    }
    ctx.input = (Str){0};
    print(o, S("path: "));
    printi64(o, runs);
    print(o, S(" runs, "));
    printi64(o, total/1000);
    print(o, S(" ms total, slowest "));
    printi64(o, worst/1000);
    print(o, S(" ms ("));
    print(o, cstr(worst_name));
    print(o, S(")\n"));
    flush(o);
    return bad;
}


// Reading a file, for -f

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
        "usage: hl-test [-s SEED] [-n ITERS] [-i START] [-t WHICH] [-v]\n"
        "  -s SEED   random seed, decimal or 0x hex (default 0x5eed)\n"
        "  -n ITERS  random iterations to run (default 3000)\n"
        "  -i START  first iteration number, to replay a failure (default 0)\n"
        "  -t WHICH  only run known, golden, arena, random or path\n"
        "  -v        golden: list every case\n"
        "  -f LANG FILE  highlight FILE, print a letter per byte, no tests\n"
        "  -z PCT    path: scale the inputs to PCT percent (default 100)\n"
        "  -l MS     path: the time limit per input in ms (default 400)\n"
    );
}

static i32 run(Arena all) {
    Arena mem = all;
    Arena perm = {0}, scratch = {0}, big = {0}, small = {0}, rsmall = {0};
    {
        iz sz[] = {(iz)8<<20, (iz)4<<20, (iz)40<<20, (iz)1<<20, (iz)1<<20};
        Arena *to[] = {&perm, &scratch, &big, &small, &rsmall};
        for (i32 i = 0; i < 5; i++) {
            mem.beg = (u8 *)(((uptr)mem.beg + PAGE - 1) & ~(uptr)(PAGE - 1));
            *to[i]  = (Arena){mem.beg, mem.beg + sz[i]};
            mem.beg += sz[i];
        }
    }
    ctx.out = newoutput(&small, GetStdHandle(STD_OUTPUT_HANDLE), 0, 1<<16);
    AddVectoredExceptionHandler(1, onfault);
    alive();
    CreateThread(0, 0, watchdog, 0, 0, 0);

    u64 seed  = 0x5eed;
    u64 iters = 3000;
    u64 start = 0;
    u64 limit = 400;
    Str which = {0};
    Str mlang = {0};
    Str mtext = {0};
    b32 verbose = 0;
    Args args = getargs(&small);
    for (i32 i = 1; i < args.argc; i++) {
        Str arg  = args.argv[i];
        u64 *dst = 0;
        if (equals(arg, S("-v"))) {
            verbose = 1;
            continue;
        } else if (equals(arg, S("-t")) && i+1<args.argc) {
            which = args.argv[++i];
            continue;
        } else if (equals(arg, S("-f")) && i+2<args.argc) {
            mlang = args.argv[++i];
            mtext = args.argv[++i];
            continue;
        } else if (equals(arg, S("-z"))) {
            dst = &scale;
        } else if (equals(arg, S("-l"))) {
            dst = &limit;
        } else if (equals(arg, S("-s"))) {
            dst = &seed;
        } else if (equals(arg, S("-n"))) {
            dst = &iters;
        } else if (equals(arg, S("-i"))) {
            dst = &start;
        }
        if (!dst || i+1>=args.argc || !parseu64(args.argv[++i], dst)) {
            print(ctx.out, S("hl-test: bad argument: "));
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
    if (mlang.len) {
        // Show how a file highlights: its text, then a letter per byte.
        Str code = readpath(&perm, mtext);
        if (!code.len) {
            print(o, S("hl-test: cannot read the file\n"));
            flush(o);
            return 2;
        }
        Arena a = scratch;
        HlSpans sp = hl_highlight(&a, mlang, code);
        check_spans(code, sp, "-f");
        print(o, render(&a, code, sp));
        flush(o);
        return 0;
    }
    b32 all_tests = !which.len;
    i32 bad = 0;
    u64 t0  = GetTickCount64();

    if (all_tests || equals(which, S("known"))) {
        bad += run_known(scratch);
    }
    if (all_tests || equals(which, S("golden"))) {
        bad += run_golden(scratch, verbose);
    }
    if (all_tests || equals(which, S("arena"))) {
        bad += run_arena(scratch);
    }
    if (all_tests || equals(which, S("random"))) {
        ctx.iter = 0;
        for (u64 i = 0; i < iters; i++) {
            iteration(perm, rsmall, seed, (i64)(start + i));
            if (i%64 == 0) {
                perm.beg = perm.beg;    // each iteration works on a copy
            }
        }
        ctx.iter  = -1;
        ctx.phase = 0;
        ctx.input = (Str){0};
        print(o, S("random: ok seed=0x"));
        printhex(o, seed, 1);
        print(o, S(" iters="));
        printi64(o, (i64)iters);
        print(o, S("\n"));
    }
    if (all_tests || equals(which, S("path"))) {
        bad += run_paths(perm, big, (i64)limit);
    }
    u64 ms = max(GetTickCount64() - t0, (u64)1);
    print(o, bad ? S("hl-test: FAILED, ") : S("hl-test: ok, "));
    printi64(o, bad);
    print(o, S(" problems, "));
    printi64(o, (i64)ms);
    print(o, S(" ms\n"));
    flush(o);
    return bad ? 1 : 0;
}

void mainCRTStartup(void) {
    u8 *mem = VirtualAlloc(0, (uz)((iz)128<<20), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        ExitProcess(3);
    }
    ExitProcess((u32)run((Arena){mem, mem + ((iz)128<<20)}));
}
