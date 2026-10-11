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

#pragma once

#include "dualsubs_identity.h"

#include <afxcmn.h>
#include "IDirectVobSub.h"
#include "IDirectVobSubXy.h"
#include "IMultiSubtitleTrackLoader.h"

class CDVSBasePPage : public CBasePropertyPage
{
public:
	// we have to override these to use external, resource-only dlls
	STDMETHODIMP GetPageInfo(LPPROPPAGEINFO pPageInfo);
	STDMETHODIMP Activate(HWND hwndParent, LPCRECT pRect, BOOL fModal);

protected:
	CComQIPtr<IDirectVobSub2> m_pDirectVobSub;
    CComQIPtr<IXyOptions> m_pDirectVobSubXy;
    // The filter also exposes the container-side loader; used by the Dual
    // Subtitles page to list the current media's embedded subtitle streams.
    CComQIPtr<IMultiSubtitleTrackLoader> m_pTrackLoader;

	virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {return(false);}
	virtual void UpdateObjectData(bool fSave) {}
	virtual void UpdateControlData(bool fSave) {}

protected:
    CDVSBasePPage(TCHAR* pName, LPUNKNOWN lpunk, int DialogId, int TitleId);

	bool m_fDisableInstantUpdate;

private:
    BOOL m_bIsInitialized;

    HRESULT OnConnect(IUnknown* pUnknown), OnDisconnect(), OnActivate(), OnDeactivate(), OnApplyChanges();
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
	bool m_fAttached;
	void AttachControls(), DetachControls();

	CMap<UINT, UINT&, CWnd*, CWnd*> m_controls;

protected:
	void BindControl(UINT id, CWnd& control);

	// Set an edit control's text only when it actually differs.
	//
	// UpdateControlData(false) runs on EVERY instant-apply (it is reached via
	// OnApplyChanges -> OnActivate), i.e. on each EN_CHANGE while typing.
	// SetWindowText unconditionally would reset the caret to position 0 on
	// every keystroke, making text fields unusable. Comparing first keeps the
	// caret where the user left it while still refreshing on real changes.
	static void SetEditTextIfChanged(CWnd& wnd, const CString& text);
};

[uuid("60765CF5-01C2-4ee7-A44B-C791CF25FEA0")]
class CDVSMainPPage : public CDVSBasePPage
{
	void FreeLangs(), AllocLangs(int nLangs);

	WCHAR m_fn[MAX_PATH];
	int m_iSelectedLanguage, m_nLangs;
	WCHAR** m_ppLangs;
	bool m_fOverridePlacement;
    bool m_fHideTrayIcon;
	int	m_PlacementXperc, m_PlacementYperc;
	STSStyle m_defStyle;
	bool m_fOnlyShowForcedVobSubs;
	CSimpleTextSubtitle::EPARCompensationType m_ePARCompensationType;

	CEdit m_fnedit;
	CComboBox m_langs;
	CButton m_oplacement;
	CSpinButtonCtrl m_subposx, m_subposy;
	CButton m_styles, m_forcedsubs;
	CButton m_AutoPARCompensation;
	CComboBox m_PARCombo;
    CButton m_hide_tray_icon;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSMainPPage(LPUNKNOWN lpunk, HRESULT* phr);
	virtual ~CDVSMainPPage();
};

[uuid("0180E49C-13BF-46db-9AFD-9F52292E1C22")]
class CDVSGeneralPPage : public CDVSBasePPage
{
	int m_HorExt, m_VerExt, m_ResX2, m_ResX2minw, m_ResX2minh;
	int m_LoadLevel;
	bool m_fExternalLoad, m_fWebLoad, m_fEmbeddedLoad;

	CComboBox m_verext;
	CButton m_mod32fix;
	CComboBox m_resx2;
	CSpinButtonCtrl m_resx2w, m_resx2h;
	CComboBox m_load;
	CButton m_extload, m_webload, m_embload;

protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSGeneralPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR *pName = NAME("DirectVobSub Property Page (general settings)"));
};

[uuid("A8B25C0E-0894-4531-B668-AB1599FAF7F6")]
class CDVSMiscPPage : public CDVSBasePPage
{
	bool m_fFlipPicture, m_fFlipSubtitles, m_fHideSubtitles, m_fOSD, m_fDoPreBuffering, m_fReloaderDisabled, m_fSaveFullPath;

    int m_colorSpace, m_yuvRange;

	CButton m_flippic, m_flipsub, m_hidesub, m_showosd, m_prebuff, m_autoreload, m_savefullpath, m_instupd;
    CComboBox m_colorSpaceDropList, m_yuvRangeDropList;

protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSMiscPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR *pName = NAME("DirectVobSub Property Page (misc settings)"));
};

[uuid("ACE4747B-35BD-4e97-9DD7-1D4245B0695C")]
class CDVSTimingPPage : public CDVSBasePPage
{
	int m_SubtitleSpeedMul, m_SubtitleSpeedDiv, m_SubtitleDelay;
	bool m_fMediaFPSEnabled;
	double m_MediaFPS;

	CSpinButtonCtrl m_subdelay, m_subspeedmul, m_subspeeddiv;    

protected:
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSTimingPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR *pName = NAME("DirectVobSub Timing Property Page"));
};

[uuid("69CE757B-E8C0-4B0A-9EA0-CEA284096F98")]
class CDVSMorePPage : public CDVSBasePPage
{
    int m_overlay_cache_max_item_num, m_overlay_no_blur_cache_max_item_num, m_path_cache_max_item_num, 
        m_scan_line_data_cache_max_item_num, m_subpixel_pos_level;
    int m_layout_size_opt;
    SIZE m_layout_size;

    CSpinButtonCtrl m_path_cache, m_scanline_cache, m_overlay_no_blur_cache, m_overlay_cache;

    CSpinButtonCtrl m_layout_size_x, m_layout_size_y;
    CComboBox m_combo_subpixel_pos, m_combo_layout_size_opt;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    virtual void UpdateControlData(bool fSave);
    virtual void UpdateObjectData(bool fSave);

public:
    CDVSMorePPage(LPUNKNOWN lpunk, HRESULT* phr);
};

[uuid("F544E0F5-CA3C-47ea-A64D-35FCF1602396")]
class CDVSAboutPPage : public CDVSBasePPage
{
public:
    CDVSAboutPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR* pName=NAME("About Property Page"));
    
    bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
};

[uuid("525F116F-04AD-40a2-AE2F-A0C4E1AFEF98")]
class CDVSZoomPPage : public CDVSBasePPage
{
	NORMALIZEDRECT m_rect;

	CSpinButtonCtrl m_posx, m_posy, m_scalex, m_scaley;

protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSZoomPPage(LPUNKNOWN lpunk, HRESULT* phr);
};

[uuid("C2D6D98F-09CA-4524-AF64-1049B5665C9C")]
class CDVSColorPPage : public CDVSBasePPage
{
    CListCtrl m_outputFmtList, m_inputFmtList;
    CButton m_followUpstreamPreferredOrder, m_btnColorUp, m_btnColorDown;
    
    static const int MAX_COLOR_SPACE = 256;

    typedef DirectVobSubXyOptions::ColorSpaceOpt ColorSpaceOpt;
    ColorSpaceOpt *m_outputColorSpace;
    int m_outputColorSpaceCount;

    ColorSpaceOpt *m_inputColorSpace;
    int m_inputColorSpaceCount;

    bool m_fFollowUpstream;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSColorPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR *pName = NAME("DirectVobSub Property Page (color settings)"));
    ~CDVSColorPPage();
};

[uuid("CE77C59C-CFD2-429f-868C-8B04D23F94CA")]
class CDVSPathsPPage : public CDVSBasePPage
{
	CStringArray m_paths;

	CListBox m_pathlist;
	CEdit m_path;
	CButton m_browse, m_remove, m_add;

protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
	virtual void UpdateControlData(bool fSave);
	virtual void UpdateObjectData(bool fSave);

public:
    CDVSPathsPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR* pName = NAME("DirectVobSub Paths Property Page"));
};

//
// XySubFilter
//
[uuid(DUALSUBS_CLSID_MAINPAGE_STR)]
class CXySubFilterMainPPage : public CDVSBasePPage
{
    void FreeLangs(), AllocLangs(int nLangs);

    WCHAR m_fn[MAX_PATH];
    int m_iSelectedLanguage, m_nLangs;
    WCHAR** m_ppLangs;
    bool m_fOverridePlacement;
    bool m_fHideTrayIcon;
    int m_PlacementXperc, m_PlacementYperc;
    STSStyle m_defStyle;
    bool m_fForceDefaultStyle;
    bool m_fOnlyShowForcedVobSubs;
    CSimpleTextSubtitle::EPARCompensationType m_ePARCompensationType;
    int m_LoadLevel;
    bool m_fExternalLoad, m_fWebLoad, m_fEmbeddedLoad;

    CEdit m_fnedit;
    CComboBox m_langs;
    CButton m_oplacement;
    CSpinButtonCtrl m_subposx, m_subposy;
    CButton m_styles, m_force_default_style, m_forcedsubs;
    CButton m_hide_tray_icon;
    CComboBox m_load;
    CButton m_extload, m_webload, m_embload;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    virtual void UpdateControlData(bool fSave);
    virtual void UpdateObjectData(bool fSave);

public:
    CXySubFilterMainPPage(LPUNKNOWN lpunk, HRESULT* phr);
    virtual ~CXySubFilterMainPPage();
};

[uuid(DUALSUBS_CLSID_MOREPAGE_STR)]
class CXySubFilterMorePPage : public CDVSBasePPage
{
    int m_overlay_cache_max_item_num, m_overlay_no_blur_cache_max_item_num, m_path_cache_max_item_num, 
        m_scan_line_data_cache_max_item_num, m_subpixel_pos_level;
    int m_layout_size_opt;
    int m_yuv_matrix, m_yuv_range, m_rgb_level;
    SIZE m_layout_size;

    bool m_fHideSubtitles, m_fAllowMoving, m_fReloaderDisabled;
    bool m_render_to_original_video_size;

    int  m_cache_size, m_auto_cache_size;

    CButton m_hidesub, m_allowmoving, m_autoreload, m_instupd;

    CSpinButtonCtrl m_path_cache, m_scanline_cache, m_overlay_no_blur_cache, m_overlay_cache;

    CSpinButtonCtrl m_layout_size_x, m_layout_size_y;
    CComboBox m_combo_subpixel_pos, m_combo_layout_size_opt;
    CButton m_checkbox_render_to_original_video_size;

    CComboBox m_combo_yuv_matrix, m_combo_yuv_range, m_combo_rgb_level;
    CEdit m_edit_cache_size;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    virtual void UpdateControlData(bool fSave);
    virtual void UpdateObjectData(bool fSave);

public:
    CXySubFilterMorePPage(LPUNKNOWN lpunk, HRESULT* phr);
};

[uuid(DUALSUBS_CLSID_DUALPAGE_STR)]
class CXySubFilterDualPPage : public CDVSBasePPage
{
    bool m_fDualSubEnabled;
    CString m_secondaryFile;
    int m_secondaryScale, m_secondaryOpacity, m_secondaryVertical, m_secondaryTopMargin, m_secondaryZOrder;
    int m_primaryVertical;
    CString m_secondaryLanguageList;

    CButton m_dual_sub_enabled;
    CEdit m_secondary_file;
    CButton m_secondary_browse;
    CSpinButtonCtrl m_secondary_scale, m_secondary_opacity, m_secondary_top_margin, m_secondary_z_order;
    CComboBox m_secondary_vertical, m_primary_vertical;
    CEdit m_secondary_lang_list;
    CButton m_secondary_reload;

    // Embedded-stream picker: list of the current media's embedded subtitle
    // streams (label + raw id), a Refresh button and a note line.
    CComboBox m_secondary_embedded_list;
    CButton m_secondary_embedded_refresh;
    CStatic m_secondary_embedded_note;
    std::vector<EmbeddedStreamInfo> m_embedded_streams;
    // Enumeration opens the media (header scan), so it is done once when the
    // page is first shown and on explicit Refresh - not on every instant-apply.
    bool m_embedded_populated;
protected:
    virtual bool OnMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    virtual void UpdateControlData(bool fSave);
    virtual void UpdateObjectData(bool fSave);

    // Fills the embedded-stream combo from the filter's enumeration of the
    // current media. Never throws/errors: on failure it shows an empty list and
    // a note, and logs.
    void PopulateEmbeddedList();
    // Writes the option(s) that select the currently chosen embedded entry.
    void ApplyEmbeddedSelection();

public:
    CXySubFilterDualPPage(LPUNKNOWN lpunk, HRESULT* phr);
};

[uuid(DUALSUBS_CLSID_TIMINGPAGE_STR)]
class CXySubFilterTimingPPage : public CDVSTimingPPage
{
public:
    CXySubFilterTimingPPage(LPUNKNOWN lpunk, HRESULT* phr
        , TCHAR *pName = NAME(DUALSUBS_FILTER_NAME " Property Page (timing)"))
        : CDVSTimingPPage(lpunk, phr, pName) {}
};

[uuid(DUALSUBS_CLSID_PATHSPAGE_STR)]
class CXySubFilterPathsPPage : public CDVSPathsPPage
{
public:
    CXySubFilterPathsPPage(LPUNKNOWN lpunk, HRESULT* phr
        , TCHAR* pName = NAME(DUALSUBS_FILTER_NAME " Property Page (path)"))
        : CDVSPathsPPage(lpunk, phr, pName) {}
};

[uuid(DUALSUBS_CLSID_ABOUTPAGE_STR)]
class CXySubFilterAboutPPage : public CDVSAboutPPage
{
public:
    CXySubFilterAboutPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR* pName=NAME(DUALSUBS_FILTER_NAME " Property Page (about)"))
        : CDVSAboutPPage(lpunk, phr, pName) {}
};

//
// XySubFilterConsumer
//
[uuid(DUALSUBS_CLSID_CONSUMER_GENERALPAGE_STR)]
class CXySubFilterConsumerGeneralPPage : public CDVSGeneralPPage
{
public:
    CXySubFilterConsumerGeneralPPage(LPUNKNOWN lpunk, HRESULT* phr
        , TCHAR *pName = NAME(DUALSUBS_FILTER_NAME "Consumer Property Page (General)"))
        : CDVSGeneralPPage(lpunk, phr, pName) {}
};

[uuid(DUALSUBS_CLSID_CONSUMER_MISCPAGE_STR)]
class CXySubFilterConsumerMiscPPage : public CDVSMiscPPage
{
public:
    CXySubFilterConsumerMiscPPage(LPUNKNOWN lpunk, HRESULT* phr
        , TCHAR *pName = NAME(DUALSUBS_FILTER_NAME "Consumer Property Page (Misc)"))
        : CDVSMiscPPage(lpunk, phr, pName) {}
};

[uuid(DUALSUBS_CLSID_CONSUMER_COLORPAGE_STR)]
class CXySubFilterConsumerAboutPPage : public CDVSAboutPPage
{
public:
    CXySubFilterConsumerAboutPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR* pName=NAME(DUALSUBS_FILTER_NAME "Consumer Property Page (about)"))
        : CDVSAboutPPage(lpunk, phr, pName) {}
};

[uuid(DUALSUBS_CLSID_CONSUMER_ABOUTPAGE_STR)]
class CXySubFilterConsumerColorPPage : public CDVSColorPPage
{
public:
    CXySubFilterConsumerColorPPage(LPUNKNOWN lpunk, HRESULT* phr, TCHAR* pName=NAME(DUALSUBS_FILTER_NAME "Consumer Property Page (color)"))
        : CDVSColorPPage(lpunk, phr, pName) {}
};
