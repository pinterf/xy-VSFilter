// Standalone verification for the container-side subtitle source
// (CTsSubtitleSource): opens a real Blu-ray .m2ts, enumerates the embedded
// subtitle streams and compares the demuxed PES payloads of one PID against a
// reference .sup produced by `ffmpeg -map 0:N -c copy` on the same file.
//
// The comparison covers the FULL extracted window, not just the first payloads:
// every PES payload and every PRESENTATION_SEG (cue) PTS must line up with the
// reference .sup timeline. That is what catches a timing/parse regression that
// only shows up past the first cues (e.g. a cue window that never advances).
//
// Usage: ts_demux_test <media.m2ts|playlist.mpls> <reference.sup> [pid]
//   pid defaults to 0x1201. Exit code is the number of failed checks.

#include "stdafx.h"
#include "TsSubtitleSource.h"
#include "HdmvSub.h"
#include "StdioFile64.h"
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

// The subtitles static library logs through xy_logger (which lives in the
// filter DLL). This standalone test only needs the symbols to link, so
// provide a minimal definition here.
namespace xy_logger
{
#if ENABLE_XY_LOG
// Standalone test has no log4cplus appenders configured; silence the logger so
// the library's XY_LOG_* calls don't print configuration errors.
log4cplus::Logger g_logger = log4cplus::Logger::getInstance(L"ts_demux_test");
struct StubLoggerInit
{
    StubLoggerInit() { g_logger.setLogLevel(log4cplus::OFF_LOG_LEVEL); }
} g_stub_logger_init;
#endif
}

std::wostream& operator<<(std::wostream& os, const POINT& obj)
{
    return os << L"(" << obj.x << L"," << obj.y << L")";
}
std::wostream& operator<<(std::wostream& os, const SIZE& obj)
{
    return os << L"(" << obj.cx << L"," << obj.cy << L")";
}
std::wostream& operator<<(std::wostream& os, const RECT& obj)
{
    return os << L"(" << obj.left << L"," << obj.top << L"," << obj.right << L"," << obj.bottom << L")";
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            printf("FAIL: %s\n", msg);                                       \
        } else {                                                             \
            printf("ok  : %s\n", msg);                                       \
        }                                                                    \
    } while (0)

// Reference .sup segment: 13-byte PES header wrapper + segment bytes.
struct SupSeg
{
    unsigned __int64 pts;        // 90kHz
    unsigned char    seg_type;   // first byte of the PGS segment
    std::vector<BYTE> payload;   // segment header + body (what CHdmvSub parses)
};

static bool LoadSup(const wchar_t* path, std::vector<SupSeg>& out)
{
    CStdioFile64 f;
    CFileException ex;
    if (!f.Open(path, CFile::modeRead | CFile::typeBinary | CFile::shareDenyNone, &ex))
    {
        printf("cannot open sup %ls\n", path);
        return false;
    }
    ULONGLONG len = f.GetLength();
    std::vector<BYTE> buf((size_t)len);
    if (len)
    {
        f.Read(&buf[0], (UINT)len);
    }
    f.Close();

    size_t i = 0;
    while (i + 13 <= buf.size())
    {
        if (!(buf[i] == 'P' && buf[i + 1] == 'G'))
        {
            break;
        }
        SupSeg s;
        s.pts = ((unsigned __int64)buf[i + 2] << 24) | ((unsigned __int64)buf[i + 3] << 16)
              | ((unsigned __int64)buf[i + 4] << 8) | buf[i + 5];
        int size = (buf[i + 11] << 8) | buf[i + 12];
        size_t total = 13 + (size_t)size;
        if (i + total > buf.size())
        {
            break;
        }
        s.payload.assign(buf.begin() + i + 10, buf.begin() + i + total);
        s.seg_type = buf[i + 10];
        out.push_back(s);
        i += total;
    }
    return !out.empty();
}

// Collects payloads from the TS demuxer; also records the PES start PTS.
class CollectSink : public ITsPesSink
{
public:
    std::vector<std::vector<BYTE> > payloads;
    std::vector<REFERENCE_TIME> pts;
    int max_count;
    CollectSink(int n) : max_count(n) {}

    HRESULT OnPesPayload(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
    {
        if ((int)payloads.size() >= max_count)
        {
            return S_FALSE;
        }
        payloads.push_back(std::vector<BYTE>(data, data + len));
        pts.push_back(rtStart);
        return S_OK;
    }
};

int wmain(int argc, wchar_t** argv)
{
    // Enumeration-only mode: 'ts_demux_test <media> list' opens the container,
    // prints the discovered subtitle streams and exits. No reference .sup is
    // needed and nothing is demuxed, so it is safe for huge library files.
    bool list_only = (argc >= 3 && _wcsicmp(argv[2], L"list") == 0);
    if (argc < 3)
    {
        printf("usage: ts_demux_test <media.m2ts|playlist.mpls> <reference.sup|list> [pid]\n");
        return 2;
    }

    // Accept a Blu-ray playlist (or BDMV path) as well as a raw .m2ts. The
    // resolver is the exact code the filter uses on the embedded path.
    CString media_path(argv[1]);
    CString resolved;
    REFERENCE_TIME rt_in = _I64_MAX, rt_out = _I64_MAX;
    if (ResolveBdmvPlaylist(media_path, resolved, rt_in, rt_out))
    {
        printf("input is a Blu-ray playlist: '%ls'\n", (LPCWSTR)media_path);
        printf("  resolved stream: '%ls'\n", (LPCWSTR)resolved);
        printf("  in=%I64d out=%I64d (100ns) = %.3f s .. %.3f s\n",
            rt_in, rt_out, rt_in / 10000000.0, rt_out / 10000000.0);
        CHECK(!resolved.IsEmpty(), "playlist resolved to a stream (.m2ts) file");
        CHECK(rt_in != _I64_MAX, "playlist in-point read via CHdmvClipInfo::ReadPlaylist");
        media_path = resolved;
    }
    else
    {
        printf("input is not a playlist; opening as a raw container\n");
    }

    CTsSubtitleSource src;
    CHECK(src.Open(media_path, rt_in, rt_out), "open media file + auto-detect packet size");
    if (!src.IsOpen())
    {
        printf("\nFAILED (%d failures)\n", g_failures);
        return g_failures;
    }

    printf("packet size: %d (tp_extra: %d), pmt pid: 0x%04X\n",
        src.GetPacketSize(), src.GetTpExtraSize(), src.GetPmtPid());
    const std::vector<TsSubtitleStream>& streams0 = src.GetSubtitleStreams();
    printf("subtitle streams found: %d\n", (int)streams0.size());
    for (size_t i = 0; i < streams0.size(); i++)
    {
        printf("  [%d] type 0x%02X pid 0x%04X lang '%s'\n",
            (int)i, streams0[i].stream_type, streams0[i].pid,
            (LPCSTR)streams0[i].language);
    }
    CHECK(!streams0.empty(), "at least one embedded subtitle stream enumerated");

    if (list_only)
    {
        printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
        return g_failures;
    }

    WORD pid = 0x1201;
    if (argc >= 4)
    {
        pid = (WORD)wcstol(argv[3], NULL, 0);
    }
    CHECK(src.SelectPid(pid), "select the target subtitle pid");
    printf("selected pid 0x%04X (type 0x%02X), first pts %I64d\n",
        pid, src.GetSelectedStreamType(), src.GetFirstPts());

    if (rt_in != _I64_MAX)
    {
        printf("rebase offset used by the source: %I64d (playlist in-point %I64d)\n",
            src.GetFirstPts(), rt_in);
        CHECK(src.GetFirstPts() == rt_in,
            "playlist in-point is the rebase offset (PES PTS shifted to graph t=0)");
    }

    // The reference .sup covers a fixed extract window (900 s on this disc).
    // Demux exactly that window so the two timelines are comparable in full:
    // ParseRange stops once the stream has been parsed past the last reference
    // cue (plus a generous tail), which also exercises the demand-driven path.
    std::vector<SupSeg> sup;
    CHECK(LoadSup(argv[2], sup), "load reference .sup");
    printf("reference .sup segments: %d, first pts(ms) %I64u\n",
        (int)sup.size(), sup.empty() ? 0 : sup[0].pts / 90);

    REFERENCE_TIME ref_last_rt = 0;
    for (size_t i = 0; i < sup.size(); i++)
    {
        REFERENCE_TIME rt = (REFERENCE_TIME)(sup[i].pts * 10000 / 90);   // 90kHz -> 100ns
        if (rt > ref_last_rt) ref_last_rt = rt;
    }

    CollectSink sink((int)sup.size() + 4096);
    // Demand-driven parse: read only up to the reference window's end plus 1 s
    // of tail, then stop. Returns true when it stopped early (more remains).
    const __int64 kMaxPackets = (__int64)192 * 40000000;   // safety bound (~7.7 GB)
    src.Rewind();
    src.ParseRange(&sink, ref_last_rt + 10000000, kMaxPackets);
    printf("demuxed %d PES payloads (bounded to the reference window)\n",
        (int)sink.payloads.size());
    CHECK(sink.payloads.size() >= 8, "demuxed enough PES payloads to check");

    // =====================================================================
    // 1) FULL byte-level comparison: every demuxed PES must be byte-identical
    //    to the corresponding reference .sup segment, over the whole window.
    // =====================================================================
    const int compared = (int)std::min<size_t>(sink.payloads.size(), sup.size());
    int first_mismatch = -1;
    for (int i = 0; i < compared; i++)
    {
        if (sink.payloads[i] != sup[i].payload)
        {
            first_mismatch = i;
            break;
        }
    }
    printf("byte-identical payloads vs .sup: %d / %d\n",
        first_mismatch < 0 ? compared : first_mismatch, compared);
    if (first_mismatch >= 0)
    {
        printf("  first mismatch at PES #%d (ts %d bytes, sup %d bytes)\n",
            first_mismatch, (int)sink.payloads[first_mismatch].size(),
            (int)sup[first_mismatch].payload.size());
    }
    CHECK(compared > 0 && first_mismatch < 0,
        "ALL demuxed PES payloads are byte-identical to the reference .sup (full window)");

    // =====================================================================
    // 2) FULL-PTS comparison: every demuxed PES PTS, rebased to the container
    //    start, must match the reference .sup segment PTS within tolerance.
    //    A repeat-the-same-PTS bug (PES reusing the previous PTS, unbounded
    //    eager parse, or a stopped window) shows up here past the first cues.
    // =====================================================================
    const __int64 kTolUs = 50000;   // 50 ms
    int pts_mismatch = -1;
    __int64 worst_us = 0;
    if (!sup.empty() && !sink.pts.empty())
    {
        for (int i = 0; i < compared; i++)
        {
            __int64 sup_rt = (__int64)(sup[i].pts * 10000 / 90);   // 90kHz -> 100ns
            __int64 diff_us = (sink.pts[i] - sup_rt) / 10;
            if (diff_us < 0) diff_us = -diff_us;
            if (diff_us > worst_us) worst_us = diff_us;
            if (diff_us > kTolUs)
            {
                pts_mismatch = i;
                printf("  PTS mismatch at PES #%d: ts=%I64d us sup=%I64d us diff=%I64d us\n",
                    i, (__int64)(sink.pts[i] / 10), (__int64)(sup_rt / 10),
                    (__int64)((sink.pts[i] - sup_rt) / 10));
                break;
            }
        }
        printf("full-window PTS: worst |diff| = %I64d us over %d PES\n", worst_us, compared);
    }
    CHECK(pts_mismatch < 0 && compared > 0,
        "ALL demuxed PES PTS match the reference .sup timeline (full window, <=50ms)");

    // =====================================================================
    // 3) CUE timeline: the PRESENTATION_SEG (cue start) starts must match the
    //    reference .sup PTS list one-for-one. This is the user-visible timeline
    //    (what subtitle is shown when), independent of the payload bytes.
    // =====================================================================
    {
        std::vector<__int64> ts_cues;    // rebased 100ns
        for (size_t i = 0; i < sink.payloads.size(); i++)
        {
            if (!sink.payloads[i].empty() && sink.payloads[i][0] == 0x16)   // PRESENTATION_SEG
            {
                ts_cues.push_back(sink.pts[i]);
            }
        }
        std::vector<__int64> sup_cues;
        for (size_t i = 0; i < sup.size(); i++)
        {
            if (sup[i].seg_type == 0x16)
            {
                sup_cues.push_back((__int64)(sup[i].pts * 10000 / 90));
            }
        }
        printf("cue count: ts=%d sup=%d\n", (int)ts_cues.size(), (int)sup_cues.size());

        int cue_mismatch = -1;
        __int64 cue_worst_us = 0;
        size_t n = (std::min)(ts_cues.size(), sup_cues.size());
        for (size_t i = 0; i < n; i++)
        {
            __int64 d = (ts_cues[i] - sup_cues[i]) / 10;
            if (d < 0) d = -d;
            if (d > cue_worst_us) cue_worst_us = d;
            if (d > kTolUs)
            {
                cue_mismatch = (int)i;
                printf("  cue PTS mismatch at cue #%d: ts=%I64d us sup=%I64d us\n",
                    (int)i, (__int64)(ts_cues[i] / 10), (__int64)(sup_cues[i] / 10));
                break;
            }
        }
        printf("cue PTS: worst |diff| = %I64d us\n", cue_worst_us);
        // The demand-driven parse may overshoot the reference window by up to one
        // look-ahead's worth of cues; those extra cues are real data just past
        // the extract, so allow a bounded tail rather than an exact match.
        CHECK(!ts_cues.empty() && ts_cues.size() >= sup_cues.size(),
            "demuxed PRESENTATION_SEG count covers the whole reference window");
        CHECK(ts_cues.size() <= sup_cues.size() + 8,
            "demuxed PRESENTATION_SEG count does not overshoot the window materially");
        CHECK(cue_mismatch < 0,
            "every cue start PTS matches the reference .sup timeline (full window, <=50ms)");
        // Monotonic: cues must be non-decreasing; a frozen window repeats one PTS.
        bool monotonic = true;
        for (size_t i = 1; i < ts_cues.size(); i++)
        {
            if (ts_cues[i] < ts_cues[i - 1]) { monotonic = false; break; }
        }
        CHECK(monotonic, "demuxed cue start PTS are monotonically non-decreasing");
        if (!ts_cues.empty() && ts_cues.size() > 1)
        {
            printf("cue span: %.3f s .. %.3f s (count %d)\n",
                ts_cues.front() / 1e7, ts_cues.back() / 1e7, (int)ts_cues.size());
        }
    }

    // =====================================================================
    // 4) Every demuxed payload decodes with the existing PGS decoder
    //    (CHdmvSub) without a sample rejection.
    // =====================================================================
    {
        CHdmvSub decoder;
        int ok = 0;
        for (size_t i = 0; i < sink.payloads.size(); i++)
        {
            if (sink.payloads[i].empty())
            {
                continue;
            }
            HRESULT hr = decoder.ParseSample(&sink.payloads[i][0], (int)sink.payloads[i].size(),
                sink.pts[i], sink.pts[i] + 1);
            if (SUCCEEDED(hr))
            {
                ok++;
            }
        }
        printf("payloads accepted by CHdmvSub: %d / %d\n", ok, (int)sink.payloads.size());
        CHECK(ok == (int)sink.payloads.size(), "every demuxed payload is accepted by the PGS decoder");
    }

    // =====================================================================
    // 5) Demand-driven parity: advancing the source incrementally (the pattern
    //    TsSubtitleProvider uses - Rewind once, then ParseRange per requested
    //    time) must deliver exactly the same payloads/timeline as one bounded
    //    parse of the whole window. This is what proves on-demand parsing does
    //    not decorrelate a track's timeline as time advances (the user's
    //    "subtitles repeat / both tracks flip together" symptom).
    // =====================================================================
    {
        CollectSink inc_sink((int)sup.size() + 4096);
        CollectSink ref_sink((int)sup.size() + 4096);

        // One bounded parse for the reference timeline.
        src.Rewind();
        src.ParseRange(&ref_sink, ref_last_rt + 10000000, kMaxPackets);

        // Incremental parse: step the horizon steadily across the window, the
        // way a player asks for frames.
        src.Rewind();
        const REFERENCE_TIME kStep = 10000000;   // 1 s
        for (REFERENCE_TIME t = 0; t <= ref_last_rt + 10000000 && !ref_sink.payloads.empty(); t += kStep)
        {
            src.ParseRange(&inc_sink, t + 50000000, kMaxPackets);   // +5 s look-ahead
            if ((int)inc_sink.payloads.size() >= (int)ref_sink.payloads.size())
            {
                break;
            }
        }

        printf("incremental demuxed %d PES, bounded %d PES\n",
            (int)inc_sink.payloads.size(), (int)ref_sink.payloads.size());
        bool same = inc_sink.payloads.size() >= ref_sink.payloads.size();
        size_t n = (std::min)(inc_sink.payloads.size(), ref_sink.payloads.size());
        for (size_t i = 0; same && i < n; i++)
        {
            if (inc_sink.payloads[i] != ref_sink.payloads[i] || inc_sink.pts[i] != ref_sink.pts[i])
            {
                same = false;
                printf("  incremental divergence at PES #%d (ts pts=%I64d, bounded pts=%I64d)\n",
                    (int)i, (__int64)(inc_sink.pts[i] / 10), (__int64)(ref_sink.pts[i] / 10));
            }
        }
        CHECK(same, "incremental demand-driven parse == bounded parse (same payloads + PTS)");
    }

    printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures;
}
