# xy-VSFilter (pfmod) v3.2.0.802 (20181031)
https://github.com/pinterf/xy-VSFilter/

A modification by pinterf. 

As of 31st October 2018 the mod is on the level of v3.1.0.752 (https://github.com/Cyberbeing/xy-VSFilter/tree/xy_sub_filter_rc4)
Plus:
- Source is Visual Studio 2017 friendly
- VSFilter: TextSub Avisynth filter: accepts native 10/16bit clip
- VSFilter: MaskSub Avisynth filter: new pixel_type parameter
- Fix possible x64 crash - truncated memory address
- Fix TextSub crash that could occur in rare circumstances

Forum: https://forum.doom9.org/showthread.php?t=168282
Other useful link: https://avisynth.org.ru/docs/english/externalfilters/vsfilter.htm
All credits to the previous authors.


Info:
- Package contains VSFilter.dll and XySubFilter.dll (x86 and x64 versions)
- Although they were built using v141_xp toolset and the necessary options, may not work under XP.
- VSFilter.DLL: Included Avisynth filters (name and parameter signature)
  VobSub "cs"
  TextSub "c[file]s[charset]i[fps]f[vfr]s"
  TextSubSwapUV "b"
  MaskSub "[file]s[width]i[height]i[fps]f[length]i[charset]i[vfr]s[pixel_type]s"
- Warning by the XySubFilter-3.1.0.746 beta authors (on forum's 1st post, from 2015):
  "Note: XySubFilter requires a compatible subtitle consumer. We recommend madVR 0.87.5+ or MPC-HC 1.7.2+ (EVR-CP)
  After downloading XySubFilter BETA3, you must ensure to run the 'Install' bat (not only replace) or else 
  XySubFilter's autoload helper required for entering the DirectShow graph with external subtitles will not be installed."

Change log:
- v3.2.0.802 (20181031)
-- Change version
   Cyberbeing have released a patch for their original xy-VSFilter, their version stepped to v3.1.0.752.
   Since I thought the project was full-dead, my version numbering kept - after a range skip - the original numbering, 
   Now I'm changing my versioning scheme to avoid possible future collision.
-- Fix TextSub crash that could occur in rare circumstances (shssoichiro)
-- Fix for SSA/ASS repositioning becoming permanently disabled after typesetting was displayed
   (madshi -> Cyberbeing v3.1.0.752 7th October 2018)
   Apply https://github.com/Cyberbeing/xy-VSFilter/commit/cf8f5b27de77fe649341bfab0fdfd498e1ad2fa6
-- Add BT.2020 Support 
   (madshi -> Cyberbeing v3.1.0.751 27th September 2018)
   Apply https://github.com/Cyberbeing/xy-VSFilter/commit/4804cf2cc1c6cb78c3d35566454c0ca682e82085 (xy_sub_filter_rc4)

- v3.1.0.801 (20180904)
-- fix random crash in x64 build (address truncated to 32 bits, Rasterizer::Draw and Rasterizer::FillSolidRect were affected)
-- XySubFilter.DLL now compiled with v141_xp toolset instead of v141 (like VSFilter.DLL)
-- From upstream (madshi, Cyberbeing)
   Fix a possible infinite loop in the Real Text subtitle parser on 64-bit
   Fix external SRT subtitles with italic tag not being flagged as repositionable
   Disable repositioning for SSA/ASS subtitles with default position level
   Add setting to allow repositioning of SSA/ASS dialog

- v3.1.0.800 (20180411)
-- Bring source to XySubFilter-3.1.0.746 level:
   Apply file differences between Cyberbeing XySubFilter-3.1.0.746 (source from the release pack) 
   and xy-VSFilter 3.0.0.306 mod.
-- make the source VS2017 compatible again
-- make VSFilter work again as an Avisynth plugin
-- Versioning: 3.1.0 prefix is kept, build number goes from 800 and up for my mod manually in src/filters/transform/vsfilter/version_in.h

- v3.0.0.400 (20180405) (work in February 2018)
-- Get the last known xy-vsfilter version (3.0.0.306) which was still working as the Avisynth plugin.
   https://github.com/Cyberbeing/xy-VSFilter/releases/tag/3.0.0.306
   Plan: have a native 10/16 bit compatible Avisynth TextSub filter.
-- xy-vsfilter 3.0.0.306 rebuilt with Visual Studio 2017
-- Avisynth+ headers
-- Native support for YUV420P10 and YUV420P16 formats in Avisynth filter TextSub and probably VobSub.
   Internal conversion to and from P010 and P016 with SSE2 support
-- Still doesn't work in any MT mode (I think): automatically registers itself as MT_SERIALIZED to prevent troubles
-- Avisynth filter MaskSub: new pixel_type (default: "RGB32") parameter. Can be set to "YUV420P10" or "YUV420P16"
-- Versioning: 3.0.0 prefix kept, build number goes from 400 and up for my mod manually in src/filters/transform/vsfilter/version_in.h

xy-VSFilter
