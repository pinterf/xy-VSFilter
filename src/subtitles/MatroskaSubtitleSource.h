/*
 * Container-side subtitle source (fork addition), Matroska flavour.
 *
 * Reads a Matroska (.mkv/.mka) file directly - a hand written EBML parser, no
 * libmatroska / FFmpeg / any third party dependency - and hands the frames of
 * one chosen subtitle track to the SAME ITsPesSink the transport-stream source
 * uses. A PGS (S_HDMV/PGS) track's frames are PGS access units, exactly what a
 * .sup / the TS path feeds CHdmvSub, so the existing decoders are reused
 * unchanged. Text tracks (S_TEXT/UTF8, S_TEXT/ASS) are decoded by the existing
 * RTS text renderer.
 *
 * Structure walked: EBML header -> Segment (unknown size tolerated) ->
 * Info (TimestampScale/Duration) and Tracks/TrackEntry (number, type, codec,
 * language, name, codec private) -> Cluster (Timestamp) -> SimpleBlock /
 * BlockGroup > Block, whose frame data plus (cluster + relative) ms timestamp
 * become one delivered payload.
 *
 * Demand-driven like the TS path: ParseRange() parses forward from the current
 * position to a horizon and stops, so a 15 GB remux is never read past the play
 * position; Rewind() restarts from the Segment body. Blocks are located by
 * walking element headers and skipping bodies by size, so only the selected
 * track's frames are ever read.
 */

#pragma once

#include "TsSubtitleSource.h"   // ITsPesSink (reused, same sink kind)

class CStdioFile64;

// What kind of subtitle track this is.
enum MatroskaSubtitleKind
{
    MKV_SUB_NONE = 0,
    MKV_SUB_PGS,        // S_HDMV/PGS -> CHdmvSub
    MKV_SUB_DVB,        // S_DVBSUB   -> not rendered by this fork
    MKV_SUB_TEXT        // S_TEXT/*   -> CRenderedTextSubtitle
};

// One subtitle track as advertised by Tracks/TrackEntry.
struct MatroskaSubtitleStream
{
    DWORD      track_number;   // TrackNumber (0xD7), 1-based
    BYTE       track_type;     // TrackType (0x83); 0x11 == subtitle
    CStringA   codec_id;       // CodecID (0x86), e.g. "S_HDMV/PGS"
    CStringA   language;       // Language (0x22B59C), empty when absent
    CString    name;           // Name (0x536E) or a generated label
    std::vector<BYTE> codec_private;   // CodecPrivate (0x63A2), may be empty
    MatroskaSubtitleKind kind;

    MatroskaSubtitleStream() : track_number(0), track_type(0), kind(MKV_SUB_NONE) {}
    bool Decodable() const { return kind == MKV_SUB_PGS || kind == MKV_SUB_TEXT; }
};

class CMatroskaSubtitleSource
{
public:
    CMatroskaSubtitleSource();
    ~CMatroskaSubtitleSource();

    // Opens 'path', validates the EBML magic, walks Segment > Info + Tracks and
    // enumerates the subtitle tracks. No clusters are read here.
    bool Open(LPCTSTR path);
    void Close();
    bool IsOpen() const { return m_open; }

    const std::vector<MatroskaSubtitleStream>& GetSubtitleStreams() const { return m_streams; }

    // Selection by index into GetSubtitleStreams(), or by TrackNumber.
    bool SelectStream(int index);
    bool SelectTrackNumber(DWORD track_number);
    DWORD GetSelectedTrackNumber() const { return m_selected_track; }
    CStringA GetSelectedCodecId() const { return m_selected_codec; }
    MatroskaSubtitleKind GetSelectedKind() const { return m_selected_kind; }

    // Container duration from Info (Duration * TimestampScale), 0 when absent.
    REFERENCE_TIME GetDuration() const { return m_duration; }

    // Demand-driven: continues from the CURRENT position and stops as soon as a
    // block whose timestamp exceeds 'stop_rt' has been delivered, or EOF /
    // 'max_bytes' read. _I64_MAX parses to EOF. Returns true while data may
    // remain (caller can advance one look-ahead step at a time); when it returns
    // early the position is left at the next unprocessed element so the next
    // call continues without a gap.
    bool ParseRange(ITsPesSink* sink, REFERENCE_TIME stop_rt, __int64 max_bytes = 0);

    // Restarts parsing from the Segment body and clears block state; the track
    // list and selection are kept. Used when the graph seeks backwards.
    void Rewind();

    // Timestamp of the last delivered block in REFERENCE_TIME units. Matroska
    // timestamps are already relative to the container start, so unlike PES PTS
    // they need no rebasing. _I64_MIN until anything parsed.
    REFERENCE_TIME GetLastParsedRt() const { return m_last_parsed_rt; }

    // Asks a running ParseRange() to stop and return at the next block boundary.
    void Interrupt() { m_interrupt = true; }

private:
    // ---- EBML primitives -------------------------------------------------
    bool ReadAt(__int64 pos, void* dst, int n);
    bool ReadElementHeader(unsigned __int64* id, __int64* body, __int64* end);
    bool ReadUnsigned(__int64 body, __int64 end, unsigned __int64* out);
    bool ReadElementBytes(__int64 body, __int64 end, std::vector<BYTE>& out);

    bool ScanHeader();                 // fill m_streams / m_duration
    bool ParseTracksElement(__int64 body, __int64 end);
    bool ParseSeekHeadElement(__int64 body, __int64 end);   // records m_tracks_offset
    void ParseInfoElement(__int64 body, __int64 end);

    // Splits a Block/SimpleBlock payload into frames and delivers each of them.
    void HandleBlock(const BYTE* body, int len, ITsPesSink* sink);

    static MatroskaSubtitleKind ClassifyCodec(const CStringA& codec);

    CStdioFile64* m_file;
    bool          m_open;
    __int64       m_file_size;
    __int64       m_pos;            // next byte to read
    __int64       m_segment_body;   // Segment body offset (Rewind target)
    __int64       m_segment_end;    // Segment end or file size when unknown
    __int64       m_tracks_offset;  // Tracks offset from SeekHead, or -1

    std::vector<MatroskaSubtitleStream> m_streams;
    DWORD         m_selected_track;
    CStringA      m_selected_codec;
    MatroskaSubtitleKind m_selected_kind;
    REFERENCE_TIME m_duration;

    unsigned __int64 m_timestamp_scale;   // ns per tick, default 1000000
    unsigned __int64 m_cluster_ts;        // current cluster Timestamp (ms)

    // Container ends of the Cluster / BlockGroup elements currently open.
    std::vector<__int64> m_containers;

    REFERENCE_TIME m_last_parsed_rt;
    volatile LONG  m_interrupt;
    bool           m_sink_stop;
};

// Builds an IXySubRenderProvider that renders 'track' from the Matroska file
// 'container_path'. track == 0 picks the first decodable subtitle track.
// PGS -> CHdmvSub; text -> the existing RTS text renderer.
HRESULT CreateMatroskaSubtitleProvider(LPCTSTR container_path, WORD track,
                                       IXySubRenderProvider** provider);

// True when 'path' looks like a Matroska/EBML file (magic), so a caller can
// dispatch the embedded-stream selection to the right container source.
bool IsMatroskaFile(LPCTSTR path);
