#pragma once

// ---------------------------------------------------------------------------
// DualSubs / MultiSubFilter identity
// ---------------------------------------------------------------------------
//
// All COM/registry/DLL/display *identity* for this fork lives here, so that:
//
//   * this fork can be installed ALONGSIDE stock XySubFilter without
//     replacing it (distinct CLSID, registry key, DLL and filter name), and
//   * returning to an upstream-mergeable state is a single flag flip
//     (DUALSUBS_UPSTREAM_IDENTITY 1 restores the original XySubFilter
//     identity), instead of a 285-site revert.
//
// Only *identity* is parameterised. Internal C++ class/type names are left
// exactly as upstream so that diffs against upstream stay small and reviewable.
//
// To merge a change back upstream: set DUALSUBS_UPSTREAM_IDENTITY to 1 and the
// build produces byte-for-byte the upstream identity again (the identity-only
// hunks then reduce to a no-op against upstream).

#ifndef DUALSUBS_UPSTREAM_IDENTITY
#define DUALSUBS_UPSTREAM_IDENTITY 0
#endif

#if DUALSUBS_UPSTREAM_IDENTITY

// --- Upstream XySubFilter identity (merge-back state) ---
#define DUALSUBS_FILTER_NAME        "XySubFilter"
#define DUALSUBS_FILTER_DISPLAY     "XySubFilter"
#define DUALSUBS_REG_VENDOR_NAME    "Gabest"     // HKCU\Software\Gabest\XySubFilter
#define DUALSUBS_REG_KEY_FILTER_NAME "XySubFilter"
#define DUALSUBS_PROFILE_NAME       DUALSUBS_REG_KEY_FILTER_NAME
#define DUALSUBS_DLL_NAME           "XySubFilter.dll"
#define DUALSUBS_OUTPUT_NAME        "XySubFilter"
#define DUALSUBS_CONFIG_EXPORT      XySubFilterConfiguration
#define DUALSUBS_CONSUMER_CONFIG_EXPORT XySubFilterConsumerConfiguration
#define DUALSUBS_PAGE_PREFIX        "XySubFilter"
#define DUALSUBS_CLSID_FILTER       2DFCB782-EC20-4A7C-B530-4577ADB33F21
#define DUALSUBS_CLSID_AUTOLOADER   6B237877-902B-4C6C-92F6-E63169A5166C
#define DUALSUBS_CLSID_MAINPAGE     4514EFD4-E09B-4995-9668-143F12994FE7
#define DUALSUBS_CLSID_DUALPAGE     8F3B7C51-5E4D-4A2B-9C1E-6D7A0B2C3D4E
#define DUALSUBS_CLSID_MOREPAGE     E7946C91-1083-4F0E-AC45-5CF6BE7DB4C7
#define DUALSUBS_CLSID_TIMINGPAGE   1438A8B4-E8AF-4A81-BCA8-970751A9EE82
#define DUALSUBS_CLSID_PATHSPAGE    26DD9A01-7F95-40DE-B53D-4B4CE4023280
#define DUALSUBS_CLSID_ABOUTPAGE    D0DE7ADC-7DC6-43EC-912D-ABD44D7453FB
#define DUALSUBS_CLSID_CONSUMER     db6974b5-9bd1-45f5-8462-27a88019b352
#define DUALSUBS_CLSID_CONSUMER_GENERALPAGE b7f66b77-10b9-48e4-be40-2e6e4d00292e
#define DUALSUBS_CLSID_CONSUMER_MISCPAGE    f2609280-2d03-4251-a1e4-65b1aff9dafe
#define DUALSUBS_CLSID_CONSUMER_COLORPAGE   2e714223-34bc-430d-b52a-5bd23ddbf049
#define DUALSUBS_CLSID_CONSUMER_ABOUTPAGE   c371c901-97eb-4bb4-b742-d53a0eaa7b3c

#else

// --- DualSubs / MultiSubFilter identity (default) ---
// Distinct from upstream in every dimension so both can be installed at once.
#define DUALSUBS_FILTER_NAME        "MultiSubFilter"
#define DUALSUBS_FILTER_DISPLAY     "MultiSubFilter"
// Registry root: HKCU\Software\MultiSubFilter\MultiSubFilter (no vendor
// namespace), so the fork's settings are completely separate from upstream's
// HKCU\Software\Gabest\XySubFilter and can never collide.
#define DUALSUBS_REG_VENDOR_NAME    "MultiSubFilter"
#define DUALSUBS_REG_KEY_FILTER_NAME    "MultiSubFilter"
#define DUALSUBS_PROFILE_NAME       DUALSUBS_REG_KEY_FILTER_NAME // HKCU\Software\<...>\<name>
#define DUALSUBS_DLL_NAME           "MultiSubFilter.dll"
#define DUALSUBS_OUTPUT_NAME        "MultiSubFilter"
#define DUALSUBS_CONFIG_EXPORT      MultiSubFilterConfiguration
#define DUALSUBS_CONSUMER_CONFIG_EXPORT MultiSubFilterConsumerConfiguration
#define DUALSUBS_PAGE_PREFIX        "MultiSubFilter"
#define DUALSUBS_CLSID_FILTER       67CDB97D-9EC5-44F0-B901-C788491156D5
#define DUALSUBS_CLSID_AUTOLOADER   C9CB0AA6-613F-43AE-AC40-1D06E94117F3
#define DUALSUBS_CLSID_MAINPAGE     446F00BD-4736-43FE-8751-68DB4AE98B98
#define DUALSUBS_CLSID_DUALPAGE     79FDC3FB-4020-42EE-989E-07B03608B947
#define DUALSUBS_CLSID_MOREPAGE     B1C4DB7A-9DEA-4175-AEBF-6F9FBD8DECAB
#define DUALSUBS_CLSID_TIMINGPAGE   97831154-EBDC-43C9-860B-8F645BDE6862
#define DUALSUBS_CLSID_PATHSPAGE    6D89476F-CE37-44BC-BFDD-33B006D58F50
#define DUALSUBS_CLSID_ABOUTPAGE    AF6DA275-1658-4F43-A1A2-4849694C0D22
#define DUALSUBS_CLSID_CONSUMER     B215E91A-AB47-4809-B478-79A39BF67D85
#define DUALSUBS_CLSID_CONSUMER_GENERALPAGE 85BDF7BF-8CBE-4BB2-B6EC-FD837A7C8A8C
#define DUALSUBS_CLSID_CONSUMER_MISCPAGE    7817E565-035C-4789-8BF9-67B5D62750D3
#define DUALSUBS_CLSID_CONSUMER_COLORPAGE   16C6F1AD-B929-44EF-A654-F2FDA8FA99F6
#define DUALSUBS_CLSID_CONSUMER_ABOUTPAGE   58A74B02-C666-4D13-81E9-1C0D080664D7

#endif

// Convenience: quoted forms and wide forms used by the various call sites.
#define DUALSUBS_QUOTE_(x) #x
#define DUALSUBS_QUOTE(x) DUALSUBS_QUOTE_(x)

// Stringized form of every CLSID (for uuid attributes and string GUIDs).
// Needs the raw token, hence the QUOTE double-expansion above.
#define DUALSUBS_CLSID_FILTER_STR      DUALSUBS_QUOTE(DUALSUBS_CLSID_FILTER)
#define DUALSUBS_CLSID_AUTOLOADER_STR  DUALSUBS_QUOTE(DUALSUBS_CLSID_AUTOLOADER)
#define DUALSUBS_CLSID_MAINPAGE_STR    DUALSUBS_QUOTE(DUALSUBS_CLSID_MAINPAGE)
#define DUALSUBS_CLSID_DUALPAGE_STR    DUALSUBS_QUOTE(DUALSUBS_CLSID_DUALPAGE)
#define DUALSUBS_CLSID_MOREPAGE_STR    DUALSUBS_QUOTE(DUALSUBS_CLSID_MOREPAGE)
#define DUALSUBS_CLSID_TIMINGPAGE_STR  DUALSUBS_QUOTE(DUALSUBS_CLSID_TIMINGPAGE)
#define DUALSUBS_CLSID_PATHSPAGE_STR   DUALSUBS_QUOTE(DUALSUBS_CLSID_PATHSPAGE)
#define DUALSUBS_CLSID_ABOUTPAGE_STR   DUALSUBS_QUOTE(DUALSUBS_CLSID_ABOUTPAGE)
#define DUALSUBS_CLSID_CONSUMER_STR    DUALSUBS_QUOTE(DUALSUBS_CLSID_CONSUMER)
#define DUALSUBS_CLSID_CONSUMER_GENERALPAGE_STR DUALSUBS_QUOTE(DUALSUBS_CLSID_CONSUMER_GENERALPAGE)
#define DUALSUBS_CLSID_CONSUMER_MISCPAGE_STR    DUALSUBS_QUOTE(DUALSUBS_CLSID_CONSUMER_MISCPAGE)
#define DUALSUBS_CLSID_CONSUMER_COLORPAGE_STR   DUALSUBS_QUOTE(DUALSUBS_CLSID_CONSUMER_COLORPAGE)
#define DUALSUBS_CLSID_CONSUMER_ABOUTPAGE_STR   DUALSUBS_QUOTE(DUALSUBS_CLSID_CONSUMER_ABOUTPAGE)

#define DUALSUBS_W_FILTER_NAME       L"" DUALSUBS_FILTER_NAME
#define DUALSUBS_W_FILTER_DISPLAY    L"" DUALSUBS_FILTER_DISPLAY
#define DUALSUBS_W_PROFILE_NAME      L"" DUALSUBS_PROFILE_NAME
#define DUALSUBS_PAGE_NAME(suffix)   L"" DUALSUBS_PAGE_PREFIX suffix

// ---------------------------------------------------------------------------
// GUID values for the CLSID_* symbols in moreuuids.h.
//
// The CLSIDs above are stored as bare A-B-C-D tokens so they can be stringized
// directly into [uuid(...)] attributes. A macro holding a comma-separated
// initializer cannot be fed through DEFINE_GUID (a function-like macro), so the
// values are re-derived from the stringized form via __uuidof on a tag struct.
// __uuidof on an attribute GUID is a compile-time constant, so this stays
// identical in INITGUID and non-INITGUID translation units and needs no
// allocation or run-time work.
//
// Only C++ translation units use these; the resource compiler gets a harmless
// comment instead, because it cannot parse __declspec/__uuidof.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
namespace dualsubs_identity_detail {
struct __declspec(uuid(DUALSUBS_CLSID_FILTER_STR))     tag_filter {};
struct __declspec(uuid(DUALSUBS_CLSID_AUTOLOADER_STR)) tag_autoloader {};
struct __declspec(uuid(DUALSUBS_CLSID_MAINPAGE_STR))   tag_mainpage {};
struct __declspec(uuid(DUALSUBS_CLSID_DUALPAGE_STR))   tag_dualpage {};
struct __declspec(uuid(DUALSUBS_CLSID_MOREPAGE_STR))   tag_morepage {};
struct __declspec(uuid(DUALSUBS_CLSID_TIMINGPAGE_STR)) tag_timingpage {};
struct __declspec(uuid(DUALSUBS_CLSID_PATHSPAGE_STR))  tag_pathspage {};
struct __declspec(uuid(DUALSUBS_CLSID_ABOUTPAGE_STR))  tag_aboutpage {};
struct __declspec(uuid(DUALSUBS_CLSID_CONSUMER_STR))   tag_consumer {};
}

// DEFINE_GUID-equivalent that accepts a comma-separated token list, so it can
// be dropped in where DEFINE_GUID(...) previously stood.
#ifdef INITGUID
#  define DUALSUBS_DEFINE_GUID(name, tag) \
     EXTERN_C const GUID DECLSPEC_SELECTANY name = __uuidof(dualsubs_identity_detail::tag)
#else
#  define DUALSUBS_DEFINE_GUID(name, tag) \
     EXTERN_C const GUID FAR name
#endif

// Export a function under its identity-derived name. The module-definition file
// cannot be preprocessed, so the configuration entry point is exported with a
// linker pragma instead of a .def line. x86 needs the stdcall-decorated
// internal name; x64 undecorates.
#ifdef _WIN64
#  define DUALSUBS_EXPORT_PRAGMA(name) \
     "/export:" DUALSUBS_QUOTE(name)
#else
#  define DUALSUBS_EXPORT_PRAGMA(name) \
     "/export:" DUALSUBS_QUOTE(name) "=_" DUALSUBS_QUOTE(name) "@16"
#endif
#endif // __cplusplus
