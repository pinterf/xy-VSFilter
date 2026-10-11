// Standalone checks for the multi-track composition core:
//  * CompositeSubRenderFrame: the frame contract (GetBitmapCount; delegation
//    with per-track offset; anchor output/clip/colorspace) and the merge path
//    (single alpha-composited canvas) with per-track scale/opacity.
//  * MultiTrackSubRenderProvider: 1 track is a zero-copy pass-through; >1 track
//    ALWAYS produces one merged bitmap regardless of the consumer's
//    combineBitmaps preference; per-track scale/opacity/z-order/placement are
//    applied by the composition itself.
//
// Not a gtest suite: it is a self-contained console program so it can be built
// with cl.exe without the unit_test/csri harness.

#include "stdafx.h"
#include "CompositeSubRenderFrame.h"
#include "MultiTrackSubRenderProvider.h"
#include "IMultiSubtitleTrackLoader.h"
#include "IDirectVobSubXy.h"
#include <stdio.h>
#include <map>
#include <vector>

// MultiTrackSubRenderProvider / CompositeSubRenderFrame log through xy_logger
// (which lives in the filter DLL). This standalone test only needs the symbols
// to link, so provide a minimal definition here.
namespace xy_logger
{
#if ENABLE_XY_LOG
log4cplus::Logger g_logger = log4cplus::Logger::getInstance(L"composite_frame_test");
struct StubLoggerInit
{
    StubLoggerInit() { g_logger.setLogLevel(log4cplus::OFF_LOG_LEVEL); }
} g_stub_logger_init;
#endif
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            printf("FAIL: %s  (line %d)\n", msg, __LINE__);                  \
        } else {                                                             \
            printf("ok  : %s\n", msg);                                       \
        }                                                                    \
    } while (0)

// Minimal IXySubRenderFrame with a single RGB32 bitmap.
class StubFrame : public CUnknown, public IXySubRenderFrame
{
public:
    StubFrame(int w, int h, POINT pos, DWORD fill, int color_space)
        : CUnknown(NAME("StubFrame"), NULL)
        , m_size{w, h}, m_pos(pos), m_color_space(color_space), m_id(0)
    {
        m_pixels.assign((size_t)w * h, fill);
        m_output_rect = CRect(0, 0, w, h);
        m_clip_rect = CRect(0, 0, w, h);
    }

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        return QI(IXySubRenderFrame) __super::NonDelegatingQueryInterface(riid, ppv);
    }

    STDMETHODIMP GetOutputRect(RECT* r) { if (!r) return E_POINTER; *r = m_output_rect; return S_OK; }
    STDMETHODIMP GetClipRect(RECT* r) { if (!r) return E_POINTER; *r = m_clip_rect; return S_OK; }
    STDMETHODIMP GetXyColorSpace(int* cs) { if (!cs) return E_POINTER; *cs = m_color_space; return S_OK; }
    STDMETHODIMP GetBitmapCount(int* c) { if (!c) return E_POINTER; *c = 1; return S_OK; }
    STDMETHODIMP GetBitmap(int index, ULONGLONG* id, POINT* position, SIZE* size, LPCVOID* pixels, int* pitch)
    {
        if (index != 0) return E_INVALIDARG;
        if (!id && !position && !size && !pixels && !pitch) return S_FALSE;
        if (id) *id = m_id;
        if (position) *position = m_pos;
        if (size) *size = m_size;
        if (pixels) *pixels = &m_pixels[0];
        if (pitch) *pitch = m_size.cx * 4;
        return S_OK;
    }
    STDMETHODIMP GetBitmapExtra(int, LPVOID) { return E_NOTIMPL; }

    const DWORD* PixelPtr() const { return &m_pixels[0]; }
    SIZE m_size; POINT m_pos; std::vector<DWORD> m_pixels;
    int m_color_space; ULONGLONG m_id;
    CRect m_output_rect, m_clip_rect;
};

// Provider that hands out one fixed frame.
class StubProvider : public CUnknown, public IXySubRenderProvider
{
public:
    StubProvider(IXySubRenderFrame* frame) : CUnknown(NAME("StubProvider"), NULL), m_frame(frame) {}

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        return QI(IXySubRenderProvider) __super::NonDelegatingQueryInterface(riid, ppv);
    }
    STDMETHODIMP Connect(IXyOptions*) { return S_OK; }
    STDMETHODIMP RequestFrame(IXySubRenderFrame** f, REFERENCE_TIME)
    {
        if (!f) return E_POINTER;
        *f = m_frame;
        if (*f) (*f)->AddRef();
        return S_OK;
    }
    STDMETHODIMP Invalidate(REFERENCE_TIME) { return S_OK; }

    CComPtr<IXySubRenderFrame> m_frame;
};

// Minimal IXyOptions: only the combine flag matters here.
class StubConsumer : public CUnknown, public IXyOptions
{
public:
    StubConsumer() : CUnknown(NAME("StubConsumer"), NULL), m_max_bitmap_count2(4) {}

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        return QI(IXyOptions) __super::NonDelegatingQueryInterface(riid, ppv);
    }

    STDMETHODIMP XyGetInt(unsigned field, int* value)
    {
        if (field == DirectVobSubXyOptions::INT_MAX_BITMAP_COUNT2)
        {
            if (!value) return S_FALSE;
            *value = m_max_bitmap_count2;
            return S_OK;
        }
        return E_INVALIDARG;
    }
    STDMETHODIMP XyGetBool(unsigned, bool*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetSize(unsigned, SIZE*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetRect(unsigned, RECT*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetUlonglong(unsigned, ULONGLONG*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetDouble(unsigned, double*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetString(unsigned, LPWSTR*, int*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetBin(unsigned, LPVOID*, int*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetBin2(unsigned, void*, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetBool(unsigned, bool) { return E_INVALIDARG; }
    STDMETHODIMP XySetInt(unsigned, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetSize(unsigned, SIZE) { return E_INVALIDARG; }
    STDMETHODIMP XySetRect(unsigned, RECT) { return E_INVALIDARG; }
    STDMETHODIMP XySetUlonglong(unsigned, ULONGLONG) { return E_INVALIDARG; }
    STDMETHODIMP XySetDouble(unsigned, double) { return E_INVALIDARG; }
    STDMETHODIMP XySetString(unsigned, LPWSTR, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetBin(unsigned, LPVOID, int) { return E_INVALIDARG; }
    STDMETHODIMP XyGetBools(unsigned, int, bool*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetInts(unsigned, int, int*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetSizes(unsigned, int, SIZE*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetRects(unsigned, int, RECT*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetUlonglongs(unsigned, int, ULONGLONG*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetDoubles(unsigned, int, double*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetStrings(unsigned, int, LPWSTR*, int*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetBins(unsigned, int, LPVOID*, int*) { return E_INVALIDARG; }
    STDMETHODIMP XyGetBins2(unsigned, int, void*, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetBools(unsigned, int, bool) { return E_INVALIDARG; }
    STDMETHODIMP XySetInts(unsigned, int, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetSizes(unsigned, int, SIZE) { return E_INVALIDARG; }
    STDMETHODIMP XySetRects(unsigned, int, RECT) { return E_INVALIDARG; }
    STDMETHODIMP XySetUlonglongs(unsigned, int, ULONGLONG) { return E_INVALIDARG; }
    STDMETHODIMP XySetDoubles(unsigned, int, double) { return E_INVALIDARG; }
    STDMETHODIMP XySetStrings(unsigned, int, LPWSTR, int) { return E_INVALIDARG; }
    STDMETHODIMP XySetBins(unsigned, int, LPVOID, int) { return E_INVALIDARG; }

    int m_max_bitmap_count2;
};

// Loader that resolves a path to a pre-registered stub provider.
class StubLoader : public CUnknown, public IMultiSubtitleTrackLoader
{
public:
    StubLoader() : CUnknown(NAME("StubLoader"), NULL) {}

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        return QI(IMultiSubtitleTrackLoader) __super::NonDelegatingQueryInterface(riid, ppv);
    }
    STDMETHODIMP CreateTrackProviderFromFile(LPCWSTR file_path, IXySubRenderProvider** provider)
    {
        if (!provider) return E_POINTER;
        *provider = NULL;
        auto it = m_by_path.find(std::wstring(file_path ? file_path : L""));
        if (it == m_by_path.end()) return S_FALSE;
        *provider = it->second;
        (*provider)->AddRef();
        return S_OK;
    }

    // Container-embedded sources are out of scope for the composition test:
    // exercise the same path-map lookup keyed by "media|value|kind" when
    // registered.
    STDMETHODIMP CreateTrackProviderFromEmbedded(LPCWSTR media_path, WORD value,
        EmbeddedSelectKind kind, IXySubRenderProvider** provider)
    {
        if (!provider) return E_POINTER;
        *provider = NULL;
        wchar_t key[512];
        _snwprintf_s(key, _TRUNCATE, L"%s|%u|%u", media_path ? media_path : L"", (unsigned)value, (unsigned)kind);
        auto it = m_by_path.find(std::wstring(key));
        if (it == m_by_path.end()) return S_FALSE;
        *provider = it->second;
        (*provider)->AddRef();
        return S_OK;
    }

    // Enumeration is a property-page concern; the composition test has no media
    // and never calls it.
    STDMETHODIMP EnumerateEmbeddedStreams(LPCWSTR media_path,
        std::vector<EmbeddedStreamInfo>* out)
    {
        if (!out) return E_POINTER;
        out->clear();
        return S_FALSE;
    }

    std::map<std::wstring, CComPtr<IXySubRenderProvider>> m_by_path;
};

// Reads the single bitmap out of a (merged) frame and returns a pixel.
static DWORD MergedPixel(IXySubRenderFrame* f, LONG x, LONG y)
{
    POINT p = {0}; SIZE s = {0}; LPCVOID px = NULL; int pitch = 0;
    if (FAILED(f->GetBitmap(0, NULL, &p, &s, &px, &pitch)) || !px) return 0xDEADBEEF;
    if (x < p.x || y < p.y || x >= p.x + s.cx || y >= p.y + s.cy) return 0xDEADBEEF;
    const DWORD* buf = (const DWORD*)px;
    return buf[(size_t)(y - p.y) * (pitch / 4) + (x - p.x)];
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);   // never lose output if a check faults
    const POINT primary_pos = {10, 300};
    const POINT secondary_pos = {20, 5};
    const POINT secondary_offset = {0, 50}; // e.g. this is what ALIGN_TOP resolves to

    StubFrame* primary_raw = new StubFrame(4, 3, primary_pos, 0xFF112233, XY_CS_ARGB_F);
    CComPtr<StubFrame> primary;
    primary.Attach(primary_raw);
    StubFrame* secondary_raw = new StubFrame(2, 2, secondary_pos, 0xFFAABBCC, XY_CS_ARGB_F);
    CComPtr<StubFrame> secondary;
    secondary.Attach(secondary_raw);

    // ---------------------------------------------------------------------
    // CompositeSubRenderFrame contract (multi-bitmap mode).
    // ---------------------------------------------------------------------
    CComPtr<CompositeSubRenderFrame> composite;
    composite.Attach(new CompositeSubRenderFrame());
    composite->AddTrack(primary, POINT{0, 0}, 100.0, 255);
    composite->AddTrack(secondary, secondary_offset, 100.0, 255);

    int count = -1;
    CHECK(SUCCEEDED(composite->GetBitmapCount(&count)) && count == 2, "GetBitmapCount == c1 + c2 (2)");

    RECT out = {0};
    CHECK(SUCCEEDED(composite->GetOutputRect(&out)) && out.right == 4 && out.bottom == 3,
        "GetOutputRect delegates to anchor");
    int cs = -1;
    CHECK(SUCCEEDED(composite->GetXyColorSpace(&cs)) && cs == XY_CS_ARGB_F, "GetXyColorSpace delegates to anchor");

    ULONGLONG id0 = 0; POINT p0 = {0}; SIZE s0 = {0}; LPCVOID px0 = NULL; int pitch0 = 0;
    CHECK(SUCCEEDED(composite->GetBitmap(0, &id0, &p0, &s0, &px0, &pitch0)), "GetBitmap(0) succeeds");
    CHECK(p0.x == primary_pos.x && p0.y == primary_pos.y, "GetBitmap(0) position == primary position (offset 0)");
    CHECK(s0.cx == 4 && s0.cy == 3, "GetBitmap(0) size == primary size");
    CHECK(px0 == primary->PixelPtr(), "GetBitmap(0) pixels == primary buffer (no copy)");

    ULONGLONG id1 = 0; POINT p1 = {0}; SIZE s1 = {0}; LPCVOID px1 = NULL; int pitch1 = 0;
    CHECK(SUCCEEDED(composite->GetBitmap(1, &id1, &p1, &s1, &px1, &pitch1)), "GetBitmap(1) succeeds");
    CHECK(p1.x == secondary_pos.x + secondary_offset.x && p1.y == secondary_pos.y + secondary_offset.y,
        "GetBitmap(1) position == secondary position + offset");
    CHECK(s1.cx == 2 && s1.cy == 2, "GetBitmap(1) size == secondary size");
    CHECK(px1 == secondary->PixelPtr(), "GetBitmap(1) pixels == secondary buffer (no copy)");
    CHECK(*(const DWORD*)px1 == 0xFFAABBCC, "GetBitmap(1) first pixel is the secondary's");

    CHECK(composite->GetBitmap(2, NULL, NULL, NULL, NULL, NULL) == E_INVALIDARG, "index out of range -> E_INVALIDARG");
    CHECK(composite->GetBitmap(0, NULL, NULL, NULL, NULL, NULL) == S_FALSE, "all-NULL out params -> S_FALSE");

    // Scale seam: a scaled track resamples (pixels differ from source buffer) and
    // reports the scaled size, while the count still sums.
    CComPtr<CompositeSubRenderFrame> scaled;
    scaled.Attach(new CompositeSubRenderFrame());
    scaled->AddTrack(primary, POINT{0, 0}, 100.0, 255);
    scaled->AddTrack(secondary, POINT{0, 0}, 200.0, 255);
    int scount = -1;
    scaled->GetBitmapCount(&scount);
    CHECK(scount == 2, "scaled composite count still sums to 2");
    SIZE s1s = {0}; LPCVOID px1s = NULL; int pitch1s = 0;
    CHECK(SUCCEEDED(scaled->GetBitmap(1, NULL, NULL, &s1s, &px1s, &pitch1s)), "scaled GetBitmap(1) succeeds");
    CHECK(s1s.cx == 4 && s1s.cy == 4, "scale 200% doubles the secondary size");
    CHECK(px1s != NULL && px1s != secondary->PixelPtr(), "scaled track provides its own resampled buffer");

    // Opacity seam: opacity 128 tints; count unaffected.
    CComPtr<CompositeSubRenderFrame> tinted;
    tinted.Attach(new CompositeSubRenderFrame());
    tinted->AddTrack(primary, POINT{0, 0}, 100.0, 255);
    tinted->AddTrack(secondary, POINT{0, 0}, 100.0, 128);
    LPCVOID px1t = NULL; SIZE s1t = {0}; int pitch1t = 0;
    CHECK(SUCCEEDED(tinted->GetBitmap(1, NULL, NULL, &s1t, &px1t, &pitch1t)), "tinted GetBitmap(1) succeeds");
    CHECK(px1t != secondary->PixelPtr(), "opacity<255 produces a tinted buffer");

    // ---------------------------------------------------------------------
    // Export of the merge path (now the only composition path the provider
    // uses) still works at the class level, both with 2 and 3 tracks.
    // ---------------------------------------------------------------------
    {
        const DWORD primary_color = 0xFF112233;
        const DWORD secondary_color = 0xFFAABBCC;
        CComPtr<CompositeSubRenderFrame> c;
        c.Attach(new CompositeSubRenderFrame());
        c->SetMergeMode(true);
        c->AddTrack(primary, POINT{0, 0}, 100.0, 255);
        c->AddTrack(secondary, secondary_offset, 100.0, 255);

        int n = -1;
        CHECK(SUCCEEDED(c->GetBitmapCount(&n)) && n == 1, "merge=true + 2 tracks => count 1");

        POINT p = {0}; SIZE s = {0}; LPCVOID px = NULL; int pitch = 0;
        CHECK(SUCCEEDED(c->GetBitmap(0, NULL, &p, &s, &px, &pitch)), "merged GetBitmap(0) succeeds");

        // Union rect: primary x[10,14) y[300,303); secondary (20,55) 2x2 ->
        // x[20,22) y[55,57). Union x[10,22) y[55,303) => pos (10,55), size 12x248.
        CHECK(p.x == 10 && p.y == 55, "merged canvas anchored at union top-left (10,55)");
        CHECK(s.cx == 12 && s.cy == 248, "merged canvas size == union (12x248)");
        CHECK(px != NULL && px != primary->PixelPtr() && px != secondary->PixelPtr(),
            "merged canvas owns its own buffer");

        const DWORD* buf = (const DWORD*)px;
        const int stride = pitch / 4;
        DWORD at_primary = buf[(size_t)(primary_pos.y - p.y) * stride + (primary_pos.x - p.x)];
        DWORD at_secondary = buf[(size_t)(secondary_pos.y + secondary_offset.y - p.y) * stride
            + (secondary_pos.x + secondary_offset.x - p.x)];
        DWORD at_empty = buf[0];
        CHECK(at_primary == primary_color, "merged canvas contains primary ink at its offset");
        CHECK(at_secondary == secondary_color, "merged canvas contains secondary ink at its offset");
        CHECK(at_empty == 0, "merged canvas is transparent where no track paints");

        CHECK(c->GetBitmap(1, NULL, NULL, NULL, NULL, &pitch) == E_INVALIDARG,
            "merge mode index 1 -> E_INVALIDARG");
    }

    // ---------------------------------------------------------------------
    // MultiTrackSubRenderProvider: pass-through vs always-merge.
    // ---------------------------------------------------------------------
    {
        CComPtr<StubLoader> loader;
        loader.Attach(new StubLoader());

        StubFrame* p_raw = new StubFrame(4, 3, POINT{0, 0}, 0xFF112233, XY_CS_ARGB_F);
        CComPtr<StubFrame> pframe; pframe.Attach(p_raw);
        CComPtr<StubProvider> pprov;
        pprov.Attach(new StubProvider(pframe));

        CComPtr<StubConsumer> consumer;
        consumer.Attach(new StubConsumer());
        consumer->m_max_bitmap_count2 = 4; // combineBitmaps = false

        // --- single track: exact pass-through, no wrapper.
        CComPtr<MultiTrackSubRenderProvider> one;
        one.Attach(new MultiTrackSubRenderProvider());
        one->SetLoader(loader);
        one->SetTrackCount(1);
        one->SetTrackProvider(0, pprov);
        one->Connect(consumer);

        CComPtr<IXySubRenderFrame> one_out;
        CHECK(SUCCEEDED(one->RequestFrame(&one_out, 0)) && one_out != NULL, "1 track: RequestFrame succeeds");
        CHECK(one_out.p == (IXySubRenderFrame*)pframe.p, "1 track: source frame returned unchanged (pass-through)");

        // --- three tracks, combine flag FALSE: still a single merged bitmap.
        StubFrame* s1_raw = new StubFrame(2, 2, POINT{0, 0}, 0xFFAA0000, XY_CS_ARGB_F); // blue-ish (BGR)
        CComPtr<StubFrame> s1; s1.Attach(s1_raw);
        CComPtr<StubProvider> s1p; s1p.Attach(new StubProvider(s1));
        StubFrame* s2_raw = new StubFrame(2, 2, POINT{0, 0}, 0xFF00BB00, XY_CS_ARGB_F); // green-ish
        CComPtr<StubFrame> s2; s2.Attach(s2_raw);
        CComPtr<StubProvider> s2p; s2p.Attach(new StubProvider(s2));

        loader->m_by_path[L"b.ass"] = s1p;
        loader->m_by_path[L"c.ass"] = s2p;

        CComPtr<MultiTrackSubRenderProvider> three;
        three.Attach(new MultiTrackSubRenderProvider());
        three->SetLoader(loader);
        three->SetTrackCount(3);
        three->SetTrackProvider(0, pprov);
        three->SetTrackFile(1, L"b.ass");
        three->SetTrackFile(2, L"c.ass");
        three->Connect(consumer);

        CComPtr<IXySubRenderFrame> three_out;
        CHECK(SUCCEEDED(three->RequestFrame(&three_out, 0)) && three_out != NULL, "3 tracks: RequestFrame succeeds");
        int n3 = -1;
        three_out->GetBitmapCount(&n3);
        CHECK(n3 == 1, "3 tracks + combine flag FALSE => single merged bitmap (1)");
        CHECK(three_out.p != (IXySubRenderFrame*)pframe.p, "3 tracks: output is a composite, not the source frame");

        // --- combine flag TRUE gives the same shape.
        consumer->m_max_bitmap_count2 = 1;
        CComPtr<IXySubRenderFrame> three_out2;
        CHECK(SUCCEEDED(three->RequestFrame(&three_out2, 0)) && three_out2 != NULL, "3 tracks (combine TRUE): RequestFrame succeeds");
        int n3b = -1;
        three_out2->GetBitmapCount(&n3b);
        CHECK(n3b == 1, "3 tracks + combine flag TRUE => single merged bitmap (1) (identical to FALSE)");

        // -----------------------------------------------------------------
        // z-order: overlapping secondaries, highest z wins the overlap.
        // -----------------------------------------------------------------
        DWORD color_b = 0xFFAA0000; // s1
        DWORD color_c = 0xFF00BB00; // s2

        // s1 z=1, s2 z=5 -> c (higher) wins.
        three->SetTrackZOrder(1, 1);
        three->SetTrackZOrder(2, 5);
        CComPtr<IXySubRenderFrame> zc;
        three->RequestFrame(&zc, 0);
        CHECK(MergedPixel(zc, 0, 0) == color_c, "z_order: z=5 secondary overwrites z=1 secondary at overlap");

        // swap -> b wins.
        three->SetTrackZOrder(1, 9);
        three->SetTrackZOrder(2, 2);
        CComPtr<IXySubRenderFrame> zb;
        three->RequestFrame(&zb, 0);
        CHECK(MergedPixel(zb, 0, 0) == color_b, "z_order: reversing z-order flips which track wins the overlap");

        // -----------------------------------------------------------------
        // scale: a 200% secondary is visibly enlarged in the merged canvas.
        // -----------------------------------------------------------------
        {
            CComPtr<StubFrame> big; big.Attach(new StubFrame(2, 2, POINT{0, 0}, 0xFF00FF00, XY_CS_ARGB_F));
            CComPtr<StubProvider> bigp; bigp.Attach(new StubProvider(big));
            loader->m_by_path[L"big.ass"] = bigp;

            CComPtr<MultiTrackSubRenderProvider> sc;
            sc.Attach(new MultiTrackSubRenderProvider());
            sc->SetLoader(loader);
            sc->SetTrackCount(2);
            sc->SetTrackProvider(0, pprov);      // primary 4x3 at (0,0)
            sc->SetTrackFile(1, L"big.ass");     // 2x2 at (0,0)
            sc->SetTrackScale(1, 200.0);
            sc->Connect(consumer);

            CComPtr<IXySubRenderFrame> sco;
            sc->RequestFrame(&sco, 0);
            DWORD at33 = MergedPixel(sco, 3, 3);
            DWORD at41 = MergedPixel(sco, 4, 1);
            CHECK(at33 == 0xFF00FF00, "scale 200%: scaled secondary ink reaches (3,3)");
            CHECK(at41 == 0xDEADBEEF, "scale 200%: no scaled ink beyond 4x4 (x=4 is out of bounds)");
        }

        // -----------------------------------------------------------------
        // opacity: an opaque + a 50% secondary blend at the overlap.
        // -----------------------------------------------------------------
        {
            CComPtr<StubFrame> op; op.Attach(new StubFrame(2, 2, POINT{0, 0}, 0xFF00FFFF, XY_CS_ARGB_F));
            CComPtr<StubProvider> opp; opp.Attach(new StubProvider(op));
            loader->m_by_path[L"op.ass"] = opp;

            CComPtr<MultiTrackSubRenderProvider> oc;
            oc.Attach(new MultiTrackSubRenderProvider());
            oc->SetLoader(loader);
            oc->SetTrackCount(2);
            oc->SetTrackProvider(0, pprov);      // far away, no overlap with the pair below
            oc->SetTrackFile(1, L"b.ass");       // opaque 0xFFAA0000 at (0,0)
            oc->SetTrackOpacity(1, 255);
            oc->SetTrackZOrder(1, 1);
            oc->Connect(consumer);

            // Add a second secondary only via a 3-track build (opacity 128).
            oc->SetTrackCount(3);
            oc->SetTrackFile(2, L"op.ass");
            oc->SetTrackOpacity(2, 128);         // 50% green over opaque blue-ish
            oc->SetTrackZOrder(2, 2);
            oc->Connect(consumer);

            CComPtr<IXySubRenderFrame> oco;
            oc->RequestFrame(&oco, 0);
            DWORD blended = MergedPixel(oco, 0, 0);
            CHECK(blended != 0xFFAA0000 && blended != 0xFF00FFFF,
                "opacity<255: overlay blends rather than replacing the track below");
            // 0xFF00FFFF (cyan) at opacity 128 over 0xFFAA0000 (opaque blue-ish):
            // premultiplied src-over => r drops, g/b gain cyan ink.
            DWORD ra = (0xFFAA0000 >> 16) & 0xff, ga = (0xFFAA0000 >> 8) & 0xff, ba = 0xFFAA0000 & 0xff;
            DWORD rb = (blended >> 16) & 0xff, gb = (blended >> 8) & 0xff, bb = blended & 0xff;
            CHECK(ra == 0xAA && ga == 0 && ba == 0 && rb < ra && gb > ga && bb > ba,
                "opacity<255: blended pixel mixes both source colours (r down, g/b up)");
        }
    }

    // ---------------------------------------------------------------------
    // Regression: merged-frame bitmap identity.
    //
    // ISubRenderFrame::GetBitmap's id "is guaranteed to change if the content
    // of the bitmap has changed" (SubRenderIntf.h). The stock single-track
    // provider honours this (a fresh id per frame); the merged multi-track
    // composite must too. Returning a constant id (0) makes the consumer's
    // bitmap cache reuse the first merged frame forever -> the on-screen
    // subtitles never update ("the same subtitles repeat over and over", both
    // tracks frozen together). This is the dual/merged path the embedded and
    // external secondary tracks use.
    // ---------------------------------------------------------------------
    {
        CComPtr<StubLoader> ld; ld.Attach(new StubLoader());

        StubFrame* p_raw = new StubFrame(4, 3, POINT{0, 0}, 0xFF112233, XY_CS_ARGB_F);
        CComPtr<StubProvider> pp; pp.Attach(new StubProvider(p_raw));

        StubFrame* s_raw = new StubFrame(2, 2, POINT{0, 0}, 0xFFAABBCC, XY_CS_ARGB_F);
        CComPtr<StubProvider> sp; sp.Attach(new StubProvider(s_raw));

        ld->m_by_path[L"s.ass"] = sp;

        CComPtr<StubConsumer> cons; cons.Attach(new StubConsumer());
        cons->m_max_bitmap_count2 = 1;   // combine consumer (e.g. MPC-HC)

        CComPtr<MultiTrackSubRenderProvider> mt;
        mt.Attach(new MultiTrackSubRenderProvider());
        mt->SetLoader(ld);
        mt->SetTrackCount(2);
        mt->SetTrackProvider(0, pp);
        mt->SetTrackFile(1, L"s.ass");
        mt->Connect(cons);

        {
            // Distinct, nonzero source ids so a correct composite cannot return 0.
            p_raw->m_id = 0xA1;
            s_raw->m_id = 0xB2;

            CComPtr<IXySubRenderFrame> f1;
            CHECK(SUCCEEDED(mt->RequestFrame(&f1, 0)) && f1 != NULL, "id check: RequestFrame #1 succeeds");
            ULONGLONG id1 = 0;
            CHECK(SUCCEEDED(f1->GetBitmap(0, &id1, NULL, NULL, NULL, NULL)), "id check: GetBitmap #1 succeeds");

            // Content changes before the next requested frame (new cue).
            p_raw->m_id = 0xA3;
            s_raw->m_id = 0xB4;
            p_raw->m_pixels[0] = 0xFF445566;

            CComPtr<IXySubRenderFrame> f2;
            CHECK(SUCCEEDED(mt->RequestFrame(&f2, 1000000)) && f2 != NULL, "id check: RequestFrame #2 succeeds");
            ULONGLONG id2 = 0;
            CHECK(SUCCEEDED(f2->GetBitmap(0, &id2, NULL, NULL, NULL, NULL)), "id check: GetBitmap #2 succeeds");

            CHECK(id1 != 0, "merged frame bitmap id is not the constant 0");
            CHECK(id1 != id2, "merged frame bitmap id changes when track content changes (no stale reuse)");
        }   // f1/f2 released before mt (the composite refers to the source frames)
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED",
        g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
