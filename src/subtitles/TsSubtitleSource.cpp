/*
 * Container-side subtitle source (fork addition). See TsSubtitleSource.h.
 */

#include "stdafx.h"
#include "TsSubtitleSource.h"
#include "StdioFile64.h"
#include "hdmv_subtitle_provider.h"
#include "HdmvSub.h"
#include "DVBSub.h"
#include "IDirectVobSubXy.h"
#include "../DSUtil/HdmvClipInfo.h"
#include <atlpath.h>

// ---------------------------------------------------------------------------
// CTsSubtitleSource
// ---------------------------------------------------------------------------

CTsSubtitleSource::CTsSubtitleSource()
    : m_file(NULL)
    , m_open(false)
    , m_packet_size(0)
    , m_file_size(0)
    , m_pmt_pid(0)
    , m_selected_pid(0)
    , m_selected_type(0)
    , m_first_pts(_I64_MAX)
    , m_pes_active(false)
    , m_last_pts(_I64_MAX)
    , m_last_parsed_rt(_I64_MIN)
    , m_interrupt(0)
    , m_sink_stop(false)
    , m_playlist_in(_I64_MAX)
    , m_playlist_out(_I64_MAX)
{
}

CTsSubtitleSource::~CTsSubtitleSource()
{
    Close();
}

void CTsSubtitleSource::Close()
{
    delete m_file;
    m_file = NULL;
    m_open = false;
    m_streams.clear();
    m_pmt_pid = 0;
    m_selected_pid = 0;
    m_selected_type = 0;
    m_first_pts = _I64_MAX;
    m_pes.clear();
    m_pes_active = false;
    m_last_pts = _I64_MAX;
    m_last_parsed_rt = _I64_MIN;
    m_sink_stop = false;
    m_playlist_in = _I64_MAX;
    m_playlist_out = _I64_MAX;
}

bool CTsSubtitleSource::Open(LPCTSTR path, REFERENCE_TIME in_point, REFERENCE_TIME out_point)
{
    Close();
    m_playlist_in = in_point;
    m_playlist_out = out_point;

    CStdioFile64* f = DEBUG_NEW CStdioFile64();
    if (!f)
    {
        return false;
    }
    CFileException ex;
    if (!f->Open(path, CFile::modeRead | CFile::typeBinary | CFile::shareDenyNone, &ex))
    {
        delete f;
        return false;
    }

    m_file = f;
    m_file_size = (__int64)f->GetLength();

    // Restore to a clean start for the detection scan.
    f->Seek(0, CFile::begin);

    if (!DetectPacketSize())
    {
        Close();
        return false;
    }

    f->Seek(0, CFile::begin);
    if (!ScanProgramInfo())
    {
        Close();
        return false;
    }

    m_open = true;
    return true;
}

// Reads a window at the file start and decides 188 vs 192 byte packets by the
// run length of the 0x47 sync byte at the respective offset.
bool CTsSubtitleSource::DetectPacketSize()
{
    const int kProbe = 188 * 512;
    std::vector<BYTE> buf(kProbe);
    UINT got = (UINT)m_file->Read(&buf[0], kProbe);
    if (got < 188 * 8)
    {
        return false;
    }

    int sync192 = 0, sync188 = 0;
    int n192 = got / 192, n188 = got / 188;
    for (int i = 0; i < n192 && (i + 1) * 192 <= (int)got; i++)
    {
        if (buf[i * 192 + 4] == 0x47)
        {
            sync192++;
        }
    }
    for (int i = 0; i < n188 && (i + 1) * 188 <= (int)got; i++)
    {
        if (buf[i * 188] == 0x47)
        {
            sync188++;
        }
    }

    // A BDMV .m2ts has m_file_size % 192 == 0; prefer 192 when both look
    // plausible and the file length supports it.
    if (sync192 >= n192 - 1 && sync192 >= 1 &&
        (m_file_size % 192 == 0 || sync192 > sync188))
    {
        m_packet_size = 192;
    }
    else if (sync188 >= n188 - 1 && sync188 >= 1)
    {
        m_packet_size = 188;
    }
    else
    {
        return false;
    }
    return true;
}

bool CTsSubtitleSource::IsSubtitleStreamType(BYTE t)
{
    // 0x90 HDMV PGS, 0x91 DVB subtitles. 0x92 (teletext) is not renderable.
    return t == 0x90 || t == 0x91;
}

REFERENCE_TIME CTsSubtitleSource::ParsePts(const BYTE* p)
{
    // 33-bit PTS in 5 bytes (marker bits ignored).
    __int64 pts = ((__int64)((p[0] >> 1) & 0x07) << 30)
                | ((__int64)p[1] << 22)
                | ((__int64)((p[2] >> 1) & 0x7F) << 15)
                | ((__int64)p[3] << 7)
                | (__int64)(p[4] >> 1);
    return (REFERENCE_TIME)(pts * 10000 / 90);   // 90kHz -> 100ns
}

bool CTsSubtitleSource::ParsePatSection(const BYTE* sec, int len, WORD* pmt_pid)
{
    if (len < 12 || sec[0] != 0x00)
    {
        return false;
    }
    int section_length = ((sec[1] & 0x0F) << 8) | sec[2];
    int end = 3 + section_length - 4; // exclude CRC32
    if (end > len)
    {
        end = len;
    }
    for (int i = 8; i + 4 <= end; i += 4)
    {
        WORD program = (WORD)((sec[i] << 8) | sec[i + 1]);
        WORD pid = (WORD)(((sec[i + 2] & 0x1F) << 8) | sec[i + 3]);
        if (program != 0 && pid != 0)
        {
            *pmt_pid = pid;   // first program
            return true;
        }
    }
    return false;
}

bool CTsSubtitleSource::ParsePmtSection(const BYTE* sec, int len, WORD* pcr_pid,
                                        std::vector<TsSubtitleStream>& out)
{
    if (len < 16 || sec[0] != 0x02)
    {
        return false;
    }
    int section_length = ((sec[1] & 0x0F) << 8) | sec[2];
    int end = 3 + section_length - 4;
    if (end > len)
    {
        end = len;
    }

    *pcr_pid = (WORD)(((sec[8] & 0x1F) << 8) | sec[9]);
    int program_info_length = ((sec[10] & 0x0F) << 8) | sec[11];

    int i = 12 + program_info_length;
    while (i + 5 <= end)
    {
        BYTE stream_type = sec[i];
        WORD pid = (WORD)(((sec[i + 1] & 0x1F) << 8) | sec[i + 2]);
        int es_info_length = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4];

        if (IsSubtitleStreamType(stream_type))
        {
            TsSubtitleStream s;
            s.stream_type = stream_type;
            s.pid = pid;
            s.decodable = true;

            // Walk ES descriptors: 0x0A = ISO 639 language.
            int d = i + 5;
            int d_end = i + 5 + es_info_length;
            if (d_end > end)
            {
                d_end = end;
            }
            while (d + 2 <= d_end)
            {
                BYTE tag = sec[d];
                BYTE dlen = sec[d + 1];
                if (tag == 0x0A && dlen >= 3)
                {
                    s.language = CStringA((LPCSTR)&sec[d + 2], 3);
                }
                d += 2 + dlen;
            }

            if (s.language.IsEmpty())
            {
                s.name.Format(_T("Embedded %s pid 0x%04X"),
                    stream_type == 0x90 ? _T("PGS") : _T("DVB"), pid);
            }
            else
            {
                s.name.Format(_T("Embedded %s pid 0x%04X (%S)"),
                    stream_type == 0x90 ? _T("PGS") : _T("DVB"), pid,
                    s.language.GetString());
            }
            out.push_back(s);
        }

        i += 5 + es_info_length;
    }
    return !out.empty();
}

// Sequential scan for PAT (PID 0) -> PMT (PID 0x0100 typically). Also records
// the smallest PTS seen in the program window so payloads can be rebased the
// same way ffmpeg rebases a remuxed .sup (relative to the container start).
bool CTsSubtitleSource::ScanProgramInfo()
{
    const int kWindow = 48 * 1024 * 1024;   // enough to cover PAT/PMT + first PTS
    const int kBlock = m_packet_size * 2000;

    std::vector<BYTE> blk(kBlock);
    std::vector<BYTE> pat_section;
    std::vector<BYTE> pmt_section;
    bool have_pmt_pid = false;
    __int64 read_total = 0;

    while (read_total < kWindow)
    {
        UINT got = (UINT)m_file->Read(&blk[0], kBlock);
        if (got < (UINT)m_packet_size)
        {
            break;
        }
        read_total += got;

        for (UINT off = 0; off + m_packet_size <= got; off += m_packet_size)
        {
            const BYTE* ts = &blk[off + (m_packet_size - 188)];
            if (ts[0] != 0x47)
            {
                continue;
            }
            WORD pid = (WORD)(((ts[1] & 0x1F) << 8) | ts[2]);
            bool pusi = (ts[1] & 0x40) != 0;
            bool has_payload = (ts[3] & 0x10) != 0;
            bool has_af = (ts[3] & 0x20) != 0;
            int idx = 4;
            if (has_af)
            {
                idx = 5 + ts[4];
            }
            if (!has_payload || idx >= 188)
            {
                continue;
            }
            const BYTE* payload = ts + idx;
            int paylen = 188 - idx;

            // Track the smallest PTS in the program for rebasing.
            if (pusi && paylen >= 14 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1 &&
                (payload[7] & 0x80))
            {
                REFERENCE_TIME pts = ParsePts(payload + 9);
                if (pts < m_first_pts)
                {
                    m_first_pts = pts;
                }
            }

            // Section assembly on the PAT / PMT pids.
            if (pid == 0x0000 || (have_pmt_pid && pid == m_pmt_pid))
            {
                std::vector<BYTE>& sec = (pid == 0x0000) ? pat_section : pmt_section;
                const BYTE* p = payload;
                int n = paylen;
                if (pusi)
                {
                    sec.clear();
                    if (n >= 1)
                    {
                        int ptr = p[0];
                        p += 1 + ptr;
                        n -= 1 + ptr;
                    }
                }
                if (n > 0)
                {
                    sec.insert(sec.end(), p, p + n);
                }
                if (sec.size() >= 3)
                {
                    int need = 3 + (((sec[1] & 0x0F) << 8) | sec[2]);
                    if ((int)sec.size() >= need)
                    {
                        if (pid == 0x0000 && !have_pmt_pid)
                        {
                            WORD pmt = 0;
                            if (ParsePatSection(&sec[0], need, &pmt))
                            {
                                m_pmt_pid = pmt;
                                have_pmt_pid = true;
                            }
                        }
                        else if (have_pmt_pid && pid == m_pmt_pid && m_streams.empty())
                        {
                            WORD pcr = 0;
                            ParsePmtSection(&sec[0], need, &pcr, m_streams);
                        }
                    }
                }
            }
        }

        if (have_pmt_pid && !m_streams.empty())
        {
            break;
        }
    }

    if (m_playlist_in != _I64_MAX)
    {
        // Playlist playback window: the clip PTS m_rtIn maps to graph t=0, so
        // rebase on the in-point instead of the stream's own first PTS. (The
        // graph's own timeline for a playlist also starts at t=0, so subtitle
        // and video line up.)
        m_first_pts = m_playlist_in;
    }
    else if (m_first_pts == _I64_MAX)
    {
        m_first_pts = 0;
    }
    return !m_streams.empty();
}

bool CTsSubtitleSource::SelectStream(int index)
{
    if (index < 0 || index >= (int)m_streams.size())
    {
        return false;
    }
    m_selected_pid = m_streams[index].pid;
    m_selected_type = m_streams[index].stream_type;
    m_pes.clear();
    m_pes_active = false;
    return true;
}

bool CTsSubtitleSource::SelectPid(WORD pid)
{
    for (size_t i = 0; i < m_streams.size(); i++)
    {
        if (m_streams[i].pid == pid)
        {
            return SelectStream((int)i);
        }
    }
    return false;
}

void CTsSubtitleSource::FlushPes(ITsPesSink* sink)
{
    if (!m_pes_active || m_pes.size() < 9)
    {
        m_pes.clear();
        m_pes_active = false;
        return;
    }

    const BYTE* p = &m_pes[0];
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 1))
    {
        m_pes.clear();
        m_pes_active = false;
        return;
    }

    int hlen = 9 + p[8];
    if (hlen > (int)m_pes.size())
    {
        m_pes.clear();
        m_pes_active = false;
        return;
    }

    REFERENCE_TIME rt = _I64_MAX;
    if (p[7] & 0x80 && hlen >= 14)
    {
        rt = ParsePts(p + 9);
        m_last_pts = rt;
    }
    else
    {
        // No PTS on this PES: reuse the previous one (PGS continuity).
        rt = m_last_pts;
    }

    if (rt != _I64_MAX)
    {
        // Track how far the file has been parsed (even for PES that are dropped
        // below), so a demand-driven caller can advance its look-ahead.
        REFERENCE_TIME stable_rt = rt - m_first_pts;
        if (stable_rt < 0)
        {
            stable_rt = 0;
        }
        m_last_parsed_rt = stable_rt;
    }

    if (rt != _I64_MAX && sink)
    {
        // Playlist window: skip PES that lie wholly outside [in, out). A PGS
        // .sup extracted with ffmpeg keeps only this window, so dropping the
        // out-of-window PES reproduces the reference timeline exactly.
        if (!IsInPlaylistWindow(rt))
        {
            m_pes.clear();
            m_pes_active = false;
            return;
        }
        REFERENCE_TIME stable = rt - m_first_pts;
        if (stable < 0)
        {
            stable = 0;
        }
        if (sink->OnPesPayload(p + hlen, (int)m_pes.size() - hlen, stable, stable + 1) == S_FALSE)
        {
            m_sink_stop = true;   // consumer has enough
        }
    }

    m_pes.clear();
    m_pes_active = false;
}

void CTsSubtitleSource::HandlePacket(const BYTE* ts, ITsPesSink* sink)
{
    if (ts[0] != 0x47)
    {
        return;
    }
    WORD pid = (WORD)(((ts[1] & 0x1F) << 8) | ts[2]);
    if (pid != m_selected_pid)
    {
        return;
    }

    bool pusi = (ts[1] & 0x40) != 0;
    bool has_payload = (ts[3] & 0x10) != 0;
    bool has_af = (ts[3] & 0x20) != 0;
    int idx = 4;
    if (has_af)
    {
        idx = 5 + ts[4];
    }
    if (!has_payload || idx >= 188)
    {
        return;
    }

    if (pusi)
    {
        // A new PES starts: finish the previous one.
        FlushPes(sink);
        m_pes.assign(ts + idx, ts + 188);
        m_pes_active = true;
    }
    else if (m_pes_active)
    {
        m_pes.insert(m_pes.end(), ts + idx, ts + 188);
    }

    // If the PES header declares a length, flush as soon as it is complete so
    // payloads stream out promptly instead of waiting for the next PUSI.
    if (m_pes_active && m_pes.size() >= 6)
    {
        int packet_length = (m_pes[4] << 8) | m_pes[5];
        if (packet_length > 0)
        {
            int total = 6 + packet_length;
            if ((int)m_pes.size() >= total)
            {
                m_pes.resize(total);
                FlushPes(sink);
            }
        }
    }
}

void CTsSubtitleSource::Rewind()
{
    // Restart the parse from the container start and clear reassembly state.
    if (m_open)
    {
        m_file->Seek(0, CFile::begin);
    }
    m_pes.clear();
    m_pes_active = false;
    m_last_pts = _I64_MAX;
    m_last_parsed_rt = _I64_MIN;
    m_sink_stop = false;
}

bool CTsSubtitleSource::ParseRange(ITsPesSink* sink, REFERENCE_TIME stop_rt, __int64 max_packets)
{
    if (!m_open || !m_selected_pid)
    {
        return false;
    }

    // Continues from the current file position; the caller controls seeking
    // (Rewind() for the container start). state (m_pes / m_last_pts) is carried
    // across calls so PES split over a block boundary survive.
    m_sink_stop = false;

    const int kBlock = m_packet_size * 2000;
    std::vector<BYTE> blk(kBlock);
    __int64 packets = 0;

    for (;;)
    {
        // Resume exactly where the previous call stopped. Reading a block then
        // returning early would otherwise discard the unprocessed tail and the
        // next call would read past it (losing PES).
        __int64 block_start = (__int64)m_file->GetPosition();
        UINT got = (UINT)m_file->Read(&blk[0], kBlock);
        if (got < (UINT)m_packet_size)
        {
            FlushPes(sink);
            return false;   // EOF
        }

        bool stop = false;
        UINT off = 0;
        for (; off + m_packet_size <= got; off += m_packet_size)
        {
            HandlePacket(&blk[off + (m_packet_size - 188)], sink);
            packets++;
            if (m_sink_stop)
            {
                stop = true;
                off += m_packet_size;
                break;
            }
            // Demand-driven stop: once the stream has been parsed past the
            // requested window there is nothing more to decode for 'now'.
            if (stop_rt != _I64_MAX && m_last_parsed_rt != _I64_MIN &&
                m_last_parsed_rt > stop_rt)
            {
                stop = true;
                off += m_packet_size;
                break;
            }
            if (max_packets > 0 && packets >= max_packets)
            {
                stop = true;
                off += m_packet_size;
                break;
            }
        }

        if (stop)
        {
            // Reposition the file to the first unprocessed packet so the next
            // ParseRange continues without a gap.
            __int64 consumed = block_start + (__int64)off;
            if (consumed < m_file_size)
            {
                m_file->Seek(consumed, CFile::begin);
            }
            return true;
        }

        if (InterlockedExchange(&m_interrupt, 0))
        {
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// TsSubtitleProvider: pull payloads from a CTsSubtitleSource into the existing
// CHdmvSub / CDVBSub decoder, mirroring HdmvSubtitleProviderImpl usage.
// ---------------------------------------------------------------------------

// Minimal IMediaSample carrying reassembled PES payload + timestamps. DVBSub's
// IMediaSample-based ParseSample needs this; the PGS byte API is used directly.
class CTsMediaSample : public CUnknown, public IMediaSample
{
public:
    CTsMediaSample(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
        : CUnknown(NAME("CTsMediaSample"), NULL)
        , m_data(data, data + len)
        , m_start(rtStart), m_stop(rtStop)
    {
    }

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        if (riid == __uuidof(IMediaSample))
        {
            return GetInterface((IMediaSample*)this, ppv);
        }
        return __super::NonDelegatingQueryInterface(riid, ppv);
    }

    STDMETHODIMP GetPointer(BYTE** ppBuffer) { if (!ppBuffer) return E_POINTER; *ppBuffer = &m_data[0]; return S_OK; }
    STDMETHODIMP_(LONG) GetSize(void) { return (LONG)m_data.size(); }
    STDMETHODIMP GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd)
    {
        if (!pTimeStart || !pTimeEnd) return E_POINTER;
        *pTimeStart = m_start; *pTimeEnd = m_stop; return S_OK;
    }
    STDMETHODIMP SetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd)
    {
        if (pTimeStart) m_start = *pTimeStart;
        if (pTimeEnd) m_stop = *pTimeEnd;
        return S_OK;
    }
    STDMETHODIMP IsSyncPoint(void) { return S_OK; }
    STDMETHODIMP SetSyncPoint(BOOL) { return S_OK; }
    STDMETHODIMP IsPreroll(void) { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) { return S_OK; }
    STDMETHODIMP_(LONG) GetActualDataLength(void) { return (LONG)m_data.size(); }
    STDMETHODIMP SetActualDataLength(LONG) { return S_OK; }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** ppMediaType) { if (ppMediaType) *ppMediaType = NULL; return S_OK; }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*) { return S_OK; }
    STDMETHODIMP IsDiscontinuity(void) { return S_FALSE; }
    STDMETHODIMP SetDiscontinuity(BOOL) { return S_OK; }
    STDMETHODIMP GetMediaTime(LONGLONG*, LONGLONG*) { return S_OK; }
    STDMETHODIMP SetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd)
    {
        if (pTimeStart) m_start = *pTimeStart;
        if (pTimeEnd) m_stop = *pTimeEnd;
        return S_OK;
    }

private:
    std::vector<BYTE> m_data;
    REFERENCE_TIME m_start, m_stop;
};

class TsSubtitleProvider;

// Feeds the CHdmvSub / CDVBSub decoder from the container source.
class CTsPesSink : public ITsPesSink
{
public:
    CTsPesSink(CBaseSub* sub, CCritSec* lock, bool is_hdmv)
        : m_sub(sub), m_lock(lock), m_is_hdmv(is_hdmv) {}

    HRESULT OnPesPayload(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
    {
        CAutoLock cAutoLock(m_lock);
        if (m_is_hdmv)
        {
            CHdmvSub* hdmv = static_cast<CHdmvSub*>(m_sub);
            return hdmv->ParseSample((BYTE*)data, len, rtStart, rtStop);
        }
        // DVB: the decoder wants the PES payload via IMediaSample with the time.
        CComPtr<CTsMediaSample> sample = DEBUG_NEW CTsMediaSample(data, len, rtStart, rtStop);
        if (!sample) return E_OUTOFMEMORY;
        return m_sub->ParseSample(sample);
    }

private:
    CBaseSub* m_sub;
    CCritSec* m_lock;
    bool m_is_hdmv;
};

class __declspec(uuid("C4B0E1A7-3D2F-4C9A-9E7B-51A2D8F60C13"))
TsSubtitleProvider : public CUnknown, public IXySubRenderProvider, public ISubStream, protected CAMThread
{
public:
    TsSubtitleProvider(LPCTSTR path, WORD pid)
        : CUnknown(NAME("TsSubtitleProvider"), NULL)
        , m_path(path), m_pid(pid), m_pSub(NULL), m_helper(NULL), m_sink(NULL)
        , m_playlist_in(_I64_MAX), m_playlist_out(_I64_MAX)
        , m_parsed_rt(_I64_MIN)   // nothing parsed yet
        , m_req_stop(0)
        , m_last_now(0)
        , m_started(false)
        , m_eof(false)
    {
    }

    ~TsSubtitleProvider()
    {
        // Unblock the worker first, then tear down what it may touch. The worker
        // only ever waits in GetRequest()/CallWorker, so closing the request
        // event lets it exit before the decoder/sink are destroyed.
        if (CAMThread::ThreadExists())
        {
            CAMThread::CallWorker(REQ_EXIT);
        }
        CAMThread::Close();
        delete m_sink;
        delete m_pSub;
        delete m_helper;
    }

    bool Open()
    {
        // A Blu-ray playlist is not a transport stream: resolve it to the real
        // .m2ts plus the playback window before opening. Raw containers are
        // unaffected (ResolveBdmvPlaylist returns false and m_path is used).
        CString stream_path;
        if (ResolveBdmvPlaylist(m_path, stream_path, m_playlist_in, m_playlist_out))
        {
            m_path = stream_path;
        }
        else
        {
            m_playlist_in = _I64_MAX;
            m_playlist_out = _I64_MAX;
        }

        if (!m_source.Open(m_path, m_playlist_in, m_playlist_out))
        {
            return false;
        }
        if (m_pid == 0)
        {
            const std::vector<TsSubtitleStream>& s = m_source.GetSubtitleStreams();
            if (s.empty())
            {
                return false;
            }
            m_source.SelectStream(0);
        }
        else if (!m_source.SelectPid(m_pid))
        {
            return false;
        }
        delete m_pSub;
        delete m_helper;
        delete m_sink;
        m_pSub = (m_source.GetSelectedStreamType() == 0x90)
            ? (CBaseSub*)DEBUG_NEW CHdmvSub()
            : (CBaseSub*)DEBUG_NEW CDVBSub();
        bool is_hdmv = (m_source.GetSelectedStreamType() == 0x90);
        m_pid = m_source.GetSelectedPid();
        if (!m_pSub)
        {
            return false;
        }
        m_helper = DEBUG_NEW HdmvSubtitleProviderImpl(m_pSub);
        m_sink = DEBUG_NEW CTsPesSink(m_pSub, &m_csCritSec, is_hdmv);
        if (!m_helper || !m_sink)
        {
            return false;
        }

        m_name.Format(_T("Embedded TS pid 0x%04X"), m_pid);
        // The worker only serves parse-on-demand requests; it no longer parses
        // the whole file eagerly at construction.
        return CAMThread::Create() == TRUE;
    }

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        CheckPointer(ppv, E_POINTER);
        *ppv = NULL;
        return
            QI(IPersist)
            QI(ISubStream)
            QI(IXySubRenderProvider)
            __super::NonDelegatingQueryInterface(riid, ppv);
    }

    // IXySubRenderProvider
    STDMETHODIMP Connect(IXyOptions* consumer) { CAutoLock l(&m_csCritSec); return m_helper->Connect(consumer); }

    STDMETHODIMP RequestFrame(IXySubRenderFrame** f, REFERENCE_TIME now)
    {
        // Pull the decoder forward far enough that every cue at 'now' is known,
        // then ask it for the frame. Parsing is demand-driven: only the slice up
        // to the look-ahead horizon is demuxed, and it happens on the worker
        // thread so this call never blocks on slow file I/O.
        AdvanceTo(now);
        CAutoLock l(&m_csCritSec);
        return m_helper->RequestFrame(f, now);
    }

    STDMETHODIMP Invalidate(REFERENCE_TIME rt = -1) { CAutoLock l(&m_csCritSec); return m_helper->Invalidate(rt); }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* p) { return p ? *p = __uuidof(this), S_OK : E_POINTER; }

    // ISubStream
    STDMETHODIMP_(int) GetStreamCount() { return 1; }
    STDMETHODIMP GetStreamInfo(int i, WCHAR** ppName, LCID* pLCID)
    {
        if (i != 0) return E_INVALIDARG;
        if (ppName)
        {
            *ppName = (WCHAR*)CoTaskMemAlloc((m_name.GetLength() + 1) * sizeof(WCHAR));
            if (!*ppName) return E_OUTOFMEMORY;
            wcscpy_s(*ppName, m_name.GetLength() + 1, CStringW(m_name));
        }
        if (pLCID) *pLCID = 0;
        return S_OK;
    }
    STDMETHODIMP_(int) GetStream() { return 0; }
    STDMETHODIMP SetStream(int i) { return i == 0 ? S_OK : E_FAIL; }
    STDMETHODIMP Reload() { return S_OK; }

    HRESULT SetYuvType(CBaseSub::ColorType colorType, CBaseSub::YuvRangeType yuvRangeType)
    {
        CAutoLock l(&m_csCritSec);
        Invalidate(-1);
        return m_pSub ? m_pSub->SetYuvType(colorType, yuvRangeType) : E_FAIL;
    }

protected:
    // Worker requests. All file access and decoder feeding happens here, so the
    // render thread never touches the source directly.
    enum Request
    {
        REQ_PARSE_TO   = 1,   // param in m_req_stop (rebased 100ns PTS horizon)
        REQ_REWIND     = 2,
        REQ_EXIT       = 7
    };

    // Parse forward until the requested horizon is covered.
    void AdvanceTo(REFERENCE_TIME now)
    {
        const REFERENCE_TIME kLookAhead = 5 * 10000000;   // 5 s

        // Position at the container start exactly once. Parsing then always
        // proceeds forward, so the parsed region is [start .. m_parsed_rt] and a
        // forward seek into unparsed territory triggers more reading, so a slow
        // 15 GB clip is never parsed past the play position.
        //
        // A backward seek (>50 ms earlier than the previous request) is the one
        // case that must go back to the start: the decoder prunes the segments
        // it has passed, so the earlier timeline has to be re-demuxed. Normal
        // playback is monotonically increasing, so this only fires on a seek.
        bool rewind = !m_started || (now + 5000000 < m_last_now);
        if (rewind)
        {
            CAMThread::CallWorker(REQ_REWIND);
            m_started = true;
        }
        m_last_now = now;

        if (m_eof)
        {
            return;   // nothing left to read
        }
        m_req_stop = now + kLookAhead;
        if (m_req_stop <= m_parsed_rt)
        {
            return;   // horizon already covered
        }
        CAMThread::CallWorker(REQ_PARSE_TO);
    }

    DWORD ThreadProc()
    {
        for (;;)
        {
            DWORD cmd = CAMThread::GetRequest();
            if (cmd == REQ_EXIT)
            {
                CAMThread::Reply(0);
                return 0;   // Reply before returning: CallWorker() is waiting
            }

            switch (cmd)
            {
            case REQ_REWIND:
                // Restart the source AND the decoder: after a seek backwards the
                // old presentation segments must not be mixed with the new parse.
                m_source.Rewind();
                {
                    CAutoLock l(&m_csCritSec);
                    m_pSub->Reset();
                }
                m_parsed_rt = _I64_MIN;
                m_eof = false;
                break;

            case REQ_PARSE_TO:
            {
                REFERENCE_TIME stop = m_req_stop;
                bool more = m_source.ParseRange(m_sink, stop);
                m_parsed_rt = m_source.GetLastParsedRt();
                if (!more)
                {
                    m_eof = true;
                    // Match the previous eager behaviour: signal end of stream
                    // once the file has been fully demuxed.
                    CAutoLock l(&m_csCritSec);
                    m_pSub->EndOfStream();
                }
                break;
            }

            default:
                break;
            }
            CAMThread::Reply(0);
        }
    }

private:
    CString m_path;
    WORD m_pid;
    CString m_name;
    REFERENCE_TIME m_playlist_in;
    REFERENCE_TIME m_playlist_out;
    CTsSubtitleSource m_source;
    CBaseSub* m_pSub;
    HdmvSubtitleProviderImpl* m_helper;
    CTsPesSink* m_sink;
    CCritSec m_csCritSec;

    // Demand-driven state. m_req_stop is published by the render thread right
    // before CallWorker(); m_parsed_rt / m_eof are published by the worker and
    // read on the render thread only after CallWorker() has returned (it blocks
    // on the worker's Reply), so the event handshake orders every access.
    REFERENCE_TIME m_req_stop;
    REFERENCE_TIME m_parsed_rt;
    REFERENCE_TIME m_last_now;   // previous requested time (backward-seek detect)
    bool m_started;
    bool m_eof;
};

HRESULT CreateTsSubtitleProvider(LPCTSTR container_path, WORD pid,
                                 IXySubRenderProvider** provider)
{
    if (!provider)
    {
        return E_POINTER;
    }
    *provider = NULL;
    if (!container_path || !*container_path)
    {
        return E_INVALIDARG;
    }

    TsSubtitleProvider* p = DEBUG_NEW TsSubtitleProvider(container_path, pid);
    if (!p)
    {
        return E_OUTOFMEMORY;
    }
    if (!p->Open())
    {
        p->Release();
        return E_FAIL;
    }
    *provider = p;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Blu-ray playlist resolution
// ---------------------------------------------------------------------------
//
// The filter's graph reports a BDMV playlist ("...\BDMV\PLAYLIST\00000.mpls")
// as the media file, not a transport stream. This resolves that to the real
// .m2ts using the EXISTING CHdmvClipInfo parser and returns the playback
// window so the subtitle timeline can be rebased onto the graph timeline.
bool ResolveBdmvPlaylist(LPCTSTR path, CString& stream_path,
                         REFERENCE_TIME& in_point, REFERENCE_TIME& out_point)
{
    stream_path.Empty();
    in_point = _I64_MAX;
    out_point = _I64_MAX;

    if (!path || !*path)
    {
        return false;
    }

    CString strPath(path);
    CString strLower(strPath);
    strLower.MakeLower();

    CString strPlaylist;

    if (strLower.Right(5) == _T(".mpls"))
    {
        // A playlist file was handed to us directly (this is what the filter
        // reports for a BDMV folder).
        strPlaylist = strPath;
    }
    else if (strLower.Find(_T("bdmv")) >= 0 &&
             strLower.Right(5) != _T(".m2ts") && strLower.Right(4) != _T(".mts"))
    {
        // Any other BDMV path (the BDMV folder or the disc root): let
        // CHdmvClipInfo pick the main-movie playlist, the same way the rest of
        // the app does. Raw transport streams are excluded above so that
        // opening a .m2ts directly keeps the existing, unoffset behaviour.
        // FindMainMovie appends "\BDMV\" itself, so it wants the disc root;
        // trim the path at the "\BDMV" component when one is present.
        CString strRoot = strPath;
        int nBdmv = strLower.Find(_T("\\bdmv"));
        if (nBdmv >= 0)
        {
            strRoot = strPath.Left(nBdmv);
        }
        if (strRoot.IsEmpty())
        {
            return false;
        }

        CAtlList<CHdmvClipInfo::PlaylistItem> main_playlist;
        CAtlList<CHdmvClipInfo::PlaylistItem> all_playlists;
        CHdmvClipInfo clip_info;
        if (FAILED(clip_info.FindMainMovie(strRoot, strPlaylist, main_playlist, all_playlists)) ||
            strPlaylist.IsEmpty())
        {
            return false;
        }
    }
    else
    {
        // Not a Blu-ray playlist: the raw-container path stays untouched.
        return false;
    }

    REFERENCE_TIME rt_duration = 0;
    CAtlList<CHdmvClipInfo::PlaylistItem> playlist;
    CHdmvClipInfo parser;
    HRESULT hr = parser.ReadPlaylist(strPlaylist, rt_duration, playlist);
    if (FAILED(hr) || playlist.IsEmpty())
    {
        XY_LOG_WARN(_T("ResolveBdmvPlaylist: ReadPlaylist failed for '") << strPlaylist
            << _T("' hr=") << hr);
        return false;
    }

    // m_strFileName is already "<BDMV dir>\STREAM\<clip>.M2TS" (ReadPlaylist
    // builds it from the playlist folder), i.e. the "..\STREAM\<name>" layout.
    // Several playlist items mean a multi-clip playlist (seamless branching /
    // playlist chaining); only the first clip is opened here - the offset is
    // relative to it, so clips after the first are not represented.
    const CHdmvClipInfo::PlaylistItem& item = playlist.GetHead();
    stream_path = item.m_strFileName;
    in_point = item.m_rtIn;
    out_point = item.m_rtOut;

    XY_LOG_INFO(_T("ResolveBdmvPlaylist: '") << strPlaylist << _T("' -> '") << stream_path
        << _T("' in=") << in_point << _T(" out=") << out_point
        << _T(" (items=") << (int)playlist.GetCount() << _T(")"));
    return true;
}

