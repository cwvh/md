// Platform interface: everything portable code (parser, layout, view) may
// ask of the operating system, and everything the operating system may ask
// of it. No OS types appear here, so each platform can implement it with
// its own text and graphics stack: DirectWrite/Direct2D on Windows,
// CoreText/Core Graphics on macOS, Pango/Cairo on Linux.
//
// Conventions
//   - Lengths, coordinates and sizes are device-independent pixels (DIPs) as
//     f32: 1 DIP is 1/96 inch, so a 96 DPI screen has 1 DIP per pixel. The
//     platform scales to the real display. Portable code never sees device
//     pixels, except that images report their size in pixels, which the
//     layout code treats as DIPs (a 100 px image is 100 DIPs wide).
//   - Origin is the top left, y grows downward.
//   - Text is UTF-8 and not null-terminated (Str). Text positions are UTF-8
//     byte offsets into a paragraph's text; the platform converts to and from
//     its native encoding internally. Offsets passed in should lie on code
//     point boundaries; offsets returned always do.
//   - Functions named os_* are implemented by the platform (main.c and the
//     files it includes). Functions named app_* are implemented by portable
//     code and called by the platform, always on the single UI thread.
//   - Objects the platform hands out (Paragraph, Image) are owned by the
//     platform, not by an arena: they live until os_*_free. Everything else
//     is plain data.

// Geometry and color

typedef struct { f32 x, y; }       Point;
typedef struct { f32 w, h; }       Size;
typedef struct { f32 x, y, w, h; } Rect;
typedef struct { u8 r, g, b, a; }  Color;   // straight (not premultiplied) alpha

static Color rgba(u8 r, u8 g, u8 b, u8 a) {
    return (Color){r, g, b, a};
}

static Color rgb(u8 r, u8 g, u8 b) {
    return (Color){r, g, b, 255};
}

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v<lo ? lo : v>hi ? hi : v;
}


// Memory

// Carve a fixed-size arena off the front of another, without zeroing it.
// Debug builds need page-aligned arenas so guard pages never overlap.
static Arena carve(Arena *a, iz size) {
#ifdef DEBUG
    a->beg = (u8 *)(((uptr)a->beg + PAGE - 1) & ~(uptr)(PAGE - 1));
    assert(size%PAGE == 0);
#endif
    assert(size <= a->end - a->beg);
    Arena r = {a->beg, a->beg+size};
    a->beg += size;
    return r;
}


// Text

typedef enum {
    FONT_PROPORTIONAL,      // the system UI / body font
    FONT_MONOSPACE,         // for code
} FontKind;

// One styled span of a paragraph. Adjacent runs form a single flow of text
// that is wrapped as a whole; runs may split words (bold in mid-word).
//
// An inline box (box_w > 0 and box_h > 0) reserves an empty rectangle in
// the line for something the portable code draws itself, typically an
// image. Its text is ignored; it occupies exactly one position in the
// paragraph, the 3 bytes of U+FFFC (EF BF BC). The box's bottom edge sits
// on the baseline of the line and the line grows to fit its height. Find
// where it ended up with os_para_rects() over its 3 bytes, then draw into
// that rectangle. A box may carry a link id like any other run.
typedef struct {
    Str      text;          // UTF-8; "\n" is a hard line break
    FontKind font;
    f32      size;          // font size in DIPs (the em height)
    b32      bold;
    b32      italic;
    b32      strike;
    b32      underline;
    Color    color;
    i32      link;          // opaque id for hit testing, 0 for none
    f32      box_w;
    f32      box_h;
} Run;

typedef struct Paragraph Paragraph;

// Lay out runs as one paragraph wrapped at max_width DIPs (zero or less
// means never wrap). The text is copied: runs and the strings they point to
// may be discarded as soon as this returns. The paragraph's text is the
// concatenation of the run texts (a box counting for its 3 bytes), and all
// offsets below index into it. Fonts: missing glyphs fall back to other
// system fonts, so any script and color emoji render without portable code
// doing anything. Returns 0 on failure. Free with os_para_free.
static Paragraph *os_para_new(Run const *runs, iz nruns, f32 max_width);

// Re-wrap at a new width without rebuilding. Cheap compared to os_para_new,
// so use it when only the window width changed.
static void os_para_set_width(Paragraph *, f32 max_width);

static void os_para_free(Paragraph *);

typedef enum {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT,
} Align;

// Paragraph-wide style, applied after os_para_new (and kept across
// os_para_set_width). Lines are line_height DIPs tall, the text centered in
// them as in CSS; zero keeps the font's natural height. A paragraph with
// inline boxes ignores line_height, so that a tall box can grow its line.
// align places each line within max_width; with no wrapping width, lines
// are laid out from the left whatever align says.
static void os_para_style(Paragraph *, f32 line_height, Align);

// Length in bytes of the paragraph's text.
static iz os_para_length(Paragraph *);

// Extent of the laid-out text: the width of its widest line (not
// max_width) and the total height of all lines.
static Size os_para_size(Paragraph *);

typedef struct {
    iz  beg;                // byte range of the line, including any trailing
    iz  end;                //   whitespace and line break
    f32 top;                // y of the line's top edge
    f32 height;
    f32 baseline;           // distance from top to the baseline
} LineMetrics;

static iz          os_para_lines(Paragraph *);
static LineMetrics os_para_line(Paragraph *, iz index);

typedef struct {
    iz  offset;             // nearest caret position, a byte offset in
                            //   [0, length]: the start of the hit
                            //   character, or its end for the trailing half
    b32 inside;             // the point lies on a character; false for a
                            //   point beyond the end of a line or outside
                            //   the paragraph (offset is then the nearest)
    i32 link;               // link id of the run under the point; 0 when
                            //   not inside or the run has no link
} TextHit;

// Hit test a point given relative to the paragraph's top left.
static TextHit os_para_hit(Paragraph *, Point p);

// Rectangles, relative to the paragraph's top left, covering the bytes
// [beg, end): one per line segment (more with mixed bidirectional text).
// For selection and find highlights, and for locating inline boxes and
// the backgrounds of inline code. Writes at most cap rectangles to out and
// returns how many the range needs, so pass cap 0 to count.
static iz os_para_rects(Paragraph *, iz beg, iz end, Rect *out, iz cap);

// Draw with the paragraph's top left at origin. Valid only inside app_paint.
static void os_para_draw(Paragraph *, Point origin);


// Drawing. Valid only inside app_paint. Everything is anti-aliased and
// in DIPs, so fractional coordinates are fine.

// Fill the whole window. Call first in app_paint.
static void os_clear(Color);
static void os_fill_rect(Rect, Color);
static void os_fill_rrect(Rect, f32 radius, Color);
static void os_stroke_rrect(Rect, f32 radius, f32 width, Color);  // stroke centered on the edge
static void os_stroke_line(Point a, Point b, f32 width, Color);

// Clips nest; every push needs a pop before app_paint returns. Rectangles
// only.
static void os_clip_push(Rect);
static void os_clip_pop(void);


// Images

typedef struct Image Image;

// Decode PNG, JPEG, GIF, BMP and whatever else the platform decoder knows
// from a byte buffer; this takes bytes so that local files and downloads
// share one path. The bytes may be discarded afterwards. Returns 0 if
// undecodable. Free with os_image_free.
static Image *os_image_decode(Str bytes);
static void   os_image_free(Image *);

// Size in pixels (see the units note above).
static Size os_image_size(Image *);

// Animation: a still image has 1 frame and delay 0. delay is in
// milliseconds, at least 10.
static i32 os_image_frames(Image *);
static i32 os_image_delay(Image *, i32 frame);

// Draw frame (0 to frames-1), scaled to fill dst. The frame is fully
// composited: for an animated GIF it already includes the earlier frames
// as the file's disposal rules dictate, so the caller just advances the
// frame number on a timer. Playing in order is cheap; jumping around
// recomposites from the start. Valid only inside app_paint.
static void os_image_draw(Image *, i32 frame, Rect dst);


// Window and system

typedef enum {
    CURSOR_ARROW,
    CURSOR_HAND,
    CURSOR_IBEAM,
} Cursor;

// Set the mouse cursor over the window. Sticky: it stays until set again,
// so call it from app_mouse_move (and after anything that moves content
// under a still mouse).
static void os_set_cursor(Cursor);

// Ask for an app_paint soon. Cheap, and repeated calls coalesce.
static void os_invalidate(void);

static void os_set_title(Str title);

// Start (or restart) a repeating timer: app_timer(id) is called about
// every ms milliseconds until os_timer_stop. Ids are small positive
// integers chosen by the caller.
static void os_timer_start(i32 id, i32 ms);
static void os_timer_stop(i32 id);

// True if the user's system theme is dark. Re-query on app_theme_changed.
static b32 os_dark_mode(void);

// Display scale factor of the window's monitor (1 at 96 DPI, 1.5 at 144).
// Informational: all drawing is already in DIPs, so layout doesn't need it.
static f32 os_scale(void);

// Client area of the window in DIPs.
static Size os_window_size(void);

// Ask the window to close; app_close is not called.
static void os_quit(void);

// Read a whole file into the arena. Fails (returns 0) if it can't be
// opened or won't fit. The data is not terminated. The path is UTF-8.
static b32 os_read_file(Arena *a, Str path, Str *out);

// Watch one file (UTF-8 path) and call app_file_changed when it changes;
// the call is debounced, so editors that write in several steps give one
// notification. Replaces any earlier watch; an empty path stops watching.
static void os_watch_file(Str path);

// Open an http, https or mailto URL in the default handler. Anything else
// is refused (returns 0): a link in a document must never launch a program
// or open a file by itself.
static b32 os_open_url(Str url);

// Show the open-file dialog. On success writes the chosen path (UTF-8)
// into the arena and returns 1.
static b32 os_open_dialog(Arena *a, Str *path);

// The clipboard as UTF-8 text. Line breaks in what the app hands over are
// LF; the platform converts to and from what its clipboard uses.
static void os_clipboard_set(Str utf8);

// The clipboard's text, copied into the arena (with LF line breaks), or 0
// if it holds none or it won't fit.
static b32 os_clipboard_get(Arena *a, Str *out);

// Milliseconds on a monotonic clock, from an arbitrary start. For telling
// double and triple clicks apart.
static u64 os_time_ms(void);


// Input

typedef enum {
    KEY_NONE = 0,
    // Letters are their upper-case ASCII code, digits their ASCII code.
    KEY_A = 'A', KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S,
    KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0 = '0', KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8,
    KEY_9,
    KEY_SPACE = ' ',
    KEY_SLASH = '/',
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN,
    KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_BACKSPACE, KEY_TAB,
    KEY_ENTER, KEY_ESCAPE,
    KEY_PLUS,               // the = / + key or the keypad plus
    KEY_MINUS,              // the - key or the keypad minus
    KEY_F1 = 0x120, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
} Key;

// Modifier bits. MOD_PRIMARY is the shortcut modifier: Ctrl on Windows and
// Linux, Command on macOS. Use it for Ctrl+F, Ctrl+C, zoom, etc.
enum {
    MOD_SHIFT   = 1,
    MOD_PRIMARY = 2,
    MOD_ALT     = 4,
};

typedef enum {
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_MIDDLE,
} Button;


// Callbacks, implemented by portable code (view.c). The
// platform calls them from its message loop; all take DIPs. Memory: the
// platform gives the app one permanent arena at init, and a scratch arena
// by value on every paint (its contents don't survive the call). Portable
// code may carve more arenas out of perm at init.

// First call. path is the command-line file (UTF-8), empty if none. size
// is the initial client area. Called once, after the window exists and
// before the first paint.
static void app_init(Arena *perm, Str path, Size size);

// Draw the whole window. Called inside a begin/end draw pair, never
// re-entrantly. The platform has not cleared anything: call os_clear.
static void app_paint(Arena scratch, Size size);

// The client area changed size (also on monitor DPI change).
static void app_resize(Size size);

// A key went down (auto-repeat arrives as more calls); mods is a MOD_ mask.
static void app_key(Key key, u32 mods);

// Typed text, one code point at a time, already keyboard-layout and IME
// processed. Control characters are not delivered, only printable text.
static void app_char(i32 rune);

static void app_mouse_move(Point p, u32 mods);
static void app_mouse_leave(void);                      // the pointer left the window
static void app_mouse_down(Point p, Button b, u32 mods);
static void app_mouse_up(Point p, Button b, u32 mods);
static void app_double_click(Point p, u32 mods);        // left button; follows a mouse_down

// Wheel or trackpad scroll in notches (one notch is 1.0; high-resolution
// wheels deliver fractions). dy > 0 means the wheel turned away from the
// user, i.e. scroll toward the start of the document; dx > 0 is rightward.
// With MOD_PRIMARY held it is conventionally a zoom.
static void app_wheel(Point p, f32 dx, f32 dy, u32 mods);

static void app_timer(i32 id);
static void app_file_changed(void);
static void app_file_dropped(Str path);
static void app_theme_changed(void);

// The window is about to close.
static void app_close(void);
