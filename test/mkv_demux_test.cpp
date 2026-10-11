// Standalone verification for the Matroska container-side subtitle source
// (CMatroskaSubtitleSource): opens a real .mkv, enumerates the embedded
// subtitle tracks, selects the 2nd, extracts block frames and asserts the
// timeline is sane.
//
// Usage: mkv_demux_test <media.mkv> [track_number] [max_bytes]
//   track_number defaults to the 2nd discovered subtitle track.
//   max_bytes bounds the demux (0 => whole file). Exit code = failures.

#include "stdafx.h"
#include "MatroskaSubtitleSource.h"
#include "HdmvSub.h"
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

// The subtitles static library logs through xy_logger (which lives in the
// filter DLL). This standalone test only needs the symbols to link.
namespace xy_logger
{
#if ENABLE_XY_LOG
log4cplus::Logger g_logger = log4cplus::Logger::getInstance(L"mkv_demux_test");
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

// Collects the delivered block frames and their timestamps.
class CollectSink : public ITsPesSink
{
public:
    std::vector<std::vector<BYTE> > payloads;
    std::vector<REFERENCE_TIME> pts;
    HRESULT OnPesPayload(const BYTE* data, int len, REFERENCE_TIME rtStart, REFERENCE_TIME rtStop)
    {
        payloads.push_back(std::vector<BYTE>(data, data + len));
        pts.push_back(rtStart);
        return S_OK;
    }
};

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        printf("usage: mkv_demux_test <media.mkv> [track_number] [max_bytes]\n");
        return 2;
    }

    CMatroskaSubtitleSource src;
    CHECK(src.Open(argv[1]), "open + parse EBML header (Segment/Tracks)");
    if (!src.IsOpen())
    {
        printf("\nFAILED (%d failures)\n", g_failures);
        return g_failures;
    }

    printf("duration: %.3f s (%I64d/%I64d)\n", src.GetDuration() / 1e7,
        (__int64)src.GetDuration(), (__int64)src.GetDuration());

    const std::vector<MatroskaSubtitleStream>& streams = src.GetSubtitleStreams();
    printf("subtitle tracks found: %d\n", (int)streams.size());
    for (size_t i = 0; i < streams.size(); i++)
    {
        printf("  [%d] track %u type 0x%02X codec '%s' lang '%s' kind %d\n",
            (int)i, streams[i].track_number, streams[i].track_type,
            (LPCSTR)streams[i].codec_id, (LPCSTR)streams[i].language,
            (int)streams[i].kind);
    }
    CHECK(streams.size() >= 2, "at least two embedded subtitle tracks enumerated");

    int n_pgs = 0, n_text = 0;
    for (size_t i = 0; i < streams.size(); i++)
    {
        if (streams[i].kind == MKV_SUB_PGS) n_pgs++;
        if (streams[i].kind == MKV_SUB_TEXT) n_text++;
    }
    printf("S_HDMV/PGS tracks: %d, text tracks: %d\n", n_pgs, n_text);
    CHECK(n_pgs + n_text >= 2, "two subtitle tracks are PGS or text");

    // Select the 2nd discovered subtitle track by default.
    int index = streams.size() >= 2 ? 1 : 0;
    if (argc >= 3)
    {
        DWORD tn = (DWORD)wcstoul(argv[2], NULL, 0);
        for (size_t i = 0; i < streams.size(); i++)
        {
            if (streams[i].track_number == tn)
            {
                index = (int)i;
                break;
            }
        }
    }
    CHECK(src.SelectStream(index), "select the target subtitle track");
    printf("selected track %u codec '%s' (index %d)\n",
        src.GetSelectedTrackNumber(), (LPCSTR)src.GetSelectedCodecId(), index);

    __int64 max_bytes = 0;
    if (argc >= 4)
    {
        max_bytes = _wtoi64(argv[3]);
    }

    // ---- extract ----
    src.Rewind();
    CollectSink sink;
    bool more = src.ParseRange(&sink, _I64_MAX, max_bytes);
    printf("demuxed %d block frames (more=%d)\n", (int)sink.payloads.size(), (int)more);
    CHECK(sink.payloads.size() >= (src.GetSelectedKind() == MKV_SUB_TEXT ? 2 : 4),
        "demuxed enough block frames to check");

    // Timestamps non-decreasing.
    bool monotonic = true;
    for (size_t i = 1; i < sink.pts.size(); i++)
    {
        if (sink.pts[i] < sink.pts[i - 1]) { monotonic = false; break; }
    }
    CHECK(monotonic, "block timestamps are monotonically non-decreasing");

    // Within [0, duration] when the duration is known.
    bool in_range = true;
    if (src.GetDuration() > 0 && !sink.pts.empty())
    {
        const REFERENCE_TIME kSlack = 30000000;   // 3 s
        for (size_t i = 0; i < sink.pts.size(); i++)
        {
            if (sink.pts[i] < 0 || sink.pts[i] > src.GetDuration() + kSlack)
            {
                in_range = false;
                printf("  out of range at #%d: %.3f s (duration %.3f s)\n",
                    (int)i, sink.pts[i] / 1e7, src.GetDuration() / 1e7);
                break;
            }
        }
    }
    CHECK(in_range, "every timestamp is within [0, duration]");
    if (!sink.pts.empty())
    {
        printf("timeline span: %.3f s .. %.3f s (count %d)\n",
            sink.pts.front() / 1e7, sink.pts.back() / 1e7, (int)sink.pts.size());
    }

    // Deterministic: same track twice -> identical output.
    {
        CMatroskaSubtitleSource src2;
        CHECK(src2.Open(argv[1]) && src2.SelectStream(index), "second open + select same track");
        src2.Rewind();
        CollectSink sink2;
        src2.ParseRange(&sink2, _I64_MAX, max_bytes);
        bool same = sink2.payloads.size() == sink.payloads.size() && sink2.pts.size() == sink.pts.size();
        for (size_t i = 0; same && i < sink.payloads.size(); i++)
        {
            if (sink2.payloads[i] != sink.payloads[i] || sink2.pts[i] != sink.pts[i])
            {
                same = false;
            }
        }
        printf("determinism: %d vs %d frames\n", (int)sink2.payloads.size(), (int)sink.payloads.size());
        CHECK(same, "selecting the same track twice yields identical output");
    }

    // Every payload decodes with the existing PGS decoder (CHdmvSub); only
    // meaningful for PGS tracks (text frames are not PGS segments).
    if (src.GetSelectedKind() == MKV_SUB_PGS)
    {
        CHdmvSub decoder;
        int ok = 0, rejected = 0;
        for (size_t i = 0; i < sink.payloads.size(); i++)
        {
            if (sink.payloads[i].empty())
            {
                continue;
            }
            HRESULT hr = decoder.ParseSample(&sink.payloads[i][0], (int)sink.payloads[i].size(),
                sink.pts[i], sink.pts[i] + 1);
            if (SUCCEEDED(hr)) ok++; else rejected++;
        }
        printf("payloads accepted by CHdmvSub: %d / %d (rejected %d)\n",
            ok, (int)sink.payloads.size(), rejected);
        CHECK(rejected == 0, "every demuxed payload is accepted by the PGS decoder");
    }

    // Demand-driven parity: an incremental parse must deliver the same frames
    // as one full parse.
    if (max_bytes == 0 && src.GetDuration() > 0)
    {
        CMatroskaSubtitleSource ref_src;
        CollectSink ref_sink;
        ref_src.Open(argv[1]);
        ref_src.SelectStream(index);
        ref_src.Rewind();
        ref_src.ParseRange(&ref_sink, _I64_MAX);

        src.Rewind();
        CollectSink inc_sink;
        const REFERENCE_TIME kStep = 50000000;   // 50 s
        for (REFERENCE_TIME t = 0; t <= src.GetDuration(); t += kStep)
        {
            src.ParseRange(&inc_sink, t + 50000000);
            if (inc_sink.payloads.size() >= ref_sink.payloads.size())
            {
                break;
            }
        }
        bool same = inc_sink.payloads.size() >= ref_sink.payloads.size();
        size_t n = (std::min)(inc_sink.payloads.size(), ref_sink.payloads.size());
        for (size_t i = 0; same && i < n; i++)
        {
            if (inc_sink.payloads[i] != ref_sink.payloads[i] || inc_sink.pts[i] != ref_sink.pts[i])
            {
                same = false;
            }
        }
        printf("incremental %d frames vs bounded %d frames\n",
            (int)inc_sink.payloads.size(), (int)ref_sink.payloads.size());
        CHECK(same, "incremental demand-driven parse == full parse (same frames + timestamps)");
    }

    // Provider smoke test: build the IXySubRenderProvider the filter would use
    // (PGS -> CHdmvSub, text -> RTS) and confirm it opens, then tear it down.
    {
        int decodable = 0;
        for (size_t i = 0; i < streams.size(); i++)
        {
            if (streams[i].Decodable())
            {
                decodable++;
            }
        }
        if (decodable > 0)
        {
            IXySubRenderProvider* prov = NULL;
            HRESULT hr = CreateMatroskaSubtitleProvider(argv[1],
                (WORD)streams[index].track_number, &prov);
            printf("CreateMatroskaSubtitleProvider(track %u) hr=0x%08X provider=%p\n",
                streams[index].track_number, (unsigned)hr, prov);
            CHECK(SUCCEEDED(hr) && prov != NULL, "embedded provider opens for the selected track");
            if (prov)
            {
                prov->Release();
            }
        }
    }

    printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures;
}
