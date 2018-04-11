/* 
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include <afxdlgs.h>
#include <atlpath.h>
#include "resource.h"
#include "../../../Subtitles/VobSubFile.h"
#include "../../../Subtitles/RTS.h"
#include "../../../Subtitles/SSF.h"
#include "../../../SubPic/PooledSubPic.h"
#include "../../../SubPic/SimpleSubPicProviderImpl.h"
#include "../../../subpic/color_conv_table.h"
#include "../../../subpic/SimpleSubPicWrapper.h"
#include "DirectVobSub.h"
#include "vfr.h"

#ifndef _WIN64
#include "vd2/extras/FilterSDK/VirtualDub.h"
#else
#include "vd2/plugin/vdplugin.h"
#include "vd2/plugin/vdvideofilt.h"
#endif

#include <emmintrin.h>

using namespace DirectVobSubXyOptions;

//
// Generic interface
//

namespace Plugin
{

class CFilter : public CUnknown, public CDirectVobSub, public CAMThread, public CCritSec
{
private:
	CString m_fn;

protected:
	float m_fps;
	CCritSec m_csSubLock;
	CComPtr<ISimpleSubPicProvider> m_simple_provider;
	CComPtr<ISubPicProvider> m_pSubPicProvider;
	DWORD_PTR m_SubPicProviderId;

    CSimpleTextSubtitle::YCbCrMatrix m_script_selected_yuv;
    CSimpleTextSubtitle::YCbCrRange m_script_selected_range;

    bool m_fLazyInit;
public:
    CFilter() 
        : CUnknown(NAME("CFilter"), NULL)
        , CDirectVobSub(DirectVobFilterOptions, &m_csSubLock)
        , m_fps(-1), m_SubPicProviderId(0), m_fLazyInit(false)
    {
        m_xy_str_opt[STRING_NAME] = L"CFilter";

        //fix me: should not do init here
        CacheManager::GetPathDataMruCache()->SetMaxItemNum(m_xy_int_opt[INT_PATH_DATA_CACHE_MAX_ITEM_NUM]);
        CacheManager::GetScanLineData2MruCache()->SetMaxItemNum(m_xy_int_opt[INT_SCAN_LINE_DATA_CACHE_MAX_ITEM_NUM]);
        CacheManager::GetOverlayNoBlurMruCache()->SetMaxItemNum(m_xy_int_opt[INT_OVERLAY_NO_BLUR_CACHE_MAX_ITEM_NUM]);
        CacheManager::GetOverlayMruCache()->SetMaxItemNum(m_xy_int_opt[INT_OVERLAY_CACHE_MAX_ITEM_NUM]);

        XyFwGroupedDrawItemsHashKey::GetCacher()->SetMaxItemNum(m_xy_int_opt[INT_BITMAP_MRU_CACHE_ITEM_NUM]);
        CacheManager::GetBitmapMruCache()->SetMaxItemNum(m_xy_int_opt[INT_BITMAP_MRU_CACHE_ITEM_NUM]);

        CacheManager::GetClipperAlphaMaskMruCache()->SetMaxItemNum(m_xy_int_opt[INT_CLIPPER_MRU_CACHE_ITEM_NUM]);
        CacheManager::GetTextInfoCache()->SetMaxItemNum(m_xy_int_opt[INT_TEXT_INFO_CACHE_ITEM_NUM]);
        //CacheManager::GetAssTagListMruCache()->SetMaxItemNum(m_xy_int_opt[INT_ASS_TAG_LIST_CACHE_ITEM_NUM]);

        SubpixelPositionControler::GetGlobalControler().SetSubpixelLevel( static_cast<SubpixelPositionControler::SUBPIXEL_LEVEL>(m_xy_int_opt[INT_SUBPIXEL_POS_LEVEL]) );

        m_script_selected_yuv = CSimpleTextSubtitle::YCbCrMatrix_AUTO;
        m_script_selected_range = CSimpleTextSubtitle::YCbCrRange_AUTO;

        CAMThread::Create();
      }
	virtual ~CFilter() {CAMThread::CallWorker(0);}

    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv)
    {
        CheckPointer(ppv, E_POINTER);
        
        return QI(IDirectVobSub)
            QI(IDirectVobSub2)
            QI(IXyOptions)
            QI(IFilterVersion)
            __super::NonDelegatingQueryInterface(riid, ppv);
    }
    
	CString GetFileName() {CAutoLock cAutoLock(this); return m_fn;}
	void SetFileName(CString fn) {CAutoLock cAutoLock(this); m_fn = fn;}

    void SetYuvMatrix(SubPicDesc& dst)
    {
        ColorConvTable::YuvMatrixType yuv_matrix = ColorConvTable::BT601;
        ColorConvTable::YuvRangeType yuv_range = ColorConvTable::RANGE_TV;

        if ( m_xy_int_opt[INT_COLOR_SPACE]==CDirectVobSub::YuvMatrix_AUTO )
        {
            switch(m_script_selected_yuv)
            {
            case CSimpleTextSubtitle::YCbCrMatrix_BT601:
                yuv_matrix = ColorConvTable::BT601;
                break;
            case CSimpleTextSubtitle::YCbCrMatrix_BT709:
                yuv_matrix = ColorConvTable::BT709;
                break;
            case CSimpleTextSubtitle::YCbCrMatrix_AUTO:
            default:
                yuv_matrix = ColorConvTable::BT601;                
                break;
            }
        }
        else
        {
            switch(m_xy_int_opt[INT_COLOR_SPACE])
            {
            case CDirectVobSub::BT_601:
                yuv_matrix = ColorConvTable::BT601;
                break;
            case CDirectVobSub::BT_709:
                yuv_matrix = ColorConvTable::BT709;
                break;
            case CDirectVobSub::GUESS:
                yuv_matrix = (dst.w > m_bt601Width || dst.h > m_bt601Height) ? ColorConvTable::BT709 : ColorConvTable::BT601;
                break;
            }
        }

        if( m_xy_int_opt[INT_YUV_RANGE]==CDirectVobSub::YuvRange_Auto )
        {
            switch(m_script_selected_range)
            {
            case CSimpleTextSubtitle::YCbCrRange_PC:
                yuv_range = ColorConvTable::RANGE_PC;
                break;
            case CSimpleTextSubtitle::YCbCrRange_TV:
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            case CSimpleTextSubtitle::YCbCrRange_AUTO:
            default:        
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            }
        }
        else
        {
            switch(m_xy_int_opt[INT_YUV_RANGE])
            {
            case CDirectVobSub::YuvRange_TV:
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            case CDirectVobSub::YuvRange_PC:
                yuv_range = ColorConvTable::RANGE_PC;
                break;
            case CDirectVobSub::YuvRange_Auto:
                yuv_range = ColorConvTable::RANGE_TV;
                break;
            }
        }

        ColorConvTable::SetDefaultConvType(yuv_matrix, yuv_range);
    }

    bool Render(SubPicDesc& dst, REFERENCE_TIME rt, float fps)
    {
        HRESULT hr = NOERROR;
        if(!m_pSubPicProvider)
            return(false);

          if(!m_fLazyInit)
          {
            m_fLazyInit = true;

            SetYuvMatrix(dst);
          }
        CSize size(dst.w, dst.h);

          if(!m_simple_provider)
          {
            if(!(m_simple_provider = new SimpleSubPicProvider2(dst.type, size, size, CRect(CPoint(0,0), size), this, &hr)) || FAILED(hr))
            {
              m_simple_provider = NULL;
              return(false);
            }
#if 0
            hr = XySetSize(SIZE_ORIGINAL_VIDEO, size); // E_INVALID_ARG
            CHECK_N_LOG(hr, "Failed to set option");
#endif
            m_xy_size_opt[SIZE_ORIGINAL_VIDEO] = size; // PF20180411 readonly property, set here
          }

          if(m_SubPicProviderId != (DWORD_PTR)(ISubPicProvider*)m_pSubPicProvider)
          {
            CSize playres(0,0);
            CLSID clsid;
            CComQIPtr<IPersist> tmp = m_pSubPicProvider;
            tmp->GetClassID(&clsid);
            if(clsid == __uuidof(CRenderedTextSubtitle))
            {
              CRenderedTextSubtitle* pRTS = dynamic_cast<CRenderedTextSubtitle*>((ISubPicProvider*)m_pSubPicProvider);
              playres = pRTS->m_dstScreenSize;
            }
            m_xy_size_opt[SIZE_ASS_PLAY_RESOLUTION] = playres;

            m_simple_provider->SetSubPicProvider(m_pSubPicProvider);
            m_SubPicProviderId = (DWORD_PTR)(ISubPicProvider*)m_pSubPicProvider;
          }

		CComPtr<ISimpleSubPic> pSubPic;
		if(!m_simple_provider->LookupSubPic(rt, &pSubPic))
			return(false);

        if(dst.type == MSP_RGB32 || dst.type == MSP_RGB24 || dst.type == MSP_RGB16 || dst.type == MSP_RGB15)
            dst.h = -dst.h;
          pSubPic->AlphaBlt(&dst);

        return(true);
    }

	DWORD ThreadProc()
	{
		SetThreadPriority(m_hThread, THREAD_PRIORITY_LOWEST);

		CAtlArray<HANDLE> handles;
		handles.Add(GetRequestHandle());

		CString fn = GetFileName();
		CFileStatus fs;
		fs.m_mtime = 0;
		CFileGetStatus(fn, fs);

		while(1)
		{
			DWORD i = WaitForMultipleObjects(handles.GetCount(), handles.GetData(), FALSE, 1000);

			if(WAIT_OBJECT_0 == i)
			{
				Reply(S_OK);
				break;
			}
			else if(WAIT_OBJECT_0 + 1 >= i && i <= WAIT_OBJECT_0 + handles.GetCount())
			{
				if(FindNextChangeNotification(handles[i - WAIT_OBJECT_0]))
				{
					CFileStatus fs2;
					fs2.m_mtime = 0;
					CFileGetStatus(fn, fs2);

					if(fs.m_mtime < fs2.m_mtime)
					{
						fs.m_mtime = fs2.m_mtime;

						if(CComQIPtr<ISubStream> pSubStream = m_pSubPicProvider)
						{
							CAutoLock cAutoLock(&m_csSubLock);
							pSubStream->Reload();
						}
					}
				}
			}
			else if(WAIT_TIMEOUT == i)
			{
				CString fn2 = GetFileName();

				if(fn != fn2)
				{
					CPath p(fn2);
					p.RemoveFileSpec();
					HANDLE h = FindFirstChangeNotification(p, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE); 
					if(h != INVALID_HANDLE_VALUE)
					{
						fn = fn2;
						handles.SetCount(1);
						handles.Add(h);
					}
				}
			}
			else // if(WAIT_ABANDONED_0 == i || WAIT_FAILED == i)
			{
				break;
			}
		}

		m_hThread = 0;

		for(size_t i = 1; i < handles.GetCount(); i++)
			FindCloseChangeNotification(handles[i]);

		return 0;
	}
};

class CVobSubFilter : virtual public CFilter
{
public:
	CVobSubFilter(CString fn = _T(""))
	{
		if(!fn.IsEmpty()) Open(fn);
	}

	bool Open(CString fn)
	{
		SetFileName(_T(""));
		m_pSubPicProvider = NULL;

		if(CVobSubFile* vsf = new CVobSubFile(&m_csSubLock))
		{
			m_pSubPicProvider = (ISubPicProvider*)vsf;
			if(vsf->Open(CString(fn))) SetFileName(fn);
			else m_pSubPicProvider = NULL;
		}

		return !!m_pSubPicProvider;
	}
};

class CTextSubFilter : virtual public CFilter
{
	int m_CharSet;

public:
	CTextSubFilter(CString fn = _T(""), int CharSet = DEFAULT_CHARSET, float fps = -1)
		: m_CharSet(CharSet)
	{
		m_fps = fps;
		if(!fn.IsEmpty()) Open(fn, CharSet);
	}

	int GetCharSet() {return(m_CharSet);}

	bool Open(CString fn, int CharSet = DEFAULT_CHARSET)
	{
		SetFileName(_T(""));
		m_pSubPicProvider = NULL;

      if(!m_pSubPicProvider)
      {
        if(ssf::CRenderer* ssf = new ssf::CRenderer(&m_csSubLock))
        {
          m_pSubPicProvider = (ISubPicProvider*)ssf;
          if(ssf->Open(CString(fn))) SetFileName(fn);
          else m_pSubPicProvider = NULL;
        }
      }

      if(!m_pSubPicProvider)
      {
        if(CRenderedTextSubtitle* rts = new CRenderedTextSubtitle(&m_csSubLock))
        {
          m_pSubPicProvider = (ISubPicProvider*)rts;
          if(rts->Open(CString(fn), CharSet)) SetFileName(fn);
          else m_pSubPicProvider = NULL;

          m_script_selected_yuv = rts->m_eYCbCrMatrix;
          m_script_selected_range = rts->m_eYCbCrRange;
        }
      }

		return !!m_pSubPicProvider;
	}
};

#ifndef _WIN64
    //
    // old VirtualDub interface
    //

    namespace VirtualDub
    {
        class CVirtualDubFilter : virtual public CFilter
        {
        public:
            CVirtualDubFilter() {}
            virtual ~CVirtualDubFilter() {}

            virtual int RunProc(const FilterActivation* fa, const FilterFunctions* ff) {
                SubPicDesc dst;
                dst.type = MSP_RGB32;
                dst.w = fa->src.w;
                dst.h = fa->src.h;
                dst.bpp = 32;
                dst.pitch = fa->src.pitch;
                dst.bits = (BYTE*)fa->src.data;

                Render(dst, 10000i64 * fa->pfsi->lSourceFrameMS, (float)1000 / fa->pfsi->lMicrosecsPerFrame);

                return 0;
            }

            virtual long ParamProc(FilterActivation* fa, const FilterFunctions* ff) {
                fa->dst.offset  = fa->src.offset;
                fa->dst.modulo  = fa->src.modulo;
                fa->dst.pitch   = fa->src.pitch;

                return 0;
            }

            virtual int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) = 0;
            virtual void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) = 0;
            virtual bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) = 0;
        };

        class CVobSubVirtualDubFilter : public CVobSubFilter, public CVirtualDubFilter
        {
        public:
            CVobSubVirtualDubFilter(CString fn = _T(""))
                : CVobSubFilter(fn) {}

            int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) {
                AFX_MANAGE_STATE(AfxGetStaticModuleState());

                CFileDialog fd(TRUE, NULL, GetFileName(), OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY,
                               _T("VobSub files (*.idx;*.sub)|*.idx;*.sub||"), CWnd::FromHandle(hwnd), 0);

                if (fd.DoModal() != IDOK) {
                    return 1;
                }

                return Open(fd.GetPathName()) ? 0 : 1;
            }

            void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) {
                sprintf(str, " (%s)", !GetFileName().IsEmpty() ? CStringA(GetFileName()) : " (empty)");
            }

            bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) {
                CStringA fn(GetFileName());
                fn.Replace("\\", "\\\\");
                _snprintf_s(buf, buflen, buflen, "Config(\"%s\")", fn);
                return true;
            }
        };

        class CTextSubVirtualDubFilter : public CTextSubFilter, public CVirtualDubFilter
        {
        public:
            CTextSubVirtualDubFilter(CString fn = _T(""), int CharSet = DEFAULT_CHARSET)
                : CTextSubFilter(fn, CharSet) {}

            int ConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd) {
                AFX_MANAGE_STATE(AfxGetStaticModuleState());

                const TCHAR formats[] = _T("TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||");
                CFileDialog fd(TRUE, NULL, GetFileName(), OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_ENABLETEMPLATE | OFN_ENABLEHOOK,
                               formats, CWnd::FromHandle(hwnd), sizeof(OPENFILENAME));
                UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam);

                fd.m_pOFN->hInstance = AfxGetResourceHandle();
                fd.m_pOFN->lpTemplateName = MAKEINTRESOURCE(IDD_TEXTSUBOPENTEMPLATE);
                fd.m_pOFN->lpfnHook = (LPOFNHOOKPROC)OpenHookProc;
                fd.m_pOFN->lCustData = (LPARAM)DEFAULT_CHARSET;

                if (fd.DoModal() != IDOK) {
                    return 1;
                }

                return Open(fd.GetPathName(), fd.m_pOFN->lCustData) ? 0 : 1;
            }

            void StringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str) {
                if (!GetFileName().IsEmpty()) {
                    sprintf(str, " (%s, %d)", CStringA(GetFileName()), GetCharSet());
                } else {
                    sprintf(str, " (empty)");
                }
            }

            bool FssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen) {
                CStringA fn(GetFileName());
                fn.Replace("\\", "\\\\");
                _snprintf_s(buf, buflen, buflen, "Config(\"%s\", %d)", fn, GetCharSet());
                return true;
            }
        };

        int vobsubInitProc(FilterActivation* fa, const FilterFunctions* ff)
        {
            *(CVirtualDubFilter**)fa->filter_data = DEBUG_NEW CVobSubVirtualDubFilter();
            return !(*(CVirtualDubFilter**)fa->filter_data);
        }

        int textsubInitProc(FilterActivation* fa, const FilterFunctions* ff)
        {
            *(CVirtualDubFilter**)fa->filter_data = DEBUG_NEW CTextSubVirtualDubFilter();
            return !(*(CVirtualDubFilter**)fa->filter_data);
        }

        void baseDeinitProc(FilterActivation* fa, const FilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            SAFE_DELETE(f);
        }

        int baseRunProc(const FilterActivation* fa, const FilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->RunProc(fa, ff) : 1;
        }

        long baseParamProc(FilterActivation* fa, const FilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->ParamProc(fa, ff) : 1;
        }

        int baseConfigProc(FilterActivation* fa, const FilterFunctions* ff, HWND hwnd)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->ConfigProc(fa, ff, hwnd) : 1;
        }

        void baseStringProc(const FilterActivation* fa, const FilterFunctions* ff, char* str)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                f->StringProc(fa, ff, str);
            }
        }

        bool baseFssProc(FilterActivation* fa, const FilterFunctions* ff, char* buf, int buflen)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->FssProc(fa, ff, buf, buflen) : false;
        }

        void vobsubScriptConfig(IScriptInterpreter* isi, void* lpVoid, CScriptValue* argv, int argc)
        {
            FilterActivation* fa = (FilterActivation*)lpVoid;
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                delete f;
            }
            f = DEBUG_NEW CVobSubVirtualDubFilter(CString(*argv[0].asString()));
            *(CVirtualDubFilter**)fa->filter_data = f;
        }

        void textsubScriptConfig(IScriptInterpreter* isi, void* lpVoid, CScriptValue* argv, int argc)
        {
            FilterActivation* fa = (FilterActivation*)lpVoid;
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                delete f;
            }
            f = DEBUG_NEW CTextSubVirtualDubFilter(CString(*argv[0].asString()), argv[1].asInt());
            *(CVirtualDubFilter**)fa->filter_data = f;
        }

        ScriptFunctionDef vobsub_func_defs[] = {
            { (ScriptFunctionPtr)vobsubScriptConfig, "Config", "0s" },
            { NULL },
        };

        CScriptObject vobsub_obj = {
            NULL, vobsub_func_defs
        };

        struct FilterDefinition filterDef_vobsub = {
            NULL, NULL, NULL,           // next, prev, module
            "VobSub",                   // name
            "Adds subtitles from a vob sequence.", // desc
            "Gabest",                   // maker
            NULL,                       // private_data
            sizeof(CVirtualDubFilter**), // inst_data_size
            vobsubInitProc,             // initProc
            baseDeinitProc,             // deinitProc
            baseRunProc,                // runProc
            baseParamProc,              // paramProc
            baseConfigProc,             // configProc
            baseStringProc,             // stringProc
            NULL,                       // startProc
            NULL,                       // endProc
            &vobsub_obj,                // script_obj
            baseFssProc,                // fssProc
        };

        ScriptFunctionDef textsub_func_defs[] = {
            { (ScriptFunctionPtr)textsubScriptConfig, "Config", "0si" },
            { NULL },
        };

        CScriptObject textsub_obj = {
            NULL, textsub_func_defs
        };

        struct FilterDefinition filterDef_textsub = {
            NULL, NULL, NULL,           // next, prev, module
            "TextSub",                  // name
            "Adds subtitles from srt, sub, psb, smi, ssa, ass file formats.", // desc
            "Gabest",                   // maker
            NULL,                       // private_data
            sizeof(CVirtualDubFilter**), // inst_data_size
            textsubInitProc,            // initProc
            baseDeinitProc,             // deinitProc
            baseRunProc,                // runProc
            baseParamProc,              // paramProc
            baseConfigProc,             // configProc
            baseStringProc,             // stringProc
            NULL,                       // startProc
            NULL,                       // endProc
            &textsub_obj,               // script_obj
            baseFssProc,                // fssProc
        };

        static FilterDefinition* fd_vobsub;
        static FilterDefinition* fd_textsub;

        extern "C" __declspec(dllexport) int __cdecl VirtualdubFilterModuleInit2(FilterModule* fm, const FilterFunctions* ff, int& vdfd_ver, int& vdfd_compat)
        {
            fd_vobsub = ff->addFilter(fm, &filterDef_vobsub, sizeof(FilterDefinition));
            if (!fd_vobsub) {
                return 1;
            }
            fd_textsub = ff->addFilter(fm, &filterDef_textsub, sizeof(FilterDefinition));
            if (!fd_textsub) {
                return 1;
            }

            vdfd_ver = VIRTUALDUB_FILTERDEF_VERSION;
            vdfd_compat = VIRTUALDUB_FILTERDEF_COMPATIBLE;

            return 0;
        }

        extern "C" __declspec(dllexport) void __cdecl VirtualdubFilterModuleDeinit(FilterModule* fm, const FilterFunctions* ff)
        {
            ff->removeFilter(fd_textsub);
            ff->removeFilter(fd_vobsub);
        }
    }/**/

#else
    //
    // VirtualDub new plugin interface sdk 1.1
    //
    namespace VirtualDubNew
    {
        class CVirtualDubFilter : virtual public CFilter
        {
        public:
            CVirtualDubFilter() {}
            virtual ~CVirtualDubFilter() {}

            virtual int RunProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff) {
                SubPicDesc dst;
                dst.type = MSP_RGB32;
                dst.w = fa->src.w;
                dst.h = fa->src.h;
                dst.bpp = 32;
                dst.pitch = fa->src.pitch;
                dst.bits = (BYTE*)fa->src.data;

                Render(dst, 10000i64 * fa->pfsi->lSourceFrameMS, (float)1000 / fa->pfsi->lMicrosecsPerFrame);

                return 0;
            }

            virtual long ParamProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff) {
                fa->dst.offset  = fa->src.offset;
                fa->dst.modulo  = fa->src.modulo;
                fa->dst.pitch   = fa->src.pitch;

                return 0;
            }

            virtual int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) = 0;
            virtual void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) = 0;
            virtual bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) = 0;
        };

        class CVobSubVirtualDubFilter : public CVobSubFilter, public CVirtualDubFilter
        {
        public:
            CVobSubVirtualDubFilter(CString fn = _T(""))
                : CVobSubFilter(fn) {}

            int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) {
                AFX_MANAGE_STATE(AfxGetStaticModuleState());

                CFileDialog fd(TRUE, NULL, GetFileName(), OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY,
                               _T("VobSub files (*.idx;*.sub)|*.idx;*.sub||"), CWnd::FromHandle((HWND)hwnd), 0);

                if (fd.DoModal() != IDOK) {
                    return 1;
                }

                return Open(fd.GetPathName()) ? 0 : 1;
            }

            void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) {
                sprintf(str, " (%s)", !GetFileName().IsEmpty() ? CStringA(GetFileName()) : " (empty)");
            }

            bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) {
                CStringA fn(GetFileName());
                fn.Replace("\\", "\\\\");
                _snprintf_s(buf, buflen, buflen, "Config(\"%s\")", fn);
                return true;
            }
        };

        class CTextSubVirtualDubFilter : public CTextSubFilter, public CVirtualDubFilter
        {
        public:
            CTextSubVirtualDubFilter(CString fn = _T(""), int CharSet = DEFAULT_CHARSET)
                : CTextSubFilter(fn, CharSet) {}

            int ConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd) {
                AFX_MANAGE_STATE(AfxGetStaticModuleState());

                /* off encoding changing */
#ifndef _DEBUG
                const TCHAR formats[] = _T("TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||");
                CFileDialog fd(TRUE, NULL, GetFileName(), OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_ENABLETEMPLATE | OFN_ENABLEHOOK,
                               formats, CWnd::FromHandle((HWND)hwnd), sizeof(OPENFILENAME));
                UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam);

                fd.m_pOFN->hInstance = AfxGetResourceHandle();
                fd.m_pOFN->lpTemplateName = MAKEINTRESOURCE(IDD_TEXTSUBOPENTEMPLATE);
                fd.m_pOFN->lpfnHook = (LPOFNHOOKPROC)OpenHookProc;
                fd.m_pOFN->lCustData = (LPARAM)DEFAULT_CHARSET;
#else
                const TCHAR formats[] = _T("TextSub files (*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt)|*.sub;*.srt;*.smi;*.ssa;*.ass;*.xss;*.psb;*.txt||");
                CFileDialog fd(TRUE, NULL, GetFileName(), OFN_ENABLESIZING | OFN_HIDEREADONLY,
                               formats, CWnd::FromHandle((HWND)hwnd), sizeof(OPENFILENAME));
#endif
                if (fd.DoModal() != IDOK) {
                    return 1;
                }

                return Open(fd.GetPathName(), fd.m_pOFN->lCustData) ? 0 : 1;
            }

            void StringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str) {
                if (!GetFileName().IsEmpty()) {
                    sprintf(str, " (%s, %d)", CStringA(GetFileName()), GetCharSet());
                } else {
                    sprintf(str, " (empty)");
                }
            }

            bool FssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen) {
                CStringA fn(GetFileName());
                fn.Replace("\\", "\\\\");
                _snprintf_s(buf, buflen, buflen, "Config(\"%s\", %d)", fn, GetCharSet());
                return true;
            }
        };

        int vobsubInitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
        {
            return ((*(CVirtualDubFilter**)fa->filter_data = DEBUG_NEW CVobSubVirtualDubFilter()) == NULL);
        }

        int textsubInitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
        {
            return ((*(CVirtualDubFilter**)fa->filter_data = DEBUG_NEW CTextSubVirtualDubFilter()) == NULL);
        }

        void baseDeinitProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            SAFE_DELETE(f);
        }

        int baseRunProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->RunProc(fa, ff) : 1;
        }

        long baseParamProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->ParamProc(fa, ff) : 1;
        }

        int baseConfigProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, VDXHWND hwnd)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->ConfigProc(fa, ff, hwnd) : 1;
        }

        void baseStringProc(const VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* str)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                f->StringProc(fa, ff, str);
            }
        }

        bool baseFssProc(VDXFilterActivation* fa, const VDXFilterFunctions* ff, char* buf, int buflen)
        {
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            return f ? f->FssProc(fa, ff, buf, buflen) : false;
        }

        void vobsubScriptConfig(IVDXScriptInterpreter* isi, void* lpVoid, VDXScriptValue* argv, int argc)
        {
            VDXFilterActivation* fa = (VDXFilterActivation*)lpVoid;
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                delete f;
            }
            f = DEBUG_NEW CVobSubVirtualDubFilter(CString(*argv[0].asString()));
            *(CVirtualDubFilter**)fa->filter_data = f;
        }

        void textsubScriptConfig(IVDXScriptInterpreter* isi, void* lpVoid, VDXScriptValue* argv, int argc)
        {
            VDXFilterActivation* fa = (VDXFilterActivation*)lpVoid;
            CVirtualDubFilter* f = *(CVirtualDubFilter**)fa->filter_data;
            if (f) {
                delete f;
            }
            f = DEBUG_NEW CTextSubVirtualDubFilter(CString(*argv[0].asString()), argv[1].asInt());
            *(CVirtualDubFilter**)fa->filter_data = f;
        }

        VDXScriptFunctionDef vobsub_func_defs[] = {
            { (VDXScriptFunctionPtr)vobsubScriptConfig, "Config", "0s" },
            { NULL },
        };

        VDXScriptObject vobsub_obj = {
            NULL, vobsub_func_defs
        };

        struct VDXFilterDefinition filterDef_vobsub = {
            NULL, NULL, NULL,           // next, prev, module
            "VobSub",                   // name
            "Adds subtitles from a vob sequence.", // desc
            "Gabest",                   // maker
            NULL,                       // private_data
            sizeof(CVirtualDubFilter**), // inst_data_size
            vobsubInitProc,             // initProc
            baseDeinitProc,             // deinitProc
            baseRunProc,                // runProc
            baseParamProc,              // paramProc
            baseConfigProc,             // configProc
            baseStringProc,             // stringProc
            NULL,                       // startProc
            NULL,                       // endProc
            &vobsub_obj,                // script_obj
            baseFssProc,                // fssProc
        };

        VDXScriptFunctionDef textsub_func_defs[] = {
            { (VDXScriptFunctionPtr)textsubScriptConfig, "Config", "0si" },
            { NULL },
        };

        VDXScriptObject textsub_obj = {
            NULL, textsub_func_defs
        };

        struct VDXFilterDefinition filterDef_textsub = {
            NULL, NULL, NULL,           // next, prev, module
            "TextSub",                  // name
            "Adds subtitles from srt, sub, psb, smi, ssa, ass file formats.", // desc
            "Gabest",                   // maker
            NULL,                       // private_data
            sizeof(CVirtualDubFilter**), // inst_data_size
            textsubInitProc,            // initProc
            baseDeinitProc,             // deinitProc
            baseRunProc,                // runProc
            baseParamProc,              // paramProc
            baseConfigProc,             // configProc
            baseStringProc,             // stringProc
            NULL,                       // startProc
            NULL,                       // endProc
            &textsub_obj,               // script_obj
            baseFssProc,                // fssProc
        };

        static VDXFilterDefinition* fd_vobsub;
        static VDXFilterDefinition* fd_textsub;

        extern "C" __declspec(dllexport) int __cdecl VirtualdubFilterModuleInit2(VDXFilterModule* fm, const VDXFilterFunctions* ff, int& vdfd_ver, int& vdfd_compat)
        {
            if (((fd_vobsub = ff->addFilter(fm, &filterDef_vobsub, sizeof(VDXFilterDefinition))) == NULL)
                    || ((fd_textsub = ff->addFilter(fm, &filterDef_textsub, sizeof(VDXFilterDefinition))) == NULL)) {
                return 1;
            }

            vdfd_ver = VIRTUALDUB_FILTERDEF_VERSION;
            vdfd_compat = VIRTUALDUB_FILTERDEF_COMPATIBLE;

            return 0;
        }

        extern "C" __declspec(dllexport) void __cdecl VirtualdubFilterModuleDeinit(VDXFilterModule* fm, const VDXFilterFunctions* ff)
        {
            ff->removeFilter(fd_textsub);
            ff->removeFilter(fd_vobsub);
        }
    }
#endif
    //
    // Avisynth interface
    //
#if 0 // no CPP 2.0 interface! PF 20180224
    namespace AviSynth1
    {
#include "avisynth/avisynth1.h"

        class CAvisynthFilter : public GenericVideoFilter, virtual public CFilter
        {
        public:
            CAvisynthFilter(PClip c, IScriptEnvironment* env) : GenericVideoFilter(c) {}

            PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) {
                PVideoFrame frame = child->GetFrame(n, env);

                env->MakeWritable(&frame);

                SubPicDesc dst;
                dst.w = vi.width;
                dst.h = vi.height;
                dst.pitch = frame->GetPitch();
                dst.bits = (BYTE*)frame->GetWritePtr();
                dst.bpp = vi.BitsPerPixel();
                dst.type =
                    vi.IsRGB32() ? (env->GetVar("RGBA").AsBool() ? MSP_RGBA : MSP_RGB32) :
                        vi.IsRGB24() ? MSP_RGB24 :
                        vi.IsYUY2() ? MSP_YUY2 :
                        -1;

                float fps = m_fps > 0 ? m_fps : (float)vi.fps_numerator / vi.fps_denominator;

                Render(dst, (REFERENCE_TIME)(10000000i64 * n / fps), fps);

                return frame;
            }
        };

        class CVobSubAvisynthFilter : public CVobSubFilter, public CAvisynthFilter
        {
        public:
            CVobSubAvisynthFilter(PClip c, const char* fn, IScriptEnvironment* env)
                : CVobSubFilter(CString(fn))
                , CAvisynthFilter(c, env) {
                if (!m_pSubPicProvider) {
                    env->ThrowError("VobSub: Can't open \"%s\"", fn);
                }
            }
        };

        AVSValue __cdecl VobSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            return (DEBUG_NEW CVobSubAvisynthFilter(args[0].AsClip(), args[1].AsString(), env));
        }

        class CTextSubAvisynthFilter : public CTextSubFilter, public CAvisynthFilter
        {
        public:
            CTextSubAvisynthFilter(PClip c, IScriptEnvironment* env, const char* fn, int CharSet = DEFAULT_CHARSET, float fps = -1)
                : CTextSubFilter(CString(fn), CharSet, fps)
                , CAvisynthFilter(c, env) {
                if (!m_pSubPicProvider) {
                    env->ThrowError("TextSub: Can't open \"%s\"", fn);
                }
            }
        };

        AVSValue __cdecl TextSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            return (DEBUG_NEW CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString()));
        }

        AVSValue __cdecl TextSubCreateSI(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            return (DEBUG_NEW CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString(), args[2].AsInt()));
        }

        AVSValue __cdecl TextSubCreateSIF(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            return (DEBUG_NEW CTextSubAvisynthFilter(args[0].AsClip(), env, args[1].AsString(), args[2].AsInt(), (float)args[3].AsFloat()));
        }

        AVSValue __cdecl MaskSubCreateSIIFI(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            AVSValue rgb32("RGB32");
            AVSValue  tab[5] = {
                args[1],
                args[2],
                args[3],
                args[4],
                rgb32
            };
            AVSValue value(tab, 5);
            const char* nom[5] = {
                "width",
                "height",
                "fps",
                "length",
                "pixel_type"
            };
            AVSValue clip(env->Invoke("Blackness", value, nom));
            env->SetVar(env->SaveString("RGBA"), true);
            return (DEBUG_NEW CTextSubAvisynthFilter(clip.AsClip(), env, args[0].AsString()));
        }

        extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit(IScriptEnvironment* env)
        {
            env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
            env->AddFunction("TextSub", "cs", TextSubCreateS, 0);
            env->AddFunction("TextSub", "csi", TextSubCreateSI, 0);
            env->AddFunction("TextSub", "csif", TextSubCreateSIF, 0);
            env->AddFunction("MaskSub", "siifi", MaskSubCreateSIIFI, 0);
            env->SetVar(env->SaveString("RGBA"), false);
            return NULL;
        }
    }
#endif

#if 0 // no CPP 2.5 interface! PF 20180224
    namespace AviSynth25
    {
#include "avisynth/avisynth25.h"

        static bool s_fSwapUV = false;

        class CAvisynthFilter : public GenericVideoFilter, virtual public CFilter
        {
        public:
            VFRTranslator* vfr;

            CAvisynthFilter(PClip c, IScriptEnvironment* env, VFRTranslator* _vfr = 0) : GenericVideoFilter(c), vfr(_vfr) {}

            PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) {
                PVideoFrame frame = child->GetFrame(n, env);

                env->MakeWritable(&frame);

                SubPicDesc dst;
                dst.w = vi.width;
                dst.h = vi.height;
                dst.pitch = frame->GetPitch();
                dst.pitchUV = frame->GetPitch(PLANAR_U);
                dst.bits = (BYTE*)frame->GetWritePtr();
                dst.bitsU = frame->GetWritePtr(PLANAR_U);
                dst.bitsV = frame->GetWritePtr(PLANAR_V);
                dst.bpp = dst.pitch / dst.w * 8; //vi.BitsPerPixel();
                dst.type =
                    vi.IsRGB32() ? (env->GetVar("RGBA").AsBool() ? MSP_RGBA : MSP_RGB32)  :
                        vi.IsRGB24() ? MSP_RGB24 :
                        vi.IsYUY2() ? MSP_YUY2 :
                /*vi.IsYV12()*/ vi.pixel_type == VideoInfo::CS_YV12 ? (s_fSwapUV ? MSP_IYUV : MSP_YV12) :
                /*vi.IsIYUV()*/ vi.pixel_type == VideoInfo::CS_IYUV ? (s_fSwapUV ? MSP_YV12 : MSP_IYUV) :
                        -1;

                float fps = m_fps > 0 ? m_fps : (float)vi.fps_numerator / vi.fps_denominator;

                REFERENCE_TIME timestamp;

                if (!vfr) {
                    timestamp = (REFERENCE_TIME)(10000000i64 * n / fps);
                } else {
                    timestamp = (REFERENCE_TIME)(10000000 * vfr->TimeStampFromFrameNumber(n));
                }

                Render(dst, timestamp, fps);

                return frame;
            }
        };

        class CVobSubAvisynthFilter : public CVobSubFilter, public CAvisynthFilter
        {
        public:
            CVobSubAvisynthFilter(PClip c, const char* fn, IScriptEnvironment* env)
                : CVobSubFilter(CString(fn))
                , CAvisynthFilter(c, env) {
                if (!m_pSubPicProvider) {
                    env->ThrowError("VobSub: Can't open \"%s\"", fn);
                }
            }
        };

        AVSValue __cdecl VobSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            return (DEBUG_NEW CVobSubAvisynthFilter(args[0].AsClip(), args[1].AsString(), env));
        }

        class CTextSubAvisynthFilter : public CTextSubFilter, public CAvisynthFilter
        {
        public:
            CTextSubAvisynthFilter(PClip c, IScriptEnvironment* env, const char* fn, int CharSet = DEFAULT_CHARSET, float fps = -1, VFRTranslator* vfr = 0) //vfr patch
                : CTextSubFilter(CString(fn), CharSet, fps)
                , CAvisynthFilter(c, env, vfr) {
                if (!m_pSubPicProvider) {
                    env->ThrowError("TextSub: Can't open \"%s\"", fn);
                }
            }
        };

        AVSValue __cdecl TextSubCreateGeneral(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            if (!args[1].Defined()) {
                env->ThrowError("TextSub: You must specify a subtitle file to use");
            }
            VFRTranslator* vfr = 0;
            if (args[4].Defined()) {
                vfr = GetVFRTranslator(args[4].AsString());
            }

            return (DEBUG_NEW CTextSubAvisynthFilter(
                        args[0].AsClip(),
                        env,
                        args[1].AsString(),
                        args[2].AsInt(DEFAULT_CHARSET),
                        (float)args[3].AsFloat(-1),
                        vfr));
        }

        AVSValue __cdecl TextSubSwapUV(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
            s_fSwapUV = args[0].AsBool(false);
            return AVSValue();
        }

        AVSValue __cdecl MaskSubCreate(AVSValue args, void* user_data, IScriptEnvironment* env)/*SIIFI*/
        {
            if (!args[0].Defined()) {
                env->ThrowError("MaskSub: You must specify a subtitle file to use");
            }
            if (!args[3].Defined() && !args[6].Defined()) {
                env->ThrowError("MaskSub: You must specify either FPS or a VFR timecodes file");
            }
            VFRTranslator* vfr = 0;
            if (args[6].Defined()) {
                vfr = GetVFRTranslator(args[6].AsString());
            }

            AVSValue rgb32("RGB32");
            AVSValue fps(args[3].AsFloat(25));
            AVSValue  tab[6] = {
                args[1],
                args[2],
                args[3],
                args[4],
                rgb32
            };
            AVSValue value(tab, 5);
            const char* nom[5] = {
                "width",
                "height",
                "fps",
                "length",
                "pixel_type"
            };
            AVSValue clip(env->Invoke("Blackness", value, nom));
            env->SetVar(env->SaveString("RGBA"), true);
            //return (DNew CTextSubAvisynthFilter(clip.AsClip(), env, args[0].AsString()));
            return (DEBUG_NEW CTextSubAvisynthFilter(
                        clip.AsClip(),
                        env,
                        args[0].AsString(),
                        args[5].AsInt(DEFAULT_CHARSET),
                        (float)args[3].AsFloat(-1),
                        vfr));
        }

        extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env)
        {
            env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
            env->AddFunction("TextSub", "c[file]s[charset]i[fps]f[vfr]s", TextSubCreateGeneral, 0);
            env->AddFunction("TextSubSwapUV", "b", TextSubSwapUV, 0);
            env->AddFunction("MaskSub", "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s", MaskSubCreate, 0);
            env->SetVar(env->SaveString("RGBA"), false);
            return NULL;
        }
    }
#endif


// PF20180224 Avs2.6 interface using AVS+ headers
    namespace AviSynth26
    {
#include "avisynth/avisynth.h"

      static bool s_fSwapUV = false;

      class CAvisynthFilter : public GenericVideoFilter, virtual public CFilter
      {
      public:
        VFRTranslator * vfr;

        CAvisynthFilter(PClip c, IScriptEnvironment* env, VFRTranslator* _vfr = 0) : GenericVideoFilter(c), vfr(_vfr) {}

        // Helpers for YUV420P10<->P010 and YUV420P16<->P016 conversion

        template<bool before>
        static void prepare_luma_shift6_c(uint8_t* pdst, int dstpitch, const uint8_t *src, int srcpitch, int width, int height)
        {
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
              if (before)
                reinterpret_cast<uint16_t*>(pdst)[x] = reinterpret_cast<const uint16_t *>(src)[x] << 6;
              else
                reinterpret_cast<uint16_t*>(pdst)[x] = reinterpret_cast<const uint16_t *>(src)[x] >> 6;
            }
            src += srcpitch;
            pdst += dstpitch;
          }
        }

        template<bool before>
        static void prepare_luma_shift6_sse2(uint8_t* pdst, int dstpitch, const uint8_t *src, int srcpitch, int width, int height)
        {
          const int modw = (width / 8) * 8; // 8 uv pairs at a time
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < modw; x += 8) {
              __m128i y = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint16_t *>(src) + x));
              if(before)
                y = _mm_slli_epi16(y, 6); // make 10->16 bits
              else
                y = _mm_srli_epi16(y, 6); // make 16->10 bits
              _mm_store_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint16_t *>(pdst) + x), y);
            }

            for (int x = modw; x < width; x++) {
              if (before)
                reinterpret_cast<uint16_t*>(pdst)[x] = reinterpret_cast<const uint16_t *>(src)[x] << 6;
              else
                reinterpret_cast<uint16_t*>(pdst)[x] = reinterpret_cast<const uint16_t *>(src)[x] >> 6;
            }
            src += srcpitch;
            pdst += dstpitch;
          }
        }

        template<bool shift6>
        static void prepare_to_interleaved_uv_c(uint8_t* pdst, int dstpitch, const uint8_t *srcu, const uint8_t *srcv, int pitchUV, int width, int height)
        {
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
              uint16_t u, v;
              if (shift6) {
                u = reinterpret_cast<const uint16_t *>(srcu)[x] << 6; // make 10->16 bits
                v = reinterpret_cast<const uint16_t *>(srcv)[x] << 6; // make 10->16 bits
              }
              else {
                u = reinterpret_cast<const uint16_t *>(srcu)[x];
                v = reinterpret_cast<const uint16_t *>(srcv)[x];
              }
              uint32_t uv = (v << 16) | u;
              reinterpret_cast<uint32_t*>(pdst)[x] = uv;
            }
            srcu += pitchUV;
            srcv += pitchUV;
            pdst += dstpitch;
          }
        }

        template<bool shift6>
        static void prepare_to_interleaved_uv_sse2(uint8_t* pdst, int dstpitch, const uint8_t *srcu, const uint8_t *srcv, int pitchUV, int width, int height)
        {
          const int modw = (width / 8) * 8; // 8 uv pairs at a time
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < modw; x += 8) {
              __m128i u = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint16_t *>(srcu) + x));
              __m128i v = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint16_t *>(srcv) + x));
              if (shift6) {
                u = _mm_slli_epi16(u, 6); // make 10->16 bits
                v = _mm_slli_epi16(v, 6);
              }
              __m128i uv;
              uv = _mm_unpacklo_epi16(u, v); // (v << 16) | u;
              _mm_store_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint32_t *>(pdst) + x), uv);
              uv = _mm_unpackhi_epi16(u, v); // (v << 16) | u;
              _mm_store_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint32_t *>(pdst) + x + 4), uv);
            }

            for (int x = modw; x < width; x++) {
              uint16_t u, v;
              if (shift6) {
                u = reinterpret_cast<const uint16_t *>(srcu)[x] << 6; // make 10->16 bits
                v = reinterpret_cast<const uint16_t *>(srcv)[x] << 6; // make 10->16 bits
              }
              else {
                u = reinterpret_cast<const uint16_t *>(srcu)[x];
                v = reinterpret_cast<const uint16_t *>(srcv)[x];
              }
              uint32_t uv = (v << 16) | u;
              reinterpret_cast<uint32_t*>(pdst)[x] = uv;
            }
            srcu += pitchUV;
            srcv += pitchUV;
            pdst += dstpitch;
          }
        }


        template<bool shift6>
        static void prepare_from_interleaved_uv_c(uint8_t* pdstu, uint8_t* pdstv, int pitchUV, const uint8_t *src, int srcpitch, int width, int height)
        {
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
              uint32_t uv = reinterpret_cast<const uint32_t*>(src)[x];
              uint16_t u = uv & 0xFFFF;
              uint16_t v = uv >> 16;
              if (shift6) {
                u >>= 6;
                v >>= 6;
              }
              reinterpret_cast<uint16_t*>(pdstu)[x] = u;
              reinterpret_cast<uint16_t*>(pdstv)[x] = v;
            }
            pdstu += pitchUV;
            pdstv += pitchUV;
            src += srcpitch;
          }
        }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4309)
#endif
        // fake _mm_packus_epi32 (orig is SSE4.1 only)
        static __forceinline __m128i _MM_PACKUS_EPI32(__m128i a, __m128i b)
        {
          const static __m128i val_32 = _mm_set1_epi32(0x8000);
          const static __m128i val_16 = _mm_set1_epi16(0x8000);

          a = _mm_sub_epi32(a, val_32);
          b = _mm_sub_epi32(b, val_32);
          a = _mm_packs_epi32(a, b);
          a = _mm_add_epi16(a, val_16);
          return a;
        }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

        template<bool shift6, bool hasSSE41>
        static void prepare_from_interleaved_uv_sse2(uint8_t* pdstu, uint8_t* pdstv, int pitchUV, const uint8_t *src, int srcpitch, int width, int height)
        {
          const int modw = (width / 8) * 8;
          auto mask0000FFFF = _mm_set1_epi32(0x0000FFFF);
          for (int y = 0; y < height; y++) {
            for (int x = 0; x < modw; x += 8) {
              auto uv_lo = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint32_t*>(src) + x));
              auto uv_hi = _mm_load_si128(reinterpret_cast<const __m128i *>(reinterpret_cast<const uint32_t*>(src) + x + 4));
              if (shift6) {
                uv_lo = _mm_srli_epi16(uv_lo, 6);
                uv_hi = _mm_srli_epi16(uv_hi, 6);
              }
              auto u_lo = _mm_and_si128(uv_lo, mask0000FFFF);
              auto u_hi = _mm_and_si128(uv_hi, mask0000FFFF);
              auto u = shift6 ? _mm_packs_epi32(u_lo, u_hi) : (hasSSE41 ? _mm_packus_epi32(u_lo, u_hi) : _MM_PACKUS_EPI32(u_lo, u_hi));
              _mm_store_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint16_t*>(pdstu) + x), u);

              auto v_lo = _mm_srli_epi32(uv_lo, 16);
              auto v_hi = _mm_srli_epi32(uv_hi, 16);
              auto v = shift6 ? _mm_packs_epi32(v_lo, v_hi) : (hasSSE41 ? _mm_packus_epi32(v_lo, v_hi) : _MM_PACKUS_EPI32(v_lo, v_hi));
              _mm_store_si128(reinterpret_cast<__m128i *>(reinterpret_cast<uint16_t*>(pdstv) + x), v);
            }

            for (int x = modw; x < width; x++) {
              uint32_t uv = reinterpret_cast<const uint32_t*>(src)[x];
              uint16_t u = uv & 0xFFFF;
              uint16_t v = uv >> 16;
              if (shift6) {
                u >>= 6;
                v >>= 6;
              }
              reinterpret_cast<uint16_t*>(pdstu)[x] = u;
              reinterpret_cast<uint16_t*>(pdstv)[x] = v;
            }

            pdstu += pitchUV;
            pdstv += pitchUV;
            src += srcpitch;
          }
        }

        // Crash and/or corruption if MT. To be tested, why
        int __stdcall SetCacheHints(int cachehints, int frame_range) override {
          return cachehints == CACHE_GET_MTMODE ? MT_SERIALIZED : 0;
        }

        PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) {
          PVideoFrame frame = child->GetFrame(n, env);
          
          const bool sse2 = (env->GetCPUFlags() & CPUF_SSE2) != 0;
          const bool sse41 = (env->GetCPUFlags() & CPUF_SSE4_1) != 0;
          
          SubPicDesc dst;
          // dst pointers: later
          dst.w = vi.width;
          dst.h = vi.height;
          
          // PF todo: check what to put here for 10 bits? 10 or 16? Now pitch/dst.w would 
          // dst.bpp = dst.pitch / dst.w * 8; //vi.BitsPerPixel();
          if (vi.BitsPerComponent() == 10)
            dst.bpp = 16;
          if (vi.BitsPerComponent() == 16)
            dst.bpp = 16;
          else
            // PF180224  fix1: not pitch/width but rowsize/width should be used!!
            dst.bpp = frame->GetRowSize() / dst.w * 8; //vi.BitsPerPixel();

          BYTE *pdst_save;
          PVideoFrame buffer;

          dst.type =
            vi.IsRGB32() ? (env->GetVar("RGBA").AsBool() ? MSP_RGBA : MSP_RGB32) :
            vi.IsRGB24() ? MSP_RGB24 :
            vi.IsYUY2() ? MSP_YUY2 :
            /*vi.IsYV12()*/ vi.pixel_type == VideoInfo::CS_YV12 ? (s_fSwapUV ? MSP_IYUV : MSP_YV12) :
            /*vi.IsIYUV()*/ vi.pixel_type == VideoInfo::CS_IYUV ? (s_fSwapUV ? MSP_YV12 : MSP_IYUV) :
            vi.pixel_type == VideoInfo::CS_YUV420P10 ? MSP_P010 : // P.F. 180224 10 bit support
            vi.pixel_type == VideoInfo::CS_YUV420P16 ? MSP_P016 : // P.F. 180224 16 bit support
            -1;

          bool semi_packed_p10 = (vi.pixel_type == VideoInfo::CS_YUV420P10) || (vi.pixel_type == VideoInfo::CS_YUV422P10);
          bool semi_packed_p16 = (vi.pixel_type == VideoInfo::CS_YUV420P16) || (vi.pixel_type == VideoInfo::CS_YUV422P16);
          // P010/P016 format:
          // Single buffer
          // n lines   YYYYYYYYYYYYYY
          // n/2 lines UVUVUVUVUVUVUV
          // Pitch is common. P010 is upshifted to 16 bits

          if (semi_packed_p10 || semi_packed_p16) {
            VideoInfo vi2 = vi;
            int cheight = vi.height >> vi.GetPlaneHeightSubsampling(PLANAR_U);
            int cwidth = vi.width >> vi.GetPlaneWidthSubsampling(PLANAR_U);
            vi2.height = vi.height + cheight;
            vi2.pixel_type = semi_packed_p16 ? VideoInfo::CS_Y16 : VideoInfo::CS_Y10;
            buffer = env->NewVideoFrame(vi2);
            BYTE *pdst = buffer->GetWritePtr();
            pdst_save = pdst;

            // luma
            int pitch = buffer->GetPitch();
            int srcpitch = frame->GetPitch();
            const BYTE *src = frame->GetReadPtr();
            if (semi_packed_p16) {
              // no shift, native copy
              env->BitBlt(pdst, pitch, src, srcpitch, frame->GetRowSize(), vi.height);
            }
            else {
              // shift by 6 make 10->16 bits
              if (sse2)
                prepare_luma_shift6_sse2<true>(pdst, pitch, src, srcpitch, vi.width, vi.height); // true: before
              else
                prepare_luma_shift6_c<true>(pdst, pitch, src, srcpitch, vi.width, vi.height); // true: before
            }

            pdst += pitch * vi.height;

            // Chroma
            int pitchUV = frame->GetPitch(PLANAR_U);
            const BYTE *srcu = frame->GetReadPtr(PLANAR_U);
            const BYTE *srcv = frame->GetReadPtr(PLANAR_V);

            if (sse2) {
              if (semi_packed_p16)
                prepare_to_interleaved_uv_sse2<false>(pdst, pitch, srcu, srcv, pitchUV, cwidth, cheight);
              else
                prepare_to_interleaved_uv_sse2<true>(pdst, pitch, srcu, srcv, pitchUV, cwidth, cheight); // shift6 inside
            }
            else {
              if (semi_packed_p16)
                prepare_to_interleaved_uv_c<false>(pdst, pitch, srcu, srcv, pitchUV, cwidth, cheight);
              else
                prepare_to_interleaved_uv_c<true>(pdst, pitch, srcu, srcv, pitchUV, cwidth, cheight); // shift6 inside
            }
            // buffer is ready.
            // Fill dst pointers
            dst.pitch = pitch;
            dst.pitchUV = pitch; // n/a? // ? in P010 no separate UV pointers
            dst.bits = pdst_save;
            dst.bitsU = pdst_save + pitch * vi.height; // ? in P010 no separate pointers
            dst.bitsV = pdst_save + pitch * vi.height;
          }
          else {
            // 8 bit classic
            env->MakeWritable(&frame);

            dst.pitch = frame->GetPitch();
            dst.pitchUV = frame->GetPitch(PLANAR_U);
            dst.bits = frame->GetWritePtr();
            dst.bitsU = frame->GetWritePtr(PLANAR_U);
            dst.bitsV = frame->GetWritePtr(PLANAR_V);
          }

          // Common part
          float fps = m_fps > 0 ? m_fps : (float)vi.fps_numerator / vi.fps_denominator;

          REFERENCE_TIME timestamp;

          if (!vfr) {
            timestamp = (REFERENCE_TIME)(10000000i64 * n / fps);
          }
          else {
            timestamp = (REFERENCE_TIME)(10000000 * vfr->TimeStampFromFrameNumber(n));
          }

          Render(dst, timestamp, fps);

          if (semi_packed_p10 || semi_packed_p16) {
            // convert semi packed formats back to Avisynth YUV420P10 and P16 formats
            BYTE *src = pdst_save; // dst.bits;
            PVideoFrame frame = env->NewVideoFrame(vi);
            BYTE *pdst = frame->GetWritePtr();

            int pitch = frame->GetPitch();
            //env->BitBlt(pdst, pitch, src, dst.pitch, dst.pitch, vi.height); // copy Y
            
            // Luma
            if (semi_packed_p16) {
              env->BitBlt(pdst, pitch, src, dst.pitch, vi.width * sizeof(uint16_t), vi.height);
            }
            else {
              // shift by 6 make 10->16 bits
              if (sse2)
                prepare_luma_shift6_sse2<false>(pdst, pitch, src, dst.pitch, vi.width, vi.height); // false: after
              else
                prepare_luma_shift6_c<false>(pdst, pitch, src, dst.pitch, vi.width, vi.height); // false: after
            }
            src += dst.pitch *vi.height;

            // Chroma
            int cheight = vi.height >> vi.GetPlaneHeightSubsampling(PLANAR_U);
            int cwidth = vi.width >> vi.GetPlaneWidthSubsampling(PLANAR_U);
            int pitchUV = frame->GetPitch(PLANAR_U);
            BYTE *pdstu = frame->GetWritePtr(PLANAR_U);
            BYTE *pdstv = frame->GetWritePtr(PLANAR_V);
            if (sse41) {
              if (semi_packed_p16)
                prepare_from_interleaved_uv_sse2<false, true>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight);
              else
                prepare_from_interleaved_uv_sse2<true, true>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight); // true: shift 6
            }
            else if (sse2) {
              if (semi_packed_p16)
                prepare_from_interleaved_uv_sse2<false, false>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight);
              else
                prepare_from_interleaved_uv_sse2<true, false>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight); // true: shift 6
            }
            else {
              if (semi_packed_p16)
                prepare_from_interleaved_uv_c<false>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight);
              else
                prepare_from_interleaved_uv_c<true>(pdstu, pdstv, pitchUV, src, dst.pitch, cwidth, cheight); // true: shift 6
            }
            return frame;
          }
          else {
            return frame;
          }
        }
      };


        class CVobSubAvisynthFilter : public CVobSubFilter, public CAvisynthFilter
        {
        public:
          CVobSubAvisynthFilter(PClip c, const char* fn, IScriptEnvironment* env)
            : CVobSubFilter(CString(fn))
            , CAvisynthFilter(c, env) {
            if (!m_pSubPicProvider) {
              env->ThrowError("VobSub: Can't open \"%s\"", fn);
            }
          }
        };

        AVSValue __cdecl VobSubCreateS(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
          return (DEBUG_NEW CVobSubAvisynthFilter(args[0].AsClip(), args[1].AsString(), env));
        }

        class CTextSubAvisynthFilter : public CTextSubFilter, public CAvisynthFilter
        {
        public:
          CTextSubAvisynthFilter(PClip c, IScriptEnvironment* env, const char* fn, int CharSet = DEFAULT_CHARSET, float fps = -1, VFRTranslator* vfr = 0) //vfr patch
            : CTextSubFilter(CString(fn), CharSet, fps)
            , CAvisynthFilter(c, env, vfr) {
            if (!m_pSubPicProvider) {
              env->ThrowError("TextSub: Can't open \"%s\"", fn);
            }
          }
        };

        AVSValue __cdecl TextSubCreateGeneral(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
          if (!args[1].Defined()) {
            env->ThrowError("TextSub: You must specify a subtitle file to use");
          }
          VFRTranslator* vfr = 0;
          if (args[4].Defined()) {
            vfr = GetVFRTranslator(args[4].AsString());
          }

          return (DEBUG_NEW CTextSubAvisynthFilter(
            args[0].AsClip(),
            env,
            args[1].AsString(),
            args[2].AsInt(DEFAULT_CHARSET),
            (float)args[3].AsFloat(-1),
            vfr));
        }

        AVSValue __cdecl TextSubSwapUV(AVSValue args, void* user_data, IScriptEnvironment* env)
        {
          s_fSwapUV = args[0].AsBool(false);
          return AVSValue();
        }

        AVSValue __cdecl MaskSubCreate(AVSValue args, void* user_data, IScriptEnvironment* env)/*SIIFI*/
        {
          if (!args[0].Defined()) {
            env->ThrowError("MaskSub: You must specify a subtitle file to use");
          }
          if (!args[3].Defined() && !args[6].Defined()) {
            env->ThrowError("MaskSub: You must specify either FPS or a VFR timecodes file");
          }
          VFRTranslator* vfr = 0;
          if (args[6].Defined()) {
            vfr = GetVFRTranslator(args[6].AsString());
          }

          // well, its a source filter
          AVSValue fps(args[3].AsFloat(25));
          AVSValue  tab[6] = {
            args[1],
            args[2],
            args[3],
            args[4],
            args[7].AsString("RGB32") // PF 180224 new! pixel_type parameter defaulting to RGB32
          };
          AVSValue value(tab, 5);
          const char* nom[5] = {
            "width",
            "height",
            "fps",
            "length",
            "pixel_type"
          };
          AVSValue clip(env->Invoke("Blackness", value, nom));
          env->SetVar(env->SaveString("RGBA"), true);
          //return (DNew CTextSubAvisynthFilter(clip.AsClip(), env, args[0].AsString()));
          return (DEBUG_NEW CTextSubAvisynthFilter(
            clip.AsClip(),
            env,
            args[0].AsString(),
            args[5].AsInt(DEFAULT_CHARSET),
            (float)args[3].AsFloat(-1),
            vfr));
        }

        /* New 2.6 requirement!!! */
        // Declare and initialise server pointers static storage.
        const AVS_Linkage *AVS_linkage = 0;

        /* New 2.6 requirement!!! */
        // DLL entry point called from LoadPlugin() to setup a user plugin.
        extern "C" __declspec(dllexport) const char* __stdcall
          AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {

          /* New 2.6 requirment!!! */
          // Save the server pointers.
          AVS_linkage = vectors;
          env->AddFunction("VobSub", "cs", VobSubCreateS, 0);
          env->AddFunction("TextSub", "c[file]s[charset]i[fps]f[vfr]s", TextSubCreateGeneral, 0);
          env->AddFunction("TextSubSwapUV", "b", TextSubSwapUV, 0);
          env->AddFunction("MaskSub", "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s[pixel_type]s", MaskSubCreate, 0); // new pixel_type parameter
          env->SetVar(env->SaveString("RGBA"), false);
          return NULL;
        }
      }
    }
    


UINT_PTR CALLBACK OpenHookProc(HWND hDlg, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uiMsg) {
        case WM_NOTIFY: {
            OPENFILENAME* ofn = ((OFNOTIFY*)lParam)->lpOFN;

            if (((NMHDR*)lParam)->code == CDN_FILEOK) {
                ofn->lCustData = (LPARAM)CharSetList[SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_GETCURSEL, 0, 0)];
            }

            break;
        }

        case WM_INITDIALOG: {
            SetWindowLongPtr(hDlg, GWLP_USERDATA, lParam);

            for (ptrdiff_t i = 0; i < CharSetLen; i++) {
                CString s;
                s.Format(_T("%s (%d)"), CharSetNames[i], CharSetList[i]);
                SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_ADDSTRING, 0, (LPARAM)(LPCTSTR)s);
                if (CharSetList[i] == (int)((OPENFILENAME*)lParam)->lCustData) {
                    SendMessage(GetDlgItem(hDlg, IDC_COMBO1), CB_SETCURSEL, i, 0);
                }
            }

            break;
        }

        default:
            break;
    }

    return FALSE;
}
