/*
 * Container-side subtitle source (fork addition).
 *
 * Reads a Blu-ray / MPEG-TS file directly (no splitter, no FFmpeg, no new
 * dependency) and hands the payloads of one chosen subtitle elementary stream
 * to a sink. A .sup file is nothing but concatenated PES payloads, so the
 * extracted payloads feed the exact same CHdmvSub / CDVBSub decoders that the
 * external-file path already uses.
 *
 * Verified against the real disc (FATE_ZERO_BOX1_DISC1/BDMV/STREAM/00000.m2ts):
 * TSMuxer-style 192-byte packets (4-byte TP_extra_header + 188-byte TS), PAT
 * PID 0x0000, PMT PID 0x0100, PGS stream type 0x90 on PIDs 0x1200/0x1201.
 */

#pragma once

#include <afx.h>
#include <vector>
#include "XySubRenderIntf.h"

class CStdioFile64;

// One subtitle elementary stream as advertised by the PMT.
struct TsSubtitleStream
{
    BYTE     stream_type;   // 0x90 = HDMV PGS, 0x91 = DVB subtitles, ...
    WORD     pid;
    CStringA language;      // ISO 639-2 from descriptor 0x0A, empty when absent
    bool     decodable;     // this fork can render it (PGS / DVB)
    CString  name;          // human readable label

    TsSubtitleStream() : stream_type(0), pid(0), decodable(false) {}
};

// Receives one reassembled PES payload (PES header already removed). rtStart /
// rtStop are the PES PTS in 100ns REFERENCE_TIME units, already rebased to the
// container start (see CTsSubtitleSource::GetFirstPts).
class ITsPesSink
{
public:
    virtual ~ITsPesSink() {}
    virtual HRESULT OnPesPayload(const BYTE* data, int len,
                                 REFERENCE_TIME rtStart, REFERENCE_TIME rtStop) = 0;
};

class CTsSubtitleSource
{
public:
    CTsSubtitleSource();
    ~CTsSubtitleSource();

    // Opens 'path', auto-detects 188 vs 192-byte packets, validates the sync
    // byte and reads PAT/PMT to enumerate the subtitle streams.
    //
    // in_point / out_point are the playlist playback window in REFERENCE_TIME
    // (100ns) units, i.e. the clip PTS that maps to the graph timeline t=0 and
    // the clip PTS at which playback stops. Pass _I64_MAX for both when there is
    // no playlist (raw container): the timeline is then rebased onto the first
    // PTS found in the file, exactly as before.
    bool Open(LPCTSTR path, REFERENCE_TIME in_point = _I64_MAX,
              REFERENCE_TIME out_point = _I64_MAX);
    void Close();
    bool IsOpen() const { return m_open; }

    int GetPacketSize() const { return m_packet_size; }             // 188 or 192
    int GetTpExtraSize() const { return m_packet_size - 188; }      // 0 or 4

    const std::vector<TsSubtitleStream>& GetSubtitleStreams() const { return m_streams; }
    WORD GetPmtPid() const { return m_pmt_pid; }

    // Smallest PTS (raw 90kHz-based REFERENCE_TIME) seen in the program during
    // the header scan; subtract this to get the container-relative timeline the
    // renderer uses (matches what ffmpeg emits for a remuxed .sup).
    REFERENCE_TIME GetFirstPts() const { return m_first_pts; }

    // Selection by index into GetSubtitleStreams(), or by raw PID.
    bool SelectStream(int index);
    bool SelectPid(WORD pid);
    WORD GetSelectedPid() const { return m_selected_pid; }
    BYTE GetSelectedStreamType() const { return m_selected_type; }

    // Demand-driven reassembly: continues from the CURRENT file position and
    // stops as soon as a PES whose rebased PTS exceeds 'stop_rt' has been
    // delivered, or EOF / max_packets is reached. _I64_MAX parses to EOF.
    // Returns true while data may remain (so a caller can advance one
    // look-ahead step at a time instead of parsing the whole file up front);
    // when it returns early the file is repositioned at the first unprocessed
    // packet, so the next call continues without a gap.
    bool ParseRange(ITsPesSink* sink, REFERENCE_TIME stop_rt, __int64 max_packets = 0);

    // Restarts parsing from the container start and clears reassembly state;
    // used when the graph seeks backwards.
    void Rewind();

    // Rebased PTS of the last PES seen while parsing. Advances across PES-free
    // gaps (it tracks the stream, not the delivery), so a demand-driven caller
    // can tell how far the file has been parsed. _I64_MIN until anything parsed.
    REFERENCE_TIME GetLastParsedRt() const { return m_last_parsed_rt; }

    // Asks a running ParseRange() to stop and return at the next block boundary.
    void Interrupt() { m_interrupt = true; }

private:
    bool DetectPacketSize();
    bool ScanProgramInfo();

    void HandlePacket(const BYTE* ts188, ITsPesSink* sink);
    void FlushPes(ITsPesSink* sink);

    // True when a PES with PTS 'rt' falls inside the playlist playback window.
    // Always true for a raw container (no window / no in-point).
    bool IsInPlaylistWindow(REFERENCE_TIME rt) const
    {
        if (m_playlist_in == _I64_MAX)
        {
            return true;
        }
        if (rt < m_playlist_in)
        {
            return false;
        }
        if (m_playlist_out != _I64_MAX && rt >= m_playlist_out)
        {
            return false;
        }
        return true;
    }

    static bool ParsePatSection(const BYTE* sec, int len, WORD* pmt_pid);
    static bool ParsePmtSection(const BYTE* sec, int len, WORD* pcr_pid,
                                std::vector<TsSubtitleStream>& out);
    static bool IsSubtitleStreamType(BYTE t);
    static REFERENCE_TIME ParsePts(const BYTE* p);   // 5 bytes at 'p'

    CStdioFile64* m_file;
    bool          m_open;
    int           m_packet_size;
    __int64       m_file_size;

    std::vector<TsSubtitleStream> m_streams;
    WORD          m_pmt_pid;
    WORD          m_selected_pid;
    BYTE          m_selected_type;
    REFERENCE_TIME m_first_pts;

    // PES reassembly (per selected PID).
    bool             m_pes_active;
    std::vector<BYTE> m_pes;
    REFERENCE_TIME   m_last_pts;
    REFERENCE_TIME   m_last_parsed_rt;   // rebased PTS of the last PES seen

    volatile LONG     m_interrupt;
    bool              m_sink_stop;

    // Playback window (playlist in/out point) applied as a rebase offset; see
    // Open(). _I64_MAX means "derive from the stream's first PTS".
    REFERENCE_TIME    m_playlist_in;
    REFERENCE_TIME    m_playlist_out;
};

// Resolves a Blu-ray playlist path (or any path under a BDMV tree, e.g.
// "...\BDMV\PLAYLIST\00000.mpls" or the BDMV / disc root itself) to the actual
// transport stream file plus its playback window, using the existing
// CHdmvClipInfo playlist parser.
//
//   in_point / out_point are the clip PTS (100ns) that map to the graph
//   timeline t=0 and to the end of playback (m_rtIn / m_rtOut of the first
//   PlaylistItem). They are _I64_MAX / 0 when 'path' is not a playlist.
//
// Returns false when 'path' is not a Blu-ray playlist at all (raw container
// stays untouched). A single-clip playlist is fully supported; a multi-clip
// playlist resolves to its first item only (limitation noted in the .cpp).
bool ResolveBdmvPlaylist(LPCTSTR path, CString& stream_path,
                         REFERENCE_TIME& in_point, REFERENCE_TIME& out_point);

// Builds an IXySubRenderProvider that renders 'pid' from 'container_path'
// through the existing PGS/DVB decoder. pid == 0 picks the first decodable
// subtitle stream. Lives in subtitles.lib; the filter calls this.
HRESULT CreateTsSubtitleProvider(LPCTSTR container_path, WORD pid,
                                 IXySubRenderProvider** provider);
