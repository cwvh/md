// Verifies the hand-written declarations in win32.c against the official
// mingw headers. Never part of md.exe: it includes windows.h and links the
// C runtime, so it is built and run by `make comcheck`.
//
// Two modes, because win32.c and windows.h cannot share a translation unit
// whole (the same function names have different parameter types):
//
//   default   Layout. Includes the official headers, then win32.c with
//             COMCHECK defined, which leaves out its constants and function
//             prototypes and keeps every struct, vtable and GUID. Static
//             assertions check that each vtable slot has the same offset as
//             the official one, and each struct the same size and field
//             offsets. GUID values cannot be compared at compile time in C,
//             so main() compares them at run time.
//
//   -DCONSTS  Constants. Includes only the official headers and
//             comcheck-consts.inc, which the Makefile generates from the
//             constants block in win32.c: one static assertion per constant
//             comparing our value with the official macro or enumerator.
//
// Function prototypes are not checked. They are few and each is a plain
// import by name; a wrong one shows up at once as a crash or link error.

#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <stdio.h>
#include <string.h>

#ifdef CONSTS

typedef signed int i32;
#  include "comcheck-consts.inc"

int main(void) {
    puts("comcheck: constants ok");
    return 0;
}

#else

#define COMCHECK
#include "base.c"
#include "win32.c"

static b32 os_write(Output *o) {
    (void)o;
    return 0;
}

// Slot or field named om in ours and tm in theirs.
#define SLOT(ours, theirs, om, tm) \
    _Static_assert(__builtin_offsetof(ours, om) == __builtin_offsetof(theirs, tm), \
                   #ours "." #om " is not at the official offset")
#define SAME(ours, theirs, m)  SLOT(ours, theirs, m, m)
#define SIZE(ours, theirs) \
    _Static_assert(sizeof(ours) == sizeof(theirs), #ours " is not the official size")
// A vtable is as long as its last declared slot says.
#define LAST(ours, theirs, m) \
    _Static_assert(sizeof(ours) == __builtin_offsetof(theirs, m) + sizeof(void *), \
                   #ours " has the wrong number of slots")

// IUnknown
SLOT(UnknownVtbl, IUnknownVtbl, QueryInterface, QueryInterface);
SLOT(UnknownVtbl, IUnknownVtbl, AddRef, AddRef);
SLOT(UnknownVtbl, IUnknownVtbl, Release, Release);
LAST(UnknownVtbl, IUnknownVtbl, Release);

// ID2D1Factory
#define F(m) SLOT(D2dFactoryVtbl, ID2D1FactoryVtbl, m, m)
SLOT(D2dFactoryVtbl, ID2D1FactoryVtbl, base.QueryInterface, Base.QueryInterface);
SLOT(D2dFactoryVtbl, ID2D1FactoryVtbl, base.AddRef, Base.AddRef);
SLOT(D2dFactoryVtbl, ID2D1FactoryVtbl, base.Release, Base.Release);
F(ReloadSystemMetrics); F(GetDesktopDpi); F(CreateRectangleGeometry);
F(CreateRoundedRectangleGeometry); F(CreateEllipseGeometry); F(CreateGeometryGroup);
F(CreateTransformedGeometry); F(CreatePathGeometry); F(CreateStrokeStyle);
F(CreateDrawingStateBlock); F(CreateWicBitmapRenderTarget); F(CreateHwndRenderTarget);
F(CreateDxgiSurfaceRenderTarget); F(CreateDCRenderTarget);
LAST(D2dFactoryVtbl, ID2D1FactoryVtbl, CreateDCRenderTarget);
#undef F

// ID2D1HwndRenderTarget. The official vtable nests: HwndRenderTarget >
// RenderTarget > Resource > IUnknown.
#define R(m) SLOT(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, m, Base.m)
SLOT(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, base.QueryInterface, Base.Base.Base.QueryInterface);
SLOT(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, base.AddRef, Base.Base.Base.AddRef);
SLOT(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, base.Release, Base.Base.Base.Release);
SLOT(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, GetFactory, Base.Base.GetFactory);
R(CreateBitmap); R(CreateBitmapFromWicBitmap); R(CreateSharedBitmap);
R(CreateBitmapBrush); R(CreateSolidColorBrush); R(CreateGradientStopCollection);
R(CreateLinearGradientBrush); R(CreateRadialGradientBrush);
R(CreateCompatibleRenderTarget); R(CreateLayer); R(CreateMesh);
R(DrawLine); R(DrawRectangle); R(FillRectangle); R(DrawRoundedRectangle);
R(FillRoundedRectangle); R(DrawEllipse); R(FillEllipse); R(DrawGeometry);
R(FillGeometry); R(FillMesh); R(FillOpacityMask); R(DrawBitmap); R(DrawText);
R(DrawTextLayout); R(DrawGlyphRun); R(SetTransform); R(GetTransform);
R(SetAntialiasMode); R(GetAntialiasMode); R(SetTextAntialiasMode);
R(GetTextAntialiasMode); R(SetTextRenderingParams); R(GetTextRenderingParams);
R(SetTags); R(GetTags); R(PushLayer); R(PopLayer); R(Flush);
R(SaveDrawingState); R(RestoreDrawingState); R(PushAxisAlignedClip);
R(PopAxisAlignedClip); R(Clear); R(BeginDraw); R(EndDraw); R(GetPixelFormat);
R(SetDpi); R(GetDpi); R(GetSize); R(GetPixelSize); R(GetMaximumBitmapSize);
R(IsSupported);
SAME(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, CheckWindowState);
SAME(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, Resize);
SAME(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, GetHwnd);
LAST(D2dRtVtbl, ID2D1HwndRenderTargetVtbl, GetHwnd);
#undef R

// IDWriteFactory
#define W(m) SAME(DwFactoryVtbl, IDWriteFactoryVtbl, m)
SLOT(DwFactoryVtbl, IDWriteFactoryVtbl, base.QueryInterface, QueryInterface);
SLOT(DwFactoryVtbl, IDWriteFactoryVtbl, base.AddRef, AddRef);
SLOT(DwFactoryVtbl, IDWriteFactoryVtbl, base.Release, Release);
W(GetSystemFontCollection); W(CreateCustomFontCollection);
W(RegisterFontCollectionLoader); W(UnregisterFontCollectionLoader);
W(CreateFontFileReference); W(CreateCustomFontFileReference); W(CreateFontFace);
W(CreateRenderingParams); W(CreateMonitorRenderingParams);
W(CreateCustomRenderingParams); W(RegisterFontFileLoader);
W(UnregisterFontFileLoader); W(CreateTextFormat); W(CreateTypography);
W(GetGdiInterop); W(CreateTextLayout); W(CreateGdiCompatibleTextLayout);
W(CreateEllipsisTrimmingSign); W(CreateTextAnalyzer); W(CreateNumberSubstitution);
W(CreateGlyphRunAnalysis);
LAST(DwFactoryVtbl, IDWriteFactoryVtbl, CreateGlyphRunAnalysis);
#undef W

// IDWriteTextFormat + IDWriteTextLayout. The official C vtable is flat; the
// layout's own overloads of the font getters carry an IDWriteTextLayout_
// prefix there, which we spell with an L.
#define L(m) SAME(DwLayoutVtbl, IDWriteTextLayoutVtbl, m)
#define LL(m) SLOT(DwLayoutVtbl, IDWriteTextLayoutVtbl, L##m, IDWriteTextLayout_##m)
SLOT(DwLayoutVtbl, IDWriteTextLayoutVtbl, base.QueryInterface, QueryInterface);
SLOT(DwLayoutVtbl, IDWriteTextLayoutVtbl, base.AddRef, AddRef);
SLOT(DwLayoutVtbl, IDWriteTextLayoutVtbl, base.Release, Release);
L(SetTextAlignment); L(SetParagraphAlignment); L(SetWordWrapping);
L(SetReadingDirection); L(SetFlowDirection); L(SetIncrementalTabStop);
L(SetTrimming); L(SetLineSpacing); L(GetTextAlignment); L(GetParagraphAlignment);
L(GetWordWrapping); L(GetReadingDirection); L(GetFlowDirection);
L(GetIncrementalTabStop); L(GetTrimming); L(GetLineSpacing); L(GetFontCollection);
L(GetFontFamilyNameLength); L(GetFontFamilyName); L(GetFontWeight);
L(GetFontStyle); L(GetFontStretch); L(GetFontSize); L(GetLocaleNameLength);
L(GetLocaleName);
L(SetMaxWidth); L(SetMaxHeight); L(SetFontCollection); L(SetFontFamilyName);
L(SetFontWeight); L(SetFontStyle); L(SetFontStretch); L(SetFontSize);
L(SetUnderline); L(SetStrikethrough); L(SetDrawingEffect); L(SetInlineObject);
L(SetTypography); L(SetLocaleName); L(GetMaxWidth); L(GetMaxHeight);
LL(GetFontCollection); LL(GetFontFamilyNameLength); LL(GetFontFamilyName);
LL(GetFontWeight); LL(GetFontStyle); LL(GetFontStretch); LL(GetFontSize);
L(GetUnderline); L(GetStrikethrough); L(GetDrawingEffect); L(GetInlineObject);
L(GetTypography); LL(GetLocaleNameLength); LL(GetLocaleName);
L(Draw); L(GetLineMetrics); L(GetMetrics); L(GetOverhangMetrics);
L(GetClusterMetrics); L(DetermineMinWidth); L(HitTestPoint);
L(HitTestTextPosition); L(HitTestTextRange);
LAST(DwLayoutVtbl, IDWriteTextLayoutVtbl, HitTestTextRange);
#undef L
#undef LL

// IDWriteInlineObject
#define I(m) SAME(DwInlineVtbl, IDWriteInlineObjectVtbl, m)
SLOT(DwInlineVtbl, IDWriteInlineObjectVtbl, base.QueryInterface, QueryInterface);
SLOT(DwInlineVtbl, IDWriteInlineObjectVtbl, base.AddRef, AddRef);
SLOT(DwInlineVtbl, IDWriteInlineObjectVtbl, base.Release, Release);
I(Draw); I(GetMetrics); I(GetOverhangMetrics); I(GetBreakConditions);
LAST(DwInlineVtbl, IDWriteInlineObjectVtbl, GetBreakConditions);
#undef I

// ID2D1Bitmap: ID2D1Resource, ID2D1Image, then its own slots.
#define B(m) SLOT(D2dBitmapVtbl, ID2D1BitmapVtbl, m, m)
SLOT(D2dBitmapVtbl, ID2D1BitmapVtbl, base.QueryInterface, Base.Base.Base.QueryInterface);
SLOT(D2dBitmapVtbl, ID2D1BitmapVtbl, base.AddRef, Base.Base.Base.AddRef);
SLOT(D2dBitmapVtbl, ID2D1BitmapVtbl, base.Release, Base.Base.Base.Release);
SLOT(D2dBitmapVtbl, ID2D1BitmapVtbl, GetFactory, Base.Base.GetFactory);
B(GetSize); B(GetPixelSize); B(GetPixelFormat); B(GetDpi); B(CopyFromBitmap);
B(CopyFromRenderTarget); B(CopyFromMemory);
LAST(D2dBitmapVtbl, ID2D1BitmapVtbl, CopyFromMemory);
#undef B

// The WIC vtables are flat in the official header.
#define WI(ours, theirs) \
    SLOT(ours, theirs, base.QueryInterface, QueryInterface); \
    SLOT(ours, theirs, base.AddRef, AddRef); \
    SLOT(ours, theirs, base.Release, Release)

// IWICImagingFactory
#define M(m) SAME(WicFactoryVtbl, IWICImagingFactoryVtbl, m)
WI(WicFactoryVtbl, IWICImagingFactoryVtbl);
M(CreateDecoderFromFilename); M(CreateDecoderFromStream); M(CreateDecoderFromFileHandle);
M(CreateComponentInfo); M(CreateDecoder); M(CreateEncoder); M(CreatePalette);
M(CreateFormatConverter); M(CreateBitmapScaler); M(CreateBitmapClipper);
M(CreateBitmapFlipRotator); M(CreateStream); M(CreateColorContext);
M(CreateColorTransformer); M(CreateBitmap); M(CreateBitmapFromSource);
M(CreateBitmapFromSourceRect); M(CreateBitmapFromMemory); M(CreateBitmapFromHBITMAP);
M(CreateBitmapFromHICON); M(CreateComponentEnumerator);
M(CreateFastMetadataEncoderFromDecoder); M(CreateFastMetadataEncoderFromFrameDecode);
M(CreateQueryWriter); M(CreateQueryWriterFromReader);
LAST(WicFactoryVtbl, IWICImagingFactoryVtbl, CreateQueryWriterFromReader);
#undef M

// IWICStream
#define M(m) SAME(WicStreamVtbl, IWICStreamVtbl, m)
WI(WicStreamVtbl, IWICStreamVtbl);
M(Read); M(Write); M(Seek); M(SetSize); M(CopyTo); M(Commit); M(Revert);
M(LockRegion); M(UnlockRegion); M(Stat); M(Clone); M(InitializeFromIStream);
M(InitializeFromFilename); M(InitializeFromMemory); M(InitializeFromIStreamRegion);
LAST(WicStreamVtbl, IWICStreamVtbl, InitializeFromIStreamRegion);
#undef M

// IWICMetadataQueryReader
#define M(m) SAME(WicQueryVtbl, IWICMetadataQueryReaderVtbl, m)
WI(WicQueryVtbl, IWICMetadataQueryReaderVtbl);
M(GetContainerFormat); M(GetLocation); M(GetMetadataByName); M(GetEnumerator);
LAST(WicQueryVtbl, IWICMetadataQueryReaderVtbl, GetEnumerator);
#undef M

// IWICBitmapFrameDecode
#define M(m) SAME(WicFrameVtbl, IWICBitmapFrameDecodeVtbl, m)
WI(WicFrameVtbl, IWICBitmapFrameDecodeVtbl);
M(GetSize); M(GetPixelFormat); M(GetResolution); M(CopyPalette); M(CopyPixels);
M(GetMetadataQueryReader); M(GetColorContexts); M(GetThumbnail);
LAST(WicFrameVtbl, IWICBitmapFrameDecodeVtbl, GetThumbnail);
#undef M

// IWICFormatConverter. Its source slots must also match the frame's, since
// a converter and a frame are both used as an IWICBitmapSource.
#define M(m) SAME(WicConverterVtbl, IWICFormatConverterVtbl, m)
WI(WicConverterVtbl, IWICFormatConverterVtbl);
M(GetSize); M(GetPixelFormat); M(GetResolution); M(CopyPalette); M(CopyPixels);
M(Initialize); M(CanConvert);
LAST(WicConverterVtbl, IWICFormatConverterVtbl, CanConvert);
#undef M
SLOT(WicConverterVtbl, IWICBitmapFrameDecodeVtbl, GetSize, GetSize);
SLOT(WicConverterVtbl, IWICBitmapFrameDecodeVtbl, CopyPixels, CopyPixels);

// IWICBitmapDecoder
#define M(m) SAME(WicDecoderVtbl, IWICBitmapDecoderVtbl, m)
WI(WicDecoderVtbl, IWICBitmapDecoderVtbl);
M(QueryCapability); M(Initialize); M(GetContainerFormat); M(GetDecoderInfo);
M(CopyPalette); M(GetMetadataQueryReader); M(GetPreview); M(GetColorContexts);
M(GetThumbnail); M(GetFrameCount); M(GetFrame);
LAST(WicDecoderVtbl, IWICBitmapDecoderVtbl, GetFrame);
#undef M
#undef WI

// Structs: total size, then every field offset.
SIZE(Guid, GUID);
SLOT(Guid, GUID, a, Data1);
SLOT(Guid, GUID, b, Data2);
SLOT(Guid, GUID, c, Data3);
SLOT(Guid, GUID, d, Data4);

SIZE(WinPoint, POINT);
SLOT(WinPoint, POINT, x, x); SLOT(WinPoint, POINT, y, y);
SIZE(WinRect, RECT);
SAME(WinRect, RECT, left); SAME(WinRect, RECT, top);
SAME(WinRect, RECT, right); SAME(WinRect, RECT, bottom);

SIZE(Msg, MSG);
SLOT(Msg, MSG, hwnd, hwnd); SLOT(Msg, MSG, message, message);
SLOT(Msg, MSG, wparam, wParam); SLOT(Msg, MSG, lparam, lParam);
SAME(Msg, MSG, time); SAME(Msg, MSG, pt);

SIZE(WndClassEx, WNDCLASSEXW);
SLOT(WndClassEx, WNDCLASSEXW, size, cbSize); SLOT(WndClassEx, WNDCLASSEXW, style, style);
SLOT(WndClassEx, WNDCLASSEXW, proc, lpfnWndProc);
SLOT(WndClassEx, WNDCLASSEXW, clsextra, cbClsExtra);
SLOT(WndClassEx, WNDCLASSEXW, wndextra, cbWndExtra);
SLOT(WndClassEx, WNDCLASSEXW, instance, hInstance);
SLOT(WndClassEx, WNDCLASSEXW, icon, hIcon); SLOT(WndClassEx, WNDCLASSEXW, cursor, hCursor);
SLOT(WndClassEx, WNDCLASSEXW, background, hbrBackground);
SLOT(WndClassEx, WNDCLASSEXW, menuname, lpszMenuName);
SLOT(WndClassEx, WNDCLASSEXW, classname, lpszClassName);
SLOT(WndClassEx, WNDCLASSEXW, iconsm, hIconSm);

SIZE(TrackMouse, TRACKMOUSEEVENT);
SLOT(TrackMouse, TRACKMOUSEEVENT, size, cbSize);
SLOT(TrackMouse, TRACKMOUSEEVENT, flags, dwFlags);
SLOT(TrackMouse, TRACKMOUSEEVENT, hwnd, hwndTrack);
SLOT(TrackMouse, TRACKMOUSEEVENT, hover, dwHoverTime);

SIZE(PaintStruct, PAINTSTRUCT);
SLOT(PaintStruct, PAINTSTRUCT, hdc, hdc); SLOT(PaintStruct, PAINTSTRUCT, erase, fErase);
SLOT(PaintStruct, PAINTSTRUCT, paint, rcPaint);
SLOT(PaintStruct, PAINTSTRUCT, restore, fRestore);
SLOT(PaintStruct, PAINTSTRUCT, incupdate, fIncUpdate);
SLOT(PaintStruct, PAINTSTRUCT, reserved, rgbReserved);

SIZE(MinMaxInfo, MINMAXINFO);
SLOT(MinMaxInfo, MINMAXINFO, reserved, ptReserved);
SLOT(MinMaxInfo, MINMAXINFO, maxsize, ptMaxSize);
SLOT(MinMaxInfo, MINMAXINFO, maxpos, ptMaxPosition);
SLOT(MinMaxInfo, MINMAXINFO, mintrack, ptMinTrackSize);
SLOT(MinMaxInfo, MINMAXINFO, maxtrack, ptMaxTrackSize);

SIZE(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, attributes, dwFileAttributes);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, created, ftCreationTime);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, accessed, ftLastAccessTime);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, written, ftLastWriteTime);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, sizehigh, nFileSizeHigh);
SLOT(FileAttributeData, WIN32_FILE_ATTRIBUTE_DATA, sizelow, nFileSizeLow);

SIZE(OpenFileName, OPENFILENAMEW);
SLOT(OpenFileName, OPENFILENAMEW, size, lStructSize);
SLOT(OpenFileName, OPENFILENAMEW, owner, hwndOwner);
SLOT(OpenFileName, OPENFILENAMEW, instance, hInstance);
SLOT(OpenFileName, OPENFILENAMEW, filter, lpstrFilter);
SLOT(OpenFileName, OPENFILENAMEW, customfilter, lpstrCustomFilter);
SLOT(OpenFileName, OPENFILENAMEW, maxcustfilter, nMaxCustFilter);
SLOT(OpenFileName, OPENFILENAMEW, filterindex, nFilterIndex);
SLOT(OpenFileName, OPENFILENAMEW, file, lpstrFile);
SLOT(OpenFileName, OPENFILENAMEW, maxfile, nMaxFile);
SLOT(OpenFileName, OPENFILENAMEW, filetitle, lpstrFileTitle);
SLOT(OpenFileName, OPENFILENAMEW, maxfiletitle, nMaxFileTitle);
SLOT(OpenFileName, OPENFILENAMEW, initialdir, lpstrInitialDir);
SLOT(OpenFileName, OPENFILENAMEW, title, lpstrTitle);
SLOT(OpenFileName, OPENFILENAMEW, flags, Flags);
SLOT(OpenFileName, OPENFILENAMEW, fileoffset, nFileOffset);
SLOT(OpenFileName, OPENFILENAMEW, fileextension, nFileExtension);
SLOT(OpenFileName, OPENFILENAMEW, defext, lpstrDefExt);
SLOT(OpenFileName, OPENFILENAMEW, custdata, lCustData);
SLOT(OpenFileName, OPENFILENAMEW, hook, lpfnHook);
SLOT(OpenFileName, OPENFILENAMEW, templatename, lpTemplateName);
SLOT(OpenFileName, OPENFILENAMEW, reserved, pvReserved);
SLOT(OpenFileName, OPENFILENAMEW, reserved2, dwReserved);
SLOT(OpenFileName, OPENFILENAMEW, flagsex, FlagsEx);

SIZE(D2dColor, D2D1_COLOR_F);
SAME(D2dColor, D2D1_COLOR_F, r); SAME(D2dColor, D2D1_COLOR_F, g);
SAME(D2dColor, D2D1_COLOR_F, b); SAME(D2dColor, D2D1_COLOR_F, a);
SIZE(D2dPoint, D2D1_POINT_2F);
SAME(D2dPoint, D2D1_POINT_2F, x); SAME(D2dPoint, D2D1_POINT_2F, y);
SIZE(D2dRect, D2D1_RECT_F);
SAME(D2dRect, D2D1_RECT_F, left); SAME(D2dRect, D2D1_RECT_F, top);
SAME(D2dRect, D2D1_RECT_F, right); SAME(D2dRect, D2D1_RECT_F, bottom);
SIZE(D2dSizeU, D2D1_SIZE_U);
SAME(D2dSizeU, D2D1_SIZE_U, width); SAME(D2dSizeU, D2D1_SIZE_U, height);
SIZE(D2dRoundedRect, D2D1_ROUNDED_RECT);
SAME(D2dRoundedRect, D2D1_ROUNDED_RECT, rect);
SLOT(D2dRoundedRect, D2D1_ROUNDED_RECT, rx, radiusX);
SLOT(D2dRoundedRect, D2D1_ROUNDED_RECT, ry, radiusY);
SIZE(D2dMatrix, D2D1_MATRIX_3X2_F);
SLOT(D2dMatrix, D2D1_MATRIX_3X2_F, m[0], m[0]);
SIZE(D2dPixelFormat, D2D1_PIXEL_FORMAT);
SLOT(D2dPixelFormat, D2D1_PIXEL_FORMAT, format, format);
SLOT(D2dPixelFormat, D2D1_PIXEL_FORMAT, alphamode, alphaMode);
SIZE(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES);
SAME(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, type);
SLOT(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, pixelformat, pixelFormat);
SLOT(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, dpix, dpiX);
SLOT(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, dpiy, dpiY);
SAME(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, usage);
SLOT(D2dRenderTargetProps, D2D1_RENDER_TARGET_PROPERTIES, minlevel, minLevel);
SIZE(D2dHwndRenderTargetProps, D2D1_HWND_RENDER_TARGET_PROPERTIES);
SAME(D2dHwndRenderTargetProps, D2D1_HWND_RENDER_TARGET_PROPERTIES, hwnd);
SLOT(D2dHwndRenderTargetProps, D2D1_HWND_RENDER_TARGET_PROPERTIES, pixelsize, pixelSize);
SLOT(D2dHwndRenderTargetProps, D2D1_HWND_RENDER_TARGET_PROPERTIES, presentoptions, presentOptions);
SIZE(D2dBrushProps, D2D1_BRUSH_PROPERTIES);
SAME(D2dBrushProps, D2D1_BRUSH_PROPERTIES, opacity);
SLOT(D2dBrushProps, D2D1_BRUSH_PROPERTIES, transform, transform);

SIZE(D2dBitmapProps, D2D1_BITMAP_PROPERTIES);
SLOT(D2dBitmapProps, D2D1_BITMAP_PROPERTIES, pixelformat, pixelFormat);
SLOT(D2dBitmapProps, D2D1_BITMAP_PROPERTIES, dpix, dpiX);
SLOT(D2dBitmapProps, D2D1_BITMAP_PROPERTIES, dpiy, dpiY);

SIZE(WicRect, WICRect);
SLOT(WicRect, WICRect, x, X); SLOT(WicRect, WICRect, y, Y);
SLOT(WicRect, WICRect, w, Width); SLOT(WicRect, WICRect, h, Height);
SIZE(PropVar, PROPVARIANT);
SAME(PropVar, PROPVARIANT, vt);
SLOT(PropVar, PROPVARIANT, reserved1, wReserved1);
SLOT(PropVar, PROPVARIANT, reserved2, wReserved2);
SLOT(PropVar, PROPVARIANT, reserved3, wReserved3);
SLOT(PropVar, PROPVARIANT, b, bVal);
SLOT(PropVar, PROPVARIANT, i, iVal);
SLOT(PropVar, PROPVARIANT, i, boolVal);
SLOT(PropVar, PROPVARIANT, w, uiVal);
SLOT(PropVar, PROPVARIANT, u, ulVal);
SLOT(PropVar, PROPVARIANT, l, lVal);

SIZE(DwTextRange, DWRITE_TEXT_RANGE);
SLOT(DwTextRange, DWRITE_TEXT_RANGE, start, startPosition);
SAME(DwTextRange, DWRITE_TEXT_RANGE, length);
SIZE(DwTextMetrics, DWRITE_TEXT_METRICS);
SAME(DwTextMetrics, DWRITE_TEXT_METRICS, left); SAME(DwTextMetrics, DWRITE_TEXT_METRICS, top);
SAME(DwTextMetrics, DWRITE_TEXT_METRICS, width);
SLOT(DwTextMetrics, DWRITE_TEXT_METRICS, widthtrailing, widthIncludingTrailingWhitespace);
SAME(DwTextMetrics, DWRITE_TEXT_METRICS, height);
SLOT(DwTextMetrics, DWRITE_TEXT_METRICS, layoutwidth, layoutWidth);
SLOT(DwTextMetrics, DWRITE_TEXT_METRICS, layoutheight, layoutHeight);
SLOT(DwTextMetrics, DWRITE_TEXT_METRICS, maxbidideptn, maxBidiReorderingDepth);
SLOT(DwTextMetrics, DWRITE_TEXT_METRICS, linecount, lineCount);
SIZE(DwLineMetrics, DWRITE_LINE_METRICS);
SAME(DwLineMetrics, DWRITE_LINE_METRICS, length);
SLOT(DwLineMetrics, DWRITE_LINE_METRICS, trailingwhitespace, trailingWhitespaceLength);
SLOT(DwLineMetrics, DWRITE_LINE_METRICS, newline, newlineLength);
SAME(DwLineMetrics, DWRITE_LINE_METRICS, height);
SAME(DwLineMetrics, DWRITE_LINE_METRICS, baseline);
SLOT(DwLineMetrics, DWRITE_LINE_METRICS, trimmed, isTrimmed);
SIZE(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS);
SLOT(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, position, textPosition);
SAME(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, length);
SAME(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, left); SAME(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, top);
SAME(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, width); SAME(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, height);
SLOT(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, bidilevel, bidiLevel);
SLOT(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, istext, isText);
SLOT(DwHitTestMetrics, DWRITE_HIT_TEST_METRICS, istrimmed, isTrimmed);
SIZE(DwInlineMetrics, DWRITE_INLINE_OBJECT_METRICS);
SAME(DwInlineMetrics, DWRITE_INLINE_OBJECT_METRICS, width);
SAME(DwInlineMetrics, DWRITE_INLINE_OBJECT_METRICS, height);
SAME(DwInlineMetrics, DWRITE_INLINE_OBJECT_METRICS, baseline);
SLOT(DwInlineMetrics, DWRITE_INLINE_OBJECT_METRICS, sideways, supportsSideways);
SIZE(DwOverhangMetrics, DWRITE_OVERHANG_METRICS);
SAME(DwOverhangMetrics, DWRITE_OVERHANG_METRICS, left); SAME(DwOverhangMetrics, DWRITE_OVERHANG_METRICS, top);
SAME(DwOverhangMetrics, DWRITE_OVERHANG_METRICS, right); SAME(DwOverhangMetrics, DWRITE_OVERHANG_METRICS, bottom);

// Scalar types we use in place of the official ones.
_Static_assert(sizeof(b32) == sizeof(BOOL), "BOOL");
_Static_assert(sizeof(uptr) == sizeof(HANDLE), "HANDLE");
_Static_assert(sizeof(iptr) == sizeof(LPARAM), "LPARAM");
_Static_assert(sizeof(c16) == sizeof(WCHAR), "WCHAR");
_Static_assert(sizeof(f32) == sizeof(FLOAT), "FLOAT");
_Static_assert(sizeof(DwTextRange) == 8, "passed by value in one register");
_Static_assert(sizeof(D2dPoint) == 8, "passed by value in one register");

static int failures;

static void guid(const char *name, const Guid *ours, const GUID *theirs) {
    if (memcmp(ours, theirs, sizeof(GUID))) {
        printf("comcheck: GUID %s differs\n", name);
        failures++;
    }
}

int main(void) {
    guid("IID_IUnknown",           &iid_unknown,      &IID_IUnknown);
    guid("IID_ID2D1Factory",       &iid_d2d_factory,  &IID_ID2D1Factory);
    guid("IID_IDWriteFactory",     &iid_dw_factory,   &IID_IDWriteFactory);
    guid("IID_IDWriteInlineObject", &iid_dw_inline,   &IID_IDWriteInlineObject);
    guid("CLSID_WICImagingFactory", &clsid_wic_factory, &CLSID_WICImagingFactory);
    guid("IID_IWICImagingFactory",  &iid_wic_factory, &IID_IWICImagingFactory);
    guid("GUID_WICPixelFormat32bppPBGRA", &wic_pbgra, &GUID_WICPixelFormat32bppPBGRA);
    guid("GUID_ContainerFormatGif",  &wic_gif,        &GUID_ContainerFormatGif);
    guid("GUID_ContainerFormatJpeg", &wic_jpeg,       &GUID_ContainerFormatJpeg);
    guid("GUID_ContainerFormatIco",  &wic_ico,        &GUID_ContainerFormatIco);
    if (!failures) {
        puts("comcheck: layout and GUIDs ok");
    }
    return failures != 0;
}

#endif
