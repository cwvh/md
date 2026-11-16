// Image decoding with the Windows Imaging Component, and GIF compositing.
// No graphics here: the result is premultiplied BGRA pixels, which gfx.c
// turns into bitmaps, so this file builds and tests without a window or
// Direct2D (imgtest.c). Included from main.c before gfx.c.
//
// A still image is decoded once, whole, into one canvas, and the decoder is
// released. An animated GIF keeps its decoder and decodes lazily: the canvas
// holds the frame last asked for, composited the way the file's disposal
// methods say, and asking for the next frame costs one frame of decoding.
// Going backward starts over from frame 0.

enum {
    IMG_MAX_SIDE    = 16384,
    IMG_MAX_BYTES   = 128<<20,      // one canvas
    IMG_MAX_FRAMES  = 10000,
    IMG_BAND        = 16384,        // pixels per CopyPixels call, at most
};

// Disposal methods of the GIF graphic control extension
enum {
    GIF_KEEP       = 1,
    GIF_BACKGROUND = 2,             // clear the frame's rectangle
    GIF_PREVIOUS   = 3,             // put back what was there before it
};

typedef struct {
    i32 left, top, w, h;            // the frame's rectangle, unclipped
    i32 delay;                      // ms
    i32 disposal;
    b32 known;                      // the rest was read
} GifFrame;

struct Image {
    i32  w, h;
    u32 *canvas;                    // w*h premultiplied BGRA, the current frame
    u32 *saved;                     // canvas before a frame with disposal 3
    u32  serial;                    // changes whenever the canvas does
    i32  nframes;
    i32  cur;                       // frame the canvas holds, -1 if none

    // Kept only while the image can still animate
    u8        *data;                // the encoded bytes, which the stream reads
    WicStream *stream;
    WicDecoder *dec;
    GifFrame  *fr;                  // nframes entries

    // For gfx.c, which creates and frees it
    void *bitmap;
    u32   bgen;
    u32   bserial;
};

// Live allocations, for the leak checks of the tests: heap blocks, and COM
// objects this file created.
static struct {
    i32 blocks;
    i32 objects;
} imgstat;

static WicFactory *wic;             // one per process, never released

static void *img_alloc(iz count, iz size) {
    iz   n = count*size;
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n>0 ? (uz)n : 1);
    if (p) {
        imgstat.blocks++;
    }
    return p;
}

static void img_free(void *p) {
    if (p) {
        HeapFree(GetProcessHeap(), 0, p);
        imgstat.blocks--;
    }
}

// Count a COM object that was just created (or a null from a failed call,
// which is not one).
static void *com_got(void *p) {
    if (p) {
        imgstat.objects++;
    }
    return p;
}

static void com_put(void *p) {
    ComObj *o = p;
    if (o) {
        COM(o, Release);
        imgstat.objects--;
    }
}

// All the WIC calls are made on a thread of their own, which the caller waits
// for. The UI thread must not make them: it is a single-threaded apartment,
// where a call into a codec that lives in another apartment (the system's
// WebP codec does) dispatches the window's messages while it waits. A paint
// would then lay the document out again from inside the decoding that the
// layout is waiting for. The worker is in the multi-threaded apartment, where
// nothing is dispatched and no call crosses an apartment. The caller's own
// wait is a plain kernel wait, which dispatches nothing either.

W32(void *) CreateThread(void *, uz, u32 (*)(void *), void *, u32, u32 *);

static struct {
    uptr go;                        // the worker has a job
    uptr done;                      // the job is done
    void (*fn)(void *);
    void *arg;
    b32   started;
} worker;

static b32 wic_ready(void) {
    if (!wic) {
        void *f = 0;
        if (!FAILED(CoCreateInstance(&clsid_wic_factory, 0, CLSCTX_INPROC_SERVER,
                                     &iid_wic_factory, &f))) {
            wic = f;
        }
    }
    return wic != 0;
}

static _Noreturn u32 worker_main(void *unused) {
    (void)unused;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(worker.go, INFINITE);
        worker.fn(worker.arg);
        SetEvent(worker.done);
    }
}

// Run fn(arg) on the worker and wait for it. Returns 0 if there is no worker.
static b32 on_worker(void (*fn)(void *), void *arg) {
    if (!worker.started) {
        worker.go   = worker.go   ? worker.go   : CreateEventW(0, 0, 0, 0);
        worker.done = worker.done ? worker.done : CreateEventW(0, 0, 0, 0);
        if (!worker.go || !worker.done || !CreateThread(0, 0, worker_main, 0, 0, 0)) {
            return 0;
        }
        worker.started = 1;
    }
    worker.fn  = fn;
    worker.arg = arg;
    SetEvent(worker.go);
    WaitForSingleObject(worker.done, INFINITE);
    return 1;
}

// An integer metadata value, if the file has it.
static b32 query_int(WicQuery *q, c16 const *name, i32 *out) {
    PropVar pv = {0};
    if (!q || FAILED(COM(q, GetMetadataByName, name, &pv))) {
        return 0;
    }
    b32 ok = 1;
    switch (pv.vt) {
    case VT_UI1:  *out = pv.b; break;
    case VT_I2:
    case VT_BOOL: *out = pv.i; break;
    case VT_UI2:  *out = pv.w; break;
    case VT_I4:   *out = pv.l; break;
    case VT_UI4:  *out = (i32)pv.u; break;
    default:      ok = 0;
    }
    PropVariantClear(&pv);
    return ok;
}

static void image_unload(Image *im) {
    com_put(im->dec);
    com_put(im->stream);
    img_free(im->data);
    img_free(im->fr);
    im->dec    = 0;
    im->stream = 0;
    im->data   = 0;
    im->fr     = 0;
}

// Everything but the bitmap, which gfx.c takes care of first. On the worker
// thread, or for an image that has no COM objects left.
static void image_drop(Image *im) {
    if (im) {
        image_unload(im);
        img_free(im->canvas);
        img_free(im->saved);
        img_free(im);
    }
}

// The frame converted to premultiplied BGRA, as a source for CopyPixels.
static WicConverter *convert(WicFrame *f) {
    void *c = 0;
    if (FAILED(COM(wic, CreateFormatConverter, &c)) || !com_got(c)) {
        return 0;
    }
    WicConverter *cv = c;
    if (FAILED(COM(cv, Initialize, f, &wic_pbgra, WICBitmapDitherTypeNone, 0, 0.0,
                   WICBitmapPaletteTypeCustom))) {
        com_put(cv);
        return 0;
    }
    return cv;
}

// Source over destination, both premultiplied.
static u32 blend(u32 s, u32 d) {
    u32 a = s >> 24;
    if (a == 255) {
        return s;
    }
    u32 r = 0;
    for (i32 sh = 0; sh < 32; sh += 8) {
        u32 c = ((s>>sh) & 255) + (((d>>sh) & 255)*(255 - a) + 127)/255;
        r |= (c > 255 ? 255 : c) << sh;
    }
    return r;
}


// Animated GIF

// The frame's rectangle clipped to the canvas, which is empty for a frame
// that lies outside it.
static b32 gif_clip(Image *im, GifFrame const *g, i32 *x0, i32 *y0, i32 *x1, i32 *y1) {
    *x0 = g->left;
    *y0 = g->top;
    *x1 = min(g->left + g->w, im->w);
    *y1 = min(g->top + g->h, im->h);
    return *x0 < *x1 && *y0 < *y1;
}

// Offset, size, delay and disposal of frame i, read from its metadata. f
// may be 0 for a frame that could not be opened, which then is empty.
static void gif_info(Image *im, i32 i, WicFrame *f) {
    GifFrame *g = im->fr + i;
    *g = (GifFrame){0};
    g->known = 1;
    g->delay = 100;
    if (!f) {
        return;
    }
    u32 w = 0;
    u32 h = 0;
    if (!FAILED(COM(f, GetSize, &w, &h))) {
        g->w = (i32)min(w, (u32)65535);
        g->h = (i32)min(h, (u32)65535);
    }
    void *r = 0;
    if (FAILED(COM(f, GetMetadataQueryReader, &r)) || !com_got(r)) {
        return;
    }
    WicQuery *q = r;
    i32 v = 0;
    if (query_int(q, L"/imgdesc/Left", &v)) {
        g->left = v;
    }
    if (query_int(q, L"/imgdesc/Top", &v)) {
        g->top = v;
    }
    if (query_int(q, L"/grctlext/Delay", &v)) {
        // Units of 10 ms. Browsers show 0 and 1 as 100 ms, as so many files
        // were made for slow software that never meant them.
        g->delay = v <= 1 ? 100 : v*10;
    }
    if (query_int(q, L"/grctlext/Disposal", &v)) {
        g->disposal = v;
    }
    com_put(q);
}

static void gif_dispose(Image *im, GifFrame const *g) {
    if (g->disposal == GIF_BACKGROUND) {
        i32 x0, y0, x1, y1;
        if (gif_clip(im, g, &x0, &y0, &x1, &y1)) {
            for (i32 y = y0; y < y1; y++) {
                memset(im->canvas + (iz)y*im->w + x0, 0, (uz)(x1 - x0)*4);
            }
        }
    } else if (g->disposal==GIF_PREVIOUS && im->saved) {
        memcpy(im->canvas, im->saved, (uz)im->w*(uz)im->h*4);
    }
}

// Draw frame f over the canvas, clipped to it. Returns 0 if the pixels could
// not be read.
static b32 gif_paint(Image *im, GifFrame const *g, WicFrame *f) {
    i32 x0, y0, x1, y1;
    if (!gif_clip(im, g, &x0, &y0, &x1, &y1)) {
        return 1;
    }
    WicConverter *cv = convert(f);
    if (!cv) {
        return 0;
    }
    i32  cw   = x1 - x0;
    i32  ch   = y1 - y0;
    i32  rows = max((i32)1, min(ch, IMG_BAND/cw));
    u32 *band = img_alloc((iz)rows*cw, 4);
    b32  ok   = band != 0;
    for (i32 y = 0; ok && y < ch; y += rows) {
        i32     n = min(rows, ch - y);
        WicRect r = {0, y, cw, n};
        ok = !FAILED(COM(cv, CopyPixels, &r, (u32)cw*4, (u32)(cw*n)*4, (u8 *)band));
        for (i32 k = 0; ok && k < n; k++) {
            u32 *dst = im->canvas + (iz)(y0 + y + k)*im->w + x0;
            u32 *src = band + (iz)k*cw;
            for (i32 x = 0; x < cw; x++) {
                dst[x] = src[x]>>24 ? blend(src[x], dst[x]) : dst[x];
            }
        }
    }
    img_free(band);
    com_put(cv);
    return ok;
}

// Put frame i on the canvas, which holds frame i-1 (or nothing, for i == 0).
// A frame that cannot be read is left out and the animation goes on.
static b32 gif_next(Image *im, i32 i) {
    void *p = 0;
    WicFrame *f = 0;
    if (!FAILED(COM(im->dec, GetFrame, (u32)i, &p))) {
        f = com_got(p);
    }
    if (!im->fr[i].known) {
        gif_info(im, i, f);
    }
    GifFrame const *g = im->fr + i;
    if (i > 0) {
        gif_dispose(im, im->fr + i - 1);
    }
    if (g->disposal == GIF_PREVIOUS) {
        if (!im->saved) {
            im->saved = img_alloc((iz)im->w*im->h, 4);
        }
        if (im->saved) {
            memcpy(im->saved, im->canvas, (uz)im->w*(uz)im->h*4);
        }
    }
    b32 ok = f && gif_paint(im, g, f);
    com_put(f);
    im->cur = i;
    im->serial++;
    return ok;
}

// Make the canvas hold frame i. In order it is one step; backward, or from
// nothing, it starts again at frame 0.
static void gif_seek(Image *im, i32 i) {
    if (i == im->cur) {
        return;
    }
    if (i < im->cur) {
        memset(im->canvas, 0, (uz)im->w*(uz)im->h*4);
        im->cur = -1;
    }
    while (im->cur < i) {
        gif_next(im, im->cur + 1);
    }
}

// Size the canvas from the logical screen, or else from the first frame,
// and draw frame 0.
static b32 gif_open(Image *im) {
    im->fr = img_alloc(im->nframes, (iz)sizeof(GifFrame));
    if (!im->fr) {
        return 0;
    }
    i32 w = 0;
    i32 h = 0;
    void *r = 0;
    if (!FAILED(COM(im->dec, GetMetadataQueryReader, &r)) && com_got(r)) {
        WicQuery *q = r;
        query_int(q, L"/logscrdesc/Width", &w);
        query_int(q, L"/logscrdesc/Height", &h);
        com_put(q);
    }
    if (w <= 0 || h <= 0) {
        void *p = 0;
        if (FAILED(COM(im->dec, GetFrame, 0, &p)) || !com_got(p)) {
            return 0;
        }
        WicFrame *f = p;
        gif_info(im, 0, f);
        com_put(f);
        w = im->fr[0].left + im->fr[0].w;
        h = im->fr[0].top  + im->fr[0].h;
    }
    if (w<1 || h<1 || w>IMG_MAX_SIDE || h>IMG_MAX_SIDE || (i64)w*h*4 > IMG_MAX_BYTES) {
        return 0;
    }
    im->w      = w;
    im->h      = h;
    im->canvas = img_alloc((iz)w*h, 4);
    im->cur    = -1;
    if (!im->canvas) {
        return 0;
    }
    // The first frame must be there; later ones may be damaged.
    return gif_next(im, 0);
}


// Still images

// Where pixel (x, y) of the file lands in the canvas once the EXIF
// orientation (1 to 8) is applied: base + x*sx + y*sy, for a w by h image.
// 1 is as stored, 2 mirrored, 3 turned half around, 4 flipped, 5 transposed,
// 6 turned a quarter clockwise, 7 transposed the other way, 8 a quarter
// counterclockwise. 5 to 8 swap the sides.
static void orient_map(i32 o, i32 w, i32 h, iz *base, iz *sx, iz *sy) {
    switch (o) {
    case 2:  *base = w - 1;         *sx = -1;  *sy = w;  break;
    case 3:  *base = (iz)w*h - 1;   *sx = -1;  *sy = -w; break;
    case 4:  *base = (iz)(h-1)*w;   *sx = 1;   *sy = -w; break;
    case 5:  *base = 0;             *sx = h;   *sy = 1;  break;
    case 6:  *base = h - 1;         *sx = h;   *sy = -1; break;
    case 7:  *base = (iz)w*h - 1;   *sx = -h;  *sy = -1; break;
    case 8:  *base = (iz)(w-1)*h;   *sx = -h;  *sy = 1;  break;
    default: *base = 0;             *sx = 1;   *sy = w;  break;
    }
}

// The frame to show of a still image: the largest, for an icon, and the
// first of anything else.
static i32 still_pick(Image *im, b32 icon) {
    i32 best = 0;
    if (icon) {
        i64 area = 0;
        for (i32 i = 0; i < im->nframes; i++) {
            void *p = 0;
            u32   w = 0;
            u32   h = 0;
            if (FAILED(COM(im->dec, GetFrame, (u32)i, &p)) || !com_got(p)) {
                continue;
            }
            WicFrame *f = p;
            if (!FAILED(COM(f, GetSize, &w, &h)) && (i64)w*h > area) {
                area = (i64)w*h;
                best = i;
            }
            com_put(f);
        }
    }
    return best;
}

// Read the whole frame into the canvas, turned as the orientation says.
static b32 still_copy(Image *im, WicConverter *cv, i32 w, i32 h, i32 orient) {
    iz base, sx, sy;
    orient_map(orient, w, h, &base, &sx, &sy);
    i32 rows = max((i32)1, min(h, IMG_BAND/w));
    im->w      = orient >= 5 ? h : w;
    im->h      = orient >= 5 ? w : h;
    im->canvas = img_alloc((iz)w*h, 4);
    u32 *band  = img_alloc((iz)rows*w, 4);
    b32  ok    = im->canvas && band;
    for (i32 y = 0; ok && y < h; y += rows) {
        i32     n = min(rows, h - y);
        WicRect r = {0, y, w, n};
        ok = !FAILED(COM(cv, CopyPixels, &r, (u32)w*4, (u32)(w*n)*4, (u8 *)band));
        for (i32 k = 0; ok && k < n; k++) {
            u32 *src = band + (iz)k*w;
            if (sx == 1) {
                memcpy(im->canvas + base + (iz)(y+k)*sy, src, (uz)w*4);
            } else {
                for (i32 x = 0; x < w; x++) {
                    im->canvas[base + x*sx + (iz)(y+k)*sy] = src[x];
                }
            }
        }
    }
    img_free(band);
    return ok;
}

static b32 still_open(Image *im, b32 icon, b32 jpeg) {
    void *p = 0;
    i32 pick = still_pick(im, icon);
    im->nframes = 1;                // only one is shown
    if (FAILED(COM(im->dec, GetFrame, (u32)pick, &p)) || !com_got(p)) {
        return 0;
    }
    WicFrame *f = p;
    u32 fw = 0;
    u32 fh = 0;
    b32 ok = !FAILED(COM(f, GetSize, &fw, &fh)) &&
             fw>=1 && fh>=1 && fw<=IMG_MAX_SIDE && fh<=IMG_MAX_SIDE &&
             (i64)fw*fh*4 <= IMG_MAX_BYTES;
    i32 orient = 1;
    if (ok && jpeg) {
        void *r = 0;
        if (!FAILED(COM(f, GetMetadataQueryReader, &r)) && com_got(r)) {
            WicQuery *q = r;
            query_int(q, L"/app1/ifd/{ushort=274}", &orient);
            com_put(q);
        }
        if (orient < 1 || orient > 8) {
            orient = 1;
        }
    }
    WicConverter *cv = ok ? convert(f) : 0;
    ok = cv && still_copy(im, cv, (i32)fw, (i32)fh, orient);
    com_put(cv);
    com_put(f);
    im->cur = 0;
    im->serial++;
    return ok;
}


// The platform interface (the rest, drawing and freeing, is in gfx.c)

static Image *decode(Str bytes) {
    if (bytes.len<=0 || bytes.len>0x7fffffff || !wic_ready()) {
        return 0;
    }
    Image *im = img_alloc(1, (iz)sizeof(Image));
    if (!im) {
        return 0;
    }
    // The stream reads the caller's bytes in place, so keep our own.
    im->data = img_alloc(bytes.len, 1);
    if (!im->data) {
        image_drop(im);
        return 0;
    }
    memcpy(im->data, bytes.data, (uz)bytes.len);
    void *p = 0;
    if (FAILED(COM(wic, CreateStream, &p)) || !(im->stream = com_got(p))) {
        image_drop(im);
        return 0;
    }
    p = 0;
    if (FAILED(COM(im->stream, InitializeFromMemory, im->data, (u32)bytes.len)) ||
            FAILED(COM(wic, CreateDecoderFromStream, im->stream, 0,
                       WICDecodeMetadataCacheOnDemand, &p)) ||
            !(im->dec = com_got(p))) {
        image_drop(im);
        return 0;
    }
    u32  n = 0;
    Guid container = {0};
    if (FAILED(COM(im->dec, GetFrameCount, &n)) || n<1 || n>IMG_MAX_FRAMES ||
            FAILED(COM(im->dec, GetContainerFormat, &container))) {
        image_drop(im);
        return 0;
    }
    im->nframes = (i32)n;
    b32 ok;
    if (guideq(&container, &wic_gif)) {
        ok = gif_open(im);
    } else {
        ok = still_open(im, guideq(&container, &wic_ico), guideq(&container, &wic_jpeg));
    }
    if (!ok) {
        image_drop(im);
        return 0;
    }
    if (im->nframes < 2) {
        image_unload(im);       // nothing left to decode
    }
    return im;
}

static Size os_image_size(Image *im) {
    return (Size){(f32)im->w, (f32)im->h};
}

static i32 os_image_frames(Image *im) {
    return im->nframes;
}

typedef struct {
    Str    bytes;
    Image *im;
    i32    frame;
    i32    delay;
} Job;

static void job_decode(void *p) {
    Job *j = p;
    j->im  = decode(j->bytes);
}

static void job_delay(void *p) {
    Job *j = p;
    Image *im = j->im;
    void *fp = 0;
    WicFrame *f = 0;
    if (!FAILED(COM(im->dec, GetFrame, (u32)j->frame, &fp))) {
        f = com_got(fp);
    }
    gif_info(im, j->frame, f);
    com_put(f);
}

static void job_seek(void *p) {
    Job *j = p;
    gif_seek(j->im, j->frame);
}

static void job_free(void *p) {
    Job *j = p;
    image_drop(j->im);
}

static Image *os_image_decode(Str bytes) {
    Job j = {bytes, 0, 0, 0};
    if (bytes.len<=0 || bytes.len>0x7fffffff || !on_worker(job_decode, &j)) {
        return 0;
    }
    return j.im;
}

static i32 os_image_delay(Image *im, i32 frame) {
    if (!im->fr || frame<0 || frame>=im->nframes) {
        return 0;
    }
    if (!im->fr[frame].known) {
        Job j = {{0}, im, frame, 0};
        on_worker(job_delay, &j);
    }
    return im->fr[frame].delay;
}

// The composited pixels of a frame: w*h premultiplied BGRA, valid until the
// next call. A still image has the one frame.
static u32 const *image_pixels(Image *im, i32 frame) {
    frame = max((i32)0, min(frame, im->nframes - 1));
    if (im->fr && frame != im->cur) {
        Job j = {{0}, im, frame, 0};
        on_worker(job_seek, &j);
    }
    return im->canvas;
}

// Everything but the bitmap, which gfx.c takes care of first.
static void image_free(Image *im) {
    if (im && im->dec) {
        Job j = {{0}, im, 0, 0};
        on_worker(job_free, &j);
    } else if (im) {
        image_drop(im);
    }
}
