/*
 * Container-side subtitle source (fork addition), Matroska flavour.
 * See MatroskaSubtitleSource.h.
 */

#include "stdafx.h"
#include "MatroskaSubtitleSource.h"
#include "StdioFile64.h"
#include "hdmv_subtitle_provider.h"
#include "HdmvSub.h"
#include "DVBSub.h"
#include "RTS.h"
#include "IDirectVobSubXy.h"
#include "../subpic/XySubRenderProviderWrapper.h"
#include <atlpath.h>

// ---------------------------------------------------------------------------
// EBML element IDs (VINT, marker bit kept)
// ---------------------------------------------------------------------------
namespace MkvId
{
    const unsigned __int64 kEbml        = 0x1A45DFA3;
    const unsigned __int64 kSegment     = 0x18538067;
    const unsigned __int64 kSeekHead    = 0x114D9B74;
    const unsigned __int64 kInfo        = 0x1549A966;
    const unsigned __int64 kTracks      = 0x1654AE6B;
    const unsigned __int64 kCluster     = 0x1F43B675;
    const unsigned __int64 kCues        = 0x1C53BB6B;
    const unsigned __int64 kAttachments = 0x1941A469;
    const unsigned __int64 kChapters    = 0x1043A770;
    const unsigned __int64 kTags        = 0x1254C367;
    const unsigned __int64 kVoid        = 0xEC;

    const unsigned __int64 kSeek        = 0x4DBB;   // SeekHead child
    const unsigned __int64 kSeekID      = 0x53AB;   // Seek -> target element id
    const unsigned __int64 kSeekPos     = 0x53AC;   // Seek -> byte offset

    const unsigned __int64 kTimestampScale = 0x2AD7B1;
    const unsigned __int64 kDuration       = 0x4489;
    const unsigned __int64 kTrackEntry     = 0xAE;
    const unsigned __int64 kTrackNumber    = 0xD7;
    const unsigned __int64 kTrackType      = 0x83;
    const unsigned __int64 kCodecID        = 0x86;
    const unsigned __int64 kCodecPrivate   = 0x63A2;
    const unsigned __int64 kLanguage       = 0x22B59C;
    const unsigned __int64 kName           = 0x536E;
    const unsigned __int64 kClusterTs      = 0xE7;
    const unsigned __int64 kBlockGroup     = 0xA0;
    const unsigned __int64 kBlock          = 0xA1;
    const unsigned __int64 kSimpleBlock    = 0xA3;
}

// A container element we descend into (as opposed to a leaf we skip by size).
static bool IsContainerElement(unsigned __int64 id)
{
    switch (id)
    {
    case MkvId::kSegment: case MkvId::kSeekHead: case MkvId::kInfo:
    case MkvId::kTracks: case MkvId::kCluster: case MkvId::kCues:
    case MkvId::kAttachments: case MkvId::kChapters: case MkvId::kTags:
    case MkvId::kBlockGroup:
        return true;
    default:
        return false;
    }
}

// Length in bytes of the VINT whose first byte is p[0]; 0 on a bad prefix.
static int VintLen(BYTE b)
{
    int len = 1;
    BYTE mask = 0x80;
    while (len <= 8 && !(b & mask))
    {
        mask >>= 1;
        len++;
    }
    return len <= 8 ? len : 0;
}

// Value of a VINT of 'len' bytes. 'keep' strips the marker bit when false.
static unsigned __int64 VintValue(const BYTE* p, int len, bool keep)
{
    unsigned __int64 v = keep ? p[0] : (unsigned __int64)(p[0] & (0xFF >> len));
    for (int i = 1; i < len; i++)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

static bool IsUnknownSize(const BYTE* p, int len)
{
    // All value bits set (marker excluded) means "unknown". An 8-byte VINT
    // carries 56 value bits (7*8), not 64, so cap the mask accordingly.
    unsigned __int64 v = VintValue(p, len, false);
    unsigned __int64 all = (1ULL << (7 * len)) - 1;
    return v == all;
}

// ---------------------------------------------------------------------------
// CMatroskaSubtitleSource
// ---------------------------------------------------------------------------

CMatroskaSubtitleSource::CMatroskaSubtitleSource()
    : m_file(NULL)
    , m_open(false)
    , m_file_size(0)
    , m_pos(0)
    , m_segment_body(0)
    , m_segment_end(0)
    , m_tracks_offset(-1)
    , m_selected_track(0)
    , m_selected_kind(MKV_SUB_NONE)
    , m_duration(0)
    , m_timestamp_scale(1000000)
    , m_cluster_ts(0)
    , m_last_parsed_rt(_I64_MIN)
    , m_interrupt(0)
    , m_sink_stop(false)
{
}

CMatroskaSubtitleSource::~CMatroskaSubtitleSource()
{
    Close();
}

void CMatroskaSubtitleSource::Close()
{
    delete m_file;
    m_file = NULL;
    m_open = false;
    m_streams.clear();
    m_selected_track = 0;
    m_selected_codec.Empty();
    m_selected_kind = MKV_SUB_NONE;
    m_duration = 0;
    m_pos = m_segment_body = m_segment_end = 0;
    m_tracks_offset = -1;
    m_containers.clear();
    m_last_parsed_rt = _I64_MIN;
}

bool CMatroskaSubtitleSource::ReadAt(__int64 pos, void* dst, int n)
{
    if (!m_file || pos < 0 || pos + n > m_file_size)
    {
        return false;
    }
    try
    {
        m_file->Seek(pos, CFile::begin);
    }
    catch (CFileException* e)
    {
        e->Delete();
        return false;
    }
    BYTE* p = (BYTE*)dst;
    int got = 0;
    while (got < n)
    {
        UINT r = m_file->Read(p + got, n - got);
        if (r == 0)
        {
            return false;
        }
        got += (int)r;
    }
    return true;
}

// Reads the element header at m_pos. 'body' is the first body byte, 'end' the
// first byte after the body (-1 for an unknown-size element). m_pos is left
// untouched (the caller decides where to continue).
bool CMatroskaSubtitleSource::ReadElementHeader(unsigned __int64* id, __int64* body, __int64* end)
{
    BYTE hdr[16];
    if (!ReadAt(m_pos, hdr, 1))
    {
        return false;
    }
    int id_len = VintLen(hdr[0]);
    if (!id_len || m_pos + id_len + 1 > m_file_size)
    {
        return false;
    }
    if (!ReadAt(m_pos, hdr, id_len + 12))
    {
        // Near EOF there may be fewer than 12 bytes; read what is available.
        int avail = (int)(m_file_size - m_pos);
        if (avail <= id_len || !ReadAt(m_pos, hdr, avail))
        {
            return false;
        }
    }

    unsigned __int64 eid = VintValue(hdr, id_len, true);

    BYTE size_first = hdr[id_len];
    int size_len = VintLen(size_first);
    if (!size_len || m_pos + id_len + size_len > m_file_size)
    {
        return false;
    }

    __int64 b = m_pos + id_len + size_len;
    if (IsUnknownSize(hdr + id_len, size_len))
    {
        *id = eid;
        *body = b;
        *end = -1;
        return true;
    }
    unsigned __int64 size = VintValue(hdr + id_len, size_len, false);
    *id = eid;
    *body = b;
    *end = b + (__int64)size;
    return *end <= m_file_size;
}

bool CMatroskaSubtitleSource::ReadUnsigned(__int64 body, __int64 end, unsigned __int64* out)
{
    __int64 n = end - body;
    if (n <= 0 || n > 8)
    {
        return false;
    }
    BYTE buf[8];
    if (!ReadAt(body, buf, (int)n))
    {
        return false;
    }
    unsigned __int64 v = 0;
    for (int i = 0; i < (int)n; i++)
    {
        v = (v << 8) | buf[i];
    }
    *out = v;
    return true;
}

bool CMatroskaSubtitleSource::ReadElementBytes(__int64 body, __int64 end, std::vector<BYTE>& out)
{
    __int64 n = end - body;
    if (n <= 0 || n > (1 << 24))
    {
        return false;
    }
    out.resize((size_t)n);
    return ReadAt(body, &out[0], (int)n);
}

// Scans forward (by element size, no bodies read) for the next Cluster start,
// used to bound an unknown-size Cluster. Returns 'limit' when none is found.
static __int64 FindNextCluster(CStdioFile64* file, __int64 from, __int64 limit)
{
    __int64 p = from;
    while (p < limit)
    {
        BYTE b;
        try
        {
            file->Seek(p, CFile::begin);
            if (file->Read(&b, 1) != 1)
            {
                break;
            }
            int id_len = VintLen(b);
            if (!id_len)
            {
                break;
            }
            BYTE hdr[16];
            file->Seek(p, CFile::begin);
            if (file->Read(hdr, id_len + 12) < (UINT)(id_len + 1))
            {
                break;
            }
            unsigned __int64 eid = VintValue(hdr, id_len, true);
            int size_len = VintLen(hdr[id_len]);
            if (!size_len)
            {
                break;
            }
            __int64 body = p + id_len + size_len;
            if (eid == MkvId::kCluster)
            {
                return p;
            }
            if (IsUnknownSize(hdr + id_len, size_len))
            {
                break;
            }
            __int64 sz = (__int64)VintValue(hdr + id_len, size_len, false);
            if (sz <= 0)
            {
                break;
            }
            p = body + sz;
        }
        catch (CFileException* e)
        {
            e->Delete();
            break;
        }
    }
    return limit;
}

MatroskaSubtitleKind CMatroskaSubtitleSource::ClassifyCodec(const CStringA& codec)
{
    if (codec.CompareNoCase("S_HDMV/PGS") == 0)
    {
        return MKV_SUB_PGS;
    }
    if (codec.CompareNoCase("S_DVBSUB") == 0)
    {
        return MKV_SUB_DVB;
    }
    if (codec.CompareNoCase("S_TEXT/UTF8") == 0 ||
        codec.CompareNoCase("S_TEXT/ASS") == 0 ||
        codec.CompareNoCase("S_TEXT/SSA") == 0 ||
        codec.CompareNoCase("S_TEXT/WEBVTT") == 0 ||
        codec.CompareNoCase("S_SSA") == 0 ||
        codec.CompareNoCase("S_ASS") == 0 ||
        codec.CompareNoCase("S_TEXT/USF") == 0)
    {
        return MKV_SUB_TEXT;
    }
    return MKV_SUB_NONE;
}

bool CMatroskaSubtitleSource::ParseSeekHeadElement(__int64 body, __int64 end)
{
    // SeekHead carries a list of Seek{SeekID, SeekPosition} entries. Muxers are
    // free to place Tracks anywhere in the file (freemkv puts it *after* all
    // clusters), so a forward scan cannot find it; the SeekHead tells us where
    // it is. Record the offsets we care about.
    __int64 p = body;
    int guard = 0;
    while (p < end && guard++ < 256)
    {
        unsigned __int64 id = 0;
        __int64 eb = 0, ee = 0;
        m_pos = p;
        if (!ReadElementHeader(&id, &eb, &ee))
        {
            break;
        }
        __int64 eee = (ee < 0) ? end : ee;
        if (id == MkvId::kSeek)
        {
            unsigned __int64 target = 0;
            __int64 target_pos = -1;
            __int64 q = eb;
            while (q < eee)
            {
                unsigned __int64 cid = 0;
                __int64 cb = 0, ce = 0;
                m_pos = q;
                if (!ReadElementHeader(&cid, &cb, &ce))
                {
                    break;
                }
                __int64 cee = (ce < 0) ? eee : ce;
                if (cid == MkvId::kSeekID)
                {
                    BYTE b[8];
                    int n = (int)(cee - cb);
                    if (n > 0 && n <= 8 && ReadAt(cb, b, n))
                    {
                        int l = VintLen(b[0]);
                        if (l>0 && l<=n)
                        {
                            target = VintValue(b, l, true);
                        }
                    }
                }
                else if (cid == MkvId::kSeekPos)
                {
                    unsigned __int64 v = 0;
                    if (ReadUnsigned(cb, cee, &v))
                    {
                        target_pos = (__int64)v;
                    }
                }
                q = cee;
            }
            if (target == MkvId::kTracks && target_pos >= 0 && m_tracks_offset < 0)
            {
                m_tracks_offset = target_pos;
            }
        }
        p = eee;
    }
    return true;
}

bool CMatroskaSubtitleSource::ParseTracksElement(__int64 body, __int64 end)
{
    __int64 p = body;
    while (p < end)
    {
        m_pos = p;
        unsigned __int64 id;
        __int64 eb, ee;
        if (!ReadElementHeader(&id, &eb, &ee))
        {
            return false;
        }
        if (id == MkvId::kTrackEntry && ee > eb)
        {
            MatroskaSubtitleStream s;
            __int64 q = eb;
            while (q < ee)
            {
                m_pos = q;
                unsigned __int64 cid;
                __int64 cb, ce;
                if (!ReadElementHeader(&cid, &cb, &ce))
                {
                    break;
                }
                if (cid == MkvId::kTrackNumber)
                {
                    unsigned __int64 v = 0;
                    if (ReadUnsigned(cb, ce, &v)) s.track_number = (DWORD)v;
                }
                else if (cid == MkvId::kTrackType)
                {
                    unsigned __int64 v = 0;
                    if (ReadUnsigned(cb, ce, &v)) s.track_type = (BYTE)v;
                }
                else if (cid == MkvId::kCodecID)
                {
                    std::vector<BYTE> b;
                    if (ReadElementBytes(cb, ce, b))
                    {
                        s.codec_id = CStringA((LPCSTR)&b[0], (int)b.size());
                    }
                }
                else if (cid == MkvId::kLanguage)
                {
                    std::vector<BYTE> b;
                    if (ReadElementBytes(cb, ce, b))
                    {
                        s.language = CStringA((LPCSTR)&b[0], (int)b.size());
                    }
                }
                else if (cid == MkvId::kName)
                {
                    std::vector<BYTE> b;
                    if (ReadElementBytes(cb, ce, b))
                    {
                        CStringW w(CA2W((LPCSTR)&b[0], CP_UTF8));
                        s.name = w;
                    }
                }
                else if (cid == MkvId::kCodecPrivate)
                {
                    ReadElementBytes(cb, ce, s.codec_private);
                }
                q = ce;
            }

            s.kind = (s.track_type == 0x11) ? ClassifyCodec(s.codec_id) : MKV_SUB_NONE;
            if (s.track_type == 0x11)
            {
                if (s.name.IsEmpty())
                {
                    const TCHAR* kind_str = s.kind == MKV_SUB_PGS ? _T("PGS")
                        : s.kind == MKV_SUB_DVB ? _T("DVB") : _T("text");
                    if (s.language.IsEmpty())
                    {
                        s.name.Format(_T("Embedded MKV %s track %u"), kind_str, s.track_number);
                    }
                    else
                    {
                        s.name.Format(_T("Embedded MKV %s track %u (%S)"),
                            kind_str, s.track_number, s.language.GetString());
                    }
                }
                m_streams.push_back(s);
            }
        }
        if (ee < 0)
        {
            break;
        }
        p = ee;
    }
    return !m_streams.empty();
}

void CMatroskaSubtitleSource::ParseInfoElement(__int64 body, __int64 end)
{
    __int64 p = body;
    while (p < end)
    {
        m_pos = p;
        unsigned __int64 id;
        __int64 eb, ee;
        if (!ReadElementHeader(&id, &eb, &ee))
        {
            return;
        }
        if (id == MkvId::kTimestampScale)
        {
            unsigned __int64 v = 0;
            if (ReadUnsigned(eb, ee, &v) && v > 0)
            {
                m_timestamp_scale = v;
            }
        }
        else if (id == MkvId::kDuration)
        {
            BYTE b[8];
            __int64 n = ee - eb;
            if ((n == 4 || n == 8) && ReadAt(eb, b, (int)n))
            {
                // Matroska floats are big-endian IEEE-754; normalise the byte
                // order into host layout before reinterpreting the bits.
                double d = 0;
                if (n == 8)
                {
                    unsigned char tmp[8];
                    for (int i = 0; i < 8; i++) tmp[i] = b[7 - i];
                    memcpy(&d, tmp, 8);
                }
                else
                {
                    unsigned char tmp[4];
                    for (int i = 0; i < 4; i++) tmp[i] = b[3 - i];
                    float f = 0;
                    memcpy(&f, tmp, 4);
                    d = f;
                }
                if (d > 0)
                {
                    // Duration is in ticks; convert to 100ns.
                    m_duration = (REFERENCE_TIME)(d * (double)m_timestamp_scale / 100.0 + 0.5);
                }
            }
        }
        if (ee < 0)
        {
            return;
        }
        p = ee;
    }
}

bool CMatroskaSubtitleSource::ScanHeader()
{
    m_pos = 0;
    unsigned __int64 id;
    __int64 body, end;
    if (!ReadElementHeader(&id, &body, &end) || id != MkvId::kEbml)
    {
        XY_LOG_WARN(_T("Matroska: not an EBML file (bad header id)"));
        return false;
    }
    m_pos = body;   // header body ignored

    while (m_pos < m_file_size)
    {
        if (!ReadElementHeader(&id, &body, &end))
        {
            break;
        }
        if (id != MkvId::kSegment)
        {
            if (end < 0)
            {
                return false;
            }
            m_pos = end;
            continue;
        }
        // Found the Segment. Unknown size -> to EOF.
        m_segment_body = body;
        m_segment_end = (end < 0) ? m_file_size : end;
        break;
    }

    if (m_segment_body == 0)
    {
        XY_LOG_WARN(_T("Matroska: no Segment element found"));
        return false;
    }

    __int64 p = m_segment_body;
    int clusters_skipped = 0;
    while (p < m_segment_end)
    {
        m_pos = p;
        if (!ReadElementHeader(&id, &body, &end))
        {
            break;
        }
        __int64 ee = (end < 0) ? m_segment_end : end;
        if (id == MkvId::kTracks)
        {
            ParseTracksElement(body, ee);
        }
        else if (id == MkvId::kSeekHead)
        {
            ParseSeekHeadElement(body, ee);
        }
        else if (id == MkvId::kInfo)
        {
            ParseInfoElement(body, ee);
        }
        else if (id == MkvId::kCluster)
        {
            // Muxer ordering is not fixed: some writers (e.g. freemkv) place
            // Clusters before Tracks, or even put Tracks after every cluster.
            // Stop scanning linearly as soon as we have the tracks we need;
            // otherwise fall back to the SeekHead offset below.
            if (!m_streams.empty())
            {
                break;
            }
            ++clusters_skipped;
            if (clusters_skipped > 4096 && m_tracks_offset >= 0)
            {
                break;   // trust the SeekHead rather than walking 14 GB
            }
        }
        if (end < 0 && id != MkvId::kSegment)
        {
            break;
        }
        p = ee;
    }

    // If a forward scan did not reach Tracks (clusters may precede it, or it may
    // live at the end of the file), use the SeekHead's reported offset.
    if (m_streams.empty() && m_tracks_offset >= 0 && m_tracks_offset < m_file_size)
    {
        unsigned __int64 id = 0;
        __int64 tb = 0, te = 0;
        m_pos = m_tracks_offset;
        if (ReadElementHeader(&id, &tb, &te) && id == MkvId::kTracks)
        {
            ParseTracksElement(tb, (te < 0) ? m_segment_end : te);
        }
    }

    if (m_streams.empty())
    {
        XY_LOG_WARN(_T("Matroska: no subtitle tracks found"));
        return false;
    }
    return true;
}

bool CMatroskaSubtitleSource::Open(LPCTSTR path)
{
    Close();

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

    if (m_file_size < 16 || !ScanHeader())
    {
        Close();
        return false;
    }

    m_open = true;
    m_pos = m_segment_body;
    m_containers.clear();
    return true;
}

bool CMatroskaSubtitleSource::SelectStream(int index)
{
    if (index < 0 || index >= (int)m_streams.size())
    {
        return false;
    }
    m_selected_track = m_streams[index].track_number;
    m_selected_codec = m_streams[index].codec_id;
    m_selected_kind = m_streams[index].kind;
    return true;
}

bool CMatroskaSubtitleSource::SelectTrackNumber(DWORD track_number)
{
    for (size_t i = 0; i < m_streams.size(); i++)
    {
        if (m_streams[i].track_number == track_number)
        {
            return SelectStream((int)i);
        }
    }
    return false;
}

void CMatroskaSubtitleSource::Rewind()
{
    if (m_open)
    {
        m_pos = m_segment_body;
    }
    m_containers.clear();
    m_cluster_ts = 0;
    m_last_parsed_rt = _I64_MIN;
    m_sink_stop = false;
}

void CMatroskaSubtitleSource::HandleBlock(const BYTE* b, int len, ITsPesSink* sink)
{
    if (len < 1)
    {
        return;
    }
    int vl = VintLen(b[0]);
    if (!vl || vl >= len)
    {
        return;
    }
    unsigned __int64 track = VintValue(b, vl, false);
    if (track != m_selected_track || len < vl + 3)
    {
        return;
    }

    // Block layout after the track-number VINT: 2-byte signed relative
    // timestamp (big-endian), then 1 flags byte, then the frame data.
    int i = vl;
    if (len < i + 3)
    {
        return;
    }
    __int64 rel = (__int64)(short)((b[i] << 8) | b[i + 1]);
    BYTE flags = b[i + 2];
    i += 3;

    // Block and cluster timestamps are in TimestampScale ticks (default 1 ms,
    // but a remux may use e.g. 100000 ns = 0.1 ms), not necessarily
    // milliseconds. Convert ticks -> ns -> 100ns REFERENCE_TIME.
    __int64 block_ticks = (__int64)m_cluster_ts + rel;
    if (block_ticks < 0)
    {
        block_ticks = 0;
    }
    REFERENCE_TIME rt = (REFERENCE_TIME)((unsigned __int64)block_ticks * m_timestamp_scale / 100);
    m_last_parsed_rt = rt;

    if (!sink)
    {
        return;
    }

    BYTE lacing = (flags >> 1) & 0x03;
    if (lacing == 0)
    {
        if (i < len && sink->OnPesPayload(b + i, len - i, rt, rt + 1) == S_FALSE)
        {
            m_sink_stop = true;
        }
        return;
    }

    // Laced block: N frames concatenated. Subtitle blocks are normally not
    // laced, but decode all three schemes so a laced text track still works.
    if (i >= len)
    {
        return;
    }
    int nframes = (int)b[i] + 1;
    i++;
    std::vector<int> sizes;
    sizes.reserve(nframes);

    if (lacing == 2)   // fixed size
    {
        int remaining = len - i;
        if (nframes <= 0 || remaining < 0)
        {
            return;
        }
        int per = remaining / nframes;
        for (int k = 0; k < nframes - 1; k++)
        {
            sizes.push_back(per);
        }
        sizes.push_back(remaining - per * (nframes - 1));
    }
    else if (lacing == 1)   // Xiph
    {
        for (int k = 0; k < nframes - 1; k++)
        {
            int sz = 0;
            for (;;)
            {
                if (i >= len) return;
                BYTE v = b[i++];
                sz += v;
                if (v != 255) break;
            }
            sizes.push_back(sz);
        }
        sizes.push_back(len - i);
    }
    else   // EBML lacing (3)
    {
        if (i >= len) return;
        int vl2 = VintLen(b[i]);
        if (!vl2) return;
        int first = (int)VintValue(b + i, vl2, false);
        i += vl2;
        sizes.push_back(first);
        __int64 prev = first;
        for (int k = 1; k < nframes - 1; k++)
        {
            if (i >= len) return;
            int vl3 = VintLen(b[i]);
            if (!vl3) return;
            __int64 v = (__int64)VintValue(b + i, vl3, false);
            __int64 bias = (__int64)((1ULL << (7 * vl3 - 1)) - 1);
            i += vl3;
            prev += (v - bias);
            sizes.push_back((int)prev);
        }
        if (nframes > 1)
        {
            int consumed = 0;
            for (size_t k = 0; k < sizes.size(); k++) consumed += sizes[k];
            sizes.push_back(len - i - consumed);
        }
    }

    for (size_t k = 0; k < sizes.size(); k++)
    {
        int sz = sizes[k];
        if (sz < 0 || i + sz > len)
        {
            break;
        }
        if (sz > 0 && sink->OnPesPayload(b + i, sz, rt, rt + 1) == S_FALSE)
        {
            m_sink_stop = true;
            break;
        }
        i += sz;
    }
}

bool CMatroskaSubtitleSource::ParseRange(ITsPesSink* sink, REFERENCE_TIME stop_rt, __int64 max_bytes)
{
    if (!m_open || !m_selected_track)
    {
        return false;
    }

    m_sink_stop = false;
    __int64 start = m_pos;

    for (;;)
    {
        if (max_bytes > 0 && m_pos - start >= max_bytes)
        {
            return true;
        }
        if (InterlockedExchange(&m_interrupt, 0))
        {
            return true;
        }
        if (m_pos >= m_segment_end)
        {
            return false;   // EOF
        }

        // Leave any container we have walked past.
        while (!m_containers.empty() && m_pos >= m_containers.back())
        {
            m_containers.pop_back();
        }

        unsigned __int64 id;
        __int64 body, end;
        if (!ReadElementHeader(&id, &body, &end))
        {
            return false;
        }

        if (IsContainerElement(id))
        {
            if (end < 0)
            {
                if (id == MkvId::kSegment)
                {
                    end = m_segment_end;
                }
                else if (id == MkvId::kCluster)
                {
                    // Unknown-size cluster: bound it at the next cluster.
                    end = FindNextCluster(m_file, body, m_segment_end);
                }
                else
                {
                    // Unknown-size inner element: cannot bound it; stop.
                    return false;
                }
            }
            else if (end > m_segment_end)
            {
                end = m_segment_end;
            }
            m_containers.push_back(end);
            m_pos = body;
            continue;
        }

        if (end < 0)
        {
            return false;   // unknown size on a leaf: invalid
        }

        // Leaf. Read only the bodies we care about; otherwise skip by size.
        if (id == MkvId::kClusterTs)
        {
            unsigned __int64 v = 0;
            if (ReadUnsigned(body, end, &v))
            {
                m_cluster_ts = v;
            }
        }
        else if (id == MkvId::kSimpleBlock || id == MkvId::kBlock)
        {
            __int64 n = end - body;
            if (n > 0 && n < (1 << 22))
            {
                std::vector<BYTE> buf((size_t)n);
                if (ReadAt(body, &buf[0], (int)n))
                {
                    HandleBlock(&buf[0], (int)n, sink);
                }
            }
        }
        m_pos = end;

        if (m_sink_stop)
        {
            return true;
        }
        if (stop_rt != _I64_MAX && m_last_parsed_rt != _I64_MIN && m_last_parsed_rt > stop_rt)
        {
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// MatroskaSubtitleProvider: pull frames from a CMatroskaSubtitleSource into the
// existing CHdmvSub decoder, mirroring TsSubtitleProvider.
// ---------------------------------------------------------------------------

class MatroskaSubtitleProvider;

// Feeds CHdmvSub from the demuxed Matroska blocks (PGS tracks only).
class CMkvPesSink : public ITsPesSink
{
public:
    CMkvPesSink(CBaseSub* sub, CCritSec* lock)
        : m_sub(sub), m_lock(lock) {}

    HRESULT OnPesPayload(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
    {
        CAutoLock cAutoLock(m_lock);
        return static_cast<CHdmvSub*>(m_sub)->ParseSample((BYTE*)data, len, rtStart, rtStop);
    }

private:
    CBaseSub* m_sub;
    CCritSec* m_lock;
};

// Matroska text subtitle frames carry only the cue text; the timing lives in
// the block. Collect the frames with their timestamps and emit a valid SRT so
// the existing RTS loader can parse them.
class CMkvTextCollector : public ITsPesSink
{
public:
    struct Cue { REFERENCE_TIME start; std::string text; };
    std::vector<Cue> cues;

    HRESULT OnPesPayload(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
    {
        if (len > 0)
        {
            Cue c;
            c.start = rtStart;
            c.text.assign((const char*)data, len);
            while (!c.text.empty() && (c.text.back() == '\n' || c.text.back() == '\r' || c.text.back() == '\0'))
            {
                c.text.erase(c.text.end() - 1);
            }
            cues.push_back(c);
        }
        return S_OK;
    }

    bool WriteSRT(FILE* f) const
    {
        for (size_t i = 0; i < cues.size(); i++)
        {
            REFERENCE_TIME start = cues[i].start;
            REFERENCE_TIME stop = (i + 1 < cues.size()) ? cues[i + 1].start - 10000
                                                        : start + 20000000;   // +2 s tail
            if (stop <= start)
            {
                stop = start + 1000000;
            }
            fprintf(f, "%d\r\n%s --> %s\r\n%s\r\n\r\n",
                (int)i + 1, SrtTime(start).GetString(), SrtTime(stop).GetString(),
                cues[i].text.c_str());
        }
        return !cues.empty();
    }

private:
    static CStringA SrtTime(REFERENCE_TIME rt)
    {
        if (rt < 0) rt = 0;
        __int64 ms = rt / 10000;
        __int64 h = ms / 3600000; ms %= 3600000;
        __int64 m = ms / 60000;   ms %= 60000;
        __int64 s = ms / 1000;    ms %= 1000;
        CStringA out;
        out.Format("%02I64d:%02I64d:%02I64d,%03I64d", h, m, s, ms);
        return out;
    }
};

class __declspec(uuid("C4B0E1A7-3D2F-4C9A-9E7B-51A2D8F60C14"))
MatroskaSubtitleProvider : public CUnknown, public IXySubRenderProvider, public ISubStream, protected CAMThread
{
public:
    MatroskaSubtitleProvider(LPCTSTR path, WORD track)
        : CUnknown(NAME("MatroskaSubtitleProvider"), NULL)
        , m_path(path), m_track(track), m_pSub(NULL), m_helper(NULL), m_sink(NULL)
        , m_parsed_rt(_I64_MIN), m_req_stop(0), m_last_now(0)
        , m_started(false), m_eof(false)
    {
    }

    ~MatroskaSubtitleProvider()
    {
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
        if (!m_source.Open(m_path))
        {
            return false;
        }
        if (m_track == 0)
        {
            const std::vector<MatroskaSubtitleStream>& s = m_source.GetSubtitleStreams();
            int idx = -1;
            for (size_t i = 0; i < s.size(); i++)
            {
                if (s[i].Decodable())
                {
                    idx = (int)i;
                    break;
                }
            }
            if (idx < 0)
            {
                return false;
            }
            m_source.SelectStream(idx);
        }
        else if (!m_source.SelectTrackNumber(m_track))
        {
            return false;
        }

        m_track = (WORD)m_source.GetSelectedTrackNumber();
        m_kind = m_source.GetSelectedKind();
        CStringA codec = m_source.GetSelectedCodecId();
        m_name.Format(_T("Embedded MKV %S track %u"), codec.GetString(), m_track);

        if (m_kind != MKV_SUB_PGS)
        {
            XY_LOG_WARN(_T("Matroska: track codec '") << CString(codec) << _T("' is not supported by this provider"));
            return false;
        }

        delete m_pSub;
        delete m_helper;
        delete m_sink;
        m_pSub = DEBUG_NEW CHdmvSub();
        if (!m_pSub)
        {
            return false;
        }
        m_helper = DEBUG_NEW HdmvSubtitleProviderImpl(m_pSub);
        m_sink = DEBUG_NEW CMkvPesSink(m_pSub, &m_csCritSec);
        if (!m_helper || !m_sink)
        {
            return false;
        }
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
    enum Request
    {
        REQ_PARSE_TO   = 1,
        REQ_REWIND     = 2,
        REQ_EXIT       = 7
    };

    void AdvanceTo(REFERENCE_TIME now)
    {
        const REFERENCE_TIME kLookAhead = 5 * 10000000;   // 5 s

        bool rewind = !m_started || (now + 5000000 < m_last_now);
        if (rewind)
        {
            CAMThread::CallWorker(REQ_REWIND);
            m_started = true;
        }
        m_last_now = now;

        if (m_eof)
        {
            return;
        }
        m_req_stop = now + kLookAhead;
        if (m_req_stop <= m_parsed_rt)
        {
            return;
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
                return 0;
            }

            switch (cmd)
            {
            case REQ_REWIND:
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
    WORD m_track;
    MatroskaSubtitleKind m_kind;
    CString m_name;
    CMatroskaSubtitleSource m_source;
    CBaseSub* m_pSub;
    HdmvSubtitleProviderImpl* m_helper;
    CMkvPesSink* m_sink;
    CCritSec m_csCritSec;

    REFERENCE_TIME m_req_stop;
    REFERENCE_TIME m_parsed_rt;
    REFERENCE_TIME m_last_now;
    bool m_started;
    bool m_eof;
};

// The RTS provider needs a live critical section for its lifetime; the
// external-file path passes the filter's. A process-wide one is enough here
// (the lock only serialises a single provider's own access).
static CCritSec& MkvTextLock()
{
    static CCritSec s_lock;
    return s_lock;
}

// Builds a text provider by dumping the selected track's frames to a temp file
// and letting the existing RTS loader parse them, then wrapping the result the
// same way the external-file path does.
static HRESULT CreateMatroskaTextProvider(const CString& path, WORD track,
                                          IXySubRenderProvider** provider)
{
    CMatroskaSubtitleSource src;
    if (!src.Open(path))
    {
        return E_FAIL;
    }
    if (track == 0)
    {
        const std::vector<MatroskaSubtitleStream>& s = src.GetSubtitleStreams();
        int idx = -1;
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i].kind == MKV_SUB_TEXT) { idx = (int)i; break; }
        }
        if (idx < 0) return E_FAIL;
        src.SelectStream(idx);
    }
    else if (!src.SelectTrackNumber(track))
    {
        return E_FAIL;
    }

    TCHAR temp_dir[MAX_PATH];
    TCHAR temp_fn[MAX_PATH];
    if (!GetTempPath(MAX_PATH, temp_dir) || !GetTempFileName(temp_dir, _T("mkv"), 0, temp_fn))
    {
        return E_FAIL;
    }
    FILE* f = _tfopen(temp_fn, _T("wb"));
    if (!f)
    {
        return E_FAIL;
    }
    bool wrote = false;
    {
        CMkvTextCollector collector;
        src.ParseRange(&collector, _I64_MAX);
        wrote = collector.WriteSRT(f);
    }
    fclose(f);
    if (!wrote)
    {
        DeleteFile(temp_fn);
        return E_FAIL;
    }

    CComPtr<ISubStream> stream;
    {
        CAutoPtr<CRenderedTextSubtitle> rts(DEBUG_NEW CRenderedTextSubtitle(&MkvTextLock()));
        if (rts && rts->Open(CString(temp_fn), DEFAULT_CHARSET) && rts->GetStreamCount() > 0)
        {
            stream = rts.Detach();
        }
    }
    DeleteFile(temp_fn);
    if (!stream)
    {
        return E_FAIL;
    }

    // Wrap exactly as the external-file path (CreateTrackProviderFromFile) does.
    IXySubRenderProvider* wrapped = NULL;
    if (CComQIPtr<IXySubRenderProvider>(stream))
    {
        wrapped = CComQIPtr<IXySubRenderProvider>(stream).Detach();
    }
    else if (CComQIPtr<ISubPicProviderEx2>(stream))
    {
        wrapped = DEBUG_NEW XySubRenderProviderWrapper2(CComQIPtr<ISubPicProviderEx2>(stream), NULL);
    }
    else if (CComQIPtr<ISubPicProviderEx>(stream))
    {
        wrapped = DEBUG_NEW XySubRenderProviderWrapper(CComQIPtr<ISubPicProviderEx>(stream), NULL);
    }
    else
    {
        return S_FALSE;
    }
    if (!wrapped)
    {
        return E_OUTOFMEMORY;
    }
    *provider = wrapped;
    return S_OK;
}

HRESULT CreateMatroskaSubtitleProvider(LPCTSTR container_path, WORD track,
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

    // Text tracks reuse the RTS renderer; PGS goes through CHdmvSub below.
    {
        CMatroskaSubtitleSource probe;
        if (probe.Open(container_path))
        {
            MatroskaSubtitleKind kind = MKV_SUB_NONE;
            if (track == 0)
            {
                const std::vector<MatroskaSubtitleStream>& s = probe.GetSubtitleStreams();
                for (size_t i = 0; i < s.size(); i++)
                {
                    if (s[i].Decodable()) { kind = s[i].kind; break; }
                }
            }
            else
            {
                for (size_t i = 0; i < probe.GetSubtitleStreams().size(); i++)
                {
                    if (probe.GetSubtitleStreams()[i].track_number == track)
                    {
                        kind = probe.GetSubtitleStreams()[i].kind;
                        break;
                    }
                }
            }
            if (kind == MKV_SUB_TEXT)
            {
                return CreateMatroskaTextProvider(CString(container_path), track, provider);
            }
        }
    }

    MatroskaSubtitleProvider* p = DEBUG_NEW MatroskaSubtitleProvider(container_path, track);
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

bool IsMatroskaFile(LPCTSTR path)
{
    if (!path || !*path)
    {
        return false;
    }
    CString s(path);
    CString lower(s);
    lower.MakeLower();
    if (lower.Right(4) == _T(".mkv") || lower.Right(4) == _T(".mka") || lower.Right(4) == _T(".webm"))
    {
        return true;
    }

    // Not a Matroska extension: sniff the EBML magic so a mislabelled file is
    // still recognised.
    CStdioFile64 f;
    CFileException ex;
    if (!f.Open(path, CFile::modeRead | CFile::typeBinary | CFile::shareDenyNone, &ex))
    {
        return false;
    }
    BYTE magic[4] = {0, 0, 0, 0};
    UINT got = f.Read(magic, 4);
    f.Close();
    return got == 4 && magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3;
}
