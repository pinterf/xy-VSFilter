#pragma once

#include "XySubRenderIntf.h"
#include <vector>

// Descriptor of one subtitle track in the multi-track compositor.
// This is the single source of truth for per-track composition data;
// nothing about "primary" or "secondary" is hardcoded.
struct SubRenderTrack
{
    enum VerticalAlign
    {
        ALIGN_AS_IS = 0, // keep the source frame's own placement (plus 'offset')
        ALIGN_TOP,       // put the track's bounding box top at 'margin'
        ALIGN_BOTTOM     // (reserved; not used by M1)
    };

    SubRenderTrack()
        : enabled(true)
        , delay(0)
        , scale(100.0)   // percent, 100.0 == identity
        , opacity(255)   // 0..255, 255 == identity
        , z_order(0)     // ascending paint order (higher drawn later / on top)
        , vertical_align(ALIGN_AS_IS)
        , margin(0)      // px, used together with vertical_align
    {
        offset.x = offset.y = 0;
    }

    CString file_path;                      // external source file for the loader
    bool enabled;
    REFERENCE_TIME delay;   // per-track time offset, applied before RequestFrame
    POINT offset;           // explicit pixel offset added to every bitmap
    double scale;           // percent; only 100.0 is currently bit-exact
    int opacity;            // 0..255
    int z_order;            // stack order
    VerticalAlign vertical_align;
    int margin;             // px
};

// A read-only frame that presents the bitmaps of several source frames as one
// IXySubRenderFrame. Bitmaps are repackaged, not merged: GetBitmapCount is the
// sum of the source counts and GetBitmap delegates to the owning source with
// that track's geometry applied. Pixels are copied only when a track requests a
// non-identity scale/opacity transform (see GetTransformedBitmap).
class CompositeSubRenderFrame : public CUnknown, public IXySubRenderFrame
{
public:
    CompositeSubRenderFrame();
    virtual ~CompositeSubRenderFrame();

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);

    // IXySubRenderFrame / ISubRenderFrame
    STDMETHODIMP GetOutputRect(RECT *outputRect);
    STDMETHODIMP GetClipRect(RECT *clipRect);
    STDMETHODIMP GetXyColorSpace(int *xyColorSpace);
    STDMETHODIMP GetBitmapCount(int *count);
    STDMETHODIMP GetBitmap(int index, ULONGLONG *id, POINT *position, SIZE *size, LPCVOID *pixels, int *pitch);
    STDMETHODIMP GetBitmapExtra(int index, LPVOID extra_info);

    // Append one source frame with its resolved geometry. Tracks are stored in
    // the order they are added; the caller orders them (z_order).
    HRESULT AddTrack(IXySubRenderFrame* frame, POINT offset, double scale, int opacity);

    // When enabled, GetBitmapCount() reports 1 and GetBitmap(0) returns a single
    // canvas on which every track's bitmaps are alpha-composited in z-order,
    // anchored at the union of their rects. Required by consumers that set
    // combineBitmaps (they render only bitmap 0 otherwise).
    void SetMergeMode(bool merge) { m_merge = merge; }

    int GetFrameCount() const { return (int)m_entries.size(); }

private:
    struct BitmapCache
    {
        std::vector<BYTE> pixels;
        int pitch, w, h;
        bool ready;
        BitmapCache() : pitch(0), w(0), h(0), ready(false) {}
    };

    struct Entry
    {
        CComPtr<IXySubRenderFrame> frame;
        int count;
        POINT offset;
        double scale;
        int opacity;
        std::vector<BitmapCache> caches;
        Entry() : count(0), scale(100.0), opacity(255) { offset.x = offset.y = 0; }
    };

    Entry* FindEntry(int index, int* local_index);
    ULONGLONG GetMergedBitmapId();
    HRESULT GetTransformedBitmap(Entry& e, int local, ULONGLONG *id, POINT *position,
        SIZE *size, LPCVOID *pixels, int *pitch);
    HRESULT BuildMergedBitmap();
    static void CompositeOnto(BitmapCache& dst, int dx, int dy, LPCVOID src_pixels,
        int src_pitch, SIZE src_size);
    static HRESULT BuildScaledBitmap(LPCVOID src_pixels, int src_pitch, SIZE src_size,
        SIZE dst_size, int opacity, int color_space, BitmapCache& cache);

    std::vector<Entry> m_entries;
    BitmapCache m_merged;
    POINT m_merged_pos;
    int m_merged_color_space;
    bool m_merge;
    int m_total_count;
    int m_anchor; // entry that supplies output/clip/colorspace (first added)
};
