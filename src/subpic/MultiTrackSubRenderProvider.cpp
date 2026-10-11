#include "stdafx.h"
#include "MultiTrackSubRenderProvider.h"
#include "IDirectVobSubXy.h"
#include <algorithm>

MultiTrackSubRenderProvider::MultiTrackSubRenderProvider()
    : CUnknown(NAME("MultiTrackSubRenderProvider"), NULL)
    , m_layout_width(0)
    , m_combine_bitmaps(false)
{
}

MultiTrackSubRenderProvider::~MultiTrackSubRenderProvider()
{
}

STDMETHODIMP MultiTrackSubRenderProvider::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    return
        QI(IXySubRenderProvider)
        __super::NonDelegatingQueryInterface(riid, ppv);
}

SubRenderTrack* MultiTrackSubRenderProvider::TrackSpec(int index)
{
    if (index < 0 || index >= (int)m_tracks.size())
    {
        return NULL;
    }
    return &m_tracks[index].spec;
}

HRESULT MultiTrackSubRenderProvider::SetTrackCount(int count)
{
    if (count < 0 || count > MAX_TRACK_COUNT)
    {
        return E_INVALIDARG;
    }
    m_tracks.resize(count);
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackProvider(int index, IXySubRenderProvider* provider)
{
    if (index < 0 || index >= (int)m_tracks.size())
    {
        return E_INVALIDARG;
    }
    m_tracks[index].provider = provider;
    m_tracks[index].spec.file_path.Empty();
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackFile(int index, LPCWSTR file_path)
{
    if (index < 0 || index >= (int)m_tracks.size())
    {
        return E_INVALIDARG;
    }
    Track& track = m_tracks[index];
    track.spec.file_path = file_path ? file_path : L"";
    track.provider = NULL;
    if (!track.spec.file_path.IsEmpty() && m_loader)
    {
        IXySubRenderProvider* provider = NULL;
        HRESULT hr = m_loader->CreateTrackProviderFromFile(track.spec.file_path, &provider);
        if (FAILED(hr))
        {
            XY_LOG_WARN("Failed to load secondary subtitle '"<<track.spec.file_path.GetString()<<"' hr:"<<XY_LOG_VAR_2_STR(hr));
            return hr;
        }
        track.provider.Attach(provider); // may be NULL if no loader matched
    }
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackEmbedded(int index, LPCWSTR media_path, WORD value,
    IMultiSubtitleTrackLoader::EmbeddedSelectKind kind)
{
    if (index < 0 || index >= (int)m_tracks.size())
    {
        return E_INVALIDARG;
    }
    Track& track = m_tracks[index];
    track.provider = NULL;
    track.spec.file_path.Empty();
    if (!media_path || !*media_path)
    {
        return E_INVALIDARG;
    }
    if (!m_loader)
    {
        XY_LOG_WARN("No loader installed; cannot open embedded stream "<<XY_LOG_VAR_2_STR(value));
        return S_FALSE;
    }

    IXySubRenderProvider* provider = NULL;
    HRESULT hr = m_loader->CreateTrackProviderFromEmbedded(media_path, value, kind, &provider);
    if (FAILED(hr))
    {
        XY_LOG_WARN("Failed to open embedded subtitle "<<XY_LOG_VAR_2_STR(value)<<" hr:"<<XY_LOG_VAR_2_STR(hr));
        return hr;
    }
    track.provider.Attach(provider); // may be NULL when nothing matched
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackEnabled(int index, bool enabled)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->enabled = enabled;
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackOffset(int index, POINT offset)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->offset = offset;
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackScale(int index, double scale_percent)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    if (scale_percent <= 0)
    {
        return E_INVALIDARG;
    }
    t->scale = scale_percent;
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackOpacity(int index, int opacity)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->opacity = opacity < 0 ? 0 : (opacity > 255 ? 255 : opacity);
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackDelay(int index, REFERENCE_TIME delay)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->delay = delay;
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackZOrder(int index, int z_order)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->z_order = z_order;
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::SetTrackPlacement(int index, SubRenderTrack::VerticalAlign align, int margin)
{
    SubRenderTrack* t = TrackSpec(index);
    if (!t) return E_INVALIDARG;
    t->vertical_align = align;
    t->margin = margin;
    return S_OK;
}

STDMETHODIMP MultiTrackSubRenderProvider::Connect(IXyOptions *consumer)
{
    m_consumer = consumer;
    ReadConsumerCombineFlag();
    for (size_t i = 0; i < m_tracks.size(); i++)
    {
        if (m_tracks[i].provider)
        {
            m_tracks[i].provider->Connect(consumer);
        }
    }
    return S_OK;
}

// Mirrors what XySubFilter::DoGetField(INT_MAX_BITMAP_COUNT2) resolves to: when
// the consumer sets combineBitmaps the effective max bitmap count is 1.
HRESULT MultiTrackSubRenderProvider::ReadConsumerCombineFlag()
{
    m_combine_bitmaps = false;
    if (!m_consumer)
    {
        return S_FALSE;
    }

    int max_bitmap_count2 = 0;
    HRESULT hr = m_consumer->XyGetInt(DirectVobSubXyOptions::INT_MAX_BITMAP_COUNT2, &max_bitmap_count2);
    if (SUCCEEDED(hr))
    {
        m_combine_bitmaps = (max_bitmap_count2 == 1);
        return S_OK;
    }

    // Fall back to the boolean if the resolved count is unavailable.
    bool combine = false;
    hr = m_consumer->XyGetBool(DirectVobSubXyOptions::BOOL_COMBINE_BITMAPS, &combine);
    if (SUCCEEDED(hr))
    {
        m_combine_bitmaps = combine;
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP MultiTrackSubRenderProvider::RequestFrame(IXySubRenderFrame** subRenderFrame, REFERENCE_TIME now)
{
    CheckPointer(subRenderFrame, E_POINTER);
    *subRenderFrame = NULL;

    // Stock re-reads this every RequestFrame; do the same so late consumer
    // preference changes are picked up without relying on Invalidate.
    ReadConsumerCombineFlag();

    if (m_tracks.empty())
    {
        return S_FALSE;
    }

    // Gather one source frame per enabled, visible track in z-order (ascending:
    // later tracks are drawn on top). The anchor (track 0) is emitted first so
    // the composite's output/clip/colorspace stay those of the primary track.
    std::vector<int> order;
    for (int i = 0; i < (int)m_tracks.size(); i++)
    {
        if (i != 0 && m_tracks[i].provider && m_tracks[i].spec.enabled)
        {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(),
        [this](int a, int b) { return m_tracks[a].spec.z_order < m_tracks[b].spec.z_order; });
    if (m_tracks[0].provider && m_tracks[0].spec.enabled)
    {
        order.insert(order.begin(), 0);
    }

    // Collect each visible track's frame exactly once.
    struct Resolved
    {
        CComPtr<IXySubRenderFrame> frame;
        POINT offset;
        double scale;
        int opacity;
    };
    std::vector<Resolved> resolved;

    for (size_t k = 0; k < order.size(); k++)
    {
        Track& track = m_tracks[order[k]];
        IXySubRenderFrame* frame = NULL;
        HRESULT hr = CollectFrame(track, now, &frame);
        if (FAILED(hr))
        {
            XY_LOG_ERROR("RequestFrame failed for track "<<order[k]<<" hr:"<<XY_LOG_VAR_2_STR(hr));
            continue;
        }
        if (!frame)
        {
            continue;
        }
        Resolved r;
        r.frame.Attach(frame);
        r.offset.x = r.offset.y = 0;
        r.scale = track.spec.scale;
        r.opacity = track.spec.opacity;
        ResolvePlacement(track, frame, &r.offset);
        resolved.push_back(r);
    }

    if (resolved.empty())
    {
        return S_FALSE;
    }

    // Single-track fast path: hand the source frame straight through so the
    // primary-only case is a clean pass-through (no wrapper, no copy). This
    // also covers the "off / no secondary" requirement exactly.
    if (resolved.size() == 1)
    {
        *subRenderFrame = resolved[0].frame.Detach();
        return S_OK;
    }

    // We own composition: whenever more than one track is active the output is
    // a single merged bitmap (see BuildMergedBitmap), independent of the
    // consumer's combineBitmaps preference. Single-track stays a pass-through
    // so the stock path is unchanged.
    CComPtr<CompositeSubRenderFrame> composite;
    composite = DEBUG_NEW CompositeSubRenderFrame();
    if (!composite)
    {
        return E_OUTOFMEMORY;
    }
    composite->SetMergeMode(true);
    for (size_t i = 0; i < resolved.size(); i++)
    {
        HRESULT hr = composite->AddTrack(resolved[i].frame, resolved[i].offset
            , resolved[i].scale, resolved[i].opacity);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    *subRenderFrame = composite.Detach();
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::CollectFrame(Track& track, REFERENCE_TIME now, IXySubRenderFrame** frame)
{
    *frame = NULL;
    if (!track.provider)
    {
        return S_FALSE;
    }
    REFERENCE_TIME track_now = now - track.spec.delay;
    IXySubRenderFrame* f = NULL;
    HRESULT hr = track.provider->RequestFrame(&f, track_now);
    if (FAILED(hr))
    {
        return hr;
    }
    *frame = f; // may be NULL when the track has nothing at this time
    return S_OK;
}

HRESULT MultiTrackSubRenderProvider::ResolvePlacement(Track& track, IXySubRenderFrame* frame, POINT* offset)
{
    offset->x = track.spec.offset.x;
    offset->y = track.spec.offset.y;

    // Compute the union bounding box of the frame's bitmaps for vertical
    // placement. ALIGN_TOP puts that box's top at 'margin'; the caller is
    // responsible for giving a track a distinct margin/order (see SubRenderTrack).
    if (track.spec.vertical_align == SubRenderTrack::ALIGN_AS_IS)
    {
        return S_OK;
    }

    int count = 0;
    if (FAILED(frame->GetBitmapCount(&count)) || count <= 0)
    {
        return S_OK;
    }

    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (int i = 0; i < count; i++)
    {
        POINT pos = {0, 0};
        SIZE size = {0, 0};
        if (FAILED(frame->GetBitmap(i, NULL, &pos, &size, NULL, NULL)))
        {
            continue;
        }
        min_x = min(min_x, (int)pos.x);
        min_y = min(min_y, (int)pos.y);
        max_x = max(max_x, (int)(pos.x + size.cx));
        max_y = max(max_y, (int)(pos.y + size.cy));
    }
    if (min_x >= max_x || min_y >= max_y)
    {
        return S_OK;
    }

    if (track.spec.vertical_align == SubRenderTrack::ALIGN_TOP)
    {
        offset->y += track.spec.margin - min_y;
    }
    else if (track.spec.vertical_align == SubRenderTrack::ALIGN_BOTTOM)
    {
        // reserved
    }
    if (m_layout_width > 0)
    {
        offset->x += (m_layout_width - (max_x - min_x)) / 2 - min_x;
    }
    return S_OK;
}

STDMETHODIMP MultiTrackSubRenderProvider::Invalidate(REFERENCE_TIME rtInvalidate)
{
    // The consumer may have changed its combineBitmaps preference; re-read it.
    ReadConsumerCombineFlag();
    for (size_t i = 0; i < m_tracks.size(); i++)
    {
        if (m_tracks[i].provider)
        {
            m_tracks[i].provider->Invalidate(rtInvalidate);
        }
    }
    return S_OK;
}
