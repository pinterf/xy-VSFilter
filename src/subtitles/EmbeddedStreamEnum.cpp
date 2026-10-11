/*
 * See EmbeddedStreamEnum.h.
 */

#include "stdafx.h"
#include <atlstr.h>
#include "EmbeddedStreamEnum.h"
#include "TsSubtitleSource.h"
#include "MatroskaSubtitleSource.h"

HRESULT EnumerateEmbeddedStreamsFromMedia(LPCTSTR media_path,
    std::vector<EmbeddedStreamInfo>* out)
{
    if (!out)
    {
        return E_POINTER;
    }
    out->clear();
    if (!media_path || !*media_path)
    {
        return S_FALSE;
    }

    CString media(media_path);

    if (IsMatroskaFile(media))
    {
        CMatroskaSubtitleSource src;
        if (!src.Open(media))
        {
            return S_FALSE;
        }
        const std::vector<MatroskaSubtitleStream>& streams = src.GetSubtitleStreams();
        for (size_t i = 0; i < streams.size(); i++)
        {
            EmbeddedStreamInfo info;
            info.index = (WORD)(i + 1);
            info.raw_id = (WORD)streams[i].track_number;
            info.codec = (DWORD)streams[i].kind;
            info.codec_name = (LPCSTR)streams[i].codec_id;
            info.language = (LPCSTR)streams[i].language;
            info.label = (LPCWSTR)streams[i].name;
            info.matroska = true;
            out->push_back(info);
        }
        return S_OK;
    }

    // TS/BDMV: resolve a playlist to its stream file, exactly as the provider
    // path does, so enumeration matches what will later be rendered.
    REFERENCE_TIME in_pt = _I64_MAX, out_pt = _I64_MAX;
    CString stream_path;
    CString resolved_path(media);
    ResolveBdmvPlaylist(media, stream_path, in_pt, out_pt);
    if (!stream_path.IsEmpty())
    {
        resolved_path = stream_path;
    }

    CTsSubtitleSource src;
    if (!src.Open(resolved_path, in_pt, out_pt))
    {
        return S_FALSE;
    }
    const std::vector<TsSubtitleStream>& streams = src.GetSubtitleStreams();
    for (size_t i = 0; i < streams.size(); i++)
    {
        EmbeddedStreamInfo info;
        info.index = (WORD)(i + 1);
        info.raw_id = streams[i].pid;
        info.codec = streams[i].stream_type;
        info.codec_name = streams[i].stream_type == 0x90 ? "PGS" : "DVB";
        info.language = (LPCSTR)streams[i].language;
        info.label = (LPCWSTR)streams[i].name;
        info.matroska = false;
        out->push_back(info);
    }
    return S_OK;
}
