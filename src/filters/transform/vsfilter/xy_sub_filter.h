#pragma once

#include "dualsubs_identity.h"

#include <atlsync.h>
#include "DirectVobSub.h"
#include "SubRenderIntf.h"
#include "SubRenderOptionsImpl.h"
#include "IMultiSubtitleTrackLoader.h"

class CDirectVobSubFilter;
class MultiTrackSubRenderProvider;

// Expose this interface if you do NOT want to work with XySubFilter in a graph
[uuid("8871ac83-89c8-44e9-913f-a9a2a322440a")]
interface IXySubFilterGraphMutex : public IUnknown
{

};

[uuid(DUALSUBS_CLSID_FILTER_STR)]
class XySubFilter
    : public CBaseFilter
    , public CDVS4XySubFilter
    , public ISpecifyPropertyPages
    , public IAMStreamSelect
    , public CAMThread
    , public SubRenderOptionsImpl
    , public ISubRenderProvider
    , public IXySubFilterGraphMutex
    , public IMultiSubtitleTrackLoader
{
public:
    friend class SubtitleInputPin2;

    XySubFilter(LPUNKNOWN punk, HRESULT* phr, const GUID& clsid = __uuidof(XySubFilter));
    virtual ~XySubFilter();

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);

    DECLARE_ISUBRENDEROPTIONS;

    //CBaseFilter
    CBasePin* GetPin(int n);
    int GetPinCount();
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName);

    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo);

    STDMETHODIMP Pause();

    // XyOptionsImpl
    virtual HRESULT OnOptionChanged(unsigned field);
    virtual HRESULT DoGetField(unsigned field, void *value);

    // IXyOptions
    STDMETHODIMP XyGetString   (unsigned field, LPWSTR   *value, int *chars);
    STDMETHODIMP XySetBool     (unsigned field, bool      value);

    // DirectVobSubImpl
    HRESULT GetCurStyles(SubStyle sub_style[], int count);
    HRESULT SetCurStyles(const SubStyle sub_style[], int count);

    // IDirectVobSub
    STDMETHODIMP get_LanguageName(int iLanguage, WCHAR** ppName);

    STDMETHODIMP get_CachesInfo(CachesInfo* caches_info);
    STDMETHODIMP get_XyFlyWeightInfo(XyFlyWeightInfo* xy_fw_info);

    // IDirectVobSub2
    STDMETHODIMP put_TextSettings(STSStyle* pDefStyle);
    STDMETHODIMP put_AspectRatioSettings(CSimpleTextSubtitle::EPARCompensationType* ePARCompensationType);

    // ISpecifyPropertyPages
    STDMETHODIMP GetPages(CAUUID* pPages);

    // IAMStreamSelect
    STDMETHODIMP Count(DWORD* pcStreams); 
    STDMETHODIMP Enable(long lIndex, DWORD dwFlags); 
    STDMETHODIMP Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags, LCID* plcid, DWORD* pdwGroup, WCHAR** ppszName, IUnknown** ppObject, IUnknown** ppUnk);

    //ISubRenderProvider
    STDMETHODIMP RequestFrame(REFERENCE_TIME start, REFERENCE_TIME stop, LPVOID context);
    STDMETHODIMP Disconnect(void);

    // IMultiSubtitleTrackLoader: builds a subtitle source from an external file,
    // reusing the default loader cascade (RTS / VobSub / SSF / SUP).
    STDMETHODIMP CreateTrackProviderFromFile(LPCWSTR file_path, IXySubRenderProvider** provider);

    // Reads an embedded subtitle stream straight from the container file; the
    // container-side counterpart of CreateTrackProviderFromFile. Dispatches to
    // the TS/BDMV or Matroska source by container type.
    STDMETHODIMP CreateTrackProviderFromEmbedded(LPCWSTR media_path, WORD value,
        EmbeddedSelectKind kind, IXySubRenderProvider** provider);

    // Lists the embedded subtitle streams of the current media (TS/BDMV or
    // Matroska) for the property page's pickable list. Container headers only.
    STDMETHODIMP EnumerateEmbeddedStreams(LPCWSTR media_path,
        std::vector<EmbeddedStreamInfo>* out);
private:
    void SetYuvMatrix();
    void SetRgbOutputLevel();
    bool Open();

    void UpdateSubtitle(bool fApplyDefStyle = true);
    void SetSubtitle(ISubStream* pSubStream, bool fApplyDefStyle = true);
    void InvalidateSubtitle(REFERENCE_TIME rtInvalidate = -1, DWORD_PTR nSubtitleId = -1);

    HRESULT UpdateParamFromConsumer(bool getNameAndVersion=false);

    int FindPreferedLanguage(bool fHideToo = true);
    void UpdatePreferedLanguages(CString l);

    // the text input pin is using these
    void AddSubStream(ISubStream* pSubStream);
    void RemoveSubStream(ISubStream* pSubStream);
    HRESULT CheckInputType(const CMediaType* pmt);

    HRESULT GetIsEmbeddedSubStream(int iSelected, bool *fIsEmbedded);

    bool LoadExternalSubtitle(IFilterGraph* pGraph);

    // Independent secondary auto-load: when the language list is configured,
    // find a subtitle next to the media whose suffix matches and use it as the
    // secondary track. No-op (stock behaviour) when the list is empty.
    bool LoadSecondarySubtitle(IFilterGraph* pGraph);

    // Records the media file path (IFileSourceFilter enumeration), used by the
    // container-side (embedded) secondary source.
    void UpdateMediaFileName(IFilterGraph* pGraph);

    // Wrap pPrimary (mandatory) + optional secondary into the multi-tracker.
    void UpdateMultiTracker(ISubRenderProvider* pPrimary);

    HRESULT GetRidOfVSFilter();
    HRESULT StartStreaming();

    HRESULT FindAndConnectConsumer(IFilterGraph* pGraph);

    // Fail-soft guard used right before we take exclusive use of the subtitle
    // consumer: returns true when another filter in the graph exposes
    // IXySubFilterGraphMutex, i.e. an Xy-family subtitle filter (the upstream
    // XySubFilter or another instance of this fork). The enumeration skips
    // ourselves, is non-recursive and stops at the first match. On a match,
    // "who" (optional) receives a human readable identifier: the filter name,
    // or its class id when the filter has no name yet.
    bool IsAnotherXySubFilterActive(IFilterGraph* pGraph, CString* who);

    void UpdateLanguageCount();

    CStringW DumpProviderInfo();
    CStringW DumpConsumerInfo();
private:
    class CFileReloaderData
    {
    public:
        ATL::CEvent ThreadStartedEvent, EndThreadEvent, RefreshEvent;
        CAtlList<CString> files;
        CAtlArray<CTime> mtime;
    } m_frd;

    void SetupFRD(CStringArray& paths, CAtlArray<HANDLE>& handles);
    DWORD ThreadProc();
private:
    ColorConvTable::YuvMatrixType m_video_yuv_matrix_decided_by_sub;
    ColorConvTable::YuvRangeType m_video_yuv_range_decided_by_sub;

    ISubStream *m_curSubStream;
    CInterfaceList<ISubStream> m_pSubStreams;
    CAtlList<bool> m_fIsSubStreamEmbeded;

    // Media (container) path used by the container-side embedded secondary
    // source; empty until the graph is enumerated.
    CStringW m_media_file_path;

    // critical section protecting all
    CCritSec m_csFilter;

    CCritSec m_csProviderFields;// critical section protecting fields to be read by consumer
                                // it is needed because otherwise consumer has to hold m_csSubLock which may be holded 
                                // by renderring thread for a long time

    CCritSec m_csConsumer;      // critical section protecting m_consumer

    CComPtr<IXySubRenderProvider> m_sub_provider;
    CComPtr<MultiTrackSubRenderProvider> m_multi_tracker;

    CAtlArray<SubtitleInputPin2*> m_pSubtitleInputPin;

    // don't set the "hide subtitles" stream until we are finished with loading
    bool m_fLoading;

    bool m_not_first_pause;

    SystrayIconData m_tbid;
    HANDLE m_hSystrayThread;

    CComPtr<ISubRenderConsumer> m_consumer;
    ULONGLONG m_context_id;

    REFERENCE_TIME m_last_requested;

    bool m_workaround_mpc_hc;//enable workaround for MPC-HC to prevent be removed from the graph

    bool m_disconnect_entered;

    // Set by the coexistence guard in FindAndConnectConsumer when we declined
    // to connect because another Xy-family filter is active; lets Pause() tell
    // an intentional stand-down apart from a real connect failure.
    bool m_stood_down_owing_to_other_xy;

    CStringW m_filter_info_string;

#ifdef SUBTITLE_FRAME_DUMP_FILE
    int   m_dump_subtitle_frame_start_rt;
    int   m_dump_subtitle_frame_end_rt;
    bool  m_dump_subtitle_frame_to_txt;
    bool  m_dump_subtitle_frame_alpha_channel;
    bool  m_dump_subtitle_frame_rgb_bmp;
    DWORD m_dump_subtitle_frame_background_color;
#endif
};
