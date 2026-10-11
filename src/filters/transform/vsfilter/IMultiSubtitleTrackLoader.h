#pragma once

#include "XySubRenderIntf.h"

#include <vector>
#include <string>

// One embedded subtitle stream as discovered from a media container, in the
// form the property page needs to present a pickable list. Filled by the
// container-side enumeration (headers only, no PES/cluster scan).
struct EmbeddedStreamInfo
{
    WORD         index;       // 1-based index into the discovered list
    WORD         raw_id;      // container-native id: TS/DVB PID or MKV TrackNumber
    DWORD        codec;       // TS stream type (0x90 PGS, 0x91 DVB) or MKV kind
    std::string  codec_name;  // "PGS"/"DVB" (TS) or CodecID (MKV, e.g. S_HDMV/PGS)
    std::string  language;    // ISO 639-2/1 code, empty when absent
    std::wstring label;       // human readable label
    bool         matroska;    // true when the stream came from a Matroska container

    EmbeddedStreamInfo() : index(0), raw_id(0), codec(0), matroska(false) {}
};

// Callback used by the subpic-layer multi-track core to obtain a subtitle
// source for an external file without depending on the vsfilter subtitles
// implementation. XySubFilter implements it; MultiTrackSubRenderProvider holds
// an instance and calls it when the secondary track has a file to load.
//
// Living in the vsfilter directory keeps the loader cascade (RTS / VobSub /
// SSF / SUP) on the owning side; the subpic layer stays free of subtitles
// dependencies.
[uuid("D8C0C2A1-8E4A-4B0E-9E16-8F1E2E4C7A55")]
interface IMultiSubtitleTrackLoader : public IUnknown
{
    // How 'value' in CreateTrackProviderFromEmbedded is interpreted.
    enum EmbeddedSelectKind
    {
        EMBEDDED_RAW   = 0,   // container-native id (TS/DVB PID, MKV TrackNumber)
        EMBEDDED_INDEX = 1    // 1-based index into the embedded subtitle list
    };

    // Creates a subtitle provider backed by 'file_path'. On success returns a
    // referenced IXySubRenderProvider in *provider (NULL if no suitable loader
    // matched the file). Callers must Release it.
    STDMETHOD(CreateTrackProviderFromFile)(LPCWSTR file_path, IXySubRenderProvider** provider) = 0;

    // Creates a provider that reads one subtitle stream out of the container at
    // 'media_path' (TS/BDMV or Matroska). 'kind' says whether 'value' is a raw
    // container id (TS/DVB PID, MKV TrackNumber) or a 1-based index into the
    // discovered embedded subtitle stream list. value == 0 selects the first
    // decodable embedded subtitle stream. On success returns a referenced
    // IXySubRenderProvider in *provider. Callers must Release it.
    STDMETHOD(CreateTrackProviderFromEmbedded)(LPCWSTR media_path, WORD value,
        EmbeddedSelectKind kind, IXySubRenderProvider** provider) = 0;

    // Enumerates the embedded subtitle streams of 'media_path' (TS/BDMV or
    // Matroska) without constructing a provider, so a property page can offer a
    // pickable list. Reuses the same container sources as
    // CreateTrackProviderFromEmbedded; opens the file and reads its headers only
    // (PAT/PMT for TS, EBML Tracks for Matroska - no PES/cluster scan).
    //
    // Returns S_OK with the (possibly empty) list in *out when the container was
    // read; S_FALSE when 'media_path' is empty/unknown or the container is not a
    // supported embedded-container (the list is left empty); E_* on failure.
    // Callers must not assume the list is non-empty on success.
    STDMETHOD(EnumerateEmbeddedStreams)(LPCWSTR media_path,
        std::vector<EmbeddedStreamInfo>* out) = 0;
};
