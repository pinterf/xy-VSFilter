// Standalone verification for embedded subtitle-stream enumeration
// (EnumerateEmbeddedStreamsFromMedia): opens a real TS/BDMV playlist or a
// Matroska file, lists the embedded subtitle streams and prints index, raw id
// (PID or TrackNumber), codec and language. This is the exact path the Dual
// Subtitles property page's pickable list uses.
//
// Usage: embedded_enum_test <media.mpls|media.m2ts|media.mkv> [more...]
// Exit code = number of failed checks. Each input must yield 2 PGS streams.

#include "stdafx.h"
#include "EmbeddedStreamEnum.h"
#include "EmbeddedStreamPicker.h"
#include <stdio.h>
#include <vector>
#include <string>

// The subtitles static library logs through xy_logger (which lives in the
// filter DLL). This standalone test only needs the symbols to link.
namespace xy_logger
{
#if ENABLE_XY_LOG
log4cplus::Logger g_logger = log4cplus::Logger::getInstance(L"embedded_enum_test");
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

static int EnumerateOne(const wchar_t* path)
{
    printf("\n=== %ls ===\n", path);

    std::vector<EmbeddedStreamInfo> streams;
    HRESULT hr = EnumerateEmbeddedStreamsFromMedia(path, &streams);
    printf("EnumerateEmbeddedStreamsFromMedia -> hr=0x%08X, %d stream(s)\n",
        (unsigned)hr, (int)streams.size());
    CHECK(hr == S_OK, "enumeration returned S_OK");

    int n_pgs = 0;
    for (size_t i = 0; i < streams.size(); i++)
    {
        const EmbeddedStreamInfo& s = streams[i];
        printf("  #%u raw=0x%04X (%s %s) codec='%s' lang='%s' label='%ls'\n",
            (unsigned)s.index, (unsigned)s.raw_id,
            s.matroska ? "mkv-track" : "ts-pid",
            s.matroska ? "MKV" : "TS",
            s.codec_name.c_str(), s.language.empty() ? "" : s.language.c_str(),
            s.label.c_str());
        // PGS is MKV kind 1 (MKV_SUB_PGS) or TS stream type 0x90.
        if ((s.matroska && s.codec_name == "S_HDMV/PGS") || (!s.matroska && s.codec == 0x90))
        {
            n_pgs++;
        }
    }
    printf("PGS streams: %d\n", n_pgs);
    CHECK(n_pgs == 2, "exactly 2 PGS streams discovered");

    CHECK(streams.size() == 2, "exactly 2 embedded subtitle streams discovered");

    // index is 1-based and must match position.
    bool indexes_ok = true;
    for (size_t i = 0; i < streams.size(); i++)
    {
        if (streams[i].index != (WORD)(i + 1)) { indexes_ok = false; break; }
    }
    CHECK(indexes_ok, "1-based indexes are sequential");

    // The picker's option mapping must round-trip on the real streams: the token
    // the page writes for stream i must resolve back to stream i, for both the
    // text field ('pid:0xNNNN' / 'track:N') and the dedicated PID option.
    bool map_ok = true;
    for (size_t i = 0; i < streams.size(); i++)
    {
        const EmbeddedStreamInfo& s = streams[i];
        CString token = EmbeddedStreamToken(s);
        int via_text = FindSelectedEmbeddedStream(streams, token, 0);
        if (via_text != (int)i)
        {
            printf("FAIL: token '%ls' for #%u resolved to %d\n",
                (const wchar_t*)token, (unsigned)s.index, via_text);
            map_ok = false;
        }
        // A TS/BDMV stream also round-trips through INT_SECONDARY_EMBEDDED_PID
        // with no text entry; a Matroska stream has no PID so it must not.
        int pid = EmbeddedStreamOptionPid(s);
        int via_pid = FindSelectedEmbeddedStream(streams, CString(), pid);
        if (s.matroska ? (pid != 0 || via_pid != -1) : (via_pid != (int)i))
        {
            printf("FAIL: pid option %d for #%u resolved to %d\n", pid, (unsigned)s.index, via_pid);
            map_ok = false;
        }
    }
    CHECK(map_ok, "picker token/pid mapping round-trips on the real streams");
    return 0;
}

// Table-driven check of the picker mapping, independent of any media file:
// synthetic TS and Matroska streams exercise the token grammar, the index
// syntax ('sub:N') and the stripping of prior embedded entries.
static void CheckPickerMapping()
{
    printf("\n=== picker mapping (synthetic) ===\n");

    std::vector<EmbeddedStreamInfo> streams;
    EmbeddedStreamInfo ts0; ts0.index = 1; ts0.raw_id = 0x1200; ts0.matroska = false; ts0.codec = 0x90;
    EmbeddedStreamInfo ts1; ts1.index = 2; ts1.raw_id = 0x1201; ts1.matroska = false; ts1.codec = 0x90;
    EmbeddedStreamInfo mk0; mk0.index = 1; mk0.raw_id = 2;      mk0.matroska = true;
    EmbeddedStreamInfo mk1; mk1.index = 2; mk1.raw_id = 3;      mk1.matroska = true;
    streams.push_back(ts0); streams.push_back(ts1);

    CHECK(EmbeddedStreamToken(ts0) == CString(_T("pid:0x1200")), "TS token is pid:0xNNNN");
    CHECK(EmbeddedStreamOptionPid(ts1) == 0x1201, "TS option pid is the raw PID");
    CHECK(EmbeddedStreamToken(mk1) == CString(_T("track:3")), "MKV token is track:N");
    CHECK(EmbeddedStreamOptionPid(mk1) == 0, "MKV option pid is 0");

    CHECK(FindSelectedEmbeddedStream(streams, CString(_T("pid:0x1201")), 0) == 1,
        "'pid:0x1201' resolves to the second TS stream");
    CHECK(FindSelectedEmbeddedStream(streams, CString(_T("sub:2")), 0) == 1,
        "'sub:2' resolves by 1-based list index");
    CHECK(FindSelectedEmbeddedStream(streams, CString(_T("pid:0x9999")), 0) == -1,
        "an unknown PID resolves to nothing");
    CHECK(FindSelectedEmbeddedStream(streams, CString(_T("C:\\a.srt;pid:0x1200")), 0) == 0,
        "the embedded entry is found inside a mixed list");
    CHECK(FindSelectedEmbeddedStream(streams, CString(), 0x1201) == 1,
        "the bare PID option selects its stream");

    std::vector<EmbeddedStreamInfo> mkv;
    mkv.push_back(mk0); mkv.push_back(mk1);
    CHECK(FindSelectedEmbeddedStream(mkv, CString(_T("track:3")), 0) == 1,
        "'track:3' resolves to the second MKV stream");
    CHECK(FindSelectedEmbeddedStream(mkv, CString(), 0x1200) == -1,
        "a TS PID does not select a Matroska stream");

    CHECK(StripEmbeddedEntries(CString(_T("C:\\a.srt;pid:0x1200;D:\\b.ass"))) == CString(_T("C:\\a.srt;D:\\b.ass")),
        "strip removes the embedded entry and keeps the paths");
    CHECK(StripEmbeddedEntries(CString(_T("track:3"))) == CString(_T("")),
        "stripping a lone embedded entry yields empty");
    CHECK(StripEmbeddedEntries(CString(_T("C:\\a.srt"))) == CString(_T("C:\\a.srt")),
        "stripping leaves a plain path list untouched");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        printf("usage: embedded_enum_test <media> [media...]\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
    {
        EnumerateOne(argv[i]);
    }
    CheckPickerMapping();
    printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures;
}
