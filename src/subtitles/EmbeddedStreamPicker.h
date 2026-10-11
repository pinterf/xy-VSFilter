/*
 * Embedded-stream picker logic (fork addition).
 *
 * The Dual Subtitles property page presents the streams discovered by
 * EnumerateEmbeddedStreamsFromMedia() and turns a picked entry into the option
 * values the filter already understands:
 *
 *   TS/BDMV  -> 'pid:0xNNNN' text entry + INT_SECONDARY_EMBEDDED_PID = PID
 *   Matroska -> 'track:N'    text entry (no PID; the dedicated option is 0)
 *
 * That mapping (list entry -> option token/value, and configured option ->
 * listed entry) is pure and has no UI dependency, so it lives here next to the
 * enumeration and is exercised by embedded_enum_test alongside the real
 * container files.
 */

#pragma once

#include <vector>
#include <atlstr.h>              // CString

#include "IMultiSubtitleTrackLoader.h"   // EmbeddedStreamInfo

// The text token the option grammar accepts for one embedded stream:
//   TS/BDMV  -> "pid:0xNNNN"  (raw PID, also mirrored in the int option)
//   Matroska -> "track:N"     (container TrackNumber)
CString EmbeddedStreamToken(const EmbeddedStreamInfo& s);

// Value for INT_SECONDARY_EMBEDDED_PID that selects 's': the raw PID for a
// TS/BDMV stream, 0 for a Matroska stream (whose selection is carried by the
// text token alone).
int EmbeddedStreamOptionPid(const EmbeddedStreamInfo& s);

// Looks up the listed stream the current configuration selects. 'text' is the
// secondary option value (a ';'/','-separated list whose embedded entries are
// 'pid:0xNNNN', 'track:N' or 'sub:N'), and 'embedded_pid_option' the value of
// INT_SECONDARY_EMBEDDED_PID. Returns the 0-based index into 'streams' or -1
// when nothing resolves (so the caller can leave the combo unselected).
int FindSelectedEmbeddedStream(const std::vector<EmbeddedStreamInfo>& streams,
    const CString& text, int embedded_pid_option);

// Removes the embedded-selection entries ('pid:...', 'track:...', 'sub:...')
// from a secondary list text, keeping external file paths. Entries are ';'
// separated, or ',' when no ';' is present - mirroring the option grammar.
CString StripEmbeddedEntries(const CString& list);
