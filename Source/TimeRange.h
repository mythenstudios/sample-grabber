#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

// Time range of a source ("FROM-TO", "FROM-") and start times in links. Pure string functions, any thread.
// Times: SS, MM:SS or H:MM:SS, decimals allowed on the last part (e.g. 90, 1:30, 1:02:00.5).
namespace sg::range
{
    constexpr double longSourceSeconds = 20.0 * 60.0;   // above: ask before grabbing everything
    constexpr double maxSourceSeconds  = 2.0 * 3600.0;  // above: a range is required

    struct Range
    {
        double from = 0.0;
        double to = -1.0;   // < 0 = to the end of the source
        bool set = false;   // false = whole source

        bool hasEnd() const { return to >= 0.0; }
        bool operator== (const Range& o) const
        {
            return set == o.set && (! set || (std::abs (from - o.from) < 1.0e-6 && std::abs (to - o.to) < 1.0e-6));
        }
        bool operator!= (const Range& o) const { return ! operator== (o); }
    };

    // Parses one time. false for anything else (empty, letters, 1:75, negative, more than three parts).
    inline bool parseTime (const juce::String& textIn, double& out)
    {
        const auto text = textIn.trim();
        if (text.isEmpty())
            return false;

        const auto parts = juce::StringArray::fromTokens (text, ":", {});
        if (parts.size() < 1 || parts.size() > 3)
            return false;

        double total = 0.0;
        for (int i = 0; i < parts.size(); ++i)
        {
            const auto p = parts[i].trim();
            const bool last = i == parts.size() - 1;
            if (p.isEmpty() || ! p.containsOnly (last ? "0123456789." : "0123456789"))
                return false;
            if (p.indexOfChar ('.') != p.lastIndexOfChar ('.') || p.startsWithChar ('.') || p.endsWithChar ('.'))
                return false;

            const double v = p.getDoubleValue();
            // Everything after the first part is a 60-based digit pair: minutes in H:MM:SS, seconds in MM:SS / H:MM:SS
            if (i > 0 && v >= 60.0)
                return false;
            total = total * 60.0 + v;
        }

        if (! std::isfinite (total) || total < 0.0 || total > 1.0e7)
            return false;
        out = total;
        return true;
    }

    struct ParseResult
    {
        bool ok = false;
        Range range;
        juce::String error;   // status text without the "ERROR" prefix
    };

    // "" = whole source, "FROM-TO", "FROM-" (to the end). Spaces around the dash are allowed.
    inline ParseResult parse (const juce::String& textIn)
    {
        ParseResult r;
        const auto text = textIn.trim();
        if (text.isEmpty())
        {
            r.ok = true;
            return r;
        }

        const juce::String formatError ("RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30");
        const int dash = text.indexOfChar ('-');
        if (dash <= 0 || text.lastIndexOfChar ('-') != dash)
        {
            r.error = formatError;
            return r;
        }

        double a = 0.0, b = -1.0;
        const auto left = text.substring (0, dash).trim();
        const auto right = text.substring (dash + 1).trim();
        if (! parseTime (left, a) || (right.isNotEmpty() && ! parseTime (right, b)))
        {
            r.error = formatError;
            return r;
        }

        if (right.isNotEmpty() && b <= a)
        {
            r.error = "RANGE ENDS BEFORE IT STARTS";
            return r;
        }
        if (right.isNotEmpty() && b - a < 0.1 - 1.0e-9)
        {
            r.error = "RANGE TOO SHORT (MIN 0.1 S)";
            return r;
        }

        r.ok = true;
        r.range.set = true;
        r.range.from = a;
        r.range.to = right.isNotEmpty() ? b : -1.0;
        return r;
    }

    // H:MM:SS or M:SS, with up to 3 decimals if the time is fractional (trailing zeros dropped)
    inline juce::String formatTime (double seconds)
    {
        seconds = juce::jmax (0.0, seconds);
        const long long ms = std::llround (seconds * 1000.0);
        const long long whole = ms / 1000;
        const int frac = (int) (ms % 1000);
        const long long h = whole / 3600;
        const int m = (int) ((whole / 60) % 60);
        const int s = (int) (whole % 60);

        auto two = [] (int v) { return (v < 10 ? "0" : "") + juce::String (v); };
        juce::String out = h > 0 ? juce::String (h) + ":" + two (m) + ":" + two (s)
                                 : juce::String (m) + ":" + two (s);
        if (frac > 0)
        {
            juce::String f = juce::String (frac).paddedLeft ('0', 3);
            while (f.endsWithChar ('0'))
                f = f.dropLastCharacters (1);
            out << "." << f;
        }
        return out;
    }

    // Whole seconds only (status lines about lengths)
    inline juce::String formatDuration (double seconds)
    {
        return formatTime (std::floor (juce::jmax (0.0, seconds)));
    }

    // Normalised text for the range field / plugin state, e.g. "1:02:00-1:05:30" or "1:02:00-"; empty = whole source
    inline juce::String toText (const Range& r)
    {
        if (! r.set)
            return {};
        return formatTime (r.from) + "-" + (r.hasEnd() ? formatTime (r.to) : juce::String());
    }

    // Seconds for tools (yt-dlp --download-sections, ffmpeg -ss/-t): plain decimal, at most 3 decimals
    inline juce::String toolSeconds (double s)
    {
        auto t = juce::String (juce::jmax (0.0, s), 3);
        while (t.containsChar ('.') && (t.endsWithChar ('0') || t.endsWithChar ('.')))
            t = t.dropLastCharacters (1);
        return t;
    }

    // File name tag "r3720-3930" (milliseconds -> "p" decimal: "r10p5-25"), open end "r3720-end". Empty if no range.
    inline juce::String fileTag (const Range& r)
    {
        if (! r.set)
            return {};
        auto num = [] (double s) { return toolSeconds (s).replaceCharacter ('.', 'p'); };
        return "r" + num (r.from) + "-" + (r.hasEnd() ? num (r.to) : juce::String ("end"));
    }

    // Inverse of fileTag (without a leading underscore). false if tag is not one of ours.
    inline bool parseFileTag (const juce::String& tag, Range& out)
    {
        if (! tag.startsWithChar ('r') || tag.length() < 4)
            return false;
        const auto body = tag.substring (1);
        const int dash = body.indexOfChar ('-');
        if (dash <= 0 || body.lastIndexOfChar ('-') != dash)
            return false;

        auto num = [] (const juce::String& t, double& v)
        {
            if (t.isEmpty() || ! t.containsOnly ("0123456789p") || t.startsWithChar ('p') || t.endsWithChar ('p')
                || t.indexOfChar ('p') != t.lastIndexOfChar ('p'))
                return false;
            v = t.replaceCharacter ('p', '.').getDoubleValue();
            return true;
        };

        Range r;
        r.set = true;
        if (! num (body.substring (0, dash), r.from))
            return false;
        const auto right = body.substring (dash + 1);
        if (right == "end")
            r.to = -1.0;
        else if (! num (right, r.to) || r.to <= r.from)
            return false;
        out = r;
        return true;
    }

    // "90", "90s", "1h2m3s", "2m", "1m30.5s" (YouTube/Twitch style) -> seconds. false for anything else.
    inline bool parseUrlTime (const juce::String& vIn, double& out)
    {
        const auto v = vIn.trim().toLowerCase();
        if (v.isEmpty())
            return false;
        if (v.containsOnly ("0123456789."))
            return parseTime (v, out);

        double total = 0.0;
        juce::String num;
        bool any = false;
        juce::String seenUnits;
        for (auto p = v.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const auto ch = *p;
            if ((ch >= '0' && ch <= '9') || ch == '.')
            {
                num << juce::String::charToString (ch);
                continue;
            }
            const double mul = ch == 'h' ? 3600.0 : ch == 'm' ? 60.0 : ch == 's' ? 1.0 : -1.0;
            if (mul < 0.0 || num.isEmpty() || seenUnits.containsChar (ch) || num.indexOfChar ('.') != num.lastIndexOfChar ('.'))
                return false;
            seenUnits << juce::String::charToString (ch);
            total += num.getDoubleValue() * mul;
            num.clear();
            any = true;
        }
        if (num.isNotEmpty() || ! any || ! std::isfinite (total))
            return false;
        out = total;
        return true;
    }

    // Start time in a link: query or fragment parameter t= or start= (e.g. youtu.be/x?t=90, watch?v=x&t=1h2m3s, #t=1m).
    // Local string parsing only. false if none or 0.
    inline bool startFromUrl (const juce::String& url, double& seconds)
    {
        const int q = url.indexOfAnyOf ("?#");
        if (q < 0)
            return false;

        for (const auto& kv : juce::StringArray::fromTokens (url.substring (q + 1), "&#?", {}))
        {
            const auto key = kv.upToFirstOccurrenceOf ("=", false, false).trim().toLowerCase();
            if ((key == "t" || key == "start") && kv.containsChar ('='))
            {
                double s = 0.0;
                if (parseUrlTime (juce::URL::removeEscapeChars (kv.fromFirstOccurrenceOf ("=", false, false)), s) && s > 0.0)
                {
                    seconds = s;
                    return true;
                }
            }
        }
        return false;
    }

    // Length the range covers for a source of the given duration (duration < 0 = unknown -> -1 if open-ended)
    inline double coveredSeconds (const Range& r, double duration)
    {
        if (! r.set)
            return duration;
        if (r.hasEnd())
            return r.to - r.from;
        return duration >= 0.0 ? juce::jmax (0.0, duration - r.from) : -1.0;
    }

    // Length rules before a grab. duration = probe/file header (< 0 = unknown); with a range the range length counts.
    //   <= 20 min -> grab;  20 min..2 h without range -> confirm (GRAB ALL);  > 2 h without range or range > 2 h -> blocked.
    //   Range starting at/after the end or ending more than 1 s after it -> blocked.
    //   Unknown length: local file -> grab; link without an end time -> confirm (a live stream would never finish).
    struct Verdict
    {
        enum class Kind { grab, confirm, blocked };
        Kind kind = Kind::grab;
        juce::String status;   // message for the status line (confirm/blocked)
    };

    inline Verdict guard (const Range& rg, double dur, bool isLink = false)
    {
        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
        Verdict v;
        auto block = [&] (const juce::String& s) { v.kind = Verdict::Kind::blocked; v.status = s; return v; };

        if (rg.set && dur >= 0.0 && (rg.from >= dur - 0.05 || (rg.hasEnd() && rg.to > dur + 1.0)))
            return block ("RANGE BEYOND END (SOURCE IS " + formatDuration (dur) + ")");

        if (isLink && dur < 0.0 && ! rg.hasEnd())
        {
            v.kind = Verdict::Kind::confirm;
            v.status = "LENGTH UNKNOWN (LIVE?)" + dot + "OPEN END - GRAB ANYWAY?";
            return v;
        }

        if (rg.set)
        {
            const double len = coveredSeconds (rg, dur);
            if (len > maxSourceSeconds)
                return block ("RANGE IS " + formatDuration (len) + dot + "MAX 2 H");
            return v;
        }
        if (dur > maxSourceSeconds)
            return block ("LONG SOURCE " + formatDuration (dur) + dot + "SET A RANGE (MAX 2 H)");
        if (dur > longSourceSeconds)
        {
            v.kind = Verdict::Kind::confirm;
            v.status = "LONG SOURCE " + formatDuration (dur) + dot + "SET A RANGE (RECOMMENDED) OR GRAB ALL";
        }
        return v;
    }

    // SG_RANGE_SELFTEST: parsing, formatting, file tags, link start times. Returns the number of failures.
    inline int runSelfTest()
    {
        int failures = 0;
        auto expect = [&] (bool ok, const juce::String& what)
        {
            std::fprintf (stderr, "[SG-RANGE] %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
            if (! ok)
                ++failures;
        };
        auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6; };

        struct T { const char* in; double v; bool ok; };
        const T times[] = { { "90", 90, true }, { "1:30", 90, true }, { "1:02:00", 3720, true }, { "0:05.25", 5.25, true },
                            { "12.5", 12.5, true }, { "75:00", 4500, true }, { "1:75", 0, false }, { "1:02:75", 0, false },
                            { "1:60:00", 0, false }, { "", 0, false }, { "abc", 0, false }, { "1:2:3:4", 0, false },
                            { "1.5:00", 0, false }, { "1..5", 0, false }, { ".5", 0, false }, { "5.", 0, false },
                            { "-5", 0, false }, { "1::2", 0, false } };
        for (const auto& t : times)
        {
            double v = -1.0;
            const bool ok = parseTime (t.in, v);
            expect (ok == t.ok && (! ok || near (v, t.v)), juce::String ("time '") + t.in + "' -> " + (ok ? juce::String (v) : "invalid"));
        }

        struct R { const char* in; bool ok; double a, b; const char* err; };
        const R ranges[] = { { "", true, 0, -1, "" }, { "1:02:00-1:05:30", true, 3720, 3930, "" }, { "10-25", true, 10, 25, "" },
                             { "10 - 25", true, 10, 25, "" }, { "1:00-", true, 60, -1, "" }, { " 0-0:15.5 ", true, 0, 15.5, "" },
                             { "25-10", false, 0, 0, "RANGE ENDS BEFORE IT STARTS" }, { "10-10", false, 0, 0, "RANGE ENDS BEFORE IT STARTS" },
                             { "-25", false, 0, 0, "RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30" },
                             { "10", false, 0, 0, "RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30" },
                             { "1-2-3", false, 0, 0, "RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30" },
                             { "a-b", false, 0, 0, "RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30" },
                             { "1:75-2:00", false, 0, 0, "RANGE: USE FROM-TO, E.G. 1:02:00-1:05:30" },
                             { "10-10.05", false, 0, 0, "RANGE TOO SHORT (MIN 0.1 S)" }, { "10-10.1", true, 10, 10.1, "" } };
        for (const auto& t : ranges)
        {
            const auto p = parse (t.in);
            bool ok = p.ok == t.ok;
            if (ok && p.ok && juce::String (t.in).trim().isNotEmpty())
                ok = p.range.set && near (p.range.from, t.a) && near (p.range.to, t.b);
            if (ok && ! p.ok)
                ok = p.error == t.err;
            expect (ok, juce::String ("range '") + t.in + "' -> " + (p.ok ? "'" + toText (p.range) + "'" : p.error));
        }

        expect (formatTime (3720) == "1:02:00", "format 3720 = " + formatTime (3720));
        expect (formatTime (750) == "12:30", "format 750 = " + formatTime (750));
        expect (formatTime (15.5) == "0:15.5", "format 15.5 = " + formatTime (15.5));
        expect (formatDuration (9667.9) == "2:41:07", "duration 9667.9 = " + formatDuration (9667.9));
        expect (toText (parse ("62:00-65:30").range) == "1:02:00-1:05:30", "normalise 62:00-65:30 = " + toText (parse ("62:00-65:30").range));
        expect (toolSeconds (10.0) == "10" && toolSeconds (10.5) == "10.5" && toolSeconds (0.125) == "0.125", "tool seconds");

        for (const char* txt : { "1:02:00-1:05:30", "0:10.5-0:25", "1:00-" })
        {
            const auto r = parse (txt).range;
            Range back;
            const auto tag = fileTag (r);
            expect (parseFileTag (tag, back) && back == r, juce::String ("file tag ") + txt + " -> " + tag);
        }
        Range dummy;
        for (const char* bad : { "r", "rx-1", "r10-5", "r1-2-3", "remix", "r10p-20", "r1-" })
            expect (! parseFileTag (bad, dummy), juce::String ("not a file tag: ") + bad);

        struct U { const char* url; bool ok; double v; };
        const U urls[] = { { "https://youtu.be/abc?t=90", true, 90 }, { "https://www.youtube.com/watch?v=abc&t=1h2m3s", true, 3723 },
                           { "https://www.youtube.com/watch?v=abc&t=2m", true, 120 }, { "https://www.youtube.com/watch?v=abc&t=95s", true, 95 },
                           { "https://www.youtube.com/embed/abc?start=30", true, 30 }, { "https://www.youtube.com/watch?v=abc#t=1m30s", true, 90 },
                           { "https://www.twitch.tv/videos/1?t=0h10m0s", true, 600 }, { "https://www.youtube.com/watch?v=abc", false, 0 },
                           { "https://www.youtube.com/watch?v=t&list=x", false, 0 }, { "https://x.com/a?t=0", false, 0 },
                           { "https://x.com/a?t=abc", false, 0 }, { "https://x.com/a?st=5", false, 0 }, { "https://x.com/a?t=1h1h", false, 0 } };
        for (const auto& u : urls)
        {
            double v = -1.0;
            const bool ok = startFromUrl (u.url, v);
            expect (ok == u.ok && (! ok || near (v, u.v)), juce::String ("url ") + u.url + " -> " + (ok ? juce::String (v) : "none"));
        }

        expect (near (coveredSeconds (parse ("10-25").range, 600), 15) && near (coveredSeconds (parse ("1:00-").range, 600), 540)
                && near (coveredSeconds (parse ("1:00-").range, -1), -1) && near (coveredSeconds ({}, 600), 600), "covered seconds");

        using K = Verdict::Kind;
        struct G { const char* range; double dur; K kind; const char* status; bool link = false; };
        const G guards[] = { { "", 635, K::grab, "" }, { "", 1200, K::grab, "" }, { "", 4350, K::confirm, "LONG SOURCE 1:12:30" },
                             { "", 7200, K::confirm, "LONG SOURCE 2:00:00" }, { "", 9667, K::blocked, "LONG SOURCE 2:41:07" },
                             { "", -1, K::grab, "" }, { "1:02:00-1:05:30", 9667, K::grab, "" }, { "1:00-", 9667, K::blocked, "RANGE IS 2:40:07" },
                             { "1:00:00-", 9667, K::grab, "" }, { "0-2:00:01", 9667, K::blocked, "RANGE IS 2:00:01" },
                             { "13:00-14:00", 750, K::blocked, "RANGE BEYOND END (SOURCE IS 12:30)" },
                             { "12:00-12:31", 750, K::grab, "" }, { "12:00-12:32", 750, K::blocked, "RANGE BEYOND END" },
                             { "12:30-", 750, K::blocked, "RANGE BEYOND END" }, { "1:00-", -1, K::grab, "" }, { "0-3:00:00", -1, K::blocked, "RANGE IS 3:00:00" },
                             { "", -1, K::confirm, "LENGTH UNKNOWN (LIVE?) \xc2\xb7 OPEN END - GRAB ANYWAY?", true }, { "1:00-", -1, K::confirm, "LENGTH UNKNOWN", true },
                             { "1:00-2:00", -1, K::grab, "", true }, { "", 635, K::grab, "", true } };
        for (const auto& g : guards)
        {
            const auto v = guard (parse (g.range).range, g.dur, g.link);
            expect (v.kind == g.kind && v.status.startsWith (juce::String::fromUTF8 (g.status)),
                    juce::String ("guard '") + g.range + "' dur " + juce::String (g.dur) + " -> " + juce::String ((int) v.kind) + " " + v.status);
        }

        std::fprintf (stderr, "[SG-RANGE] %s (%d failures)\n", failures == 0 ? "ALL OK" : "FAILURES", failures);
        return failures;
    }
}
