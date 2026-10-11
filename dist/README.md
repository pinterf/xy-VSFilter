# MultiSubFilter

A DirectShow subtitle filter that renders **several subtitle tracks at once** and
lets you control how they are composed. A fork of XySubFilter / xy-VSFilter.

It speaks the standard `SubRenderIntf` provider interface, so it works in MPC-HC,
MPC-BE, PotPlayer and anything whose video renderer exposes `ISubRenderConsumer`.
No player or renderer patch is required.

## Coexists with XySubFilter

This filter has its **own identity** (CLSID, filter name, registry key and DLL
name), so it installs *alongside* stock XySubFilter rather than replacing it.
Settings are separate too: `HKCU\Software\MultiSubFilter\MultiSubFilter`.

> **Only one subtitle renderer may be active in the graph.** If both
> `MultiSubFilter.dll` and `XySubFilter.dll` are registered with the same merit,
> a player can load *both* into one graph; two providers then fight over the
> same `ISubRenderConsumer`. Install one, or keep one unregistered / block the
> other. An `Uninstall_*.bat` for whichever you do not want is the simplest fix.

## What it does

- Renders multiple subtitle tracks simultaneously.
- **Embedded** tracks: for TS/BDMV content the filter reads the container itself,
  so two PGS tracks from one file render at once — no splitter change, no
  extracted `.sup`, no FFmpeg.
- **External** tracks: any number of files, with the usual auto-load.
- Per track: vertical placement (as-is / top / bottom), top margin, scale %,
  opacity, and z-order.
- Composition is done by the filter and delivered as a single composed bitmap,
  so it works with every consumer regardless of how many bitmaps it accepts.

## Install

### One-shot (recommended)

Extract the archive and run **`Setup_MPC-HC_x64.bat` as Administrator**. It:

1. registers `MultiSubFilter.dll`,
2. sets MPC-HC's *Subtitle Renderer* to **VS Filter / DirectVobSub**, and
3. adds `MultiSubFilter` to MPC-HC's *External Filters* as **Prefer**.

Re-running is safe (it reuses the existing filter entry). Restart MPC-HC after.

### Manual

1. Pick the build matching your player: `x64` for 64-bit MPC-HC (usual), `x86`
   for a 32-bit player.
2. Run `Install_*.bat` **as Administrator** (registers `MultiSubFilter.dll`).
   The binaries are unsigned, so Windows may block `regsvr32`: if registration
   fails, right-click `MultiSubFilter.dll` → Properties → tick **Unblock**
   (or `Unblock-File` in PowerShell), then run the installer again.
3. In MPC-HC: **Options → External Filters → Add Filter** → select
   `MultiSubFilter.dll`, set it to **Prefer**. If subtitles do not appear, make
   sure *Block VSFilter* is off. This is the only player-side step: it tells the
   player to use the filter. Subtitle behaviour itself is configured in step 4.
4. Configure tracks and composition in the filter's own page: play a video, then
   **Play → Filters → MultiSubFilter → Dual Subtitles** (see below).

### Do NOT pick "XySubFilter" in the subtitle-renderer list

MPC-HC's *Subtitle Renderer* list is fixed and compiled in; MultiSubFilter never
appears there. Its options are Internal / VS Filter / **XySubFilter** / None. The
**XySubFilter** entry refers to upstream's CLSID — selecting it while that filter
is not installed makes **MPC-HC exit immediately**. Use **VS Filter** instead.

Uninstall with `Uninstall_*.bat` (Administrator).

## Configure

Play a video, then **Play → Filters → MultiSubFilter → Dual Subtitles**:

| Control | Meaning |
|---|---|
| Enable dual subtitles | master switch (off = stock single-track behaviour) |
| Secondary subtitle file | one path, or several separated by `;` |
| Scale % / Opacity / Vertical / Top margin / Z-order | composition of the secondary |
| Primary vertical placement | move the primary track too (as-is = untouched) |
| Auto-load secondary languages | e.g. `eng,jpn`; a matching `<media>.<lang>.<ext>` is loaded as the secondary |

### Embedded second track

For TS/BDMV content set the embedded stream to use:

```
secondary_embedded_pid = 0x1200        (stream PID)
```

or inside the secondary list value, a token:

```
pid:0x1200|100|255|1|40|1
```

Note: the player's splitter already feeds one embedded subtitle stream (usually
the first); pick the *other* stream's PID as the secondary, otherwise you will
see the same track twice.

### Multi-entry file syntax

Each entry is `path[|scale[|opacity[|align[|margin[|z]]]]]`, entries separated
by `;`. Unspecified fields fall back to the controls above. Example:

```
C:\subs\movie.eng.srt|100|255|2|30|1;C:\subs\movie.jpn.ass|80|220|1|40|2
```

`align` is 0 = as-is, 1 = top, 2 = bottom.

### Scripting / hotkeys

All options are reachable through `IXyOptions` (`XySetBool`, `XySetInt`,
`XySetString` by field id), so a player or external tool can toggle dual
subtitles and choose the secondary.

## Containers

| Container | Embedded second track | Notes |
|---|---|---|
| Blu-ray TS/BDMV (`.m2ts`, `.mpls` folder) | yes | reads PAT/PMT/PES; resolves playlists to the clip and its in/out window |
| Matroska (`.mkv`) | planned | EBML parser, same decoder path |
| External files (`.srt/.ass/.sup/.idx`) | yes | any number, per the syntax above |

## Build

Visual Studio 2022 (v143), MFC + ATL, and `yasm` on `PATH` (the fork assembles
SIMD sources in thirdparty/Kasumi).

```
MSBuild VSFilter.sln /p:Configuration=Release /p:Platform=x64
```

Known: a parallel build can race on a shared intermediate directory
(`C1083 ... .asm Permission denied`); re-run the failed project serially.

## Upstream

The fork's identity is parameterised in `include/dualsubs_identity.h`. Building
with `DUALSUBS_UPSTREAM_IDENTITY=1` restores the original XySubFilter identity,
so the identity-only changes reduce to a no-op against upstream and the feature
work can be offered upstream as a patch series.

## Licence

GPL-2.0, inherited from upstream xy-VSFilter — see `License.txt`, shipped
unchanged. `SubRenderIntf.h` is BSD-licensed and used verbatim under its own
terms. Third-party components (libass, freetype, fribidi, harfbuzz, log4cplus,
unrar, VirtualDub Kasumi) keep their own notices as upstream ships them.
