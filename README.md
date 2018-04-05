# xy-VSFilter
- Origin of mod source: https://github.com/Cyberbeing/xy-VSFilter/releases/tag/3.0.0.306
- Forum: https://forum.doom9.org/showthread.php?t=168282
- Rebuilt with Visual Studio 2017
- Avisynth+ headers
- Native support for YUV420P10 and YUV420P16 formats
- Conversion to and from P010 and P016 with SSE2 support
- Still doesn't work in any MT mode (I think): automatically registers itself as MT_SERIALIZED to prevent troubles
- MaskSub: new pixel_type (default: "RGB32") parameter. Can be set to YUV420P10 or YUV420P16
xy-VSFilter
