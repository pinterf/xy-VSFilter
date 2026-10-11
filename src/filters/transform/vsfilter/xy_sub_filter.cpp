#include "stdafx.h"
#include "xy_sub_filter.h"
#include "DirectVobSubFilter.h"
#include "VSFilter.h"
#include "DirectVobSubPropPage.h"
#include "SubtitleInputPin2.h"
#include "../../../subtitles/xy_bitmap.h"
#include "../../../subtitles/hdmv_subtitle_provider.h"
#include "../../../subtitles/TsSubtitleSource.h"
#include "../../../subtitles/MatroskaSubtitleSource.h"
#include "../../../subtitles/EmbeddedStreamEnum.h"
#include "../../../subtitles/SSF.h"
#include "../../../SubPic/SimpleSubPicProviderImpl.h"
#include "../../../subpic/SimpleSubPicWrapper.h"
#include "../../../subpic/MultiTrackSubRenderProvider.h"
#include "../../../subpic/color_conv_table.h"

#include "CAutoTiming.h"
#include "xy_logger.h"

#include "moreuuids.h"
#include <Mfidl.h>
#include <evr.h>

#if ENABLE_XY_LOG_RENDERER_REQUEST
#  define TRACE_RENDERER_REQUEST(msg) XY_LOG_TRACE(msg)
#  define TRACE_RENDERER_REQUEST_TIMING(msg) XY_AUTO_TIMING(msg)
#else
#  define TRACE_RENDERER_REQUEST(msg)
#  define TRACE_RENDERER_REQUEST_TIMING(msg)
#endif

#if ENABLE_XY_LOG_TRACE_GRAPH
#  define XY_DUMP_GRAPH(graph,level) xy_logger::XyDumpGraph(graph,level)
#else
#  define XY_DUMP_GRAPH(graph,level)
#endif

using namespace DirectVobSubXyOptions;

const SubRenderOptionsImpl::OptionMap options[] = 
{
    {"name",           SubRenderOptionsImpl::OPTION_TYPE_STRING, STRING_NAME                 },
    {"version",        SubRenderOptionsImpl::OPTION_TYPE_STRING, STRING_VERSION              },
    {"yuvMatrix",      SubRenderOptionsImpl::OPTION_TYPE_STRING, STRING_YUV_MATRIX           },
    {"combineBitmaps", SubRenderOptionsImpl::OPTION_TYPE_BOOL,   BOOL_COMBINE_BITMAPS        },
    {"useDestAlpha",   SubRenderOptionsImpl::OPTION_TYPE_BOOL,   BOOL_SUB_FRAME_USE_DST_ALPHA},
    {"outputLevels",   SubRenderOptionsImpl::OPTION_TYPE_STRING, STRING_OUTPUT_LEVELS        },
    {"isMovable",      SubRenderOptionsImpl::OPTION_TYPE_BOOL,   BOOL_IS_MOVABLE             },
    {"isBitmap",       SubRenderOptionsImpl::OPTION_TYPE_BOOL,   BOOL_IS_BITMAP              },
    {0}
};

////////////////////////////////////////////////////////////////////////////
//
// Constructor
//

XySubFilter::XySubFilter( LPUNKNOWN punk, 
    HRESULT* phr, const GUID& clsid /*= __uuidof(XySubFilter)*/ )
    : CBaseFilter(NAME(DUALSUBS_FILTER_NAME), punk, &m_csFilter, clsid)
    , CDVS4XySubFilter(XyVobFilterOptions, &m_csFilter)
    , SubRenderOptionsImpl(::options, this)
    , m_curSubStream(NULL)
    , m_not_first_pause(false)
    , m_hSystrayThread(0)
    , m_context_id(0)
    , m_last_requested(-1)
    , m_workaround_mpc_hc(false)
    , m_disconnect_entered(false)
    , m_stood_down_owing_to_other_xy(false)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    XY_LOG_INFO(XY_LOG_VAR_2_STR(this));
    m_xy_str_opt[STRING_NAME] = DUALSUBS_W_FILTER_NAME;
    m_xy_str_opt[STRING_OUTPUT_LEVELS] = L"PC";
    m_filter_info_string = m_xy_str_opt[STRING_NAME];

#ifdef SUBTITLE_FRAME_DUMP_FILE
    CString dump_subtitle_frame= theApp.GetProfileString(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_start_time"),_T("00:00:00,000"));
    m_dump_subtitle_frame_start_rt = StringToReftime(dump_subtitle_frame);
    dump_subtitle_frame = theApp.GetProfileString(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_end_time"),_T("00:00:00,000"));
    m_dump_subtitle_frame_end_rt = StringToReftime(dump_subtitle_frame);

    m_dump_subtitle_frame_to_txt        = !!theApp.GetProfileInt(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_to_txt"       ),1);
    m_dump_subtitle_frame_alpha_channel = !!theApp.GetProfileInt(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_alpha_channel"),1);
    m_dump_subtitle_frame_rgb_bmp       = !!theApp.GetProfileInt(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_rgb_bmp"    ),1);

    m_dump_subtitle_frame_background_color = 0x00ffff;
    dump_subtitle_frame = theApp.GetProfileString(ResStr(IDS_R_GENERAL), _T("dump_subtitle_frame_background_color"),_T("00ffff"));
    dump_subtitle_frame.MakeLower().Trim(_T("&h")).TrimLeft(_T("0x"));
    dump_subtitle_frame = _T("0x") + dump_subtitle_frame;
    _stscanf(dump_subtitle_frame.GetString(), _T("%x"), &m_dump_subtitle_frame_background_color);
#endif

    theApp.WriteProfileString(ResStr(IDS_R_DEFTEXTPATHES), _T("Hint"), _T("The first three are fixed, but you can add more up to ten entries."));

    CString tmp;
    tmp.Format(ResStr(IDS_RP_PATH), 0);
    theApp.WriteProfileString(ResStr(IDS_R_DEFTEXTPATHES), tmp, _T("."));
    tmp.Format(ResStr(IDS_RP_PATH), 1);
    theApp.WriteProfileString(ResStr(IDS_R_DEFTEXTPATHES), tmp, _T(".\\subtitles"));
    tmp.Format(ResStr(IDS_RP_PATH), 2);
    theApp.WriteProfileString(ResStr(IDS_R_DEFTEXTPATHES), tmp, _T(".\\subs"));

    m_fLoading = true;

    m_video_yuv_matrix_decided_by_sub = ColorConvTable::NONE;
    m_video_yuv_range_decided_by_sub = ColorConvTable::RANGE_NONE;

    m_pSubtitleInputPin.Add(DEBUG_NEW SubtitleInputPin2(this, m_pLock, &m_csFilter, phr));
    ASSERT(SUCCEEDED(*phr));
    if(phr && FAILED(*phr)) return;

    m_tbid.WndCreatedEvent.Create(0, FALSE, FALSE, 0);
    m_tbid.hSystrayWnd = NULL;
    m_tbid.graph = NULL;
    m_tbid.fRunOnce = false;
    m_tbid.fShowIcon = true;

    CacheManager::GetPathDataMruCache()->SetMaxItemNum(m_xy_int_opt[INT_PATH_DATA_CACHE_MAX_ITEM_NUM]);
    CacheManager::GetScanLineData2MruCache()->SetMaxItemNum(m_xy_int_opt[INT_SCAN_LINE_DATA_CACHE_MAX_ITEM_NUM]);
    CacheManager::GetOverlayNoBlurMruCache()->SetMaxItemNum(m_xy_int_opt[INT_OVERLAY_NO_BLUR_CACHE_MAX_ITEM_NUM]);
    CacheManager::GetOverlayMruCache()->SetMaxItemNum(m_xy_int_opt[INT_OVERLAY_CACHE_MAX_ITEM_NUM]);

    XyFwGroupedDrawItemsHashKey::GetCacher()->SetMaxItemNum(m_xy_int_opt[INT_BITMAP_MRU_CACHE_ITEM_NUM]);
    CacheManager::GetBitmapMruCache()->SetMaxItemNum(m_xy_int_opt[INT_BITMAP_MRU_CACHE_ITEM_NUM]);

    CacheManager::GetClipperAlphaMaskMruCache()->SetMaxItemNum(m_xy_int_opt[INT_CLIPPER_MRU_CACHE_ITEM_NUM]);
    CacheManager::GetTextInfoCache()->SetMaxItemNum(m_xy_int_opt[INT_TEXT_INFO_CACHE_ITEM_NUM]);
    CacheManager::GetAssTagListMruCache()->SetMaxItemNum(m_xy_int_opt[INT_ASS_TAG_LIST_CACHE_ITEM_NUM]);

    std::size_t max_size = m_xy_int_opt[INT_MAX_CACHE_SIZE_MB] >= 0 ?
        m_xy_int_opt[INT_MAX_CACHE_SIZE_MB] : m_xy_int_opt[INT_AUTO_MAX_CACHE_SIZE_MB];
    max_size = max_size < (SIZE_MAX>>20) ? (max_size<<20) : SIZE_MAX;
    CRenderedTextSubtitle::SetMaxCacheSize(max_size);
    SubpixelPositionControler::GetGlobalControler().SetSubpixelLevel( static_cast<SubpixelPositionControler::SUBPIXEL_LEVEL>(m_xy_int_opt[INT_SUBPIXEL_POS_LEVEL]) );

    m_frd.ThreadStartedEvent.Create(0, FALSE, FALSE, 0);
    m_frd.EndThreadEvent.Create(0, FALSE, FALSE, 0);
    m_frd.RefreshEvent.Create(0, FALSE, FALSE, 0);
    CAMThread::Create();

    WaitForSingleObject(m_frd.ThreadStartedEvent, INFINITE);
}

XySubFilter::~XySubFilter()
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(this));
    m_frd.EndThreadEvent.Set();
    CAMThread::Close();

    for(unsigned i = 0; i < m_pSubtitleInputPin.GetCount(); i++)
        delete m_pSubtitleInputPin[i];

    m_sub_provider = NULL;
    m_multi_tracker = NULL;
    ::DeleteSystray(&m_hSystrayThread, &m_tbid);
    _ASSERTE(_CrtCheckMemory());
}

STDMETHODIMP XySubFilter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    CheckPointer(ppv, E_POINTER);

    return
        QI(IDirectVobSub)
        QI(IDirectVobSub2)
        QI(IXyOptions)
        QI(IFilterVersion)
        QI(ISpecifyPropertyPages)
        QI(IAMStreamSelect)
        QI(ISubRenderProvider)
        QI(ISubRenderFrame)
        QI(IXySubFilterGraphMutex)
        QI(IMultiSubtitleTrackLoader)
        (riid==__uuidof(ISubRenderOptions)) ? GetInterface((ISubRenderProvider*)this, ppv) :
        __super::NonDelegatingQueryInterface(riid, ppv);
}

//
// CBaseFilter
//
CBasePin* XySubFilter::GetPin(int n)
{
    CAutoLock cAutoLock(&m_csFilter);
    if (m_workaround_mpc_hc)
        return NULL;

    if(n >= 0 && n < (int)m_pSubtitleInputPin.GetCount())
        return m_pSubtitleInputPin[n];

    return NULL;
}

int XySubFilter::GetPinCount()
{
    CAutoLock cAutoLock(&m_csFilter);
    if (m_workaround_mpc_hc)
        return 0;

    return m_pSubtitleInputPin.GetCount();
}

STDMETHODIMP XySubFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName)
{
    CAutoLock cAutoLock(&m_csFilter);
    XY_LOG_INFO(XY_LOG_VAR_2_STR(this)<<" pGraph:"<<(void*)pGraph);
    HRESULT hr = NOERROR;
    if(pGraph)
    {
        bool found_vsfilter = false;
        BeginEnumFilters(pGraph, pEF, pBF)
        {
            if(pBF != (IBaseFilter*)this)
            {
                if (CComQIPtr<IXySubFilterGraphMutex>(pBF))
                {
                    return E_FAIL;
                }
                CLSID clsid;
                pBF->GetClassID(&clsid);
                if (clsid==__uuidof(CDirectVobSubFilter) || clsid==__uuidof(CDirectVobSubFilter2))
                {
                    XY_LOG_INFO("I see VSFilter");
                    if (!m_xy_bool_opt[BOOL_GET_RID_OF_VSFILTER])
                    {
                        XY_LOG_INFO("We do NOT want to get rid of VSFilter");
                        return E_FAIL;
                    }
                    found_vsfilter = true;
                }
            }
        }
        EndEnumFilters;

        bool under_mpc_hc = (theApp.m_AppName.MakeLower().Find(_T("mpc-hc"), 0)==0);
        if (under_mpc_hc)
        {
            bool found_sub_pin = false;
            bool accept_embedded = m_xy_int_opt[INT_LOAD_SETTINGS_LEVEL]==LOADLEVEL_ALWAYS ||
                (m_xy_int_opt[INT_LOAD_SETTINGS_LEVEL]!=LOADLEVEL_DISABLED && m_xy_bool_opt[BOOL_LOAD_SETTINGS_EMBEDDED]);
            CComPtr<IBaseFilter> pBF;
            if (accept_embedded)
            {
                for (int i=0, n=m_known_source_filters_guid.GetCount();i<n;i++)
                {
                    if (pBF = FindFilter(m_known_source_filters_guid[i], pGraph))
                    {
                        break;
                    }
                }
                BeginEnumPins(pBF, pEP, pPin)
                {
                    BeginEnumMediaTypes(pPin, pEM, pmt)
                    {
                        if(pmt->majortype == MEDIATYPE_Text || pmt->majortype == MEDIATYPE_Subtitle)
                        {
                            found_sub_pin = true;
                            break;
                        }
                    }
                    EndEnumMediaTypes(pmt);
                    if(found_sub_pin) break;
                }
                EndEnumPins
            }
            if (!found_sub_pin) {
                m_workaround_mpc_hc = true;
            }
        }
        hr = __super::JoinFilterGraph(pGraph, pName);
        if (FAILED(hr))
        {
            XY_LOG_ERROR("Failed to join filter graph");
            return hr;
        }
        if (found_vsfilter && m_xy_bool_opt[BOOL_GET_RID_OF_VSFILTER])
        {
            hr = GetRidOfVSFilter();
            if (FAILED(hr))
            {
                XY_LOG_FATAL("Failed to get rid of VSFilter");
                return E_FAIL;
            }
        }
        // Resolve the container path first so an embedded secondary can be
        // opened while loading, and any auto-loaded stream picks it up.
        UpdateMediaFileName(pGraph);
        LoadExternalSubtitle(pGraph);
        LoadSecondarySubtitle(pGraph);
        return hr;
    }
    else
    {
        if (m_consumer)
        {
            if (FAILED(m_consumer->Disconnect())) {
                XY_LOG_ERROR("Failed to disconnect consumer");
            }
            m_filter_info_string = m_xy_str_opt[STRING_NAME];
            CAutoLock cAutoLock(&m_csConsumer);
            m_consumer = NULL;
        }
        ::DeleteSystray(&m_hSystrayThread, &m_tbid);
        m_workaround_mpc_hc   = false;
        m_not_first_pause     = false;

        return __super::JoinFilterGraph(pGraph, pName);
    }
    return E_FAIL;
}

STDMETHODIMP XySubFilter::QueryFilterInfo( FILTER_INFO* pInfo )
{
    CheckPointer(pInfo, E_POINTER);
    ValidateReadWritePtr(pInfo, sizeof(FILTER_INFO));

    CAutoLock cAutoLock(&m_csFilter);
    HRESULT hr = __super::QueryFilterInfo(pInfo);
    if (SUCCEEDED(hr))
    {
        wcscpy_s(pInfo->achName, countof(pInfo->achName)-1, m_filter_info_string.GetString());
    }
    return hr;
}

STDMETHODIMP XySubFilter::Pause()
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(this));
    HRESULT hr = NOERROR;
    CAutoLock lck(&m_csFilter);
    if (!m_not_first_pause)
    {
        m_not_first_pause = true;
        if (m_pSubStreams.GetCount()>0 || m_xy_int_opt[INT_LOAD_SETTINGS_LEVEL]==LOADLEVEL_ALWAYS)
        {
            hr = FindAndConnectConsumer(m_pGraph);
            if (hr == S_FALSE && m_stood_down_owing_to_other_xy)
            {
                // Intentional stand-down (another Xy-family filter is active):
                // keep the player alive, do not attempt to stream. The guard
                // already logged the reason.
                XY_LOG_INFO("Stood down; another Xy-family subtitle filter owns the consumer.");
                m_filter_info_string.Format(L"%s (=> stood down)", m_xy_str_opt[STRING_NAME].GetString());
                return CBaseFilter::Pause();
            }
            if (FAILED(hr) || !m_consumer)
            {
                m_filter_info_string.Format(L"%s (=> !!!)", m_xy_str_opt[STRING_NAME].GetString());
                XY_LOG_ERROR("Failed when find and connect consumer");
            }
        }
        if (SUCCEEDED(hr))
        {
            hr = StartStreaming();
            if (FAILED(hr))
            {
                XY_LOG_ERROR("Failed to StartStreaming."<<XY_LOG_VAR_2_STR(hr));
                return hr;
            }
        }
    }
    return CBaseFilter::Pause();
}

//
// XyOptionsImpl
//

HRESULT XySubFilter::OnOptionChanged( unsigned field )
{
    HRESULT hr = DirectVobSubImpl::OnOptionChanged(field);
    if (FAILED(hr))
    {
        return hr;
    }
    switch(field)
    {
    case SIZE_ASS_PLAY_RESOLUTION:
    case SIZE_USER_SPECIFIED_LAYOUT_SIZE:
    case DOUBLE_FPS:
    case BOOL_SUB_FRAME_USE_DST_ALPHA:
        m_context_id++;
        InvalidateSubtitle();
        break;
    case STRING_FILE_NAME:
        if (!Open())
        {
            m_xy_str_opt[STRING_FILE_NAME].Empty();
            hr = E_FAIL;
            break;
        }
        if (!m_consumer && !m_fLoading)
        {
            if (FAILED(FindAndConnectConsumer(m_pGraph)))
            {
                XY_LOG_INFO("Failed when find and connect consumer");
            }
        }
        m_context_id++;
        break;
    case STRING_FILE_NAME_SECONDARY:
    case BOOL_DUAL_SUB_ENABLED:
    case INT_SECONDARY_TOP_MARGIN:
    case BOOL_SECONDARY_ALIGN_TOP:
    case INT_SECONDARY_SCALE:
    case INT_SECONDARY_OPACITY:
    case INT_SECONDARY_Z_ORDER:
    case INT_SECONDARY_VERTICAL:
    case INT_PRIMARY_VERTICAL:
    case INT_SECONDARY_EMBEDDED_PID:
        // Rebuild the multi-tracker with the new secondary configuration; the
        // primary is untouched, so this only costs a provider swap.
        UpdateMultiTracker(this);
        m_context_id++;
        InvalidateSubtitle();
        break;
    case STRING_SECONDARY_LANGUAGE_LIST:
        // A changed language list may make a different adjacent subtitle file
        // eligible, so re-run the (independent) secondary auto-load first.
        LoadSecondarySubtitle(m_pGraph);
        UpdateMultiTracker(this);
        m_context_id++;
        InvalidateSubtitle();
        break;
    case BOOL_STAND_DOWN_WHEN_ANOTHER_XY_FILTER:
        // The guard is exposed read/write so a player tool can force this filter
        // to take the consumer; when it is switched back off, retry the
        // connection we declined earlier.
        if (!m_xy_bool_opt[field] && !m_consumer && m_pGraph && !m_fLoading)
        {
            m_stood_down_owing_to_other_xy = false;
            if (FAILED(FindAndConnectConsumer(m_pGraph)))
            {
                XY_LOG_INFO("Re-connect attempt after stand-down did not succeed");
            }
        }
        break;
    case BOOL_OVERRIDE_PLACEMENT:
    case SIZE_PLACEMENT_PERC:
    case BOOL_FORCE_DEFAULT_STYLE:
        m_context_id++;
        UpdateSubtitle(false);
        break;
    case BOOL_HIDE_SUBTITLES:
        UpdateSubtitle(false);
        break;
    case BOOL_VOBSUBSETTINGS_BUFFER:
    case BOOL_VOBSUBSETTINGS_ONLY_SHOW_FORCED_SUBS:
    case BOOL_VOBSUBSETTINGS_POLYGONIZE:
    case BIN2_TEXT_SETTINGS:
    case BIN2_SUBTITLE_TIMING:
        m_context_id++;
        if (m_last_requested!=-1)
        {
            XY_LOG_WARN("Some subtitle frames are cached already!");
            m_last_requested = -1;
        }
        InvalidateSubtitle();
        break;
    case INT_RGB_OUTPUT_TV_LEVEL:
    case INT_COLOR_SPACE:
    case INT_YUV_RANGE:
        UpdateSubtitle(false);
        m_context_id++;
        break;
    case INT_OVERLAY_CACHE_MAX_ITEM_NUM:
        CacheManager::GetOverlayMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_SCAN_LINE_DATA_CACHE_MAX_ITEM_NUM:
        CacheManager::GetScanLineData2MruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_PATH_DATA_CACHE_MAX_ITEM_NUM:
        CacheManager::GetPathDataMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_OVERLAY_NO_BLUR_CACHE_MAX_ITEM_NUM:
        CacheManager::GetOverlayNoBlurMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_BITMAP_MRU_CACHE_ITEM_NUM:
        CacheManager::GetBitmapMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_CLIPPER_MRU_CACHE_ITEM_NUM:
        CacheManager::GetClipperAlphaMaskMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_TEXT_INFO_CACHE_ITEM_NUM:
        CacheManager::GetTextInfoCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_ASS_TAG_LIST_CACHE_ITEM_NUM:
        CacheManager::GetAssTagListMruCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_SUBPIXEL_VARIANCE_CACHE_ITEM_NUM:
        CacheManager::GetSubpixelVarianceCache()->SetMaxItemNum(m_xy_int_opt[field]);
        break;
    case INT_MAX_CACHE_SIZE_MB:
        {
            std::size_t max_size = m_xy_int_opt[INT_MAX_CACHE_SIZE_MB] >= 0 ?
                                   m_xy_int_opt[INT_MAX_CACHE_SIZE_MB] : m_xy_int_opt[INT_AUTO_MAX_CACHE_SIZE_MB];
            max_size = max_size < (SIZE_MAX>>20) ? (max_size<<20) : SIZE_MAX;
            CRenderedTextSubtitle::SetMaxCacheSize(max_size);
        }
        break;
    case INT_SUBPIXEL_POS_LEVEL:
        SubpixelPositionControler::GetGlobalControler().SetSubpixelLevel( static_cast<SubpixelPositionControler::SUBPIXEL_LEVEL>(m_xy_int_opt[field]) );
        m_context_id++;
        break;
    case INT_LAYOUT_SIZE_OPT:
        m_context_id++;
        InvalidateSubtitle();
        break;
    case INT_MAX_BITMAP_COUNT2:
        hr = E_INVALIDARG;
        break;
    case INT_SELECTED_LANGUAGE:
    case BIN2_CUR_STYLES:
        UpdateSubtitle(false);
        m_context_id++;
        break;
    }

    return hr;
}

HRESULT XySubFilter::DoGetField( unsigned field, void *value )
{
    HRESULT hr = NOERROR;
    switch(field)
    {
    case STRING_NAME:
    case STRING_VERSION:
        //they're read only, no need to lock
        hr = XyOptionsImpl::DoGetField(field, value);
        break;
    case STRING_YUV_MATRIX:
    case STRING_OUTPUT_LEVELS:
    case BOOL_COMBINE_BITMAPS:
    case BOOL_IS_MOVABLE:
    case BOOL_IS_BITMAP:
    case BOOL_SUB_FRAME_USE_DST_ALPHA:
        {
            CAutoLock cAutolock(&m_csProviderFields);//do NOT hold m_csSubLock so that it is faster
            hr = XyOptionsImpl::DoGetField(field, value);
        }
        break;
    case INT_LANGUAGE_COUNT:
        {
            CAutoLock cAutoLock(&m_csFilter);
            UpdateLanguageCount();
            hr = DirectVobSubImpl::DoGetField(field, value);
        }
        break;
    case INT_MAX_BITMAP_COUNT2:
        {
            CAutoLock cAutoLock(&m_csFilter);
            if (m_xy_bool_opt[BOOL_COMBINE_BITMAPS])
            {
                *(int*)value = 1;
            }
            else
            {
                *(int*)value = m_xy_int_opt[INT_MAX_BITMAP_COUNT];
            }
        }
        break;
    case INT_CUR_STYLES_COUNT:
        {
            CAutoLock cAutoLock(&m_csFilter);
            if (dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream)!=NULL)
            {
                hr = S_OK;
                CRenderedTextSubtitle * rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream);
                *(int*)value = rts->m_styles.GetCount();
            }
            else
            {
                hr = S_FALSE;
                *(int*)value = 0;
            }
        }
        break;
    default:
        hr = DirectVobSubImpl::DoGetField(field, value);
        break;
    }
    return hr;
}

//
// IXyOptions
//

STDMETHODIMP XySubFilter::XyGetString( unsigned field, LPWSTR *value, int *chars )
{
    switch(field)
    {
    case STRING_CONNECTED_CONSUMER:
        {
            CAutoLock cAutoLock(&m_csConsumer);
            if (m_consumer)
            {
                return m_consumer->GetString("name", value, chars);
            }
        }

        break;
    case STRING_CONSUMER_VERSION:
        {
            CAutoLock cAutoLock(&m_csConsumer);
            if (m_consumer)
            {
                return m_consumer->GetString("version", value, chars);
            }
        }
        break;
    }
    return DirectVobSubImpl::XyGetString(field, value,chars);
}

STDMETHODIMP XySubFilter::XySetBool(unsigned field, bool      value)
{
    switch(field)
    {
    case BOOL_COMBINE_BITMAPS:
    case BOOL_SUB_FRAME_USE_DST_ALPHA:
        {
            CAutoLock cAutolock1(&m_csFilter);
            CAutoLock cAutolock2(&m_csProviderFields);
            return XyOptionsImpl::XySetBool(field, value);
        }
        break;
    case BOOL_ALLOW_MOVING:
        {
            CAutoLock cAutolock1(&m_csFilter);
            CRenderedTextSubtitle * rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream);
            if (rts)
                m_xy_bool_opt[BOOL_IS_MOVABLE] = ((rts->IsMovable()) && ((rts->IsSimple()) || (value)));
        }
        break;
    }
    return DirectVobSubImpl::XySetBool(field, value);
}

//
// DirectVobSubImpl
//
HRESULT XySubFilter::GetCurStyles( SubStyle sub_style[], int count )
{
    CAutoLock cAutoLock(&m_csFilter);
    if (dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream)!=NULL)
    {
        CRenderedTextSubtitle * rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream);
        if (count != rts->m_styles.GetCount())
        {
            return E_INVALIDARG;
        }
        POSITION pos = rts->m_styles.GetStartPosition();
        int i = 0;
        while(pos)
        {
            CSTSStyleMap::CPair *pair = rts->m_styles.GetNext(pos);
            if (!pair)
            {
                return E_FAIL;
            }
            if (pair->m_key.GetLength()>=countof(sub_style[0].name))
            {
                return ERROR_BUFFER_OVERFLOW;
            }
            wcscpy_s(sub_style[i].name, pair->m_key.GetString());
            if (sub_style[i].style)
            {
                *(STSStyle*)(sub_style[i].style) = *pair->m_value;
            }
            i++;
        }
    }
    else
    {
        return S_FALSE;
    }
    return S_OK;
}

HRESULT XySubFilter::SetCurStyles( const SubStyle sub_style[], int count )
{
    HRESULT hr = NOERROR;
    CAutoLock cAutoLock(&m_csFilter);
    if (dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream)!=NULL)
    {
        CRenderedTextSubtitle * rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream);
        if (count != rts->m_styles.GetCount())
        {
            return E_INVALIDARG;
        }
        for (int i=0;i<count;i++)
        {
            if (!rts->m_styles.Lookup(sub_style[i].name))
            {
                return E_FAIL;
            }
        }
        bool changed = false;
        for (int i=0;i<count;i++)
        {
            STSStyle * style = NULL;
            rts->m_styles.Lookup(sub_style[i].name, style);
            ASSERT(style);
            if (sub_style[i].style)
            {
                *style = *static_cast<STSStyle*>(sub_style[i].style);
                changed = true;
            }
        }
        if (changed) {
            hr = OnOptionChanged(BIN2_CUR_STYLES);
        }
    }
    else
    {
        return E_FAIL;
    }
    return hr;
}

//
// IDirectVobSub
//
STDMETHODIMP XySubFilter::get_LanguageName(int iLanguage, WCHAR** ppName)
{
    HRESULT hr = DirectVobSubImpl::get_LanguageName(iLanguage, ppName);

    if(!ppName) return E_POINTER;

    if(hr == NOERROR)
    {
        CAutoLock cAutolock(&m_csFilter);

        hr = E_INVALIDARG;

        int i = iLanguage;

        POSITION pos = m_pSubStreams.GetHeadPosition();
        while(i >= 0 && pos)
        {
            CComPtr<ISubStream> pSubStream = m_pSubStreams.GetNext(pos);

            if(i < pSubStream->GetStreamCount())
            {
                pSubStream->GetStreamInfo(i, ppName, NULL);
                hr = NOERROR;
                break;
            }

            i -= pSubStream->GetStreamCount();
        }
    }

    return hr;
}

STDMETHODIMP XySubFilter::get_CachesInfo(CachesInfo* caches_info)
{
    XY_LOG_INFO(caches_info);
    CheckPointer(caches_info, S_FALSE);
    CAutoLock cAutolock(&m_csFilter);

    caches_info->path_cache_cur_item_num    = CacheManager::GetPathDataMruCache()->GetCurItemNum();
    caches_info->path_cache_hit_count       = CacheManager::GetPathDataMruCache()->GetCacheHitCount();
    caches_info->path_cache_query_count     = CacheManager::GetPathDataMruCache()->GetQueryCount();
    caches_info->scanline_cache2_cur_item_num= CacheManager::GetScanLineData2MruCache()->GetCurItemNum();
    caches_info->scanline_cache2_hit_count   = CacheManager::GetScanLineData2MruCache()->GetCacheHitCount();
    caches_info->scanline_cache2_query_count = CacheManager::GetScanLineData2MruCache()->GetQueryCount();
    caches_info->non_blur_cache_cur_item_num= CacheManager::GetOverlayNoBlurMruCache()->GetCurItemNum();
    caches_info->non_blur_cache_hit_count   = CacheManager::GetOverlayNoBlurMruCache()->GetCacheHitCount();
    caches_info->non_blur_cache_query_count = CacheManager::GetOverlayNoBlurMruCache()->GetQueryCount();
    caches_info->overlay_cache_cur_item_num = CacheManager::GetOverlayMruCache()->GetCurItemNum();
    caches_info->overlay_cache_hit_count    = CacheManager::GetOverlayMruCache()->GetCacheHitCount();
    caches_info->overlay_cache_query_count  = CacheManager::GetOverlayMruCache()->GetQueryCount();

    caches_info->bitmap_cache_cur_item_num  = CacheManager::GetBitmapMruCache()->GetCurItemNum();
    caches_info->bitmap_cache_hit_count     = CacheManager::GetBitmapMruCache()->GetCacheHitCount();
    caches_info->bitmap_cache_query_count   = CacheManager::GetBitmapMruCache()->GetQueryCount();

    caches_info->interpolate_cache_cur_item_num = CacheManager::GetSubpixelVarianceCache()->GetCurItemNum();
    caches_info->interpolate_cache_hit_count    = CacheManager::GetSubpixelVarianceCache()->GetCacheHitCount();
    caches_info->interpolate_cache_query_count  = CacheManager::GetSubpixelVarianceCache()->GetQueryCount();    
    caches_info->text_info_cache_cur_item_num   = CacheManager::GetTextInfoCache()->GetCurItemNum();
    caches_info->text_info_cache_hit_count      = CacheManager::GetTextInfoCache()->GetCacheHitCount();
    caches_info->text_info_cache_query_count    = CacheManager::GetTextInfoCache()->GetQueryCount();

    caches_info->word_info_cache_cur_item_num   = CacheManager::GetAssTagListMruCache()->GetCurItemNum();
    caches_info->word_info_cache_hit_count      = CacheManager::GetAssTagListMruCache()->GetCacheHitCount();
    caches_info->word_info_cache_query_count    = CacheManager::GetAssTagListMruCache()->GetQueryCount();    

    caches_info->scanline_cache_cur_item_num = CacheManager::GetScanLineDataMruCache()->GetCurItemNum();
    caches_info->scanline_cache_hit_count    = CacheManager::GetScanLineDataMruCache()->GetCacheHitCount();
    caches_info->scanline_cache_query_count  = CacheManager::GetScanLineDataMruCache()->GetQueryCount();

    caches_info->overlay_key_cache_cur_item_num = CacheManager::GetOverlayNoOffsetMruCache()->GetCurItemNum();
    caches_info->overlay_key_cache_hit_count = CacheManager::GetOverlayNoOffsetMruCache()->GetCacheHitCount();
    caches_info->overlay_key_cache_query_count = CacheManager::GetOverlayNoOffsetMruCache()->GetQueryCount();

    caches_info->clipper_cache_cur_item_num = CacheManager::GetClipperAlphaMaskMruCache()->GetCurItemNum();
    caches_info->clipper_cache_hit_count    = CacheManager::GetClipperAlphaMaskMruCache()->GetCacheHitCount();
    caches_info->clipper_cache_query_count  = CacheManager::GetClipperAlphaMaskMruCache()->GetQueryCount();

    return S_OK;
}

STDMETHODIMP XySubFilter::get_XyFlyWeightInfo( XyFlyWeightInfo* xy_fw_info )
{
    XY_LOG_INFO(xy_fw_info);
    CheckPointer(xy_fw_info, S_FALSE);
    CAutoLock cAutolock(&m_csFilter);

    xy_fw_info->xy_fw_string_w.cur_item_num = XyFwStringW::GetCacher()->GetCurItemNum();
    xy_fw_info->xy_fw_string_w.hit_count = XyFwStringW::GetCacher()->GetCacheHitCount();
    xy_fw_info->xy_fw_string_w.query_count = XyFwStringW::GetCacher()->GetQueryCount();

    xy_fw_info->xy_fw_grouped_draw_items_hash_key.cur_item_num = XyFwGroupedDrawItemsHashKey::GetCacher()->GetCurItemNum();
    xy_fw_info->xy_fw_grouped_draw_items_hash_key.hit_count = XyFwGroupedDrawItemsHashKey::GetCacher()->GetCacheHitCount();
    xy_fw_info->xy_fw_grouped_draw_items_hash_key.query_count = XyFwGroupedDrawItemsHashKey::GetCacher()->GetQueryCount();

    return S_OK;
}

//
// IDirectVobSub2
//
STDMETHODIMP XySubFilter::put_TextSettings(STSStyle* pDefStyle)
{
    XY_LOG_INFO(pDefStyle);
    HRESULT hr = DirectVobSubImpl::put_TextSettings(pDefStyle);

    if(hr == NOERROR)
    {
        UpdateSubtitle(false);
    }

    return hr;
}

STDMETHODIMP XySubFilter::put_AspectRatioSettings(CSimpleTextSubtitle::EPARCompensationType* ePARCompensationType)
{
    return E_NOTIMPL;
}

//
// ISpecifyPropertyPages
// 
STDMETHODIMP XySubFilter::GetPages(CAUUID* pPages)
{
    XY_LOG_INFO(pPages);
    CheckPointer(pPages, E_POINTER);

    pPages->cElems = 6;
    pPages->pElems = (GUID*)CoTaskMemAlloc(sizeof(GUID)*pPages->cElems);

    if(pPages->pElems == NULL) return E_OUTOFMEMORY;

    int i = 0;
    pPages->pElems[i++] = __uuidof(CXySubFilterMainPPage);
    pPages->pElems[i++] = __uuidof(CXySubFilterDualPPage);
    pPages->pElems[i++] = __uuidof(CXySubFilterMorePPage);
    pPages->pElems[i++] = __uuidof(CXySubFilterTimingPPage);
    pPages->pElems[i++] = __uuidof(CXySubFilterPathsPPage);
    pPages->pElems[i++] = __uuidof(CXySubFilterAboutPPage);

    return NOERROR;
}

//
// IAMStreamSelect
//
STDMETHODIMP XySubFilter::Count(DWORD* pcStreams)
{
    XY_LOG_INFO(pcStreams);
    if(!pcStreams) return E_POINTER;

    *pcStreams = 0;

    int nLangs = 0;
    if(SUCCEEDED(get_LanguageCount(&nLangs)))
        (*pcStreams) += nLangs;

    (*pcStreams) += 2; // enable disable force_default_style

    //fix me: support subtitle flipping
    //(*pcStreams) += 2; // normal flipped

    return S_OK;
}

STDMETHODIMP XySubFilter::Enable(long lIndex, DWORD dwFlags)
{
    CAutoLock cAutolock(&m_csFilter);
    XY_LOG_INFO(XY_LOG_VAR_2_STR(lIndex)<<XY_LOG_VAR_2_STR(dwFlags));
    HRESULT hr = NOERROR;
    if(!(dwFlags & AMSTREAMSELECTENABLE_ENABLE))
        return E_NOTIMPL;

    int nLangs = 0;
    hr = get_LanguageCount(&nLangs);
    CHECK_N_LOG(hr, "Failed to get option");

    if(!(lIndex >= 0 && lIndex < nLangs+2 /* +2 fix me: support subtitle flipping */))
        return E_INVALIDARG;

    int i = lIndex-1;

    if(i == -1) // we need this because when loading something stupid media player pushes the first stream it founds, which is "enable" in our case
    {
        if (!m_fLoading)
        {
            hr = put_HideSubtitles(false);
        }
        else
        {
            //fix me: support even when loading
            XY_LOG_ERROR("Do NOT do this when we are loading");
            return E_FAIL;
        }
    }
    else if(i >= 0 && i < nLangs)
    {
        hr = put_HideSubtitles(false);
        CHECK_N_LOG(hr, "Failed to set option");
        hr = put_SelectedLanguage(i);
        CHECK_N_LOG(hr, "Failed to set option");

        WCHAR* pName = NULL;
        hr = get_LanguageName(i, &pName);
        if(SUCCEEDED(hr))
        {
            UpdatePreferedLanguages(CString(pName));
            if(pName) CoTaskMemFree(pName);
        }
    }
    else if(i == nLangs)
    {
        if (!m_fLoading)
        {
            hr = put_HideSubtitles(true);
        }
        else
        {
            XY_LOG_ERROR("Do NOT do this when we are loading");
            return E_FAIL;
        }
    }
    else if(i == nLangs+1 || i == nLangs+2)
    {
        if (!m_fLoading)
        {
            hr = put_Flip(i == nLangs+2, m_xy_bool_opt[BOOL_FLIP_SUBTITLE]);
        }
        else
        {
            XY_LOG_ERROR("Do NOT do this when we are loading");
            return E_FAIL;
        }
    }
    CHECK_N_LOG(hr, "Failed "<<XY_LOG_VAR_2_STR(lIndex)<<XY_LOG_VAR_2_STR(dwFlags));
    return hr;
}

STDMETHODIMP XySubFilter::Info(long lIndex, AM_MEDIA_TYPE** ppmt, DWORD* pdwFlags, LCID* plcid, DWORD* pdwGroup, WCHAR** ppszName, IUnknown** ppObject, IUnknown** ppUnk)
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(lIndex)<<XY_LOG_VAR_2_STR(ppmt)
        <<XY_LOG_VAR_2_STR(pdwFlags)<<XY_LOG_VAR_2_STR(plcid)<<XY_LOG_VAR_2_STR(pdwGroup)<<XY_LOG_VAR_2_STR(ppszName)
        <<XY_LOG_VAR_2_STR(ppObject)<<XY_LOG_VAR_2_STR(ppUnk));
    HRESULT hr = NOERROR;
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    const int FLAG_CMD = 1;
    const int FLAG_EXTERNAL_SUB = 2;
    const int FLAG_PICTURE_CMD = 4;
    const int FLAG_VISIBILITY_CMD = 8;

    const int GROUP_NUM_BASE = 0x648E40;//random number

    int nLangs = 0;
    hr = get_LanguageCount(&nLangs);
    CHECK_N_LOG(hr, "Failed to get option");

    if(!(lIndex >= 0 && lIndex < nLangs+2 /* +2 fix me: support subtitle flipping */))
        return E_INVALIDARG;

    int i = lIndex-1;

    if(ppmt) 
    {
        //fix me: not supported yet
        *ppmt = NULL;
        //if(i >= 0 && i < nLangs)
        //{
        //    *ppmt = CreateMediaType(&m_pSubtitleInputPin[i]->m_mt);
        //}
    }

    if(pdwFlags)
    {
        *pdwFlags = 0;

        if(i == -1 && !m_xy_bool_opt[BOOL_HIDE_SUBTITLES]
            || i >= 0 && i < nLangs && i == m_xy_int_opt[INT_SELECTED_LANGUAGE]
            || i == nLangs && m_xy_bool_opt[BOOL_HIDE_SUBTITLES]
            || i == nLangs+1 && !m_xy_bool_opt[BOOL_FLIP_PICTURE]
            || i == nLangs+2 && m_xy_bool_opt[BOOL_FLIP_PICTURE])
        {
            *pdwFlags |= AMSTREAMSELECTINFO_ENABLED;
        }
    }

    if(plcid) *plcid = 0;

    if(pdwGroup)
    {
        *pdwGroup = GROUP_NUM_BASE;
        if(i == -1)
        {
            *pdwGroup = GROUP_NUM_BASE | FLAG_CMD | FLAG_VISIBILITY_CMD;
        }
        else if(i >= 0 && i < nLangs)
        {
            bool isEmbedded = false;
            hr = GetIsEmbeddedSubStream(i, &isEmbedded);
            ASSERT(SUCCEEDED(hr));
            if(isEmbedded)
            {
                *pdwGroup = GROUP_NUM_BASE & ~(FLAG_CMD | FLAG_EXTERNAL_SUB);
            }
            else
            {
                *pdwGroup = (GROUP_NUM_BASE & ~FLAG_CMD) | FLAG_EXTERNAL_SUB;
            }
        }
        else if(i == nLangs)
        {
            *pdwGroup = GROUP_NUM_BASE | FLAG_CMD | FLAG_VISIBILITY_CMD;
        }
        else if(i == nLangs+1)
        {
            *pdwGroup = GROUP_NUM_BASE | FLAG_CMD | FLAG_PICTURE_CMD;
        }
        else if(i == nLangs+2)
        {
            *pdwGroup = GROUP_NUM_BASE | FLAG_CMD | FLAG_PICTURE_CMD;
        }
    }

    if(ppszName)
    {
        *ppszName = NULL;

        CStringW str;
        if(i == -1) str = ResStr(IDS_M_SHOWSUBTITLES);
        else if(i >= 0 && i < nLangs)
        {
            hr = get_LanguageName(i, ppszName);
            CHECK_N_LOG(hr, "Failed to get option");
        }
        else if(i == nLangs)
        {
            str = ResStr(IDS_M_HIDESUBTITLES);
        }
        else if(i == nLangs+1)
        {
            str = ResStr(IDS_M_ORIGINALPICTURE);
        }
        else if(i == nLangs+2)
        {
            str = ResStr(IDS_M_FLIPPEDPICTURE);
        }

        if(!str.IsEmpty())
        {
            *ppszName = (WCHAR*)CoTaskMemAlloc((str.GetLength()+1)*sizeof(WCHAR));
            if(*ppszName == NULL) return S_FALSE;
            wcscpy_s(*ppszName, str.GetLength()+1, str);
        }
    }

    if(ppObject) *ppObject = NULL;

    if(ppUnk) *ppUnk = NULL;

    return hr;
}

//
// FRD
// 
void XySubFilter::SetupFRD(CStringArray& paths, CAtlArray<HANDLE>& handles)
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(paths.GetCount())<<XY_LOG_VAR_2_STR(handles.GetCount()));
    CAutoLock cAutolock(&m_csFilter);

    for(unsigned i = 2; i < handles.GetCount(); i++)
    {
        FindCloseChangeNotification(handles[i]);
    }

    paths.RemoveAll();
    handles.RemoveAll();

    handles.Add(m_frd.EndThreadEvent);
    handles.Add(m_frd.RefreshEvent);

    m_frd.mtime.SetCount(m_frd.files.GetCount());

    POSITION pos = m_frd.files.GetHeadPosition();
    for(unsigned i = 0; pos; i++)
    {
        CString fn = m_frd.files.GetNext(pos);

        CFileStatus status;
        if(CFileGetStatus(fn, status))
            m_frd.mtime[i] = status.m_mtime;

        fn.Replace('\\', '/');
        fn = fn.Left(fn.ReverseFind('/')+1);

        bool fFound = false;

        for(int j = 0; !fFound && j < paths.GetCount(); j++)
        {
            if(paths[j] == fn) fFound = true;
        }

        if(!fFound)
        {
            paths.Add(fn);

            HANDLE h = FindFirstChangeNotification(fn, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
            if(h != INVALID_HANDLE_VALUE) handles.Add(h);
        }
    }
}

DWORD XySubFilter::ThreadProc()
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(this));
    SetThreadPriority(m_hThread, THREAD_PRIORITY_LOWEST/*THREAD_PRIORITY_BELOW_NORMAL*/);

    CStringArray paths;
    CAtlArray<HANDLE> handles;

    SetupFRD(paths, handles);
    m_frd.ThreadStartedEvent.Set();
    while(1)
    {
        DWORD idx = WaitForMultipleObjects(handles.GetCount(), handles.GetData(), FALSE, INFINITE);

        if(idx == (WAIT_OBJECT_0 + 0)) // m_frd.hEndThreadEvent
        {
            break;
        }
        if(idx == (WAIT_OBJECT_0 + 1)) // m_frd.hRefreshEvent
        {
            SetupFRD(paths, handles);
        }
        else if(idx >= (WAIT_OBJECT_0 + 2) && idx < (WAIT_OBJECT_0 + handles.GetCount()))
        {
            bool fLocked = true;
            IsSubtitleReloaderLocked(&fLocked);
            if(fLocked) continue;

            if(FindNextChangeNotification(handles[idx - WAIT_OBJECT_0]) == FALSE)
                break;

            int j = 0;

            POSITION pos = m_frd.files.GetHeadPosition();
            for(int i = 0; pos && j == 0; i++)
            {
                CString fn = m_frd.files.GetNext(pos);

                CFileStatus status;
                if(CFileGetStatus(fn, status) && m_frd.mtime[i] != status.m_mtime)
                {
                    for(j = 0; j < 10; j++)
                    {
                        FILE* f = NULL;
                        if(!_tfopen_s(&f, fn, _T("rb+")))
                        {
                            fclose(f);
                            j = 0;
                            break;
                        }
                        else
                        {
                            Sleep(100);
                            j++;
                        }
                    }
                }
            }

            if(j > 0)
            {
                SetupFRD(paths, handles);
            }
            else
            {
                Sleep(500);

                POSITION pos = m_frd.files.GetHeadPosition();
                for(int i = 0; pos; i++)
                {
                    CFileStatus status;
                    if(CFileGetStatus(m_frd.files.GetNext(pos), status)
                        && m_frd.mtime[i] != status.m_mtime)
                    {
                        Open();
                        SetupFRD(paths, handles);
                        break;
                    }
                }
            }
        }
        else
        {
            break;
        }
    }

    for(unsigned i = 2; i < handles.GetCount(); i++)
    {
        FindCloseChangeNotification(handles[i]);
    }

    return 0;
}

//
// ISubRenderProvider
//

STDMETHODIMP XySubFilter::RequestFrame( REFERENCE_TIME start, REFERENCE_TIME stop, LPVOID context )
{
    TRACE_RENDERER_REQUEST_TIMING("Look up subpic for start:"<<start
        <<"("<<ReftimeToCString(start)<<")"
        <<" stop:"<<stop<<"("<<ReftimeToCString(stop)<<XY_LOG_VAR_2_STR((int)context));

    HRESULT hr;
    CComPtr<IXySubRenderFrame> sub_render_frame;
    {
        CAutoLock cAutoLock(&m_csFilter);

        hr = UpdateParamFromConsumer();
        if (FAILED(hr))
        {
            XY_LOG_ERROR("Failed to get parameters from consumer.");
            return hr;
        }

        // no, it won't overflow even without normalizing if we use normal parameters
        // (__int64 is enough for about 2000 hours if we multiply it by the max: 65536 as m_SubtitleSpeedMul)
        // anyway, m_SubtitleSpeed and m_SubtitleSpeedDiv parameters are normalized upon read
        start = (start - 10000i64 * m_SubtitleDelay) * m_SubtitleSpeedNormalizedMul / m_SubtitleSpeedNormalizedDiv;
        stop = (stop - 10000i64 * m_SubtitleDelay) * m_SubtitleSpeedNormalizedMul / m_SubtitleSpeedNormalizedDiv;

        REFERENCE_TIME now = start; //NOTE: It seems that the physically right way is (start + stop) / 2, but...
        m_last_requested = now;

        if(m_multi_tracker)
        {
            // N-track compositor: returns the primary frame directly when only
            // one track is active, otherwise a merged multi-bitmap frame.
            hr = m_multi_tracker->RequestFrame(&sub_render_frame, now);
            if (FAILED(hr))
            {
                XY_LOG_ERROR("Failed to RequestFrame."<<XY_LOG_VAR_2_STR(hr));
                return hr;
            }
            if (sub_render_frame)
            {
                sub_render_frame = DEBUG_NEW XySubRenderFrameWrapper2(sub_render_frame, m_context_id);
#ifdef SUBTITLE_FRAME_DUMP_FILE
                if (now >= m_dump_subtitle_frame_start_rt && now < m_dump_subtitle_frame_end_rt)
                {
                    CStringA dump_file;
                    dump_file.Format("%s%lld",SUBTITLE_FRAME_DUMP_FILE,now);
                    if (m_dump_subtitle_frame_to_txt &&
                        FAILED(DumpSubRenderFrame( sub_render_frame, dump_file.GetString() )))
                    {
                        XY_LOG_ERROR("Failed to dump subtitle frame to txt file: "<<dump_file.GetString());
                    }
                    CString dump_bitmap(dump_file);
                    if ((m_dump_subtitle_frame_alpha_channel || m_dump_subtitle_frame_rgb_bmp) &&
                        FAILED(
                        DumpToBitmap( sub_render_frame,
                                      dump_bitmap.GetString(),
                                      m_dump_subtitle_frame_alpha_channel,
                                      m_dump_subtitle_frame_rgb_bmp,
                                      m_dump_subtitle_frame_background_color)))
                    {
                        XY_LOG_ERROR("Failed to dump subtitle frame to file: "<<dump_bitmap.GetString());
                    }
                }
#endif
            }
            if(m_xy_bool_opt[BOOL_FLIP_SUBTITLE])
            {
                //fix me:
                ASSERT(0);
            }

            CRenderedTextSubtitle * rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream);
            m_xy_bool_opt[BOOL_IS_MOVABLE] = (!rts) || ((rts->IsMovable()) && ((rts->IsSimple()) || (m_xy_bool_opt[BOOL_ALLOW_MOVING])));

        }
    }
    CAutoLock cAutoLock(&m_csConsumer);
    //fix me: print osd message
    TRACE_RENDERER_REQUEST("Returnning "<<XY_LOG_VAR_2_STR(hr)<<XY_LOG_VAR_2_STR(sub_render_frame));
    hr =  m_consumer->DeliverFrame(start, stop, context, sub_render_frame);
    return hr;
}

STDMETHODIMP XySubFilter::Disconnect( void )
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(this));
    //If the consumer also calls provider->Disconnect inside its Disconnect implementation, 
    //we won't fall into a dead loop.
    if (!m_disconnect_entered)
    {
        CAutoLock cAutoLock(&m_csFilter);
        m_disconnect_entered = true;
        ASSERT(m_consumer);
        if (m_consumer)
        {
            HRESULT hr = m_consumer->Disconnect();
            if (FAILED(hr))
            {
                XY_LOG_WARN("Failed to disconnect with consumer!");
            }
            CAutoLock cAutoLock(&m_csConsumer);
            m_consumer = NULL;
        }
        else
        {
            XY_LOG_ERROR("No consumer connected. It is expected to be called by a consumer!");
        }
        m_filter_info_string = m_xy_str_opt[STRING_NAME];
        m_disconnect_entered = false;
        return S_OK;
    }
    return S_FALSE;
}

//
// XySubFilter
//
void XySubFilter::SetYuvMatrix()
{
    CAutoLock cAutolock(&m_csFilter);
    CAutoLock cAutoLock(&m_csProviderFields);
    m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
    if (dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream)!=NULL) {
        ColorConvTable::YuvMatrixType yuv_matrix = ColorConvTable::BT601;
        ColorConvTable::YuvRangeType  yuv_range  = ColorConvTable::RANGE_TV;
        if ( m_xy_int_opt[INT_COLOR_SPACE]==DirectVobSubImpl::YuvMatrix_AUTO )
        {
            if (m_video_yuv_matrix_decided_by_sub!=ColorConvTable::NONE)
            {
                yuv_matrix = m_video_yuv_matrix_decided_by_sub;
            }
            else
            {
                if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"601")==0)
                {
                    yuv_matrix = ColorConvTable::BT601;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"709")==0)
                {
                    yuv_matrix = ColorConvTable::BT709;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(4).CompareNoCase(L"2020")==0)
                {
                    yuv_matrix = ColorConvTable::BT2020;
                }
                else
                {
                    XY_LOG_WARN(L"Can NOT get useful YUV range from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString());
                    yuv_matrix = (m_xy_size_opt[SIZE_ORIGINAL_VIDEO].cx > m_bt601Width || 
                        m_xy_size_opt[SIZE_ORIGINAL_VIDEO].cy > m_bt601Height) ? ColorConvTable::BT709 : ColorConvTable::BT601;
                }
            }
        }
        else
        {
            switch(m_xy_int_opt[INT_COLOR_SPACE])
            {
            case DirectVobSubImpl::BT_601:
                yuv_matrix = ColorConvTable::BT601;
                break;
            case DirectVobSubImpl::BT_709:
                yuv_matrix = ColorConvTable::BT709;
                break;
            case DirectVobSubImpl::BT_2020:
                yuv_matrix = ColorConvTable::BT2020;
                break;
            case DirectVobSubImpl::GUESS:
            default:
                if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"601")==0)
                {
                    yuv_matrix = ColorConvTable::BT601;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"709")==0)
                {
                    yuv_matrix = ColorConvTable::BT709;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(4).CompareNoCase(L"2020")==0)
                {
                    yuv_matrix = ColorConvTable::BT2020;
                }
                else
                {
                    XY_LOG_WARN(L"Can NOT get useful YUV range from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString());
                    yuv_matrix = (m_xy_size_opt[SIZE_ORIGINAL_VIDEO].cx > m_bt601Width || 
                        m_xy_size_opt[SIZE_ORIGINAL_VIDEO].cy > m_bt601Height) ? ColorConvTable::BT709 : ColorConvTable::BT601;
                }
                break;
            }
        }

        if( m_xy_int_opt[INT_YUV_RANGE]==DirectVobSubImpl::YuvRange_Auto )
        {
            if (m_video_yuv_range_decided_by_sub!=ColorConvTable::RANGE_NONE)
                yuv_range = m_video_yuv_range_decided_by_sub;
            else
                yuv_range = ColorConvTable::RANGE_TV;
        }
        else
        {
            switch(m_xy_int_opt[INT_YUV_RANGE])
            {
            case DirectVobSubImpl::YuvRange_TV:
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            case DirectVobSubImpl::YuvRange_PC:
                yuv_range = ColorConvTable::RANGE_PC;
                break;
            case DirectVobSubImpl::YuvRange_Auto:
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            }
        }

        ColorConvTable::SetDefaultConvType(yuv_matrix, yuv_range);
        XySubRenderFrameCreater * sub_frame_creater = XySubRenderFrameCreater::GetDefaultCreater();

        if ( m_xy_int_opt[INT_VSFILTER_COMPACT_RGB_CORRECTION]==RGB_CORRECTION_ALWAYS ||
            (m_xy_int_opt[INT_VSFILTER_COMPACT_RGB_CORRECTION]==RGB_CORRECTION_AUTO &&
            m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].CompareNoCase(L"TV.709")==0 &&
            yuv_matrix==ColorConvTable::BT601 && yuv_range==ColorConvTable::RANGE_TV ))
        {
            sub_frame_creater->SetVsfilterCompactRgbCorrection(true);
            m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
            return;
        }
        else if (m_video_yuv_matrix_decided_by_sub==ColorConvTable::NONE)
        {
            m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
            return;
        }

        if (yuv_range==ColorConvTable::RANGE_TV) {
            m_xy_str_opt[STRING_YUV_MATRIX] = L"TV";
        }
        else if (yuv_range==ColorConvTable::RANGE_PC) {
            m_xy_str_opt[STRING_YUV_MATRIX] = L"PC";
        }
        else {
            XY_LOG_WARN("This is unexpected."<<XY_LOG_VAR_2_STR(yuv_range));
            m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
            return;
        }
        if (yuv_matrix==ColorConvTable::BT601) {
            m_xy_str_opt[STRING_YUV_MATRIX] += L".601";
        }
        else if (yuv_matrix==ColorConvTable::BT709) {
            m_xy_str_opt[STRING_YUV_MATRIX] += L".709";
        }
        else if (yuv_matrix==ColorConvTable::BT2020) {
            m_xy_str_opt[STRING_YUV_MATRIX] += L".2020";
        }
        else {
            XY_LOG_WARN("This is unexpected."<<XY_LOG_VAR_2_STR(yuv_matrix));
            m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
            return;
        }
        sub_frame_creater->SetVsfilterCompactRgbCorrection(false);
    }
    else if (dynamic_cast<HdmvSubtitleProvider*>(m_curSubStream)!=NULL
        || dynamic_cast<SupFileSubtitleProvider*>(m_curSubStream)!=NULL)
    {
        if ( m_xy_str_opt[STRING_PGS_YUV_RANGE].CompareNoCase(_T("PC"))==0 )
        {
            m_xy_str_opt[STRING_YUV_MATRIX] = L"PC";
        }
        else if ( m_xy_str_opt[STRING_PGS_YUV_RANGE].CompareNoCase(_T("TV"))==0 )
        {
            m_xy_str_opt[STRING_YUV_MATRIX] = L"TV";
        }
        else
        {
            if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Left(2).CompareNoCase(L"TV")==0)
            {
                m_xy_str_opt[STRING_YUV_MATRIX] = L"TV";
            }
            else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Left(2).CompareNoCase(L"PC")==0)
            {
                m_xy_str_opt[STRING_YUV_MATRIX] = L"PC";
            }
            else
            {
                XY_LOG_WARN(L"Can NOT get useful YUV range from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString());
                m_xy_str_opt[STRING_YUV_MATRIX] = L"TV";
            }
        }
        if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT601"))==0 )
        {
            m_xy_str_opt[STRING_YUV_MATRIX] += L"601";
        }
        else if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT709"))==0 )
        {
            m_xy_str_opt[STRING_YUV_MATRIX] += L"709";
        }
        else if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT2020"))==0 )
        {
            m_xy_str_opt[STRING_YUV_MATRIX] += L"2020";
        }
        else
        {
            if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"601")==0)
            {
                m_xy_str_opt[STRING_YUV_MATRIX] += L"601";
            }
            else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"709")==0)
            {
                m_xy_str_opt[STRING_YUV_MATRIX] += L"709";
            }
            else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(4).CompareNoCase(L"2020")==0)
            {
                m_xy_str_opt[STRING_YUV_MATRIX] += L"2020";
            }
            else
            {
                XY_LOG_WARN(L"Can NOT get useful YUV range from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString()
                    <<"A guess based on subtitle width would be done. We do NOT know the result before reveiving a sample.");
                m_xy_str_opt[STRING_YUV_MATRIX] = L"None";
            }
        }
    }
    else if (m_curSubStream)
    {
        m_xy_str_opt[STRING_YUV_MATRIX] = L"TV.601";
    }
}

void XySubFilter::SetRgbOutputLevel()
{
    const int NO_PREFERENCE = 1;
    const int PREFERED_TV = 3;

    CAutoLock cAutolock(&m_csFilter);
    CAutoLock cAutoLock(&m_csProviderFields);

    XySubRenderFrameCreater * sub_frame_creater = XySubRenderFrameCreater::GetDefaultCreater();
    if (dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream)!=NULL)
    {
        if (m_xy_int_opt[INT_RGB_OUTPUT_TV_LEVEL]==RGB_OUTPUT_LEVEL_AUTO)
        {
            if (m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]==PREFERED_TV)
            {
                sub_frame_creater->SetRgbOutputTvLevel(true);
            }
            else
            {
                sub_frame_creater->SetRgbOutputTvLevel(false);
            }
        }
        else if (m_xy_int_opt[INT_RGB_OUTPUT_TV_LEVEL]==RGB_OUTPUT_LEVEL_FORCE_TV)
        {
            sub_frame_creater->SetRgbOutputTvLevel(true);
        }
        else
        {
            sub_frame_creater->SetRgbOutputTvLevel(false);
        }

        m_xy_str_opt[STRING_OUTPUT_LEVELS] = sub_frame_creater->GetRgbOutputTvLevel() ? 
            L"TV" : L"PC";
    }
    else if (dynamic_cast<HdmvSubtitleProvider*>(m_curSubStream)!=NULL
        || dynamic_cast<SupFileSubtitleProvider*>(m_curSubStream)!=NULL)
    {
        if (m_xy_int_opt[INT_RGB_OUTPUT_TV_LEVEL]==RGB_OUTPUT_LEVEL_AUTO)
        {
            if (m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]==PREFERED_TV || 
                m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]==NO_PREFERENCE)
            {
                CompositionObject::m_output_tv_range_rgb = true;
            }
            else
            {
                CompositionObject::m_output_tv_range_rgb = false;
            }
        }
        else if (m_xy_int_opt[INT_RGB_OUTPUT_TV_LEVEL]==RGB_OUTPUT_LEVEL_FORCE_TV)
        {
            CompositionObject::m_output_tv_range_rgb = true;
        }
        else
        {
            CompositionObject::m_output_tv_range_rgb = false;
        }
    }
    else
    {
        m_xy_str_opt[STRING_OUTPUT_LEVELS] = L"PC";
    }
}

// Builds a subtitle source for one external file, using the stock loader
// cascade (text -> VobSub -> SSF -> SUP). Reusable for the primary enumeration
// and for the secondary track, so the two paths can never drift apart.
// Returns a referenced ISubStream, or NULL when no loader matched.
template<class TLock>
static CComPtr<ISubStream> CreateSubStreamFromFile(const CString& file_name, TLock* pLock
    , CAtlList<CString>* pExtraFiles /*=NULL*/)
{
    CComPtr<ISubStream> pSubStream;

    {
        XY_AUTO_TIMING(TEXT("CRenderedTextSubtitle::Open"));
        CAutoPtr<CRenderedTextSubtitle> pRTS(DEBUG_NEW CRenderedTextSubtitle(pLock));
        if(pRTS && pRTS->Open(file_name, DEFAULT_CHARSET) && pRTS->GetStreamCount() > 0)
        {
            pSubStream = pRTS.Detach();
            if (pExtraFiles)
            {
                pExtraFiles->AddTail(file_name + _T(".style"));
            }
        }
    }

    if(!pSubStream)
    {
        CAutoTiming t(TEXT("CVobSubFile::Open"), 0);
        CAutoPtr<CVobSubFile> pVSF(DEBUG_NEW CVobSubFile(pLock));
        if(pVSF && pVSF->Open(file_name) && pVSF->GetStreamCount() > 0)
        {
            pSubStream = pVSF.Detach();
            if (pExtraFiles)
            {
                pExtraFiles->AddTail(file_name.Left(file_name.GetLength()-4) + _T(".sub"));
            }
        }
    }

    if(!pSubStream)
    {
        CAutoTiming t(TEXT("ssf::CRenderer::Open"), 0);
        CAutoPtr<ssf::CRenderer> pSSF(DEBUG_NEW ssf::CRenderer(pLock));
        if(pSSF && pSSF->Open(file_name) && pSSF->GetStreamCount() > 0)
        {
            pSubStream = pSSF.Detach();
        }
    }

    if (!pSubStream)
    {
        CAutoPtr<SupFileSubtitleProvider> sup(DEBUG_NEW SupFileSubtitleProvider());
        if (sup && sup->Open(file_name) && sup->GetStreamCount() > 0)
        {
            pSubStream = sup.Detach();
        }
    }

    return pSubStream;
}

bool XySubFilter::Open()
{
    XY_AUTO_TIMING(TEXT("XySubFilter::Open"));

    HRESULT hr = NOERROR;
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    CAutoLock cAutolock(&m_csFilter);

    m_pSubStreams.RemoveAll();
    m_fIsSubStreamEmbeded.RemoveAll();

    m_frd.files.RemoveAll();

    CAtlArray<CString> paths;

    for(int i = 0; i < 10; i++)
    {
        CString tmp;
        tmp.Format(IDS_RP_PATH, i);
        CString path = theApp.GetProfileString(ResStr(IDS_R_DEFTEXTPATHES), tmp);
        if(!path.IsEmpty()) paths.Add(path);
    }

    CAtlArray<SubFile> ret;
    GetSubFileNames(m_xy_str_opt[STRING_FILE_NAME], paths, m_xy_str_opt[DirectVobSubXyOptions::STRING_LOAD_EXT_LIST], ret);

    for(unsigned i = 0; i < ret.GetCount(); i++)
    {
        if(m_frd.files.Find(ret[i].full_file_name))
            continue;

        CComPtr<ISubStream> pSubStream = CreateSubStreamFromFile(ret[i].full_file_name, &m_csFilter, &m_frd.files);

        if(pSubStream)
        {
            m_pSubStreams.AddTail(pSubStream);
            m_fIsSubStreamEmbeded.AddTail(false);
            m_frd.files.AddTail(ret[i].full_file_name);
        }
    }

    for(unsigned i = 0; i < m_pSubtitleInputPin.GetCount(); i++)
    {
        if(m_pSubtitleInputPin[i]->IsConnected())
        {
            m_pSubStreams.AddTail(m_pSubtitleInputPin[i]->GetSubStream());
            m_fIsSubStreamEmbeded.AddTail(true);
        }
    }

    hr = put_SelectedLanguage(FindPreferedLanguage());
    CHECK_N_LOG(hr, "Failed to set option");
    if (S_FALSE == hr)
        UpdateSubtitle(false); // make sure pSubPicProvider of our queue gets updated even if the stream number hasn't changed

    m_frd.RefreshEvent.Set();

    return (m_pSubStreams.GetCount() > 0);
}

void XySubFilter::UpdateSubtitle(bool fApplyDefStyle/*= true*/)
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(fApplyDefStyle));
    CAutoLock cAutolock(&m_csFilter);

    if (m_last_requested!=-1)
    {
        XY_LOG_WARN("Some subtitle frames are cached already!");
        m_last_requested = -1;
    }
    InvalidateSubtitle();

    CComPtr<ISubStream> pSubStream;

    if(!m_xy_bool_opt[BOOL_HIDE_SUBTITLES])
    {
        int i = m_xy_int_opt[INT_SELECTED_LANGUAGE];

        for(POSITION pos = m_pSubStreams.GetHeadPosition(); i >= 0 && pos; pSubStream = NULL)
        {
            pSubStream = m_pSubStreams.GetNext(pos);

            if(i < pSubStream->GetStreamCount())
            {
                CAutoLock cAutoLock(&m_csFilter);
                pSubStream->SetStream(i);
                break;
            }

            i -= pSubStream->GetStreamCount();
        }
    }

    SetSubtitle(pSubStream, fApplyDefStyle);
    XY_LOG_INFO("SelectedSubtitle:"<<m_xy_int_opt[INT_SELECTED_LANGUAGE]
    <<" YuvMatrix:"<<m_xy_str_opt[STRING_YUV_MATRIX].GetString()
    <<" outputLevels:"<<m_xy_str_opt[STRING_OUTPUT_LEVELS].GetString()
    <<" TextSubRendererVSFilterCompactRGBCorrection:"<<XySubRenderFrameCreater::GetDefaultCreater()->GetVsfilterCompactRgbCorrection());
}

void XySubFilter::SetSubtitle( ISubStream* pSubStream, bool fApplyDefStyle /*= true*/ )
{
    HRESULT hr = NOERROR;
    XY_LOG_INFO(XY_LOG_VAR_2_STR(pSubStream)<<XY_LOG_VAR_2_STR(fApplyDefStyle));
    CAutoLock cAutolock(&m_csFilter);

    CSize playres(0,0);
    m_video_yuv_matrix_decided_by_sub = ColorConvTable::NONE;
    m_video_yuv_range_decided_by_sub = ColorConvTable::RANGE_NONE;
    if(pSubStream)
    {
        CLSID clsid;
        pSubStream->GetClassID(&clsid);

        if(clsid == __uuidof(CVobSubFile))
        {
            CVobSubSettings* pVSS = dynamic_cast<CVobSubFile*>(pSubStream);

            pVSS->SetAlignment(m_xy_bool_opt[BOOL_OVERRIDE_PLACEMENT], m_xy_size_opt[SIZE_PLACEMENT_PERC].cx, m_xy_size_opt[SIZE_PLACEMENT_PERC].cy, 1, 1);
            pVSS->m_fOnlyShowForcedSubs = m_xy_bool_opt[BOOL_VOBSUBSETTINGS_ONLY_SHOW_FORCED_SUBS];
            m_xy_bool_opt[BOOL_IS_BITMAP] = true;
            m_xy_bool_opt[BOOL_IS_MOVABLE] = true;
        }
        else if(clsid == __uuidof(CVobSubStream))
        {
            CVobSubSettings* pVSS = dynamic_cast<CVobSubStream*>(pSubStream);

            pVSS->SetAlignment(m_xy_bool_opt[BOOL_OVERRIDE_PLACEMENT], m_xy_size_opt[SIZE_PLACEMENT_PERC].cx, m_xy_size_opt[SIZE_PLACEMENT_PERC].cy, 1, 1);
            pVSS->m_fOnlyShowForcedSubs = m_xy_bool_opt[BOOL_VOBSUBSETTINGS_ONLY_SHOW_FORCED_SUBS];
            m_xy_bool_opt[BOOL_IS_BITMAP] = true;
            m_xy_bool_opt[BOOL_IS_MOVABLE] = true;
        }
        else if(clsid == __uuidof(CRenderedTextSubtitle))
        {
            CRenderedTextSubtitle* pRTS = dynamic_cast<CRenderedTextSubtitle*>(pSubStream);
            pRTS->Deinit();//clear caches

            STSStyle s = m_defStyle;
            bool succeeded = pRTS->SetDefaultStyle(s);
            if (!succeeded)
            {
                XY_LOG_ERROR("Failed to set default style");
            }
            pRTS->SetForceDefaultStyle(m_xy_bool_opt[BOOL_FORCE_DEFAULT_STYLE]);

            pRTS->m_ePARCompensationType = CSimpleTextSubtitle::EPCTDisabled;
            pRTS->m_dPARCompensation = 1.00;

            switch(pRTS->m_eYCbCrMatrix)
            {
            case CSimpleTextSubtitle::YCbCrMatrix_BT601:
                m_video_yuv_matrix_decided_by_sub = ColorConvTable::BT601;
                break;
            case CSimpleTextSubtitle::YCbCrMatrix_BT709:
                m_video_yuv_matrix_decided_by_sub = ColorConvTable::BT709;
                break;
            case CSimpleTextSubtitle::YCbCrMatrix_BT2020:
                m_video_yuv_matrix_decided_by_sub = ColorConvTable::BT2020;
                break;
            default:
                m_video_yuv_matrix_decided_by_sub = ColorConvTable::NONE;
                break;
            }
            switch(pRTS->m_eYCbCrRange)
            {
            case CSimpleTextSubtitle::YCbCrRange_PC:
                m_video_yuv_range_decided_by_sub = ColorConvTable::RANGE_PC;
                break;
            case CSimpleTextSubtitle::YCbCrRange_TV:
                m_video_yuv_range_decided_by_sub = ColorConvTable::RANGE_TV;
                break;
            default:
                m_video_yuv_range_decided_by_sub = ColorConvTable::RANGE_NONE;
                break;
            }
            pRTS->Deinit();
            playres = pRTS->m_dstScreenSize;
            m_xy_bool_opt[BOOL_IS_BITMAP] = false;
            m_xy_bool_opt[BOOL_IS_MOVABLE] = ((pRTS->IsMovable()) && ((pRTS->IsSimple()) || (m_xy_bool_opt[BOOL_ALLOW_MOVING])));
        }
        else if(clsid == __uuidof(HdmvSubtitleProvider) || clsid == __uuidof(SupFileSubtitleProvider))
        {
            CompositionObject::ColorType color_type = CompositionObject::NONE;
            CompositionObject::YuvRangeType range_type = CompositionObject::RANGE_NONE;
            if ( m_xy_str_opt[STRING_PGS_YUV_RANGE].CompareNoCase(_T("PC"))==0 )
            {
                range_type = CompositionObject::RANGE_PC;
            }
            else if ( m_xy_str_opt[STRING_PGS_YUV_RANGE].CompareNoCase(_T("TV"))==0 )
            {
                range_type = CompositionObject::RANGE_TV;
            }
            else
            {
                if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Left(2).CompareNoCase(L"TV")==0)
                {
                    range_type = CompositionObject::RANGE_TV;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Left(2).CompareNoCase(L"PC")==0)
                {
                    range_type = CompositionObject::RANGE_PC;
                }
                else
                {
                    XY_LOG_WARN(L"Can NOT get useful YUV range from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString());
                    range_type = CompositionObject::RANGE_TV;
                }
            }
            if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT601"))==0 )
            {
                color_type = CompositionObject::YUV_Rec601;
            }
            else if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT709"))==0 )
            {
                color_type = CompositionObject::YUV_Rec709;
            }
            else if ( m_xy_str_opt[STRING_PGS_YUV_MATRIX].CompareNoCase(_T("BT2020"))==0 )
            {
                color_type = CompositionObject::YUV_Rec2020;
            }
            else
            {
                if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"601")==0)
                {
                    color_type = CompositionObject::YUV_Rec601;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(3).CompareNoCase(L"709")==0)
                {
                    color_type = CompositionObject::YUV_Rec709;
                }
                else if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].Right(4).CompareNoCase(L"2020")==0)
                {
                    color_type = CompositionObject::YUV_Rec2020;
                }
                else
                {
                    XY_LOG_WARN(L"Can NOT get useful YUV matrix from consumer:"<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString());
                }
            }

            if (clsid == __uuidof(HdmvSubtitleProvider))
            {
                HdmvSubtitleProvider *sub = dynamic_cast<HdmvSubtitleProvider*>(pSubStream);
                sub->SetYuvType(color_type, range_type);
            }
            else
            {
                SupFileSubtitleProvider *sub = dynamic_cast<SupFileSubtitleProvider*>(pSubStream);
                sub->SetYuvType(color_type, range_type);
            }
            m_xy_bool_opt[BOOL_IS_BITMAP] = true;
            m_xy_bool_opt[BOOL_IS_MOVABLE] = true;
        }
    }

    if(!fApplyDefStyle)
    {
        int i = 0;

        POSITION pos = m_pSubStreams.GetHeadPosition();
        while(pos)
        {
            CComPtr<ISubStream> pSubStream2 = m_pSubStreams.GetNext(pos);

            if(pSubStream == pSubStream2)
            {
                m_xy_int_opt[INT_SELECTED_LANGUAGE] = i + pSubStream2->GetStream();
                break;
            }

            i += pSubStream2->GetStreamCount();
        }
    }

    m_curSubStream = pSubStream;

    SetYuvMatrix();
    SetRgbOutputLevel();

    m_xy_size_opt[SIZE_ASS_PLAY_RESOLUTION] = playres;

    if (CComQIPtr<IXySubRenderProvider>(pSubStream))
    {
        m_sub_provider = CComQIPtr<IXySubRenderProvider>(pSubStream);
    }
    else if (CComQIPtr<ISubPicProviderEx2>(pSubStream))
    {
        m_sub_provider = DEBUG_NEW XySubRenderProviderWrapper2(CComQIPtr<ISubPicProviderEx2>(pSubStream), NULL);
    }
    else if (CComQIPtr<ISubPicProviderEx>(pSubStream))
    {
        m_sub_provider = DEBUG_NEW XySubRenderProviderWrapper(CComQIPtr<ISubPicProviderEx>(pSubStream), NULL);
    }
    else
    {
        m_sub_provider = NULL;
        if (pSubStream!=NULL) 
        {
            XY_LOG_WARN("This subtitle stream is NOT supported");
        }
    }
    if (m_sub_provider)
    {
        m_sub_provider->Connect(this);
    }

    UpdateMultiTracker(this);
}

// Splits 's' on 'sep' into 'out' (empty fields preserved).
static void XySplitTokenW(const CString& s, WCHAR sep, std::vector<CString>& out)
{
    int start = 0;
    for (;;)
    {
        int pos = s.Find(sep, start);
        if (pos < 0)
        {
            out.push_back(s.Mid(start));
            break;
        }
        out.push_back(s.Mid(start, pos - start));
        start = pos + 1;
    }
}

// One secondary track's resolved configuration.
struct XySecondarySpec
{
    CString path;
    WORD embedded_pid;   // 0 = external file; else container-embedded stream
    bool embedded;       // true when 'embedded_pid' selects a stream
    bool embedded_by_index;  // value is a 1-based list index, not a raw id
    double scale;   // percent
    int opacity;    // 0..255
    int align;      // 0=as-is, 1=top, 2=bottom
    int margin;     // px, with align 1/2
    int z_order;    // paint order

    XySecondarySpec() : embedded_pid(0), embedded(false), embedded_by_index(false), scale(100.0), opacity(255), align(1), margin(0), z_order(1) {}
};

static SubRenderTrack::VerticalAlign XyAlignFromInt(int v)
{
    switch (v)
    {
    case 1:  return SubRenderTrack::ALIGN_TOP;
    case 2:  return SubRenderTrack::ALIGN_BOTTOM;
    default: return SubRenderTrack::ALIGN_AS_IS;
    }
}

// Parses the secondary file option. Entries are separated by ';' (preferred) or,
// when no ';' is present, by ','. Each entry is a compact spec:
//
//     path[|scale[|opacity[|align[|margin[|z]]]]]
//     pid:0x1201[|scale[|opacity[|align[|margin[|z]]]]]
//
// 'path' is an external subtitle file (existing behaviour). 'pid:<hex>' names
// a container-embedded subtitle stream (Blu-ray/TS PGS or DVB) read straight
// from the media file; it uses INT_SECONDARY_EMBEDDED_PID as the default when
// written without an argument. The remaining fields fall back to the
// single-value registry options (INT_SECONDARY_SCALE / _OPACITY / _VERTICAL /
// _TOP_MARGIN / _Z_ORDER); 'z' additionally defaults to base_z + entry index so
// a plain list keeps a stable, distinct paint order.
static bool XyParseEmbeddedPid(const CString& token, WORD* pid)
{
    CString t = token;
    t.Trim();
    CString prefix = _T("pid:");
    if (t.GetLength() <= prefix.GetLength() || t.Left(prefix.GetLength()).CompareNoCase(prefix) != 0)
    {
        return false;
    }
    CString num = t.Mid(prefix.GetLength());
    num.Trim();
    if (num.IsEmpty())
    {
        return false;
    }
    long v = wcstol(num, NULL, 16);
    if (v <= 0 || v > 0x1FFF)
    {
        return false;
    }
    *pid = (WORD)v;
    return true;
}

// 'embedded' selects an embedded subtitle stream without naming a raw PID/PG
// order: the container interprets the value in its own terms. Two syntaxes map
// here, and both containers honour them:
//     'track:N'  - raw container id (MKV TrackNumber; TS/DVB pid when N <= 0x1FFF)
//     'sub:N'    - 1-based index into the discovered embedded subtitle stream list
// A bare 'embedded' / 'sub:' uses the first decodable stream (encoded as 0, the
// same sentinel the loader already treats as "first decodable").
static bool XyParseEmbeddedToken(const CString& token, WORD* value, bool* by_index)
{
    CString t = token;
    t.Trim();
    *by_index = false;

    struct { const TCHAR* p; bool idx; } prefixes[] = {
        { _T("track:"), false },
        { _T("sub:"),   true  },
    };
    for (int k = 0; k < 2; k++)
    {
        CString prefix = prefixes[k].p;
        if (t.GetLength() <= prefix.GetLength() ||
            t.Left(prefix.GetLength()).CompareNoCase(prefix) != 0)
        {
            continue;
        }
        CString num = t.Mid(prefix.GetLength());
        num.Trim();
        long v = num.IsEmpty() ? 0 : wcstol(num, NULL, 10);
        if (v < 0 || v > 0xFFFF)
        {
            return false;
        }
        *value = (WORD)v;
        *by_index = prefixes[k].idx;
        return true;
    }
    return false;
}

static void XyParseSecondaryList(const CString& list, int base_scale, int base_opacity,
    int base_align, int base_margin, int base_z, int default_embedded_pid,
    std::vector<XySecondarySpec>& out)
{
    CString trimmed = list;
    trimmed.Trim();
    if (trimmed.IsEmpty())
    {
        return;
    }

    std::vector<CString> entries;
    if (trimmed.Find(_T(';')) >= 0)
    {
        XySplitTokenW(trimmed, _T(';'), entries);
    }
    else
    {
        XySplitTokenW(trimmed, _T(','), entries);
    }

    for (size_t i = 0; i < entries.size(); i++)
    {
        CString entry = entries[i];
        entry.Trim();
        if (entry.IsEmpty())
        {
            continue;
        }

        std::vector<CString> parts;
        XySplitTokenW(entry, _T('|'), parts);

        XySecondarySpec spec;
        spec.path = parts[0];
        spec.path.Trim();
        if (spec.path.IsEmpty())
        {
            continue;
        }

        // 'pid:0x1201' (TS/BDMV raw PID), 'track:N' (container-native id) or
        // 'sub:N' (1-based index into the discovered embedded list) selects an
        // embedded stream; a bare 'pid:' / 'track:' / 'sub:' uses the default.
        WORD embedded_val = 0;
        bool embedded = false;
        bool by_index = false;
        if (spec.path.CompareNoCase(_T("pid:")) == 0)
        {
            embedded_val = (WORD)default_embedded_pid;
            embedded = true;
            if (embedded_val == 0)
            {
                continue; // no default configured: nothing to open
            }
        }
        else if (XyParseEmbeddedPid(spec.path, &embedded_val))
        {
            embedded = true;
        }
        else if (XyParseEmbeddedToken(spec.path, &embedded_val, &by_index))
        {
            embedded = true;
        }
        spec.embedded_pid = embedded_val;
        spec.embedded = embedded;
        spec.embedded_by_index = by_index;
        spec.scale = parts.size() > 1 && !parts[1].Trim().IsEmpty() ? _ttof(parts[1]) : (double)base_scale;
        if (spec.scale <= 0)
        {
            spec.scale = base_scale > 0 ? base_scale : 100;
        }
        spec.opacity = parts.size() > 2 && !parts[2].Trim().IsEmpty() ? _ttoi(parts[2]) : base_opacity;
        spec.opacity = spec.opacity < 0 ? 0 : (spec.opacity > 255 ? 255 : spec.opacity);
        spec.align = parts.size() > 3 && !parts[3].Trim().IsEmpty() ? _ttoi(parts[3]) : base_align;
        if (spec.align < 0 || spec.align > 2)
        {
            spec.align = base_align;
        }
        spec.margin = parts.size() > 4 && !parts[4].Trim().IsEmpty() ? _ttoi(parts[4]) : base_margin;
        if (spec.margin < 0)
        {
            spec.margin = 0;
        }
        spec.z_order = parts.size() > 5 && !parts[5].Trim().IsEmpty() ? _ttoi(parts[5]) : base_z + (int)i;

        out.push_back(spec);
    }
}

// Builds/refreshes the N-track renderer around the primary provider and the
// optional external secondary list. With dual-sub off (or no secondary
// configured) the tracker holds a single track and RequestFrame passes the
// primary frame straight through, so the stock path is unchanged.
void XySubFilter::UpdateMultiTracker(ISubRenderProvider* pPrimary)
{
    CAutoLock cAutolock(&m_csFilter);

    m_multi_tracker = NULL;

    if (!m_sub_provider)
    {
        return; // no primary: nothing to compose
    }

    CComPtr<MultiTrackSubRenderProvider> tracker;
    tracker = DEBUG_NEW MultiTrackSubRenderProvider();
    if (!tracker)
    {
        return;
    }
    tracker->SetLoader(static_cast<IMultiSubtitleTrackLoader*>(this));

    std::vector<XySecondarySpec> secondaries;
    if (m_xy_bool_opt[BOOL_DUAL_SUB_ENABLED] && !m_xy_str_opt[STRING_FILE_NAME_SECONDARY].IsEmpty())
    {
        XyParseSecondaryList(m_xy_str_opt[STRING_FILE_NAME_SECONDARY]
            , m_xy_int_opt[INT_SECONDARY_SCALE]
            , m_xy_int_opt[INT_SECONDARY_OPACITY]
            , m_xy_int_opt[INT_SECONDARY_VERTICAL]
            , m_xy_int_opt[INT_SECONDARY_TOP_MARGIN]
            , m_xy_int_opt[INT_SECONDARY_Z_ORDER]
            , m_xy_int_opt[INT_SECONDARY_EMBEDDED_PID]
            , secondaries);
    }

    // A bare 'secondary_embedded_pid' (non-zero) with no secondary spec adds the
    // container-embedded stream as the sole secondary, so the option alone is
    // enough to turn the feature on. Default 0 keeps this off.
    if (secondaries.empty()
        && m_xy_bool_opt[BOOL_DUAL_SUB_ENABLED]
        && m_xy_int_opt[INT_SECONDARY_EMBEDDED_PID] > 0)
    {
        CString token;
        token.Format(_T("pid:0x%04X"), m_xy_int_opt[INT_SECONDARY_EMBEDDED_PID]);
        XyParseSecondaryList(token
            , m_xy_int_opt[INT_SECONDARY_SCALE]
            , m_xy_int_opt[INT_SECONDARY_OPACITY]
            , m_xy_int_opt[INT_SECONDARY_VERTICAL]
            , m_xy_int_opt[INT_SECONDARY_TOP_MARGIN]
            , m_xy_int_opt[INT_SECONDARY_Z_ORDER]
            , m_xy_int_opt[INT_SECONDARY_EMBEDDED_PID]
            , secondaries);
    }

    // Clamp to the container's capacity (the primary occupies slot 0).
    int track_count = 1 + (int)secondaries.size();
    if (track_count > MultiTrackSubRenderProvider::MAX_TRACK_COUNT)
    {
        XY_LOG_WARN("Requested "<<track_count<<" tracks, clamping to "
            <<MultiTrackSubRenderProvider::MAX_TRACK_COUNT);
        track_count = MultiTrackSubRenderProvider::MAX_TRACK_COUNT;
        secondaries.resize(track_count - 1);
    }

    tracker->SetTrackCount(track_count);
    tracker->SetTrackProvider(0, m_sub_provider);

    // Primary placement: only touched when the user asks for it, so the default
    // (AS_IS) keeps the frame hierarchy byte-for-byte identical.
    {
        SubRenderTrack::VerticalAlign primary_align =
            XyAlignFromInt(m_xy_int_opt[INT_PRIMARY_VERTICAL]);
        if (primary_align != SubRenderTrack::ALIGN_AS_IS)
        {
            tracker->SetTrackPlacement(0, primary_align, 0);
        }
    }

    for (size_t i = 0; i < secondaries.size(); i++)
    {
        const XySecondarySpec& spec = secondaries[i];
        const int index = (int)i + 1;

        // Track N loads its own source: an external file through the injected
        // loader (the same cascade used for the primary), or an embedded stream
        // read straight from the media container.
        tracker->SetTrackEnabled(index, true);
        if (spec.embedded)
        {
            tracker->SetTrackEmbedded(index, m_media_file_path.GetString(), spec.embedded_pid,
                spec.embedded_by_index
                    ? IMultiSubtitleTrackLoader::EMBEDDED_INDEX
                    : IMultiSubtitleTrackLoader::EMBEDDED_RAW);
        }
        else
        {
            tracker->SetTrackFile(index, spec.path.GetString());
        }
        tracker->SetTrackScale(index, spec.scale);
        tracker->SetTrackOpacity(index, spec.opacity);
        tracker->SetTrackZOrder(index, spec.z_order);
        tracker->SetTrackPlacement(index, XyAlignFromInt(spec.align), spec.margin);
    }

    m_multi_tracker = tracker;
    m_multi_tracker->Connect(this);
}

STDMETHODIMP XySubFilter::CreateTrackProviderFromFile(LPCWSTR file_path, IXySubRenderProvider** provider)
{
    CheckPointer(provider, E_POINTER);
    *provider = NULL;
    if (!file_path || !*file_path)
    {
        return E_INVALIDARG;
    }

    CComPtr<ISubStream> stream = CreateSubStreamFromFile(CString(file_path), &m_csFilter, NULL);
    if (!stream)
    {
        return S_FALSE;
    }

    // Wrap exactly as the primary path does, so both sides speak
    // IXySubRenderProvider.
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
        XY_LOG_WARN("Secondary subtitle stream is NOT supported");
        return S_FALSE;
    }

    if (!wrapped)
    {
        return E_OUTOFMEMORY;
    }
    *provider = wrapped;
    return S_OK;
}

STDMETHODIMP XySubFilter::CreateTrackProviderFromEmbedded(LPCWSTR media_path, WORD value,
    EmbeddedSelectKind kind, IXySubRenderProvider** provider)
{
    CheckPointer(provider, E_POINTER);
    *provider = NULL;
    if (!media_path || !*media_path)
    {
        return E_INVALIDARG;
    }

    // Container-side source: pick the container by type, then read the subtitle
    // stream straight out of the media file with the existing decoders.
    if (IsMatroskaFile(media_path))
    {
        WORD track = value;
        if (kind == EMBEDDED_INDEX && value > 0)
        {
            // 1-based index into the discovered embedded subtitle list.
            CMatroskaSubtitleSource src;
            if (!src.Open(media_path) || value > (WORD)src.GetSubtitleStreams().size())
            {
                return E_FAIL;
            }
            track = (WORD)src.GetSubtitleStreams()[value - 1].track_number;
        }
        return CreateMatroskaSubtitleProvider(media_path, track, provider);
    }

    // TS/BDMV: EMBEDDED_INDEX resolves to the requested subtitle stream's PID
    // via the same enumeration the source itself uses; EMBEDDED_RAW is the PID.
    WORD pid = value;
    if (kind == EMBEDDED_INDEX && value > 0)
    {
        CTsSubtitleSource src;
        REFERENCE_TIME in_pt = _I64_MAX, out_pt = _I64_MAX;
        CString stream_path;
        CString resolved_path(media_path);
        ResolveBdmvPlaylist(media_path, stream_path, in_pt, out_pt);
        if (!stream_path.IsEmpty())
        {
            resolved_path = stream_path;
        }
        if (!src.Open(resolved_path, in_pt, out_pt) ||
            value > (WORD)src.GetSubtitleStreams().size())
        {
            return E_FAIL;
        }
        pid = src.GetSubtitleStreams()[value - 1].pid;
    }
    return CreateTsSubtitleProvider(media_path, pid, provider);
}

STDMETHODIMP XySubFilter::EnumerateEmbeddedStreams(LPCWSTR media_path,
    std::vector<EmbeddedStreamInfo>* out)
{
    CheckPointer(out, E_POINTER);
    out->clear();

    // The property page has no direct access to the graph: a NULL/empty
    // media_path means "the media of the current graph". Re-resolve it from the
    // live graph now rather than trusting the value captured at join time: the
    // graph may not have had its source filter yet when we joined, and the page
    // can be opened at any point during playback.
    CString media;
    {
        CAutoLock cAutolock(&m_csFilter);
        media = (media_path && *media_path) ? CString(media_path) : CString(m_media_file_path);
    }
    if (media.IsEmpty() && m_pGraph)
    {
        UpdateMediaFileName(m_pGraph);
        CAutoLock cAutolock(&m_csFilter);
        media = m_media_file_path;
    }
    if (media.IsEmpty())
    {
        return S_FALSE;   // nothing playing / media path unknown: empty list
    }

    HRESULT hr = EnumerateEmbeddedStreamsFromMedia(media, out);
    if (hr != S_OK)
    {
        XY_LOG_WARN(_T("Embedded enumeration: '")<<media<<_T("' yielded no list (hr=")
            <<(int)hr<<_T(")"));
    }
    return hr;
}

// Resolves the media (container) file path once per graph, so the embedded
// secondary source can open it. Mirrors LoadExternalSubtitle's enumeration.
void XySubFilter::UpdateMediaFileName(IFilterGraph* pGraph)
{
    CAutoLock cAutolock(&m_csFilter);
    m_media_file_path.Empty();
    if (!pGraph)
    {
        return;
    }
    BeginEnumFilters(pGraph, pEF, pBF)
    {
        if (CComQIPtr<IFileSourceFilter> pFSF = pBF)
        {
            LPOLESTR fnw = NULL;
            if (!pFSF || FAILED(pFSF->GetCurFile(&fnw, NULL)) || !fnw)
            {
                continue;
            }
            m_media_file_path = CStringW(fnw);
            CoTaskMemFree(fnw);
            break;
        }
    }
    EndEnumFilters;
}

void XySubFilter::InvalidateSubtitle( REFERENCE_TIME rtInvalidate /*= -1*/, DWORD_PTR nSubtitleId /*= -1*/ )
{
    CAutoLock cAutolock(&m_csFilter);

    if(m_sub_provider)
    {
        if (nSubtitleId == -1 || nSubtitleId == reinterpret_cast<DWORD_PTR>(m_curSubStream))
        {
            if (m_last_requested>rtInvalidate)
            {
                ASSERT(0);
                XY_LOG_ERROR("New subtitle samples received after a request.");
            }
            // Invalidate both tracks (forwards to every track provider).
            if (m_multi_tracker)
            {
                m_multi_tracker->Invalidate(rtInvalidate);
            }
            else
            {
                m_sub_provider->Invalidate(rtInvalidate);
            }
            XY_LOG_INFO("Subtitles cleared in provider and consumer");
        }
    }
}

HRESULT XySubFilter::UpdateParamFromConsumer( bool getNameAndVersion/*=false*/ )
{

#define GET_PARAM_FROM_CONSUMER(func, param, ret_addr) \
    hr = func(param, ret_addr);\
    if (FAILED(hr))\
    {\
        XY_LOG_ERROR("Failed to get "param" from consumer.");\
        return E_UNEXPECTED;\
    }

    HRESULT hr = NOERROR;
    CAutoLock cAutolock(&m_csFilter);

    if (getNameAndVersion)
    {
        LPWSTR str;
        int len;
        hr = m_consumer->GetString("name", &str, &len);
        if (FAILED(hr))
        {
            XY_LOG_ERROR("Failed to get name from consumer");
        }
        else
        {
            m_xy_str_opt[STRING_CONNECTED_CONSUMER] = CStringW(str, len);
            LocalFree(str);
        }
        hr = m_consumer->GetString("version", &str, &len);
        if (FAILED(hr))
        {
            XY_LOG_ERROR("Failed to get version from consumer");
        }
        else
        {
            m_xy_str_opt[STRING_CONSUMER_VERSION] = CStringW(str, len);
            LocalFree(str);
        }
    }
    SIZE originalVideoSize;
    GET_PARAM_FROM_CONSUMER(m_consumer->GetSize, "originalVideoSize", &originalVideoSize);
    if (originalVideoSize.cx==0 || originalVideoSize.cy==0)
    {
        return E_UNEXPECTED;
    }

    SIZE arAdjustedVideoSize;
    GET_PARAM_FROM_CONSUMER(m_consumer->GetSize, "arAdjustedVideoSize", &arAdjustedVideoSize);

    RECT videoOutputRect;
    GET_PARAM_FROM_CONSUMER(m_consumer->GetRect, "videoOutputRect", &videoOutputRect);
    if (videoOutputRect.right<=videoOutputRect.left || videoOutputRect.bottom<=videoOutputRect.top)
    {
        return E_UNEXPECTED;
    }

    RECT subtitleTargetRect;
    GET_PARAM_FROM_CONSUMER(m_consumer->GetRect, "subtitleTargetRect", &subtitleTargetRect);
    if (subtitleTargetRect.right<=subtitleTargetRect.left || subtitleTargetRect.bottom<=subtitleTargetRect.top)
    {
        return E_UNEXPECTED;
    }

    LPWSTR str;
    int len;
    CStringW consumer_yuv_matrix = m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX];
    hr = m_consumer->GetString("yuvMatrix", &str, &len);
    if (FAILED(hr)) {
        XY_LOG_WARN("Failed to get yuvMatrix from consumer.");
    }
    else
    {
        consumer_yuv_matrix = CStringW(str, len);
        LocalFree(str);
    }

    int consumer_supported_levels = 0;
    hr = m_consumer->GetInt("supportedLevels", &consumer_supported_levels);
    if (FAILED(hr))
    {
        XY_LOG_INFO("Failed to get supportedLevels from consumer");
        consumer_supported_levels = 0;
    }

    ULONGLONG rt_fps;
    hr = m_consumer->GetUlonglong("frameRate", &rt_fps);
    if (FAILED(hr))
    {
        XY_LOG_ERROR("Failed to get fps from consumer.");
        rt_fps = 10000000.0/24.0;
    }
    //fix me: is it right?
    //fix me: use REFERENCE_TIME instead?
    double fps = 10000000.0/rt_fps;
    bool update_subtitle = false;
    //bool clear_consumer = false;

    if (m_xy_size_opt[SIZE_ORIGINAL_VIDEO]!=originalVideoSize)
    {
        XY_LOG_INFO("Size original video changed from "<<m_xy_size_opt[SIZE_ORIGINAL_VIDEO]
            <<" to "<<originalVideoSize);
        m_xy_size_opt[SIZE_ORIGINAL_VIDEO] = originalVideoSize;
        update_subtitle = true;
    }
    if (m_xy_size_opt[SIZE_AR_ADJUSTED_VIDEO]!=arAdjustedVideoSize)
    {
        XY_LOG_INFO("Size AR adjusted video changed from "<<m_xy_size_opt[SIZE_AR_ADJUSTED_VIDEO]
            <<" to "<<arAdjustedVideoSize);
        m_xy_size_opt[SIZE_AR_ADJUSTED_VIDEO] = arAdjustedVideoSize;
        update_subtitle = true;
    }
    if (m_xy_rect_opt[RECT_VIDEO_OUTPUT]!=videoOutputRect)
    {
        XY_LOG_INFO("Video output rect changed from "<<m_xy_rect_opt[RECT_VIDEO_OUTPUT]
            <<" to "<<videoOutputRect);
        m_xy_rect_opt[RECT_VIDEO_OUTPUT] =videoOutputRect;
        update_subtitle = true;
    }
    if (m_xy_rect_opt[RECT_SUBTITLE_TARGET]!=subtitleTargetRect)
    {
        XY_LOG_INFO("Subtitle target rect changed from "<<m_xy_rect_opt[RECT_SUBTITLE_TARGET]
            <<" to "<<subtitleTargetRect);
        m_xy_rect_opt[RECT_SUBTITLE_TARGET] =subtitleTargetRect;
        update_subtitle = true;
    }
    if (m_xy_double_opt[DOUBLE_FPS]!=fps)
    {
        XY_LOG_INFO("FPS changed from "<<m_xy_double_opt[DOUBLE_FPS]
            <<" to "<<fps);
        hr = XySetDouble(DOUBLE_FPS, fps);
        CHECK_N_LOG(hr, "Failed to set option");
        ASSERT(SUCCEEDED(hr));
    }
    if (m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX]!=consumer_yuv_matrix) {
        XY_LOG_INFO(L"Consumer yuv matrix changed from "<<m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString()
        <<L" to "<<consumer_yuv_matrix.GetString());
        m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX] = consumer_yuv_matrix;
        update_subtitle = true;
        //clear_consumer = true;
    }
    if (m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]!=consumer_supported_levels)
    {
        XY_LOG_INFO("Consumer supportedLevels changed from "<<m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]
            <<" to "<<consumer_supported_levels);
        m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS] = consumer_supported_levels;
        update_subtitle = true;
        //clear_consumer = true;
    }

    if (update_subtitle)
    {
        UpdateSubtitle(false);
        //if (clear_consumer)
        //{
    	//    m_consumer->Clear();
        //}
    }
    return hr;
}

#define MAXPREFLANGS 5

int XySubFilter::FindPreferedLanguage( bool fHideToo /*= true*/ )
{
    XY_LOG_INFO(XY_LOG_VAR_2_STR(fHideToo));
    HRESULT hr = NOERROR;
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    int nLangs = 0;
    hr = get_LanguageCount(&nLangs);
    CHECK_N_LOG(hr, "Failed to get option");

    if(nLangs <= 0) {
        XY_LOG_WARN("There is NO languages yet");
        return(0);
    }

    for(int i = 0; i < MAXPREFLANGS; i++)
    {
        CString tmp;
        tmp.Format(IDS_RL_LANG, i);

        CString lang = theApp.GetProfileString(ResStr(IDS_R_PREFLANGS), tmp);

        if(!lang.IsEmpty())
        {
            for(int ret = 0; ret < nLangs; ret++)
            {
                CString l;
                WCHAR* pName = NULL;
                hr = get_LanguageName(ret, &pName);
                CHECK_N_LOG(hr, "Failed to get option");

                l = pName;
                CoTaskMemFree(pName);

                if(!l.CompareNoCase(lang)) return(ret);
            }
        }
    }

    return(0);
}

void XySubFilter::UpdatePreferedLanguages(CString l)
{
    XY_LOG_INFO(l);
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    CString langs[MAXPREFLANGS+1];

    int i = 0, j = 0, k = -1;
    for(; i < MAXPREFLANGS; i++)
    {
        CString tmp;
        tmp.Format(IDS_RL_LANG, i);

        langs[j] = theApp.GetProfileString(ResStr(IDS_R_PREFLANGS), tmp);

        if(!langs[j].IsEmpty())
        {
            if(!langs[j].CompareNoCase(l)) k = j;
            j++;
        }
    }

    if(k == -1)
    {
        langs[k = j] = l;
        j++;
    }

    // move the selected to the top of the list

    while(k > 0)
    {
        CString tmp = langs[k]; langs[k] = langs[k-1]; langs[k-1] = tmp;
        k--;
    }

    // move "Hide subtitles" to the last position if it wasn't our selection

    CString hidesubs;
    hidesubs.LoadString(IDS_M_HIDESUBTITLES);

    for(k = 1; k < j; k++)
    {
        if(!langs[k].CompareNoCase(hidesubs)) break;
    }

    while(k < j-1)
    {
        CString tmp = langs[k]; langs[k] = langs[k+1]; langs[k+1] = tmp;
        k++;
    }

    for(i = 0; i < j; i++)
    {
        CString tmp;
        tmp.Format(IDS_RL_LANG, i);

        theApp.WriteProfileString(ResStr(IDS_R_PREFLANGS), tmp, langs[i]);
    }
}

void XySubFilter::AddSubStream(ISubStream* pSubStream)
{
    XY_LOG_INFO(pSubStream);
    CAutoLock cAutolock(&m_csFilter);

    POSITION pos = m_pSubStreams.Find(pSubStream);
    if(!pos)
    {
        m_pSubStreams.AddTail(pSubStream);
        m_fIsSubStreamEmbeded.AddTail(true);//todo: fix me
    }

    int len = m_pSubtitleInputPin.GetCount();
    for(unsigned i = 0; i < m_pSubtitleInputPin.GetCount(); i++)
        if(m_pSubtitleInputPin[i]->IsConnected()) len--;

    if(len == 0)
    {
        HRESULT hr = S_OK;
        m_pSubtitleInputPin.Add(DEBUG_NEW SubtitleInputPin2(this, m_pLock, &m_csFilter, &hr));
    }
    UpdateSubtitle(false);
}

void XySubFilter::RemoveSubStream(ISubStream* pSubStream)
{
    XY_LOG_INFO(pSubStream);
    CAutoLock cAutolock(&m_csFilter);

    POSITION pos = m_pSubStreams.GetHeadPosition();
    POSITION pos2 = m_fIsSubStreamEmbeded.GetHeadPosition();
    while(pos!=NULL)
    {
        if( m_pSubStreams.GetAt(pos)==pSubStream )
        {
            m_pSubStreams.RemoveAt(pos);
            m_fIsSubStreamEmbeded.RemoveAt(pos2);
            break;
        }
        else
        {
            m_pSubStreams.GetNext(pos);
            m_fIsSubStreamEmbeded.GetNext(pos2);
        }
    }
    UpdateSubtitle(false);
}

HRESULT XySubFilter::CheckInputType( const CMediaType* pmt )
{
    CAutoLock cAutolock(&m_csFilter);
    bool accept_embedded = m_xy_int_opt[INT_LOAD_SETTINGS_LEVEL]==LOADLEVEL_ALWAYS ||
        (m_xy_int_opt[INT_LOAD_SETTINGS_LEVEL]!=LOADLEVEL_DISABLED && m_xy_bool_opt[BOOL_LOAD_SETTINGS_EMBEDDED]);
    return accept_embedded ? S_OK : E_NOT_SET;
}

HRESULT XySubFilter::GetIsEmbeddedSubStream( int iSelected, bool *fIsEmbedded )
{
    CAutoLock cAutolock(&m_csFilter);

    HRESULT hr = E_INVALIDARG;
    if (!fIsEmbedded)
    {
        return S_FALSE;
    }

    int i = iSelected;
    *fIsEmbedded = false;

    POSITION pos = m_pSubStreams.GetHeadPosition();
    POSITION pos2 = m_fIsSubStreamEmbeded.GetHeadPosition();
    while(i >= 0 && pos)
    {
        CComPtr<ISubStream> pSubStream = m_pSubStreams.GetNext(pos);
        bool isEmbedded = m_fIsSubStreamEmbeded.GetNext(pos2);
        if(i < pSubStream->GetStreamCount())
        {
            hr = NOERROR;
            *fIsEmbedded = isEmbedded;
            break;
        }

        i -= pSubStream->GetStreamCount();
    }
    return hr;
}

bool XySubFilter::LoadExternalSubtitle(IFilterGraph* pGraph)
{
    XY_LOG_INFO(pGraph);

    CAutoLock cAutolock(&m_csFilter);
    bool fRet = false;
    HRESULT hr = NOERROR;
    int level;
    bool load_external, load_web, load_embedded;
    hr = get_LoadSettings(&level, &load_external, &load_web, &load_embedded);
    if (FAILED(hr))
    {
        XY_LOG_ERROR("Failed to get option");
        return false;
    }

    if(level < 0 || level >= LOADLEVEL_COUNT || level==LOADLEVEL_DISABLED) {
        XY_LOG_DEBUG("Disabled by load setting: "<<XY_LOG_VAR_2_STR(level));
        return false;
    }

    if(level == LOADLEVEL_ALWAYS)
        load_external = load_web = load_embedded = true;

    // find file name

    CStringW fn;

    BeginEnumFilters(pGraph, pEF, pBF)
    {
        if(CComQIPtr<IFileSourceFilter> pFSF = pBF)
        {
            LPOLESTR fnw = NULL;
            if(!pFSF || FAILED(pFSF->GetCurFile(&fnw, NULL)) || !fnw)
                continue;
            fn = CString(fnw);
            CoTaskMemFree(fnw);
            break;
        }
    }
    EndEnumFilters;
    XY_LOG_INFO(L"fn:"<<fn.GetString());
    if((load_external || load_web) && (load_web || !(wcsstr(fn, L"http://") || wcsstr(fn, L"mms://"))))
    {
        bool fTemp = m_xy_bool_opt[BOOL_HIDE_SUBTITLES];
        fRet = !fn.IsEmpty() && SUCCEEDED(put_FileName((LPWSTR)(LPCWSTR)fn))
            || SUCCEEDED(put_FileName(L"c:\\tmp.srt"));
        if(fTemp) m_xy_bool_opt[BOOL_HIDE_SUBTITLES] = true;
    }

    return fRet;
}

// Canonicalizes a language token (ISO 639-1, 639-2 or an English language
// name) into a lowercase ISO 639-2 code, or empty when unrecognized.
static CString XyCanonicalLang(const CString& s)
{
    CString t = s;
    t.Trim();
    t.MakeLower();
    if (t.IsEmpty())
    {
        return _T("");
    }

    CStringA narrow(t);
    if (t.GetLength() == 2)
    {
        CString c2 = ISO6391To6392(narrow);
        if (!c2.IsEmpty())
        {
            return c2;
        }
    }
    if (t.GetLength() == 3)
    {
        // ISO6392ToLanguage echoes the input back when unknown, so a differing
        // result means the code is valid.
        if (ISO6392ToLanguage(narrow).CompareNoCase(t) != 0)
        {
            return t;
        }
    }

    return LanguageToISO6392(t);
}

// True when a configured token refers to the same language as the filename's
// suffix (e.g. token "jpn" vs suffix "Japanese", "ja" or "jpn").
static bool XyLanguageMatches(const CString& token, const CString& suffix)
{
    CString a = XyCanonicalLang(token);
    if (a.IsEmpty())
    {
        return false;
    }
    CString b = XyCanonicalLang(suffix);
    return !b.IsEmpty() && a == b;
}

bool XySubFilter::LoadSecondarySubtitle(IFilterGraph* pGraph)
{
    XY_LOG_INFO(pGraph);

    CAutoLock cAutolock(&m_csFilter);

    // Empty list => secondary auto-load disabled (stock behaviour).
    CString lang_list = m_xy_str_opt[STRING_SECONDARY_LANGUAGE_LIST];
    lang_list.Trim();
    if (lang_list.IsEmpty())
    {
        return false;
    }

    // An explicitly configured secondary file always wins.
    if (!m_xy_str_opt[STRING_FILE_NAME_SECONDARY].IsEmpty())
    {
        return false;
    }

    int level;
    bool load_external, load_web, load_embedded;
    if (FAILED(get_LoadSettings(&level, &load_external, &load_web, &load_embedded)))
    {
        return false;
    }
    if (level < 0 || level >= LOADLEVEL_COUNT || level == LOADLEVEL_DISABLED)
    {
        return false;
    }
    if (level == LOADLEVEL_ALWAYS)
    {
        load_external = true;
    }
    if (!load_external)
    {
        return false;
    }

    // Media file name, exactly like the primary auto-load resolves it.
    CStringW fn;
    BeginEnumFilters(pGraph, pEF, pBF)
    {
        if (CComQIPtr<IFileSourceFilter> pFSF = pBF)
        {
            LPOLESTR fnw = NULL;
            if (!pFSF || FAILED(pFSF->GetCurFile(&fnw, NULL)) || !fnw)
                continue;
            fn = CString(fnw);
            CoTaskMemFree(fnw);
            break;
        }
    }
    EndEnumFilters;

    if (fn.IsEmpty() || wcsstr(fn, L"http://") || wcsstr(fn, L"mms://"))
    {
        return false;
    }

    // Reuse the existing scanner + extension list rather than a new one.
    CAtlArray<CString> paths;
    for (int i = 0; i < 10; i++)
    {
        CString tmp;
        tmp.Format(IDS_RP_PATH, i);
        CString path = theApp.GetProfileString(ResStr(IDS_R_DEFTEXTPATHES), tmp);
        if (!path.IsEmpty()) paths.Add(path);
    }

    CAtlArray<SubFile> candidates;
    GetSubFileNames(fn, paths, m_xy_str_opt[STRING_LOAD_EXT_LIST], candidates);
    if (candidates.IsEmpty())
    {
        return false;
    }

    // Configured tokens (comma separated).
    CAtlArray<CString> tokens;
    {
        CString rest = lang_list;
        while (!rest.IsEmpty())
        {
            int sep = rest.Find(_T(','));
            CString tok = sep < 0 ? rest : rest.Left(sep);
            tok.Trim();
            if (!tok.IsEmpty()) tokens.Add(tok);
            if (sep < 0) break;
            rest = rest.Mid(sep + 1);
        }
    }
    if (tokens.IsEmpty())
    {
        return false;
    }

    // The primary stream's own file (when determinable) must not be picked as
    // the secondary, or the same subtitle would be composed on top of itself.
    CString primary_file;
    if (CRenderedTextSubtitle* rts = dynamic_cast<CRenderedTextSubtitle*>(m_curSubStream))
    {
        primary_file = rts->m_path;
        primary_file.Replace('\\', '/');
        primary_file.MakeLower();
    }

    for (size_t i = 0; i < candidates.GetCount(); i++)
    {
        const SubFile& cand = candidates[i];
        if (cand.extra_name.IsEmpty())
        {
            continue;
        }

        if (!primary_file.IsEmpty())
        {
            CString cf = cand.full_file_name;
            cf.Replace('\\', '/');
            cf.MakeLower();
            if (cf == primary_file)
            {
                continue;
            }
        }

        bool match = false;
        for (size_t t = 0; t < tokens.GetCount() && !match; t++)
        {
            match = XyLanguageMatches(tokens[t], cand.extra_name);
        }
        if (!match)
        {
            continue;
        }

        XY_LOG_INFO(L"Secondary auto-load matched: "<<cand.full_file_name.GetString());
        CStringW selected = cand.full_file_name;
        XySetString(STRING_FILE_NAME_SECONDARY, (LPWSTR)(LPCWSTR)selected, selected.GetLength());
        XySetBool(BOOL_DUAL_SUB_ENABLED, true);
        return true;
    }

    return false;
}

HRESULT IsPinConnected(IPin *pPin, bool *pResult)
{
    CComPtr<IPin> pTmp = NULL;
    HRESULT hr = pPin->ConnectedTo(&pTmp);
    if (SUCCEEDED(hr))
    {
        *pResult = true;
    }
    else if (hr == VFW_E_NOT_CONNECTED)
    {
        // The pin is not connected. This is not an error for our purposes.
        *pResult = false;
        hr = S_OK;
    }

    return hr;
}

HRESULT XySubFilter::GetRidOfVSFilter()
{
    XY_LOG_INFO("Remove VSFilter from the graph.");
    HRESULT hr = NOERROR;
    CComPtr<IBaseFilter> pBF;
    CComPtr<IPin> video_input_pin,video_output_pin;
    while((pBF=FindFilter(__uuidof(CDirectVobSubFilter2), m_pGraph)) || 
          (pBF=FindFilter(__uuidof(CDirectVobSubFilter ), m_pGraph)))
    {
        BeginEnumPins(pBF, pEP, pPin)
        {
            PIN_DIRECTION dir;
            CComPtr<IPin> pPinTo;

            hr = pPin->QueryDirection(&dir);
            if (FAILED(hr))
            {
                XY_LOG_FATAL("Failed to QueryDirection.");
                return hr;
            }
            if (dir==PINDIR_OUTPUT)
            {
                if (SUCCEEDED(pPin->ConnectedTo(&video_input_pin)))
                {
                    m_pGraph->Disconnect(video_input_pin);
                    m_pGraph->Disconnect(pPin);
                }
            }
            else/*PINDIR_INPUT*/
            {
                if (SUCCEEDED(pPin->ConnectedTo(&pPinTo)))
                {
                    m_pGraph->Disconnect(pPinTo);
                    m_pGraph->Disconnect(pPin);
                    bool is_video_pin = false;
                    BeginEnumMediaTypes(pPinTo, pEM, pmt)
                    {
                        if (pmt->majortype == MEDIATYPE_Video)
                        {
                            is_video_pin = true;
                            break;
                        }
                    }
                    EndEnumMediaTypes(pmt)
                    if (is_video_pin)
                    {
                        video_output_pin = pPinTo;
                    }
                    else if (GetPinCount()>0)
                    {
                        m_pGraph->ConnectDirect(pPinTo, GetPin(m_pSubtitleInputPin.GetCount()-1), NULL);
                    }
                }
            }
        }
        EndEnumPins
        if (video_input_pin && video_output_pin)
        {
            m_pGraph->ConnectDirect(video_input_pin, video_output_pin, NULL);
        }
        hr = m_pGraph->RemoveFilter(pBF);
        if (FAILED(hr))
        {
            XY_LOG_FATAL("Failed to remove filter");
            return hr;
        }
    }
    return hr;
}

HRESULT XySubFilter::StartStreaming()
{
    XY_LOG_INFO("");
    CAutoLock cAutolock(&m_csFilter);

    /* WARNING: calls to m_pGraph member functions from within this function will generate deadlock with Haali
     * Video Renderer in MPC. Reason is that CAutoLock's variables in IFilterGraph functions are overriden by
     * CFGManager class.
     */

    m_fLoading = false;

    m_tbid.fRunOnce = true;

    return S_OK;
}

HRESULT XySubFilter::FindAndConnectConsumer(IFilterGraph* pGraph)
{
    XY_LOG_INFO(pGraph);
    HRESULT hr = NOERROR;
    CComPtr<ISubRenderConsumer> consumer;
    ULONG meric = 0;


    CAutoLock cAutolock(&m_csFilter);
    if(pGraph)
    {
        // Fail-soft coexistence guard. The join-time guard only covers a
        // foreign Xy-family filter already present when we joined; this covers
        // the opposite load order (the other filter became active after we
        // joined). Right before we take exclusive use of the consumer we check
        // again and, when configured to stand down, decline to connect instead
        // of fighting over the consumer or tearing anything down.
        if (m_xy_bool_opt[BOOL_STAND_DOWN_WHEN_ANOTHER_XY_FILTER])
        {
            CString another;
            if (IsAnotherXySubFilterActive(pGraph, &another))
            {
                // Log once per process at WARN level; XY_LOG_ONCE is trace-only.
                static bool stand_down_logged = false;
                if (!stand_down_logged)
                {
                    stand_down_logged = true;
                    XY_LOG_WARN(L"Another Xy-family subtitle filter is active in the graph ("
                        <<another.GetString()<<L"); standing down, subtitles stay off. "
                        L"Set the registry value " DUALSUBS_W_PROFILE_NAME L"\\General\\xy_standdown to 0 "
                        L"to let this filter take over the consumer.");
                }
                m_stood_down_owing_to_other_xy = true;
                {
                    CAutoLock cConsumerLock(&m_csConsumer);
                    m_consumer = NULL;
                }
                return S_FALSE;
            }
        }

        BeginEnumFilters(pGraph, pEF, pBF)
        {
            if(pBF != (IBaseFilter*)this)
            {
                CComQIPtr<ISubRenderConsumer> new_consumer(pBF);
                CLSID filterID;
				hr = pBF->GetClassID(&filterID);
				
				if(!new_consumer && filterID == CLSID_EnhancedVideoRenderer)
				{
					//EVR wouldn't implement ISubRenderConsumer itself, but a custom presenter might.
					CComQIPtr<IMFGetService> evrservices(pBF);
					if(evrservices)
					{
						ISubRenderConsumer * tmpCI = NULL;
						HRESULT hrevr = evrservices->GetService(MR_VIDEO_RENDER_SERVICE, __uuidof(ISubRenderConsumer), (LPVOID*)&tmpCI);
						if(tmpCI)
						{
							CComQIPtr<ISubRenderConsumer> tmpconsumer(tmpCI);
							new_consumer = tmpconsumer;
							SAFE_RELEASE(tmpCI);
						}
					}
				}

                if (new_consumer)
                {
                    ULONG new_meric = 0;
                    HRESULT hr2 = new_consumer->GetMerit(&new_meric);
                    if (SUCCEEDED(hr2))
                    {
                        if (meric < new_meric)
                        {
                            consumer = new_consumer;
                            meric = new_meric;
                        }
                    }
                    else
                    {
                        XY_LOG_ERROR("Failed to get meric from consumer. "<<XY_LOG_VAR_2_STR(pBF));
                        if (!consumer)
                        {
                            XY_LOG_WARN("No consumer found yet.");
                            consumer = new_consumer;
                        }
                    }
					m_consumer = consumer;
					hr = UpdateParamFromConsumer(true);
                }
            }
        }
        EndEnumFilters;//Add a ; so that my editor can do a correct auto-indent
        if (consumer)
        {
            m_consumer = consumer;
            hr = UpdateParamFromConsumer(true);
            if (FAILED(hr))
            {
                XY_LOG_WARN("Failed to read consumer field.");
            }
            hr = consumer->Connect(this);
            if (FAILED(hr))
            {
                XY_LOG_ERROR("Failed to connect "<<XY_LOG_VAR_2_STR(consumer)<<XY_LOG_VAR_2_STR(hr));
                m_consumer = NULL;
                return hr;
            }
            XY_LOG_INFO("Connected with "<<XY_LOG_VAR_2_STR(consumer)<<" "
                <<DumpConsumerInfo().GetString()<<" as "<<DumpProviderInfo().GetString());

            //update filter info string
            do {
                LPWSTR consumer_name = NULL;
                int name_len = 0;
                hr = m_consumer->GetString("name", &consumer_name, &name_len);
                if (FAILED(hr))
                {
                    LocalFree(consumer_name);
                    XY_LOG_WARN("Failed to get consumer name.");
                    break;
                }
                m_filter_info_string.Format(L"%s (=> %s)", m_xy_str_opt[STRING_NAME].GetString(), consumer_name);
                LocalFree(consumer_name);
            } while(0);

            if(!m_hSystrayThread && !m_xy_bool_opt[BOOL_HIDE_TRAY_ICON])
            {
                m_tbid.graph = m_pGraph;
                m_tbid.dvs = this;

                m_hSystrayThread = ::CreateSystray(&m_tbid);
                XY_LOG_INFO("Systray thread created "<<m_hSystrayThread);
            }
        }
        else
        {
            XY_LOG_INFO("No consumer found.");
            hr = S_FALSE;
        }
    }
    else
    {
        XY_LOG_ERROR("Unexpected parameter. "<<"pGraph:"<<(void*)pGraph);
        hr = E_INVALIDARG;
    }
    return hr;
}

bool XySubFilter::IsAnotherXySubFilterActive(IFilterGraph* pGraph, CString* who)
{
    if (!pGraph)
    {
        return false;
    }

    // One pass over the graph, stopping at the first foreign Xy-family filter.
    // We are necessarily in the graph ourselves at this point, so skip "this"
    // by pointer identity (no recursion, no consumer lookup).
    bool found = false;
    BeginEnumFilters(pGraph, pEF, pBF)
    {
        if (pBF == (IBaseFilter*)this)
        {
            continue;
        }
        if (!CComQIPtr<IXySubFilterGraphMutex>(pBF))
        {
            continue;
        }

        found = true;
        if (who)
        {
            who->Empty();
            // Prefer the human readable name the graph knows us by; a filter
            // that has not been added under a name yet falls back to its CLSID.
            CFilterInfo fi;
            if (SUCCEEDED(pBF->QueryFilterInfo(&fi)) && fi.achName[0])
            {
                *who = fi.achName;
            }
            if (who->IsEmpty())
            {
                CLSID clsid = GUID_NULL;
                if (SUCCEEDED(pBF->GetClassID(&clsid)))
                {
                    WCHAR guid[64];
                    if (StringFromGUID2(clsid, guid, countof(guid)))
                    {
                        *who = guid;
                    }
                }
            }
            if (who->IsEmpty())
            {
                *who = L"<unknown Xy-family filter>";
            }
        }
        break;
    }
    EndEnumFilters;
    return found;
}

void XySubFilter::UpdateLanguageCount()
{
    CAutoLock cAutolock(&m_csFilter);

    int tmp = 0;
    POSITION pos = m_pSubStreams.GetHeadPosition();
    while(pos) {
        tmp += m_pSubStreams.GetNext(pos)->GetStreamCount();
    }
    m_xy_int_opt[INT_LANGUAGE_COUNT] = tmp;
}

CStringW XySubFilter::DumpProviderInfo()
{
    CAutoLock cAutoLock(&m_csProviderFields);
    CStringW strTemp;
    strTemp.Format(L"name:'%ls' version:'%ls' yuvMatrix:'%ls' outputLevels:'%ls' combineBitmaps:%ls",
        m_xy_str_opt[STRING_NAME].GetString()      , m_xy_str_opt[STRING_VERSION].GetString(),
        m_xy_str_opt[STRING_YUV_MATRIX].GetString(), m_xy_str_opt[STRING_OUTPUT_LEVELS].GetString(),
        m_xy_bool_opt[BOOL_COMBINE_BITMAPS]?L"True":L"False");
    return strTemp;
}

CStringW XySubFilter::DumpConsumerInfo()
{
    CAutoLock cAutolock(&m_csFilter);
    CStringW strTemp;
    strTemp.Format(L"name:'%ls' version:'%ls' yuvMatrix:'%ls' supportedLevels:'%d'",
        m_xy_str_opt[STRING_CONNECTED_CONSUMER].GetString(), m_xy_str_opt[STRING_CONSUMER_VERSION].GetString(),
        m_xy_str_opt[STRING_CONSUMER_YUV_MATRIX].GetString(), m_xy_int_opt[INT_CONSUMER_SUPPORTED_LEVELS]);
    return strTemp;
}
