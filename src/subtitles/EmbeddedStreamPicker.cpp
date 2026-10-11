/*
 * See EmbeddedStreamPicker.h.
 */

#include "stdafx.h"
#include <atlstr.h>
#include "EmbeddedStreamPicker.h"

CString EmbeddedStreamToken(const EmbeddedStreamInfo& s)
{
    CString t;
    if (s.matroska)
    {
        t.Format(_T("track:%u"), (unsigned)s.raw_id);
    }
    else
    {
        t.Format(_T("pid:0x%04X"), (unsigned)s.raw_id);
    }
    return t;
}

int EmbeddedStreamOptionPid(const EmbeddedStreamInfo& s)
{
    return s.matroska ? 0 : (int)s.raw_id;
}

// Splits 'list' into entries the way the option grammar does: ';' preferred,
// ',' only when no ';' is present. Empty fields are preserved.
static void SplitEntries(const CString& list, std::vector<CString>& out)
{
    WCHAR sep = list.Find(_T(';')) >= 0 ? _T(';') : _T(',');
    int start = 0;
    for (;;)
    {
        int pos = list.Find(sep, start);
        if (pos < 0)
        {
            out.push_back(list.Mid(start));
            break;
        }
        out.push_back(list.Mid(start, pos - start));
        start = pos + 1;
    }
}

// Parses one embedded-token entry: "pid:<hex>" (TS raw PID), "track:<dec>"
// (container-native id) or "sub:<dec>" (1-based index into the list). Returns
// true and fills *value / *by_index on success. A bare prefix with no number is
// not a usable value here (the option layer maps it to its default), so it is
// reported as a miss.
static bool ParseEmbeddedToken(const CString& entry, WORD* value, bool* by_index)
{
    CString t = entry;
    t.Trim();
    *by_index = false;

    struct { const TCHAR* prefix; int base; bool idx; } kinds[] = {
        { _T("pid:"),   16, false },
        { _T("track:"), 10, false },
        { _T("sub:"),   10, true  },
    };
    for (int k = 0; k < 3; k++)
    {
        CString prefix = kinds[k].prefix;
        if (t.GetLength() <= prefix.GetLength() ||
            t.Left(prefix.GetLength()).CompareNoCase(prefix) != 0)
        {
            continue;
        }
        CString num = t.Mid(prefix.GetLength());
        num.Trim();
        if (num.IsEmpty())
        {
            return false;
        }
        long v = wcstol(num, NULL, kinds[k].base);
        if (v <= 0 || v > 0xFFFF)
        {
            return false;
        }
        *value = (WORD)v;
        *by_index = kinds[k].idx;
        return true;
    }
    return false;
}

int FindSelectedEmbeddedStream(const std::vector<EmbeddedStreamInfo>& streams,
    const CString& text, int embedded_pid_option)
{
    std::vector<CString> entries;
    SplitEntries(text, entries);

    for (size_t e = 0; e < entries.size(); e++)
    {
        CString entry = entries[e];
        // A full spec is 'token|scale|opacity|...'; only the token selects.
        int bar = entry.Find(_T('|'));
        if (bar >= 0)
        {
            entry = entry.Left(bar);
        }
        entry.Trim();
        if (entry.IsEmpty())
        {
            continue;
        }

        WORD value = 0;
        bool by_index = false;
        if (!ParseEmbeddedToken(entry, &value, &by_index))
        {
            continue;
        }

        // Explicit 'sub:N': 1-based index into the discovered list.
        if (by_index)
        {
            if (value >= 1 && value <= (WORD)streams.size())
            {
                return (int)(value - 1);
            }
            continue;
        }

        for (size_t i = 0; i < streams.size(); i++)
        {
            const EmbeddedStreamInfo& s = streams[i];
            if (s.raw_id == value)
            {
                // 'pid:' names a TS PID, 'track:' a container-native id; either
                // matches a stream whose raw id agrees (the container's own kind
                // decides, which is how the option layer resolves it too).
                return (int)i;
            }
        }
    }

    // Fall back to the dedicated PID option: a non-zero PID selects that TS
    // stream even when the text list carries no embedded entry.
    if (embedded_pid_option > 0)
    {
        for (size_t i = 0; i < streams.size(); i++)
        {
            const EmbeddedStreamInfo& s = streams[i];
            if (!s.matroska && (WORD)embedded_pid_option == s.raw_id)
            {
                return (int)i;
            }
        }
    }

    return -1;
}

CString StripEmbeddedEntries(const CString& list)
{
    CString trimmed = list;
    trimmed.Trim();
    if (trimmed.IsEmpty())
    {
        return trimmed;
    }

    std::vector<CString> entries;
    SplitEntries(trimmed, entries);

    CString keep;
    for (size_t i = 0; i < entries.size(); i++)
    {
        CString e = entries[i];
        // Drop any per-entry spec so the token test sees just the selector.
        int bar = e.Find(_T('|'));
        CString head = bar >= 0 ? e.Left(bar) : e;
        head.Trim();
        if (head.IsEmpty())
        {
            continue;
        }
        CString lower = head;
        lower.MakeLower();
        bool embedded = lower.Left(4) == _T("pid:")
            || lower.Left(6) == _T("track:")
            || lower.Left(4) == _T("sub:");
        if (embedded)
        {
            continue;
        }
        e.Trim();
        keep = keep.IsEmpty() ? e : (keep + _T(";") + e);
    }
    return keep;
}
