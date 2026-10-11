# MultiSubFilter — MPC-HC quick setup

Five minutes, once. This is the exact sequence that is known to work; the
mistakes people hit are called out below.

Use the **x64** build with 64-bit MPC-HC (the normal case); use **x86** only for
a 32-bit player.

## 1. Unregister stock XySubFilter

Two subtitle providers in one graph fight over the same `ISubRenderConsumer`.
Keep exactly one. If upstream `XySubFilter.dll` is registered, remove it
(Administrator):

```
regsvr32 /u XySubFilter.dll
```

## 2. Register MultiSubFilter

Run `Install_x64.bat` **as Administrator**.

The binaries are unsigned, so Windows may block `regsvr32`. If registration
fails, right-click `MultiSubFilter.dll` → Properties → tick **Unblock**
(or `Unblock-File` in PowerShell), then run the installer again.

## 3. Add it to MPC-HC as an external filter

**Options → External Filters → Add Filter →** pick `MultiSubFilter.dll`, then
select it in the list and set it to **Prefer**.

If subtitles still do not appear, check that **Block VSFilter** is *off*.

## 4. Set MPC-HC's subtitle renderer to “VS Filter”

**Options → Subtitles → Subtitle renderer →** choose **VS Filter**
(shown as *VS Filter / DirectVobSub*).

> ### Do NOT pick “XySubFilter” in that list
> That list is fixed and compiled into MPC-HC; MultiSubFilter never appears in
> it. Its **XySubFilter** entry points at *upstream's* CLSID — and since you
> unregistered upstream in step 1, selecting it makes **MPC-HC exit immediately
> with no message**. That is not a MultiSubFilter bug. The correct choice is
> **VS Filter**.

Leave the list on **None** if you only want the filter loaded when you
explicitly add it.

## 5. Restart MPC-HC and confirm

Play a video, then **Play → Filters**. You should see `MultiSubFilter` in the
graph, with no suffix. Two suffixes mean a setup problem:

| Shown as | Meaning |
|---|---|
| `MultiSubFilter` | connected — good |
| `MultiSubFilter (=> !!!)` | no consumer found — the renderer is not exposing one; recheck steps 3–4 |
| `MultiSubFilter (=> stood down)` | the coexistence guard fired because another Xy-family filter is active — recheck step 1 |

## 6. Configure the second track

Play a video, then **Play → Filters → MultiSubFilter → Dual Subtitles**. Tick
*Enable dual subtitles*, then set the second track — either a subtitle file, or,
for Blu-ray TS/BDMV content, the embedded stream:

```
secondary_embedded_pid = 0x1200
```

Pick the *other* stream's PID as the secondary (the player's splitter already
feeds the first), otherwise the same track renders twice. See `dist/README.md`
for the full option set (`scale`, `opacity`, `align`, `margin`, `z-order`).

---

Recovery, if something goes wrong: `Uninstall_x64.bat` (Administrator) removes
the registration; delete the entry from *External Filters* and set the subtitle
renderer back to **Internal**.
