#include "stdafx.h"
#include "CompositeSubRenderFrame.h"

// Multi-track frame that repackages the bitmaps of several source frames.
//
// Contract:
//  * GetBitmapCount()          == sum of the source frames' counts
//  * GetBitmap(i)              delegates to the owning source; the track's
//                               offset is added to *position, and (when the
//                               track asks for a non-identity scale/opacity)
//                               the bitmap is resampled/tinted on demand.
//  * GetOutputRect/GetClipRect/GetXyColorSpace delegate to the anchor (first)
//                               track, preserving the stock single-track view.
//
// When a track is at scale==100/opacity==255 the delegate path touches no
// pixels at all: the pointer returned is the source frame's own buffer.

namespace
{
    // Premultiplied (ARGB): scale every channel including alpha.
    inline DWORD ApplyOpacityPremultiplied(DWORD argb, int opacity)
    {
        DWORD a = (argb >> 24) & 0xff;
        DWORD scale = (a * (DWORD)opacity + 127) / 255;
        DWORD r = ((argb >> 16) & 0xff) * scale / 255;
        DWORD g = ((argb >> 8) & 0xff) * scale / 255;
        DWORD b = (argb & 0xff) * scale / 255;
        return (scale << 24) | (r << 16) | (g << 8) | b;
    }

    // Straight alpha (ARGB_F): scale the alpha channel only, leave RGB alone.
    inline DWORD ApplyOpacityStraight(DWORD argb, int opacity)
    {
        DWORD a = (argb >> 24) & 0xff;
        DWORD na = (a * (DWORD)opacity + 127) / 255;
        return (na << 24) | (argb & 0x00ffffff);
    }
}

CompositeSubRenderFrame::CompositeSubRenderFrame()
    : CUnknown(NAME("CompositeSubRenderFrame"), NULL)
    , m_merged_color_space(XY_CS_ARGB_F)
    , m_merge(false)
    , m_total_count(0)
    , m_anchor(-1)
{
    m_merged_pos.x = m_merged_pos.y = 0;
}

CompositeSubRenderFrame::~CompositeSubRenderFrame()
{
}

STDMETHODIMP CompositeSubRenderFrame::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    return
        QI(IXySubRenderFrame)
        __super::NonDelegatingQueryInterface(riid, ppv);
}

HRESULT CompositeSubRenderFrame::AddTrack(IXySubRenderFrame* frame, POINT offset, double scale, int opacity)
{
    ASSERT(frame);
    if (!frame)
    {
        return E_INVALIDARG;
    }
    int count = 0;
    HRESULT hr = frame->GetBitmapCount(&count);
    if (FAILED(hr))
    {
        return hr;
    }
    if (count < 0)
    {
        count = 0;
    }

    Entry e;
    e.frame = frame;
    e.count = count;
    e.offset = offset;
    e.scale = scale > 0 ? scale : 100.0;
    e.opacity = opacity < 0 ? 0 : (opacity > 255 ? 255 : opacity);
    e.caches.resize(count);

    m_entries.push_back(e);
    m_total_count += count;
    m_merged.ready = false; // tracks changed: invalidate any merged canvas
    if (m_anchor < 0)
    {
        m_anchor = 0;
    }
    return S_OK;
}

STDMETHODIMP CompositeSubRenderFrame::GetOutputRect(RECT *outputRect)
{
    if (m_entries.empty())
    {
        return E_FAIL;
    }
    return m_entries[m_anchor].frame->GetOutputRect(outputRect);
}

STDMETHODIMP CompositeSubRenderFrame::GetClipRect(RECT *clipRect)
{
    if (m_entries.empty())
    {
        return E_FAIL;
    }
    return m_entries[m_anchor].frame->GetClipRect(clipRect);
}

STDMETHODIMP CompositeSubRenderFrame::GetXyColorSpace(int *xyColorSpace)
{
    if (m_entries.empty())
    {
        return E_FAIL;
    }
    return m_entries[m_anchor].frame->GetXyColorSpace(xyColorSpace);
}

STDMETHODIMP CompositeSubRenderFrame::GetBitmapCount(int *count)
{
    if (!count)
    {
        return S_FALSE;
    }
    *count = m_merge ? 1 : m_total_count;
    return S_OK;
}

CompositeSubRenderFrame::Entry* CompositeSubRenderFrame::FindEntry(int index, int* local_index)
{
    if (index < 0)
    {
        return NULL;
    }
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        if (index < m_entries[i].count)
        {
            if (local_index)
            {
                *local_index = index;
            }
            return &m_entries[i];
        }
        index -= m_entries[i].count;
    }
    return NULL;
}

STDMETHODIMP CompositeSubRenderFrame::GetBitmap(int index, ULONGLONG *id, POINT *position, SIZE *size
    , LPCVOID *pixels, int *pitch)
{
    if (m_merge)
    {
        if (index != 0)
        {
            return E_INVALIDARG;
        }
        if (!id && !position && !size && !pixels && !pitch)
        {
            return S_FALSE;
        }
        if (!m_merged.ready)
        {
            HRESULT hr = BuildMergedBitmap();
            if (FAILED(hr))
            {
                return hr;
            }
        }
        if (m_merged.pixels.empty())
        {
            return S_FALSE;
        }
        if (id)
        {
            *id = GetMergedBitmapId();
        }
        if (position)
        {
            *position = m_merged_pos;
        }
        if (size)
        {
            size->cx = m_merged.w;
            size->cy = m_merged.h;
        }
        if (pixels)
        {
            *pixels = &m_merged.pixels[0];
        }
        if (pitch)
        {
            *pitch = m_merged.pitch;
        }
        return S_OK;
    }

    int local = 0;
    Entry* e = FindEntry(index, &local);
    if (!e)
    {
        return E_INVALIDARG;
    }
    if (!id && !position && !size && !pixels && !pitch)
    {
        return S_FALSE;
    }

    if (e->scale == 100.0 && e->opacity == 255)
    {
        // Identity track: pure delegation, no pixel copy. Mirrors the stock
        // single-provider path byte-for-byte.
        HRESULT hr = e->frame->GetBitmap(local, id, position, size, pixels, pitch);
        if (FAILED(hr))
        {
            return hr;
        }
        if (position)
        {
            position->x += e->offset.x;
            position->y += e->offset.y;
        }
        return hr;
    }

    return GetTransformedBitmap(*e, local, id, position, size, pixels, pitch);
}

// Content identity of the merged canvas. The interfaces contract is explicit
// (SubRenderIntf.h: "The ID is guaranteed to change if the content of the
// bitmap has changed"), and the consumer keys its bitmap cache on this id (and
// may skip re-blitting a frame with an id it already has). The merged canvas
// must therefore carry a distinct id whenever the composite changes: the
// source frames' own ids (which DO change as cues advance) folded together with
// the layout (canvas position/size). Returning a constant id here made the
// consumer keep/reuse an already-delivered merged bitmap, which is exactly the
// "subtitles repeat / never flip" symptom.
ULONGLONG CompositeSubRenderFrame::GetMergedBitmapId()
{
    ULONGLONG h = 1469598103934665603ULL;   // FNV-1a 64 offset basis
    h = (h ^ (ULONGLONG)(DWORD)m_merged.w) * 1099511628211ULL;
    h = (h ^ (ULONGLONG)(DWORD)m_merged.h) * 1099511628211ULL;
    h = (h ^ (ULONGLONG)(DWORD)m_merged_pos.x) * 1099511628211ULL;
    h = (h ^ (ULONGLONG)(DWORD)m_merged_pos.y) * 1099511628211ULL;
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        Entry& e = m_entries[i];
        for (int j = 0; j < e.count; j++)
        {
            // Only query the id (all other out params NULL): source frames
            // answer this without copying any pixels.
            ULONGLONG id = 0;
            if (SUCCEEDED(e.frame->GetBitmap(j, &id, NULL, NULL, NULL, NULL)))
            {
                h = (h ^ id) * 1099511628211ULL;
            }
            h = (h ^ (ULONGLONG)(DWORD)j) * 1099511628211ULL;
        }
    }
    return h;
}

HRESULT CompositeSubRenderFrame::GetTransformedBitmap(Entry& e, int local, ULONGLONG *id, POINT *position
    , SIZE *size, LPCVOID *pixels, int *pitch)
{
    ULONGLONG src_id = 0;
    POINT src_position = {0, 0};
    SIZE src_size = {0, 0};
    LPCVOID src_pixels = NULL;
    int src_pitch = 0;

    HRESULT hr = e.frame->GetBitmap(local, id ? &src_id : NULL, &src_position, &src_size
        , &src_pixels, &src_pitch);
    if (FAILED(hr))
    {
        return hr;
    }
    if (id)
    {
        *id = src_id;
    }

    int color_space = XY_CS_ARGB_F;
    e.frame->GetXyColorSpace(&color_space);

    // Seam: resampling/tinting is implemented for packed 32bpp only. For planar
    // (AYUV) frames fall back to identity delegation with the offset applied, so
    // scale/opacity are simply deferred rather than mis-rendered.
    if (color_space == XY_CS_AYUV_PLANAR)
    {
        HRESULT hr2 = e.frame->GetBitmap(local, id, position, size, pixels, pitch);
        if (FAILED(hr2))
        {
            return hr2;
        }
        if (position)
        {
            position->x += e.offset.x;
            position->y += e.offset.y;
        }
        return hr2;
    }

    SIZE dst_size;
    dst_size.cx = (LONG)(src_size.cx * e.scale / 100.0 + 0.5);
    dst_size.cy = (LONG)(src_size.cy * e.scale / 100.0 + 0.5);
    if (dst_size.cx < 1) dst_size.cx = 1;
    if (dst_size.cy < 1) dst_size.cy = 1;

    ASSERT(local >= 0 && local < (int)e.caches.size());
    BitmapCache& cache = e.caches[local];
    if (!cache.ready)
    {
        hr = BuildScaledBitmap(src_pixels, src_pitch, src_size, dst_size, e.opacity, color_space, cache);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    if (position)
    {
        position->x = src_position.x + e.offset.x;
        position->y = src_position.y + e.offset.y;
    }
    if (size)
    {
        *size = dst_size;
    }
    if (pixels)
    {
        *pixels = &cache.pixels[0];
    }
    if (pitch)
    {
        *pitch = cache.pitch;
    }
    return S_OK;
}

// Composites every track's bitmaps (in the order they were added = z-order,
// ascending) onto one canvas anchored at the union of their rects. Used when the
// consumer asks for combined bitmaps; the canvas keeps layout resolution so the
// consumer still applies its own scaling. Pixels are premultiplied throughout.
HRESULT CompositeSubRenderFrame::BuildMergedBitmap()
{
    // Pass 1: union of every track's (offset/scale-adjusted) bitmap rects.
    CRect union_rect;
    union_rect.SetRectEmpty();

    for (size_t i = 0; i < m_entries.size(); i++)
    {
        Entry& e = m_entries[i];
        for (int j = 0; j < e.count; j++)
        {
            POINT pos = {0, 0};
            SIZE sz = {0, 0};
            if (FAILED(e.frame->GetBitmap(j, NULL, &pos, &sz, NULL, NULL)))
            {
                continue;
            }
            if (e.scale != 100.0)
            {
                sz.cx = (LONG)(sz.cx * e.scale / 100.0 + 0.5);
                sz.cy = (LONG)(sz.cy * e.scale / 100.0 + 0.5);
            }
            pos.x += e.offset.x;
            pos.y += e.offset.y;
            CRect r(pos, sz);
            union_rect |= r;
        }
    }

    if (union_rect.IsRectEmpty())
    {
        m_merged.w = 0;
        m_merged.h = 0;
        m_merged.pitch = 0;
        m_merged.pixels.clear();
        m_merged.ready = true;
        m_merged_pos.x = m_merged_pos.y = 0;
        return S_OK;
    }

    m_merged.w = union_rect.Width();
    m_merged.h = union_rect.Height();
    m_merged.pitch = ((m_merged.w * 4) + 15) & ~15;
    m_merged.pixels.assign((size_t)m_merged.pitch * m_merged.h, 0);
    m_merged_pos.x = union_rect.left;
    m_merged_pos.y = union_rect.top;

    m_merged_color_space = XY_CS_ARGB_F;
    if (m_anchor >= 0 && m_anchor < (int)m_entries.size())
    {
        m_entries[m_anchor].frame->GetXyColorSpace(&m_merged_color_space);
    }

    // Pass 2: alpha-composite each bitmap, in added order, at its local offset.
    for (size_t i = 0; i < m_entries.size(); i++)
    {
        Entry& e = m_entries[i];
        for (int j = 0; j < e.count; j++)
        {
            ULONGLONG id = 0;
            POINT pos = {0, 0};
            SIZE sz = {0, 0};
            LPCVOID px = NULL;
            int pitch = 0;
            HRESULT hr = GetTransformedBitmap(e, j, &id, &pos, &sz, &px, &pitch);
            if (FAILED(hr) || !px || sz.cx <= 0 || sz.cy <= 0)
            {
                continue;
            }
            int dx = pos.x - m_merged_pos.x;
            int dy = pos.y - m_merged_pos.y;
            CompositeOnto(m_merged, dx, dy, px, pitch, sz);
        }
    }

    m_merged.ready = true;
    return S_OK;
}

// src-over composite of one premultiplied ARGB source onto the destination
// canvas at (dx,dy), clipping to the canvas. Destination stays premultiplied:
//   out = src + dst * (1 - src.a)
void CompositeSubRenderFrame::CompositeOnto(BitmapCache& dst, int dx, int dy, LPCVOID src_pixels
    , int src_pitch, SIZE src_size)
{
    if (!src_pixels || src_size.cx <= 0 || src_size.cy <= 0 || dst.w <= 0 || dst.h <= 0)
    {
        return;
    }

    int x0 = max(0, dx);
    int y0 = max(0, dy);
    int x1 = min(dst.w, dx + src_size.cx);
    int y1 = min(dst.h, dy + src_size.cy);
    if (x0 >= x1 || y0 >= y1)
    {
        return;
    }

    const BYTE* src = reinterpret_cast<const BYTE*>(src_pixels);
    BYTE* d = &dst.pixels[0];

    for (int y = y0; y < y1; y++)
    {
        int sy = y - dy;
        const DWORD* s_row = reinterpret_cast<const DWORD*>(src + (size_t)sy * src_pitch) + (x0 - dx);
        DWORD* d_row = reinterpret_cast<DWORD*>(d + (size_t)y * dst.pitch) + x0;
        for (int x = x0; x < x1; x++, s_row++, d_row++)
        {
            DWORD s = *s_row;
            DWORD sa = (s >> 24) & 0xff;
            if (sa == 0)
            {
                continue;
            }
            if (sa == 0xff)
            {
                *d_row = s;
                continue;
            }
            DWORD inv = 255 - sa;
            DWORD dr = ((*d_row >> 16) & 0xff) * inv / 255 + ((s >> 16) & 0xff);
            DWORD dg = ((*d_row >> 8) & 0xff) * inv / 255 + ((s >> 8) & 0xff);
            DWORD db = (*d_row & 0xff) * inv / 255 + (s & 0xff);
            DWORD da = ((*d_row >> 24) & 0xff) * inv / 255 + sa;
            *d_row = (da << 24) | (dr << 16) | (dg << 8) | db;
        }
    }
}

HRESULT CompositeSubRenderFrame::BuildScaledBitmap(LPCVOID src_pixels, int src_pitch, SIZE src_size
    , SIZE dst_size, int opacity, int color_space, BitmapCache& cache)
{
    // Seam: only the packed 32bpp path is resampled here. Planar (AYUV) frames
    // keep their source size; scaling them is a rasteriser-side concern and is
    // intentionally out of scope for this pass.
    const int bpp = 4;
    if (src_pitch <= 0 || src_size.cx <= 0 || src_size.cy <= 0)
    {
        return E_INVALIDARG;
    }

    cache.w = dst_size.cx;
    cache.h = dst_size.cy;
    cache.pitch = ((dst_size.cx * bpp) + 15) & ~15;
    cache.pixels.resize((size_t)cache.pitch * dst_size.cy);

    const BYTE* src = reinterpret_cast<const BYTE*>(src_pixels);
    BYTE* dst = &cache.pixels[0];

    // Nearest-neighbour resample. Source frames are premultiplied; NEAREST only
    // copies whole texels, so premultiplication stays valid with no unpremultiply
    // round trip.
    const bool premultiplied = (color_space != XY_CS_ARGB);
    for (LONG y = 0; y < dst_size.cy; y++)
    {
        int sy = (int)((y * src_size.cy) / dst_size.cy);
        const DWORD* s_row = reinterpret_cast<const DWORD*>(src + (size_t)sy * src_pitch);
        DWORD* d_row = reinterpret_cast<DWORD*>(dst + (size_t)y * cache.pitch);
        for (LONG x = 0; x < dst_size.cx; x++)
        {
            int sx = (int)((x * src_size.cx) / dst_size.cx);
            DWORD p = s_row[sx];
            if (opacity != 255)
            {
                p = premultiplied ? ApplyOpacityPremultiplied(p, opacity) : ApplyOpacityStraight(p, opacity);
            }
            d_row[x] = p;
        }
    }
    cache.ready = true;
    return S_OK;
}

STDMETHODIMP CompositeSubRenderFrame::GetBitmapExtra(int index, LPVOID extra_info)
{
    if (m_merge)
    {
        if (index != 0)
        {
            return E_INVALIDARG;
        }
        // Merged canvas is always packed ARGB; no planar extra data to report.
        return S_OK;
    }
    int local = 0;
    Entry* e = FindEntry(index, &local);
    if (!e)
    {
        return E_INVALIDARG;
    }
    return e->frame->GetBitmapExtra(local, extra_info);
}
