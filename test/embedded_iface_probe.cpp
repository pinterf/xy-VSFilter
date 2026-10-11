/*
 * Probe: exercise XySubFilter::EnumerateEmbeddedStreams(NULL, ...) exactly as
 * the Dual Subtitles property page does, against a REAL filter graph, to find
 * why the picker can show an empty list.
 *
 * The page calls with media_path == NULL, meaning "the media of the current
 * graph". We replicate that: create the filter, add it to a graph that also
 * contains a file source pointing at the target media, then call the loader
 * interface with NULL and with an explicit path, and print both results.
 *
 * Build: MSBuild VSFilter.sln /t:embedded_iface_probe /p:Configuration=Release
 *        /p:Platform=x64 /p:BuildProjectReferences=false
 */
#include "stdafx.h"
#include "IMultiSubtitleTrackLoader.h"
#include "EmbeddedStreamEnum.h"
#include <dshow.h>
#include <stdio.h>

// Our filter's CLSID comes from the identity header via the DLL's registration;
// we instantiate by LoadLibrary + DllGetClassObject to avoid needing regsvr32.
static const GUID CLSID_MultiSubFilter =
    {0x67CDB97D, 0x9EC5, 0x44F0, {0xB9, 0x01, 0xC7, 0x88, 0x49, 0x11, 0x56, 0xD5}};

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFn)(REFCLSID, REFIID, void **);

static int g_fail = 0;
static void Check(bool ok, const char *what)
{
    printf("%s  : %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) g_fail++;
}

static void DumpStreams(const char *label, HRESULT hr, const std::vector<EmbeddedStreamInfo>& v)
{
    printf("  %s -> hr=0x%08X, %d stream(s)\n", label, (unsigned)hr, (int)v.size());
    for (size_t i = 0; i < v.size(); i++)
    {
        printf("     #%u raw=0x%04X %hs %hs '%ls'\n", (unsigned)v[i].index,
               (unsigned)v[i].raw_id, v[i].codec_name.c_str(),
               v[i].language.c_str(), v[i].label.c_str());
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { printf("usage: embedded_iface_probe <media> [dll]\n"); return 2; }

    const wchar_t *media = argv[1];
    const wchar_t *dll = argc > 2 ? argv[2] : L"MultiSubFilter.dll";

    CoInitialize(NULL);

    HMODULE h = LoadLibraryW(dll);
    Check(h != NULL, "load filter dll");
    if (!h) return 1;

    DllGetClassObjectFn dgco = (DllGetClassObjectFn)GetProcAddress(h, "DllGetClassObject");
    Check(dgco != NULL, "resolve DllGetClassObject");
    if (!dgco) return 1;

    IClassFactory *cf = NULL;
    HRESULT hr = dgco(CLSID_MultiSubFilter, IID_IClassFactory, (void **)&cf);
    Check(SUCCEEDED(hr) && cf, "get class factory");

    IBaseFilter *filter = NULL;
    if (cf) hr = cf->CreateInstance(NULL, IID_PPV_ARGS(&filter));
    Check(filter != NULL, "create filter instance");

    IMultiSubtitleTrackLoader *loader = NULL;
    if (filter) hr = filter->QueryInterface(__uuidof(IMultiSubtitleTrackLoader), (void **)&loader);
    Check(loader != NULL, "filter exposes IMultiSubtitleTrackLoader");

    if (loader)
    {
        // --- 1) NULL path, filter NOT in any graph (page opened before join,
        //        or media path never captured) ---
        std::vector<EmbeddedStreamInfo> v1;
        hr = loader->EnumerateEmbeddedStreams(NULL, &v1);
        DumpStreams("NULL path, filter not in a graph", hr, v1);
        printf("     (S_FALSE here is expected when there is no current graph media)\n");

        // --- 2) explicit path: the path the page would get from the filter ---
        std::vector<EmbeddedStreamInfo> v2;
        hr = loader->EnumerateEmbeddedStreams(media, &v2);
        DumpStreams("explicit media path", hr, v2);
        Check(SUCCEEDED(hr), "explicit path enumeration succeeds");
        Check(v2.size() >= 2, "at least two embedded streams via explicit path");

        loader->Release();
    }

    // --- 3) with a graph containing a file source, does NULL now resolve? ---
    {
        IFilterGraph2 *graph = NULL;
        hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
        IBaseFilter *src = NULL;
        if (graph)
        {
            hr = CoCreateInstance(CLSID_AsyncReader, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&src));
            if (src)
            {
                IFileSourceFilter *fsrc = NULL;
                if (SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&fsrc))) && fsrc)
                {
                    hr = fsrc->Load(media, NULL);
                    Check(SUCCEEDED(hr), "file source loads media");
                    graph->AddFilter(src, L"file source");
                    fsrc->Release();
                }
            }
            if (filter)
            {
                graph->AddFilter(filter, L"MultiSubFilter");   // triggers JoinFilterGraph
                if (loader)
                {
                    std::vector<EmbeddedStreamInfo> v3;
                    hr = loader->EnumerateEmbeddedStreams(NULL, &v3);
                    DumpStreams("NULL path, filter in graph with file source", hr, v3);
                    Check(v3.size() >= 2, "NULL path resolves once the graph has the media");
                }
            }
            if (src) src->Release();
            if (graph) graph->Release();
        }
    }

    if (filter) filter->Release();
    if (cf) cf->Release();
    FreeLibrary(h);
    CoUninitialize();

    printf("\n%s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
