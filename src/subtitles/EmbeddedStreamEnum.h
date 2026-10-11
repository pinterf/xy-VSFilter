/*
 * Container-side embedded subtitle enumeration (fork addition).
 *
 * Lists the subtitle streams a media container advertises, without opening a
 * provider or decoding anything: PAT/PMT for a TS/BDMV source, EBML Tracks for
 * a Matroska one. Both containers already expose this through their header scan
 * (CTsSubtitleSource::GetSubtitleStreams / CMatroskaSubtitleSource::
 * GetSubtitleStreams); this is only the dispatch that lets the filter property
 * page (and its standalone test) ask for the list in one call.
 *
 * Returns:
 *   S_OK    - *out holds the (possibly empty) discovered list.
 *   S_FALSE - the path is empty, is not a supported container, or could not be
 *             opened; *out is left empty.
 */

#pragma once

#include <vector>
#include "IMultiSubtitleTrackLoader.h"   // EmbeddedStreamInfo

HRESULT EnumerateEmbeddedStreamsFromMedia(LPCTSTR media_path,
    std::vector<EmbeddedStreamInfo>* out);
