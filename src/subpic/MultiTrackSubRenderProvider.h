#pragma once

#include "XySubRenderIntf.h"
#include "CompositeSubRenderFrame.h"
#include "IMultiSubtitleTrackLoader.h"
#include <vector>

// N-track renderer. Owns an ordered list of track sources and produces one
// multi-bitmap IXySubRenderFrame per RequestFrame by asking every enabled,
// currently-visible track for its frame and repackaging the bitmaps
// (see CompositeSubRenderFrame). Tracks carry all per-track composition data
// (offset, scale, opacity, z-order) so "A on top of B" is data, not code.
//
// When exactly one track is enabled/present the primary frame is returned
// directly with no extra wrapper work, so the single-track path stays a clean
// pass-through.
class MultiTrackSubRenderProvider : public CUnknown, public IXySubRenderProvider
{
public:
    MultiTrackSubRenderProvider();
    virtual ~MultiTrackSubRenderProvider();

    static const int MAX_TRACK_COUNT = 8;

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);

    // Track management (M1 uses at most two; the container is N-ready).
    HRESULT SetTrackCount(int count);
    int GetTrackCount() const { return (int)m_tracks.size(); }

    // Track 0 is the anchor (its frame defines output/clip/colorspace).
    HRESULT SetTrackProvider(int index, IXySubRenderProvider* provider);

    // External-file source for a track (used by the secondary). Stores the path
    // and, if a loader callback is installed, resolves it immediately. A later
    // SetTrackProvider overrides it.
    HRESULT SetTrackFile(int index, LPCWSTR file_path);

    // Container-embedded source for a track. 'media_path' is the media file,
    // 'value' the container-native stream id or 1-based list index (see
    // EmbeddedSelectKind; 0 = first decodable subtitle stream). Resolved
    // immediately through the injected loader.
    HRESULT SetTrackEmbedded(int index, LPCWSTR media_path, WORD value,
        IMultiSubtitleTrackLoader::EmbeddedSelectKind kind);
    void SetLoader(IMultiSubtitleTrackLoader* loader) { m_loader = loader; }
    HRESULT SetTrackEnabled(int index, bool enabled);
    HRESULT SetTrackOffset(int index, POINT offset);
    HRESULT SetTrackScale(int index, double scale_percent);
    HRESULT SetTrackOpacity(int index, int opacity);
    HRESULT SetTrackDelay(int index, REFERENCE_TIME delay);
    HRESULT SetTrackZOrder(int index, int z_order);
    HRESULT SetTrackPlacement(int index, SubRenderTrack::VerticalAlign align, int margin);

    // Horizontal center in the other track's coordinate space: when placement
    // is not ALIGN_AS_IS, each track's bitmaps are centered within 'layout_width'.
    void SetLayoutWidth(int layout_width) { m_layout_width = layout_width; }

    // IXySubRenderProvider
    STDMETHODIMP Connect(IXyOptions *consumer);
    STDMETHODIMP RequestFrame(IXySubRenderFrame** subRenderFrame, REFERENCE_TIME now);
    STDMETHODIMP Invalidate(REFERENCE_TIME rtInvalidate = -1);

private:
    struct Track
    {
        CComPtr<IXySubRenderProvider> provider;
        SubRenderTrack spec;
        Track() {}
    };

    SubRenderTrack* TrackSpec(int index);
    HRESULT CollectFrame(Track& track, REFERENCE_TIME now, IXySubRenderFrame** frame);
    HRESULT ResolvePlacement(Track& track, IXySubRenderFrame* frame, POINT* offset);
    HRESULT ReadConsumerCombineFlag();

    std::vector<Track> m_tracks;
    CComPtr<IMultiSubtitleTrackLoader> m_loader;
    int m_layout_width;
    CComPtr<IXyOptions> m_consumer;
    bool m_combine_bitmaps; // consumer asked for a single merged bitmap
};
