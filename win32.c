// Hand-written Win32, COM, Direct2D and DirectWrite declarations (no
// windows.h). Each COM interface is declared as a vtable with every slot in
// order; slots we never call are void * placeholders. The first half of
// this file is layout: structs, vtables and GUIDs. comcheck.c compares it
// against the official headers, so a wrong slot or field offset fails the
// build instead of crashing at runtime. The second half is constants and
// function prototypes, which comcheck.c skips (see COMCHECK).
//
// COM methods that return a struct (ID2D1RenderTarget::GetSize and friends)
// use a hidden return pointer in the C++ ABI, which the C declarations in
// the official headers get wrong. We never call them: those slots are
// placeholders.

typedef i64 iptr;

typedef struct {
    u32 a;
    u16 b;
    u16 c;
    u8  d[8];
} Guid;
_Static_assert(sizeof(Guid) == 16, "");

static b32 guideq(Guid const *x, Guid const *y) {
    u8 const *p = (u8 const *)x;
    u8 const *q = (u8 const *)y;
    for (i32 i = 0; i < 16; i++) {
        if (p[i] != q[i]) {
            return 0;
        }
    }
    return 1;
}

static const Guid iid_unknown          = {0x00000000, 0x0000, 0x0000, {0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const Guid iid_d2d_factory      = {0x06152247, 0x6f50, 0x465a, {0x92,0x45,0x11,0x8b,0xfd,0x3b,0x60,0x07}};
static const Guid iid_dw_factory    = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2,0xe8,0x1a,0xdc,0x7d,0x93,0xdb,0x48}};
static const Guid iid_dw_inline = {0x8339fde3, 0x106f, 0x47ab, {0x83,0x73,0x1c,0x62,0x95,0xeb,0x10,0xb3}};


// Plain Win32 structs. Handles are uptr, LPARAM/LRESULT are iptr.

typedef struct {
    i32 x;
    i32 y;
} WinPoint;

typedef struct {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
} WinRect;

typedef struct {
    uptr     hwnd;
    u32      message;
    uptr     wparam;
    iptr     lparam;
    u32      time;
    WinPoint pt;
    u32      private_;
} Msg;

typedef iptr (*WndProc)(uptr, u32, uptr, iptr);

typedef struct {
    u32         size;
    u32         style;
    WndProc     proc;
    i32         clsextra;
    i32         wndextra;
    uptr        instance;
    uptr        icon;
    uptr        cursor;
    uptr        background;
    c16 const  *menuname;
    c16 const  *classname;
    uptr        iconsm;
} WndClassEx;

typedef struct {
    uptr    hdc;
    b32     erase;
    WinRect paint;
    b32     restore;
    b32     incupdate;
    u8      reserved[32];
} PaintStruct;

typedef struct {
    WinPoint reserved;
    WinPoint maxsize;
    WinPoint maxpos;
    WinPoint mintrack;
    WinPoint maxtrack;
} MinMaxInfo;

typedef struct {
    u32 attributes;
    u32 created[2];
    u32 accessed[2];
    u32 written[2];     // FILETIME: low, high
    u32 sizehigh;
    u32 sizelow;
} FileAttributeData;

typedef struct {
    u32        size;
    uptr       owner;
    uptr       instance;
    c16 const *filter;
    c16       *customfilter;
    u32        maxcustfilter;
    u32        filterindex;
    c16       *file;
    u32        maxfile;
    c16       *filetitle;
    u32        maxfiletitle;
    c16 const *initialdir;
    c16 const *title;
    u32        flags;
    u16        fileoffset;
    u16        fileextension;
    c16 const *defext;
    iptr       custdata;
    void      *hook;
    c16 const *templatename;
    void      *reserved;
    u32        reserved2;
    u32        flagsex;
} OpenFileName;


// Direct2D structs

typedef struct { f32 r, g, b, a; }          D2dColor;
typedef struct { f32 x, y; }                D2dPoint;
typedef struct { f32 left, top, right, bottom; } D2dRect;
typedef struct { u32 width, height; }       D2dSizeU;
typedef struct { D2dRect rect; f32 rx, ry; } D2dRoundedRect;
typedef struct { f32 m[6]; }                D2dMatrix;     // 3x2, row-major
typedef struct { u32 format; u32 alphamode; } D2dPixelFormat;

typedef struct {
    u32            type;
    D2dPixelFormat pixelformat;
    f32            dpix;
    f32            dpiy;
    u32            usage;
    u32            minlevel;
} D2dRenderTargetProps;

typedef struct {
    uptr     hwnd;
    D2dSizeU pixelsize;
    u32      presentoptions;
} D2dHwndRenderTargetProps;

typedef struct {
    f32       opacity;
    D2dMatrix transform;
} D2dBrushProps;


// DirectWrite structs

typedef struct { u32 start, length; } DwTextRange;

typedef struct {
    f32 left;
    f32 top;
    f32 width;
    f32 widthtrailing;      // widthIncludingTrailingWhitespace
    f32 height;
    f32 layoutwidth;
    f32 layoutheight;
    u32 maxbidideptn;
    u32 linecount;
} DwTextMetrics;

typedef struct {
    u32 length;
    u32 trailingwhitespace;
    u32 newline;
    f32 height;
    f32 baseline;
    b32 trimmed;
} DwLineMetrics;

typedef struct {
    u32 position;
    u32 length;
    f32 left;
    f32 top;
    f32 width;
    f32 height;
    u32 bidilevel;
    b32 istext;
    b32 istrimmed;
} DwHitTestMetrics;

typedef struct {
    f32 width;
    f32 height;
    f32 baseline;
    b32 sideways;
} DwInlineMetrics;

typedef struct { f32 left, top, right, bottom; } DwOverhangMetrics;


// COM vtables. Every object is a pointer to a pointer to its vtable.

typedef struct UnknownVtbl UnknownVtbl;
typedef struct { UnknownVtbl const *v; } ComObj;
struct UnknownVtbl {
    i32 (*QueryInterface)(void *, Guid const *, void **);
    u32 (*AddRef)(void *);
    u32 (*Release)(void *);
};

// Call a method: COM(obj, Method, args...)
#define COM(o, m, ...) ((o)->v->m((o), ##__VA_ARGS__))

// ID2D1Factory: slots 3..16
typedef struct D2dFactoryVtbl D2dFactoryVtbl;
typedef struct { D2dFactoryVtbl const *v; } D2dFactory;
struct D2dFactoryVtbl {
    UnknownVtbl base;
    void *ReloadSystemMetrics;
    void *GetDesktopDpi;
    void *CreateRectangleGeometry;
    void *CreateRoundedRectangleGeometry;
    void *CreateEllipseGeometry;
    void *CreateGeometryGroup;
    void *CreateTransformedGeometry;
    void *CreatePathGeometry;
    void *CreateStrokeStyle;
    void *CreateDrawingStateBlock;
    void *CreateWicBitmapRenderTarget;
    i32 (*CreateHwndRenderTarget)(D2dFactory *, D2dRenderTargetProps const *,
            D2dHwndRenderTargetProps const *, void **);
    void *CreateDxgiSurfaceRenderTarget;
    void *CreateDCRenderTarget;
};

// ID2D1HwndRenderTarget (ID2D1RenderTarget plus three slots). Brushes and
// bitmaps are passed as void *: a brush only needs Release.
typedef struct D2dRtVtbl D2dRtVtbl;
typedef struct { D2dRtVtbl const *v; } D2dRt;
struct D2dRtVtbl {
    UnknownVtbl base;
    void *GetFactory;
    void *CreateBitmap;
    void *CreateBitmapFromWicBitmap;
    void *CreateSharedBitmap;
    void *CreateBitmapBrush;
    i32 (*CreateSolidColorBrush)(D2dRt *, D2dColor const *, D2dBrushProps const *, void **);
    void *CreateGradientStopCollection;
    void *CreateLinearGradientBrush;
    void *CreateRadialGradientBrush;
    void *CreateCompatibleRenderTarget;
    void *CreateLayer;
    void *CreateMesh;
    void (*DrawLine)(D2dRt *, D2dPoint, D2dPoint, void *brush, f32 width, void *style);
    void *DrawRectangle;
    void (*FillRectangle)(D2dRt *, D2dRect const *, void *brush);
    void (*DrawRoundedRectangle)(D2dRt *, D2dRoundedRect const *, void *brush, f32 width, void *style);
    void (*FillRoundedRectangle)(D2dRt *, D2dRoundedRect const *, void *brush);
    void *DrawEllipse;
    void *FillEllipse;
    void *DrawGeometry;
    void *FillGeometry;
    void *FillMesh;
    void *FillOpacityMask;
    void (*DrawBitmap)(D2dRt *, void *bitmap, D2dRect const *dst, f32 opacity,
            u32 interpolation, D2dRect const *src);
    void *DrawText;
    void (*DrawTextLayout)(D2dRt *, D2dPoint, void *layout, void *brush, u32 options);
    void *DrawGlyphRun;
    void (*SetTransform)(D2dRt *, D2dMatrix const *);
    void *GetTransform;
    void *SetAntialiasMode;
    void *GetAntialiasMode;
    void (*SetTextAntialiasMode)(D2dRt *, u32);
    void *GetTextAntialiasMode;
    void *SetTextRenderingParams;
    void *GetTextRenderingParams;
    void *SetTags;
    void *GetTags;
    void *PushLayer;
    void *PopLayer;
    void *Flush;
    void *SaveDrawingState;
    void *RestoreDrawingState;
    void (*PushAxisAlignedClip)(D2dRt *, D2dRect const *, u32 aamode);
    void (*PopAxisAlignedClip)(D2dRt *);
    void (*Clear)(D2dRt *, D2dColor const *);
    void (*BeginDraw)(D2dRt *);
    i32  (*EndDraw)(D2dRt *, u64 *tag1, u64 *tag2);
    void *GetPixelFormat;       // returns a struct, see top of file
    void (*SetDpi)(D2dRt *, f32, f32);
    void *GetDpi;
    void *GetSize;              // returns a struct
    void *GetPixelSize;         // returns a struct
    void *GetMaximumBitmapSize;
    void *IsSupported;
    void *CheckWindowState;
    i32 (*Resize)(D2dRt *, D2dSizeU const *);
    void *GetHwnd;
};

// IDWriteFactory: slots 3..23
typedef struct DwFactoryVtbl DwFactoryVtbl;
typedef struct { DwFactoryVtbl const *v; } DwFactory;
struct DwFactoryVtbl {
    UnknownVtbl base;
    void *GetSystemFontCollection;
    void *CreateCustomFontCollection;
    void *RegisterFontCollectionLoader;
    void *UnregisterFontCollectionLoader;
    void *CreateFontFileReference;
    void *CreateCustomFontFileReference;
    void *CreateFontFace;
    void *CreateRenderingParams;
    void *CreateMonitorRenderingParams;
    void *CreateCustomRenderingParams;
    void *RegisterFontFileLoader;
    void *UnregisterFontFileLoader;
    i32 (*CreateTextFormat)(DwFactory *, c16 const *family, void *collection,
            u32 weight, u32 style, u32 stretch, f32 size, c16 const *locale, void **);
    void *CreateTypography;
    void *GetGdiInterop;
    i32 (*CreateTextLayout)(DwFactory *, c16 const *, u32 len, void *format,
            f32 maxw, f32 maxh, void **);
    void *CreateGdiCompatibleTextLayout;
    void *CreateEllipsisTrimmingSign;
    void *CreateTextAnalyzer;
    void *CreateNumberSubstitution;
    void *CreateGlyphRunAnalysis;
};

// IDWriteTextFormat: slots 3..27, then IDWriteTextLayout 28..66. A layout
// is also a text format, so one vtable serves both.
typedef struct DwLayoutVtbl DwLayoutVtbl;
typedef struct { DwLayoutVtbl const *v; } DwLayout;
struct DwLayoutVtbl {
    UnknownVtbl base;
    i32 (*SetTextAlignment)(DwLayout *, u32);
    void *SetParagraphAlignment;
    i32 (*SetWordWrapping)(DwLayout *, u32);
    void *SetReadingDirection;
    void *SetFlowDirection;
    void *SetIncrementalTabStop;
    void *SetTrimming;
    void *SetLineSpacing;
    void *GetTextAlignment;
    void *GetParagraphAlignment;
    void *GetWordWrapping;
    void *GetReadingDirection;
    void *GetFlowDirection;
    void *GetIncrementalTabStop;
    void *GetTrimming;
    void *GetLineSpacing;
    void *GetFontCollection;
    void *GetFontFamilyNameLength;
    void *GetFontFamilyName;
    void *GetFontWeight;
    void *GetFontStyle;
    void *GetFontStretch;
    void *GetFontSize;
    void *GetLocaleNameLength;
    void *GetLocaleName;
    // IDWriteTextLayout
    i32 (*SetMaxWidth)(DwLayout *, f32);
    void *SetMaxHeight;
    void *SetFontCollection;
    i32 (*SetFontFamilyName)(DwLayout *, c16 const *, DwTextRange);
    i32 (*SetFontWeight)(DwLayout *, u32, DwTextRange);
    i32 (*SetFontStyle)(DwLayout *, u32, DwTextRange);
    void *SetFontStretch;
    i32 (*SetFontSize)(DwLayout *, f32, DwTextRange);
    i32 (*SetUnderline)(DwLayout *, b32, DwTextRange);
    i32 (*SetStrikethrough)(DwLayout *, b32, DwTextRange);
    i32 (*SetDrawingEffect)(DwLayout *, void *, DwTextRange);
    i32 (*SetInlineObject)(DwLayout *, void *, DwTextRange);
    void *SetTypography;
    void *SetLocaleName;
    void *GetMaxWidth;
    void *GetMaxHeight;
    void *LGetFontCollection;
    void *LGetFontFamilyNameLength;
    void *LGetFontFamilyName;
    void *LGetFontWeight;
    void *LGetFontStyle;
    void *LGetFontStretch;
    void *LGetFontSize;
    void *GetUnderline;
    void *GetStrikethrough;
    void *GetDrawingEffect;
    void *GetInlineObject;
    void *GetTypography;
    void *LGetLocaleNameLength;
    void *LGetLocaleName;
    void *Draw;
    i32 (*GetLineMetrics)(DwLayout *, DwLineMetrics *, u32 max, u32 *count);
    i32 (*GetMetrics)(DwLayout *, DwTextMetrics *);
    void *GetOverhangMetrics;
    void *GetClusterMetrics;
    void *DetermineMinWidth;
    i32 (*HitTestPoint)(DwLayout *, f32 x, f32 y, b32 *trailing, b32 *inside, DwHitTestMetrics *);
    void *HitTestTextPosition;
    i32 (*HitTestTextRange)(DwLayout *, u32 pos, u32 len, f32 ox, f32 oy,
            DwHitTestMetrics *, u32 max, u32 *count);
};

// IDWriteInlineObject, which we implement ourselves (see gfx.c).
typedef struct DwInlineVtbl DwInlineVtbl;
typedef struct { DwInlineVtbl const *v; } DwInline;
struct DwInlineVtbl {
    UnknownVtbl base;
    i32 (*Draw)(DwInline *, void *ctx, void *renderer, f32 ox, f32 oy,
            b32 sideways, b32 rtl, void *effect);
    i32 (*GetMetrics)(DwInline *, DwInlineMetrics *);
    i32 (*GetOverhangMetrics)(DwInline *, DwOverhangMetrics *);
    i32 (*GetBreakConditions)(DwInline *, u32 *before, u32 *after);
};

#ifndef COMCHECK

#define W32(r) __declspec(dllimport) r

// BEGIN CONSTANTS (each must match the official header, see comcheck.c)
enum {
    MEM_COMMIT     = 0x1000,
    MEM_RESERVE    = 0x2000,
    PAGE_NOACCESS  = 0x01,
    PAGE_READWRITE = 0x04,

    STD_INPUT_HANDLE  = -10,
    STD_OUTPUT_HANDLE = -11,
    STD_ERROR_HANDLE  = -12,

    GENERIC_READ          = (i32)0x80000000,
    FILE_SHARE_READ       = 0x0001,
    FILE_SHARE_WRITE      = 0x0002,
    FILE_SHARE_DELETE     = 0x0004,
    OPEN_EXISTING         = 3,
    FILE_ATTRIBUTE_NORMAL = 0x80,

    GMEM_MOVEABLE = 0x0002,
    CF_UNICODETEXT = 13,

    // Windows and classes
    CS_DBLCLKS          = 0x0008,
    WS_OVERLAPPEDWINDOW = 0x00cf0000,
    WS_CLIPCHILDREN     = 0x02000000,
    CW_USEDEFAULT       = (i32)0x80000000,
    SW_SHOW             = 5,
    SW_SHOWNORMAL       = 1,
    SIZE_MINIMIZED      = 1,
    HTCLIENT            = 1,
    SWP_NOMOVE          = 0x0002,
    SWP_NOZORDER        = 0x0004,
    SWP_NOACTIVATE      = 0x0010,
    IDC_ARROW           = 32512,
    IDC_IBEAM           = 32513,
    IDC_HAND            = 32649,
    MB_OK               = 0,
    MB_ICONERROR        = 0x10,

    WM_CREATE        = 0x0001,
    WM_DESTROY       = 0x0002,
    WM_SIZE          = 0x0005,
    WM_PAINT         = 0x000f,
    WM_CLOSE         = 0x0010,
    WM_ERASEBKGND    = 0x0014,
    WM_SETTINGCHANGE = 0x001a,
    WM_SETCURSOR     = 0x0020,
    WM_GETMINMAXINFO = 0x0024,
    WM_KEYDOWN       = 0x0100,
    WM_CHAR          = 0x0102,
    WM_SYSKEYDOWN    = 0x0104,
    WM_TIMER         = 0x0113,
    WM_MOUSEMOVE     = 0x0200,
    WM_LBUTTONDOWN   = 0x0201,
    WM_LBUTTONUP     = 0x0202,
    WM_LBUTTONDBLCLK = 0x0203,
    WM_RBUTTONDOWN   = 0x0204,
    WM_RBUTTONUP     = 0x0205,
    WM_MBUTTONDOWN   = 0x0207,
    WM_MBUTTONUP     = 0x0208,
    WM_MOUSEWHEEL    = 0x020a,
    WM_MOUSEHWHEEL   = 0x020e,
    WM_CAPTURECHANGED = 0x0215,
    WM_DROPFILES     = 0x0233,
    WM_DPICHANGED    = 0x02e0,

    MK_SHIFT   = 0x0004,
    MK_CONTROL = 0x0008,

    VK_BACK     = 0x08,
    VK_TAB      = 0x09,
    VK_RETURN   = 0x0d,
    VK_SHIFT    = 0x10,
    VK_CONTROL  = 0x11,
    VK_MENU     = 0x12,
    VK_ESCAPE   = 0x1b,
    VK_SPACE    = 0x20,
    VK_PRIOR    = 0x21,
    VK_NEXT     = 0x22,
    VK_END      = 0x23,
    VK_HOME     = 0x24,
    VK_LEFT     = 0x25,
    VK_UP       = 0x26,
    VK_RIGHT    = 0x27,
    VK_DOWN     = 0x28,
    VK_INSERT   = 0x2d,
    VK_DELETE   = 0x2e,
    VK_NUMPAD0  = 0x60,
    VK_ADD      = 0x6b,
    VK_SUBTRACT = 0x6d,
    VK_F1       = 0x70,
    VK_F12      = 0x7b,
    VK_OEM_PLUS  = 0xbb,
    VK_OEM_MINUS = 0xbd,
    VK_OEM_2     = 0xbf,

    // Registry
    KEY_READ = 0x20019,

    // OPENFILENAME
    OFN_FILEMUSTEXIST = 0x1000,
    OFN_PATHMUSTEXIST = 0x0800,
    OFN_EXPLORER      = 0x0080000,
    OFN_NOCHANGEDIR   = 0x0008,

    // Direct2D and DirectWrite (official enum values)
    D2D1_FACTORY_TYPE_SINGLE_THREADED = 0,
    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT = 4,
    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE = 0,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR = 1,
    DWRITE_FACTORY_TYPE_SHARED = 0,
    DWRITE_FONT_WEIGHT_NORMAL = 400,
    DWRITE_FONT_WEIGHT_BOLD = 700,
    DWRITE_FONT_STYLE_NORMAL = 0,
    DWRITE_FONT_STYLE_ITALIC = 2,
    DWRITE_FONT_STRETCH_NORMAL = 5,
    DWRITE_BREAK_CONDITION_NEUTRAL = 0,
};
// END CONSTANTS

enum {
    S_OK = 0,
    E_NOINTERFACE = (i32)0x80004002,
    E_FAIL        = (i32)0x80004005,
    E_NOT_SUFFICIENT_BUFFER = (i32)0x8007007a,   // HRESULT_FROM_WIN32(122)
    D2DERR_RECREATE_TARGET  = (i32)0x8899000c,
    ERROR_SUCCESS = 0,
};

#define INFINITE             0xffffffffu  // does not fit in an enum
#define INVALID_HANDLE_VALUE ((uptr)-1)
#define HKEY_CURRENT_USER    ((uptr)0x80000001)
#define HEAP_ZERO_MEMORY     8
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((uptr)-4)
#define MAKEINTRESOURCEW(i)  ((c16 const *)(uptr)(i))
#define FAILED(hr)           ((hr) < 0)
#define LOWORD(x)            ((u32)(x) & 0xffffu)
#define HIWORD(x)            (((u32)(x) >> 16) & 0xffffu)

W32(void *) VirtualAlloc(void *, uz, u32, u32);
W32(b32)    VirtualProtect(void *, uz, u32, u32 *);
W32(uptr)   CreateFileW(c16 const *, u32, u32, void *, u32, u32, uptr);
W32(b32)    ReadFile(uptr, u8 *, u32, u32 *, void *);
W32(b32)    GetFileSizeEx(uptr, i64 *);
W32(b32)    WriteFile(uptr, u8 const *, u32, u32 *, void *);
W32(b32)    WriteConsoleW(uptr, c16 const *, u32, u32 *, void *);
W32(uptr)   GetStdHandle(i32);
W32(b32)    GetConsoleMode(uptr, u32 *);
W32(u64)    GetTickCount64(void);
W32(b32)    QueryPerformanceCounter(i64 *);
W32(b32)    QueryPerformanceFrequency(i64 *);
W32(b32)    GetFileAttributesExW(c16 const *, i32, void *);
W32(b32)    CloseHandle(uptr);
W32(u32)    GetLastError(void);
W32(c16 *)  GetCommandLineW(void);
W32(uptr)   GetModuleHandleW(c16 const *);
W32(void *) GetProcAddress(uptr, char const *);
W32(uptr)   GetProcessHeap(void);
W32(void *) HeapAlloc(uptr, u32, uz);
W32(b32)    HeapFree(uptr, u32, void *);
W32(uptr)   GlobalAlloc(u32, uz);
W32(void *) GlobalLock(uptr);
W32(b32)    GlobalUnlock(uptr);
__declspec(dllimport) __attribute((noreturn))
void ExitProcess(u32);

W32(u16)    RegisterClassExW(WndClassEx const *);
W32(uptr)   CreateWindowExW(u32, c16 const *, c16 const *, u32, i32, i32, i32, i32,
                uptr, uptr, uptr, void *);
W32(iptr)   DefWindowProcW(uptr, u32, uptr, iptr);
W32(b32)    DestroyWindow(uptr);
W32(void)   PostQuitMessage(i32);
W32(b32)    ShowWindow(uptr, i32);
W32(b32)    GetMessageW(Msg *, uptr, u32, u32);
W32(b32)    TranslateMessage(Msg const *);
W32(iptr)   DispatchMessageW(Msg const *);
W32(uptr)   BeginPaint(uptr, PaintStruct *);
W32(b32)    EndPaint(uptr, PaintStruct const *);
W32(b32)    InvalidateRect(uptr, WinRect const *, b32);
W32(b32)    GetClientRect(uptr, WinRect *);
W32(b32)    SetWindowPos(uptr, uptr, i32, i32, i32, i32, u32);
W32(b32)    SetWindowTextW(uptr, c16 const *);
W32(b32)    AdjustWindowRectEx(WinRect *, u32, b32, u32);
W32(uptr)   SetTimer(uptr, uptr, u32, void *);
W32(b32)    KillTimer(uptr, uptr);
W32(uptr)   LoadCursorW(uptr, c16 const *);
W32(uptr)   SetCursor(uptr);
W32(uptr)   SetCapture(uptr);
W32(b32)    ReleaseCapture(void);
W32(i16)    GetKeyState(i32);
W32(b32)    ScreenToClient(uptr, WinPoint *);
W32(i32)    MessageBoxW(uptr, c16 const *, c16 const *, u32);
W32(b32)    OpenClipboard(uptr);
W32(b32)    EmptyClipboard(void);
W32(uptr)   SetClipboardData(u32, uptr);
W32(b32)    CloseClipboard(void);

W32(uptr)   ShellExecuteW(uptr, c16 const *, c16 const *, c16 const *, c16 const *, i32);
W32(void)   DragAcceptFiles(uptr, b32);
W32(u32)    DragQueryFileW(uptr, u32, c16 *, u32);
W32(void)   DragFinish(uptr);
W32(b32)    GetOpenFileNameW(OpenFileName *);
W32(i32)    RegOpenKeyExW(uptr, c16 const *, u32, u32, uptr *);
W32(i32)    RegQueryValueExW(uptr, c16 const *, void *, u32 *, void *, u32 *);
W32(i32)    RegCloseKey(uptr);
W32(i32)    DwmSetWindowAttribute(uptr, u32, void const *, u32);
W32(i32)    D2D1CreateFactory(u32, Guid const *, void const *, void **);
W32(i32)    DWriteCreateFactory(u32, Guid const *, void **);

#endif
