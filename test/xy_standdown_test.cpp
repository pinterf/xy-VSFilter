// Standalone coexistence check for the MultiSubFilter "(stand down when another
// Xy-family subtitle filter is active)" fail-soft guard.
//
// Builds a real DirectShow filter graph, loads the freshly built MultiSubFilter
// DLL *directly* (LoadLibrary + DllGetClassObject, so no regsvr32 and no
// registry writes are needed), instantiates its XySubFilter, and puts it
// opposite two graphs:
//
//   1. graph A: a foreign filter exposing IXySubFilterGraphMutex ("the other
//      Xy-family filter") + a fake ISubRenderConsumer with a high merit.
//      With the guard enabled (default) the filter MUST decline to connect
//      (Pause() succeeds, no crash) and the fake consumer must NOT have been
//      connected. With the guard disabled it MUST connect.
//   2. graph B: only the fake consumer, no foreign filter. The filter MUST
//      connect as it always has (no behaviour change when alone).
//
// The test reports PASS/FAIL per case and exits non-zero on any failure.

#include <windows.h>
#include <objbase.h>
#include <stdio.h>

// ---- minimal DirectShow bits (no strmbase dependency) ----------------------
#include <dshow.h>

// The coexistence interface (upstream definition: ATL "interface" == struct).
struct IXySubFilterGraphMutex : public IUnknown {};

// IXyOptions, copied verbatim from XyOptions.h so the vtable slots line up.
struct IXyOptions : public IUnknown
{
    STDMETHOD(XyGetBool     )(unsigned field, bool      *value) = 0;
    STDMETHOD(XyGetInt      )(unsigned field, int       *value) = 0;
    STDMETHOD(XyGetSize     )(unsigned field, SIZE      *value) = 0;
    STDMETHOD(XyGetRect     )(unsigned field, RECT      *value) = 0;
    STDMETHOD(XyGetUlonglong)(unsigned field, ULONGLONG *value) = 0;
    STDMETHOD(XyGetDouble   )(unsigned field, double    *value) = 0;
    STDMETHOD(XyGetString   )(unsigned field, LPWSTR    *value, int *chars) = 0;
    STDMETHOD(XyGetBin      )(unsigned field, LPVOID    *value, int *size ) = 0;
    STDMETHOD(XyGetBin2     )(unsigned field, void      *value, int  size ) = 0;
    STDMETHOD(XySetBool     )(unsigned field, bool      value) = 0;
    STDMETHOD(XySetInt      )(unsigned field, int       value) = 0;
    STDMETHOD(XySetSize     )(unsigned field, SIZE      value) = 0;
    STDMETHOD(XySetRect     )(unsigned field, RECT      value) = 0;
    STDMETHOD(XySetUlonglong)(unsigned field, ULONGLONG value) = 0;
    STDMETHOD(XySetDouble   )(unsigned field, double    value) = 0;
    STDMETHOD(XySetString   )(unsigned field, LPWSTR    value, int chars) = 0;
    STDMETHOD(XySetBin      )(unsigned field, LPVOID    value, int size) = 0;
};

// ---------------------------------------------------------------------------
// Identity of the fork (must match include/dualsubs_identity.h defaults).
// Kept as literals so this test does not have to include the filter headers.
// ---------------------------------------------------------------------------
static const GUID CLSID_MultiSubFilter =
    {0x67CDB97D, 0x9EC5, 0x44F0, {0xB9, 0x01, 0xC7, 0x88, 0x49, 0x11, 0x56, 0xD5}};

// The deliberately NOT identity-parameterised coexistence interface.
static const GUID IID_IXySubFilterGraphMutex =
    {0x8871ac83, 0x89c8, 0x44e9, {0x91, 0x3f, 0xa9, 0xa2, 0xa3, 0x22, 0x44, 0x0a}};

static const GUID IID_IXyOptions =
    {0x85E5D6F9, 0xBEFB, 0x4E01, {0xB0, 0x47, 0x75, 0x83, 0x59, 0xCD, 0xF9, 0xAB}};

// Option ids (see IDirectVobSubXy.h: DirectVobSubXyOptions::enum).
static const unsigned INT_LOAD_SETTINGS_LEVEL             = 75;  // LOADLEVEL_ALWAYS = 1
static const unsigned BOOL_HIDE_TRAY_ICON                 = 17;
static const unsigned BOOL_STAND_DOWN_WHEN_ANOTHER_XY     = 103;

// ISubRenderConsumer / ISubRenderOptions, copied verbatim in spirit from
// include/SubRenderIntf.h (we only need the vtable shape).
struct ISubRenderOptions : public IUnknown
{
    STDMETHOD(GetBool)(LPCSTR, bool *) = 0;
    STDMETHOD(GetInt)(LPCSTR, int *) = 0;
    STDMETHOD(GetSize)(LPCSTR, SIZE *) = 0;
    STDMETHOD(GetRect)(LPCSTR, RECT *) = 0;
    STDMETHOD(GetUlonglong)(LPCSTR, ULONGLONG *) = 0;
    STDMETHOD(GetDouble)(LPCSTR, double *) = 0;
    STDMETHOD(GetString)(LPCSTR, LPWSTR *, int *) = 0;
    STDMETHOD(GetBin)(LPCSTR, LPVOID *, int *) = 0;
    STDMETHOD(SetBool)(LPCSTR, bool) = 0;
    STDMETHOD(SetInt)(LPCSTR, int) = 0;
    STDMETHOD(SetSize)(LPCSTR, SIZE) = 0;
    STDMETHOD(SetRect)(LPCSTR, RECT) = 0;
    STDMETHOD(SetUlonglong)(LPCSTR, ULONGLONG) = 0;
    STDMETHOD(SetDouble)(LPCSTR, double) = 0;
    STDMETHOD(SetString)(LPCSTR, LPWSTR, int) = 0;
    STDMETHOD(SetBin)(LPCSTR, LPVOID, int) = 0;
};

struct ISubRenderProvider;

struct ISubRenderConsumer : public ISubRenderOptions
{
    STDMETHOD(GetMerit)(ULONG *) = 0;
    STDMETHOD(Connect)(ISubRenderProvider *) = 0;
    STDMETHOD(Disconnect)(void) = 0;
};

static const GUID IID_ISubRenderConsumer =
    {0x9DF90966, 0xFE9F, 0x4F0E, {0x88, 0x1E, 0xDA, 0xF8, 0xA5, 0x72, 0xD9, 0x00}};

// ---------------------------------------------------------------------------
// Fake foreign Xy-family filter: exposes IXySubFilterGraphMutex.
// ---------------------------------------------------------------------------
class ForeignXyFilter : public IBaseFilter, public IXySubFilterGraphMutex
{
public:
    ForeignXyFilter() : m_ref(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IPersist) || riid == __uuidof(IMediaFilter) ||
            riid == __uuidof(IBaseFilter))
            *ppv = static_cast<IBaseFilter *>(this);
        else if (riid == IID_IXySubFilterGraphMutex)
            *ppv = static_cast<IXySubFilterGraphMutex *>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID *p) { if (!p) return E_POINTER; *p = m_clsid; return S_OK; }
    void SetClsid(const CLSID &c) { m_clsid = c; }

    // IMediaFilter
    STDMETHODIMP Stop() { return S_OK; }
    STDMETHODIMP Pause() { return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME) { return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE *s) { if (s) *s = State_Stopped; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock *) { return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock **c) { if (c) *c = NULL; return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins **e) { if (e) *e = NULL; return S_OK; }
    STDMETHODIMP FindPin(LPCWSTR, IPin **p) { if (p) *p = NULL; return VFW_E_NOT_FOUND; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *i)
    {
        if (!i) return E_POINTER;
        wcscpy_s(i->achName, L"Foreign XySubFilter (fake)");
        i->pGraph = NULL;   // caller-owned; this fake simply has none
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *, LPCWSTR) { return S_OK; }
    STDMETHODIMP QueryVendorInfo(LPWSTR *p) { if (p) *p = NULL; return E_NOTIMPL; }

private:
    LONG m_ref;
    CLSID m_clsid = {0};
};

// ---------------------------------------------------------------------------
// Fake subtitle consumer with a merit high enough to beat every real renderer.
// ---------------------------------------------------------------------------
class FakeConsumer : public ISubRenderConsumer
{
public:
    FakeConsumer() : m_ref(1), m_connected(false) {}

    bool Connected() const { return m_connected; }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISubRenderConsumer) { *ppv = static_cast<ISubRenderConsumer *>(this); AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // ISubRenderOptions: only the fields the provider actually reads matter;
    // everything unknown reports "not supported".
    STDMETHODIMP GetBool(LPCSTR, bool *) { return E_NOTIMPL; }
    STDMETHODIMP GetInt(LPCSTR, int *) { return E_NOTIMPL; }
    STDMETHODIMP GetSize(LPCSTR f, SIZE *v)
    {
        if (v) { v->cx = 1920; v->cy = 1080; }
        return v ? S_OK : E_POINTER;
    }
    STDMETHODIMP GetRect(LPCSTR f, RECT *v)
    {
        if (v) { v->left = 0; v->top = 0; v->right = 1920; v->bottom = 1080; }
        return v ? S_OK : E_POINTER;
    }
    STDMETHODIMP GetUlonglong(LPCSTR, ULONGLONG *) { return E_NOTIMPL; }
    STDMETHODIMP GetDouble(LPCSTR, double *) { return E_NOTIMPL; }
    STDMETHODIMP GetString(LPCSTR f, LPWSTR *v, int *chars)
    {
        if (chars) *chars = 0;
        if (!v) return S_FALSE;
        const wchar_t *s = L"FakeConsumer";
        size_t n = wcslen(s) + 1;
        *v = (LPWSTR)LocalAlloc(LPTR, n * sizeof(wchar_t));
        if (!*v) return E_OUTOFMEMORY;
        wcscpy_s(*v, n, s);
        if (chars) *chars = (int)n;
        return S_OK;
    }
    STDMETHODIMP GetBin(LPCSTR, LPVOID *, int *) { return E_NOTIMPL; }
    STDMETHODIMP SetBool(LPCSTR, bool) { return S_OK; }
    STDMETHODIMP SetInt(LPCSTR, int) { return S_OK; }
    STDMETHODIMP SetSize(LPCSTR, SIZE) { return S_OK; }
    STDMETHODIMP SetRect(LPCSTR, RECT) { return S_OK; }
    STDMETHODIMP SetUlonglong(LPCSTR, ULONGLONG) { return S_OK; }
    STDMETHODIMP SetDouble(LPCSTR, double) { return S_OK; }
    STDMETHODIMP SetString(LPCSTR, LPWSTR, int) { return S_OK; }
    STDMETHODIMP SetBin(LPCSTR, LPVOID, int) { return S_OK; }

    // ISubRenderConsumer
    STDMETHODIMP GetMerit(ULONG *m) { if (m) *m = 0x7fffffff; return m ? S_OK : E_POINTER; }
    STDMETHODIMP Connect(ISubRenderProvider *) { m_connected = true; return S_OK; }
    STDMETHODIMP Disconnect(void) { m_connected = false; return S_OK; }

private:
    LONG m_ref;
    bool m_connected;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
typedef HRESULT (STDAPICALLTYPE *PDllGetClassObject)(REFCLSID, REFIID, LPVOID *);

static int g_failures = 0;
static void Check(bool ok, const char *what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

// Runs one scenario. Our filter joins FIRST and the foreign Xy-family filter is
// added AFTERWARDS - the load-order inversion that the join-time guard cannot
// see and that the new consumer-connect guard targets.
static void RunCase(const char *name, PDllGetClassObject dgco,
                    bool add_foreign, bool guard_off, bool expect_connect)
{
    printf("\n-- case: %s --\n", name);

    IFilterGraph2 *graph = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (FAILED(hr) || !graph) { Check(false, "create graph"); return; }

    IClassFactory *cf = NULL;
    hr = dgco(CLSID_MultiSubFilter, IID_IClassFactory, (void **)&cf);
    IBaseFilter *filter = NULL;
    if (cf) hr = cf->CreateInstance(NULL, IID_PPV_ARGS(&filter));
    if (!filter)
    {
        Check(false, "create filter");
        if (cf) cf->Release();
        graph->Release();
        return;
    }

    IXyOptions *opts = NULL;
    filter->QueryInterface(IID_IXyOptions, (void **)&opts);
    if (opts)
    {
        // Force the "always load" path so Pause() reaches FindAndConnectConsumer.
        opts->XySetInt(INT_LOAD_SETTINGS_LEVEL, 1 /*LOADLEVEL_ALWAYS*/);
        opts->XySetBool(BOOL_HIDE_TRAY_ICON, true);
        if (guard_off) opts->XySetBool(BOOL_STAND_DOWN_WHEN_ANOTHER_XY, false);
    }

    hr = graph->AddFilter(filter, L"MultiSubFilter");
    Check(SUCCEEDED(hr), "our filter joined the graph first");

    FakeConsumer *consumer = new FakeConsumer();
    graph->AddFilter((IBaseFilter *)consumer, L"FakeConsumer");

    ForeignXyFilter *foreign = NULL;
    if (add_foreign)
    {
        // Joins while we are already in the graph; our join-time guard has
        // already passed, so only the connect-time guard can react.
        foreign = new ForeignXyFilter();
        graph->AddFilter(foreign, L"Foreign XySubFilter (fake)");
    }

    hr = filter->Pause();
    Check(SUCCEEDED(hr), "Pause() returned success (player survives)");
    Check(consumer->Connected() == expect_connect,
          expect_connect ? "consumer connected (expected)"
                         : "consumer NOT connected (stood down)");

    // Teardown. The foreign filter and the fake consumer are deliberately left
    // to the process exit: the fake consumer is not a real IBaseFilter (it only
    // carries ISubRenderConsumer), so asking the graph to remove it would be a
    // harness artifact, not a filter behaviour. What matters is that removing
    // and releasing OUR filter - which runs its JoinFilterGraph(NULL) cleanup -
    // neither crashes nor hangs. The filter object survives removal because we
    // hold our own reference.
    graph->RemoveFilter(filter);
    if (opts) opts->Release();
    filter->Release();
    printf("    filter removed and released, hr paths clean\n");
    (void)consumer;
    (void)foreign;
    (void)cf;
    (void)graph;
}

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const wchar_t *dll_path = argc > 1 ? argv[1] : L"MultiSubFilterVerify.dll";
    printf("MultiSubFilter coexistence stand-down test\n");
    printf("DLL: %ls\n", dll_path);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HMODULE mod = LoadLibraryW(dll_path);
    if (!mod)
    {
        printf("FATAL: LoadLibraryW(%ls) failed: %lu\n", dll_path, GetLastError());
        return 2;
    }
    PDllGetClassObject dgco = (PDllGetClassObject)GetProcAddress(mod, "DllGetClassObject");
    if (!dgco)
    {
        printf("FATAL: no DllGetClassObject\n");
        return 2;
    }

    // 1. Guard on (default), foreign Xy filter appears after we joined.
    RunCase("guard on, foreign Xy filter joins later (load-order inversion)",
            dgco, /*add_foreign*/ true, /*guard_off*/ false, /*expect_connect*/ false);

    // 2. Guard explicitly disabled: this filter must take the consumer anyway.
    RunCase("guard off (xy_standdown=0), foreign Xy filter present",
            dgco, /*add_foreign*/ true, /*guard_off*/ true, /*expect_connect*/ true);

    // 3. No foreign filter: behaviour unchanged.
    RunCase("alone, no other Xy filter",
            dgco, /*add_foreign*/ false, /*guard_off*/ false, /*expect_connect*/ true);

    printf("\n%s (%d failure(s))\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS", g_failures);
    return g_failures ? 1 : 0;
}
