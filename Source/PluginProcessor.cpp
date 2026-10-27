#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>

#if __has_include(<signalsmith-stretch/signalsmith-stretch.h>)
 #include <signalsmith-stretch/signalsmith-stretch.h>
#elif __has_include(<signalsmith-stretch.h>)
 #include <signalsmith-stretch.h>
#else
 #error "signalsmith-stretch.h not found - see CMakeLists.txt (FetchContent signalsmith-stretch)"
#endif

namespace
{
    constexpr int N = SampleGrabberProcessor::numPads;
    constexpr bool kDebugDump = true;   // diagnostic output on stderr
    constexpr double kPriorCenter = 120.0, kPriorSigma = 1.5, kAc2Weight = 1.0;   // tempo prior and weight of the double lag (sandbox tests, see HANDOVER)
    constexpr bool kGlobalBpmForShortSelection = true;   // selection < 45 s: determine tempo from the middle of the whole file
    constexpr bool kTransientFillPads = false;   // false: TRANSIENT leaves free pads empty; true: fill missing slices by halving (old behaviour)
    constexpr bool kUseFineBpm = true;  // refine BPM at finer resolution after the coarse winner (false = old behaviour)
    constexpr int kKeyVariant = 2;      // 0 = old chroma sum, 1 = semitone mean, 2 = + frame norm, 3 = + harmonic deconvolution
    constexpr const char* pcNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    constexpr int fluxHop = 512;   // resolution of the transient curve (samples per value)

    float readInterpolated (const juce::AudioBuffer<float>& b, int ch, double pos, int endExclusive)
    {
        const int i0 = (int) pos;
        const int i1 = juce::jmin (i0 + 1, endExclusive - 1);
        const float frac = (float) (pos - (double) i0);
        const float* d = b.getReadPointer (ch);
        return d[i0] + frac * (d[i1] - d[i0]);
    }

    // Once per loaded file: positive rise of log energy per hop.
    // This makes slice computation practically free while dragging the handles.
    std::vector<float> computeFlux (const juce::AudioBuffer<float>& buf)
    {
        const int n = buf.getNumSamples() / fluxHop;
        const int numCh = buf.getNumChannels();
        std::vector<float> flux ((size_t) juce::jmax (0, n), 0.0f);

        float prev = 0.0f;
        for (int h = 0; h < n; ++h)
        {
            double e = 0.0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* d = buf.getReadPointer (ch) + h * fluxHop;
                for (int i = 0; i < fluxHop; ++i)
                    e += (double) d[i] * (double) d[i];
            }

            const float logE = std::log10 ((float) (e / (double) (fluxHop * numCh)) + 1.0e-8f);
            flux[(size_t) h] = (h > 0) ? juce::jmax (0.0f, logE - prev) : 0.0f;
            prev = logE;
        }
        return flux;
    }

    // BEAT: slice length from the tempo, grid on the beat. The phase comes from the transient curve: we search for the
    // shift at which the sum (square root) of the curve at the beats, spaced 1 beat apart, is maximal.
    // Step size: finest division from 1/16 note (then doubling each time) that splits the selection into at most 24 cells; the last pad takes a remainder. If the first beat
    // lies later than about 10 % of a slice length after the selection start, the lead-in gets its own pad, otherwise it is added to the first
    // pad. Pads behind the selection end stay empty.
    // Returns false if no tempo is known, the selection is too short or no clear beat is detectable (then: like GRID).
    bool makeBeatBounds (const std::vector<float>& flux, int a, int b, double sr, double bpm, std::array<int, N + 1>& out)
    {
        const int nFlux = (int) flux.size();
        if (bpm < 30.0 || bpm > 400.0 || sr <= 0.0 || nFlux < 8 || b - a < (int) (0.25 * sr))
            return false;

        const double beat = 60.0 / bpm * sr;   // samples per beat

        auto fluxAt = [&] (double t) -> double
        {
            const double pos = t / (double) fluxHop;
            const int i0 = (int) pos;
            if (pos < 0.0 || i0 + 1 >= nFlux)
                return 0.0;
            const double fr = pos - (double) i0;
            return (1.0 - fr) * (double) flux[(size_t) i0] + fr * (double) flux[(size_t) i0 + 1];
        };

        constexpr int numPhases = 128;
        double bestScore = -1.0, sumScore = 0.0, bestPhase = 0.0;
        for (int k = 0; k < numPhases; ++k)
        {
            const double phase = beat * (double) k / (double) numPhases;
            double s = 0.0;
            for (double t = (double) a + phase; t < (double) b; t += beat)
                s += std::sqrt (juce::jmax (0.0, fluxAt (t)));

            sumScore += s;
            if (s > bestScore) { bestScore = s; bestPhase = phase; }
        }

        if (bestScore <= 0.0 || bestScore < 1.05 * (sumScore / (double) numPhases))
            return false;   // no detectable beat

        // Step size: finest division (1/16 note, then doubling each time) that splits the selection into at most 24 cells.
        // Up to 16 cells get one pad each; if there are more (up to 24), the last pad plays the remainder. Long selections (whole songs)
        // get correspondingly long steps (bars/phrases) so the pads are spread over the whole selection.
        const double len = (double) (b - a);
        double step = beat * 0.25;
        while (len / step > 1.5 * (double) N && step < beat * 1024.0)
            step *= 2.0;

        // First grid point one hop before the beat (as in TRANSIENT, so the attack is not cut off)
        // Grid origin: first beat from the selection start. If the selection start lies just AFTER a beat
        // (tolerance about 10 % step + 1 hop), that beat counts as origin. Otherwise the grid jumps by one beat when the
        // start is moved a few milliseconds (for steps of 2+ beats the bar counting changes too).
        const double tol = juce::jmin (0.45 * beat, 0.1 * step + (double) fluxHop);
        double origin = (double) a + bestPhase;
        if (beat - bestPhase <= tol)
            origin -= beat;
        const double g0 = origin - (double) fluxHop;
        const double k0 = std::floor ((g0 - (double) a) / step + 0.1);
        const double gFirst = g0 - k0 * step;   // lies in (a - 0.1 step, a + 0.9 step]
        const double pre = gFirst - (double) a;

        std::vector<int> pts { a };
        double p = pre > 0.1 * step ? gFirst : gFirst + step;
        while ((int) pts.size() < N && p < (double) b - 0.25 * step)
        {
            pts.push_back (juce::jmax (a + 1, (int) std::lround (p)));
            p += step;
        }
        pts.push_back (b);
        while ((int) pts.size() < N + 1)
            pts.push_back (b);

        for (size_t i = 0; i < out.size(); ++i)
            out[i] = pts[i];
        return true;
    }

    // Splits [a, b) into exactly 16 slices. transient=true: find transients first, then
    // leave free pads empty (start = end = selection end; kTransientFillPads = true fills by halving as before). Grid = 16 equal parts.
    std::array<int, N + 1> makeBounds (const std::vector<float>& flux, int a, int b, double sr, bool transient)
    {
        std::vector<int> pts { a, b };

        if (transient)
        {
            const int h0 = (a + fluxHop - 1) / fluxHop;
            const int h1 = juce::jmin (b / fluxHop, (int) flux.size());

            if (h1 - h0 >= 8)
            {
                double sum = 0.0;
                for (int h = h0; h < h1; ++h)
                    sum += (double) flux[(size_t) h];
                const float thr = (float) (sum / (double) (h1 - h0)) * 2.0f;

                struct Peak { int idx; float v; };
                std::vector<Peak> peaks;
                for (int h = h0 + 1; h < h1 - 1; ++h)
                {
                    const float v = flux[(size_t) h];
                    if (v > thr && v > flux[(size_t) h - 1] && v >= flux[(size_t) h + 1])
                        peaks.push_back ({ h, v });
                }

                std::sort (peaks.begin(), peaks.end(), [] (const Peak& x, const Peak& y) { return x.v > y.v; });

                const int minDist = juce::jmax (2, (int) (0.06 * sr / (double) fluxHop));   // min. 60 ms between slices (and to the selection edge)
                // With dense material (more onsets than pads): strongest first, but with a minimum distance of about selection/32,
                // so the 15 chosen onsets are spread over the whole selection instead of clumping at the start
                const int spacing = juce::jmax (minDist, (h1 - h0) / (2 * N));
                std::vector<int> chosen;
                for (const auto& p : peaks)
                {
                    if ((int) chosen.size() >= N - 1)
                        break;
                    if (p.idx - h0 < minDist || h1 - p.idx < minDist)
                        continue;

                    bool ok = true;
                    for (int c : chosen)
                        if (std::abs (c - p.idx) < spacing) { ok = false; break; }

                    if (ok)
                        chosen.push_back (p.idx);
                }

                for (int c : chosen)
                    pts.push_back (juce::jmax (a + 1, (c - 1) * fluxHop));   // one hop earlier so the attack is not cut off
            }
        }

        std::sort (pts.begin(), pts.end());

        // Safety net: after the selection above there cannot be more points than pads; if in doubt, truncate to the first 15
        if (transient && (int) pts.size() > N + 1)
            pts.erase (pts.begin() + N, pts.end() - 1);

        // Fewer onsets than pads: free pads stay empty (start = end = selection end)
        if (transient && ! kTransientFillPads)
            while ((int) pts.size() < N + 1)
                pts.push_back (b);

        while ((int) pts.size() < N + 1)
        {
            size_t longest = 0;
            int best = -1;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                if (pts[i + 1] - pts[i] > best) { best = pts[i + 1] - pts[i]; longest = i; }

            pts.insert (pts.begin() + (std::ptrdiff_t) longest + 1, pts[longest] + best / 2);
        }

        std::array<int, N + 1> out {};
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = pts[i];
        return out;
    }

    int gLastSliceLogKey = -1;   // diagnostics only: log slices only when mode/number of used pads changes (or forced)

    void logSlices (const std::array<int, N + 1>& bounds, double sr, int mode, bool force, double bpm)
    {
        if (! kDebugDump || sr <= 0.0)
            return;

        int occupied = 0;
        for (size_t i = 0; i < (size_t) N; ++i)
            if (bounds[i + 1] - bounds[i] >= 2)
                ++occupied;

        const int key = (mode + 1) * 100 + occupied;
        if (! force && key == gLastSliceLogKey)
            return;
        gLastSliceLogKey = key;

        std::fprintf (stderr, "[SG-SLICE] %s: %d of %d pads used, selection %.2f-%.2f s, tempo %.3f BPM\n[SG-SLICE]   bounds (s):",
                      mode == 1 ? "TRANSIENT" : mode == 2 ? "BEAT" : "GRID", occupied, N, (double) bounds.front() / sr, (double) bounds.back() / sr, bpm);
        for (size_t i = 0; i < bounds.size(); ++i)
            std::fprintf (stderr, " %.2f", (double) bounds[i] / sr);
        std::fprintf (stderr, "\n");
    }

    // BPM from the transient curve: autocorrelation on a fine BPM grid (70..210) with a
    // weak, broad prior (centre 120 BPM): the octave (half/double tempo) stays ambiguous, hence x2 and 1/2 in the UI.
    float estimateBpm (const std::vector<float>& flux, int h0, int h1, double sr)
    {
        const int n = h1 - h0;
        if (n < 256)   // < about 2.7 s: too short for a reliable estimate
            return 0.0f;

        std::vector<float> e ((size_t) n);
        double mean = 0.0;
        for (int i = 0; i < n; ++i)
        {
            e[(size_t) i] = std::sqrt (flux[(size_t) (h0 + i)]);
            mean += (double) e[(size_t) i];
        }
        mean /= (double) n;
        for (auto& v : e)
            v -= (float) mean;

        const double fps = sr / (double) fluxHop;

        auto ac = [&] (double lag) -> double
        {
            const int l0 = (int) lag;
            const double fr = lag - (double) l0;
            const int m = n - l0 - 2;
            if (m < 16)
                return 0.0;

            double s = 0.0;
            for (int i = 0; i < m; ++i)
                s += (double) e[(size_t) i] * ((1.0 - fr) * (double) e[(size_t) (i + l0)] + fr * (double) e[(size_t) (i + l0 + 1)]);
            return s / (double) m;
        };

        double bestScore = -1.0, bestBpm = 0.0;
        struct Cand { double bpm, a1, a2, prior, score; };
        std::vector<Cand> curve;
        for (double bpm = 70.0; bpm <= 210.0; bpm += 0.25)
        {
            const double lag = 60.0 / bpm * fps;
            const double a1 = ac (lag), a2 = ac (2.0 * lag);
            double s = a1 + kAc2Weight * a2;
            s = juce::jmax (0.0, s);

            const double x = std::log2 (bpm / kPriorCenter) / kPriorSigma;
            const double prior = std::exp (-0.5 * x * x);
            s *= prior;
            if (kDebugDump)
                curve.push_back ({ bpm, a1, a2, prior, s });

            if (s > bestScore)
            {
                bestScore = s;
                bestBpm = bpm;
            }
        }
        if (kDebugDump && ! curve.empty())
        {
            const double bestLag = 60.0 / bestBpm * fps;
            std::fprintf (stderr, "[SG-BPM] window %.1f-%.1f s (%d frames, fps %.2f), winner %.2f BPM, lag %.2f frames = %.2f BPM per frame\n",
                          (double) h0 * fluxHop / sr, (double) h1 * fluxHop / sr, n, fps, bestBpm, bestLag, bestBpm / bestLag);

            // Local maxima of the weighted curve: the best 5 with at least 4 % spacing
            std::vector<size_t> peaksIdx;
            for (size_t i = 1; i + 1 < curve.size(); ++i)
                if (curve[i].score > curve[i - 1].score && curve[i].score >= curve[i + 1].score)
                    peaksIdx.push_back (i);
            std::sort (peaksIdx.begin(), peaksIdx.end(), [&] (size_t p, size_t q) { return curve[p].score > curve[q].score; });

            std::vector<size_t> shown;
            for (size_t i : peaksIdx)
            {
                bool distinct = true;
                for (size_t j : shown)
                    if (std::abs (curve[i].bpm - curve[j].bpm) < 0.04 * curve[j].bpm) { distinct = false; break; }
                if (! distinct)
                    continue;

                shown.push_back (i);
                const auto& c = curve[i];
                std::fprintf (stderr, "[SG-BPM]   %7.2f BPM  score %.5f  (ac1 %.5f  ac2 %.5f  prior %.3f)\n", c.bpm, c.score, c.a1, c.a2, c.prior);
                if (shown.size() >= 5)
                    break;
            }

            // Octave neighbours of the winner (half / double tempo)
            for (double f : { 0.5, 2.0 })
            {
                const double ob = bestBpm * f;
                if (ob < 70.0 || ob > 210.0)
                    continue;
                const size_t k = (size_t) std::lround ((ob - 70.0) / 0.25);
                if (k >= curve.size())
                    continue;
                const auto& c = curve[k];
                std::fprintf (stderr, "[SG-BPM]   octave x%.1f: %7.2f BPM  score %.5f  (ac1 %.5f  ac2 %.5f  prior %.3f)\n", f, c.bpm, c.score, c.a1, c.a2, c.prior);
            }
        }
        return bestScore > 0.0 ? (float) bestBpm : 0.0f;
    }

    // Fine BPM: refines the coarse tempo. The coarse curve (hop 512) snaps to whole frames;
    // here we re-measure with hop 128 and correlate over 1x to 4x the period
    // (the more periods, the sharper the peak). We search only +-4 % around the start value,
    // so the octave decision (half/double tempo) does not change.
    struct FineBpm { double bpm = 0.0, score = 0.0, scoreAtStart = 0.0; };

    FineBpm refineBpm (const juce::AudioBuffer<float>& buf, int s0, int s1, double sr, double startBpm)
    {
        FineBpm res;
        constexpr int hop = 128;
        const int n = (s1 - s0) / hop;
        if (n < 1024 || startBpm <= 0.0)
            return res;

        const int numCh = buf.getNumChannels();
        std::vector<float> e ((size_t) n);
        double mean = 0.0;
        float prev = 0.0f;
        for (int h = 0; h < n; ++h)
        {
            double en = 0.0;
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* d = buf.getReadPointer (ch) + s0 + h * hop;
                for (int i = 0; i < hop; ++i)
                    en += (double) d[i] * (double) d[i];
            }

            const float logE = std::log10 ((float) (en / (double) (hop * numCh)) + 1.0e-8f);
            const float fl = (h > 0) ? juce::jmax (0.0f, logE - prev) : 0.0f;
            prev = logE;
            e[(size_t) h] = std::sqrt (fl);
            mean += (double) e[(size_t) h];
        }
        mean /= (double) n;
        for (auto& v : e)
            v -= (float) mean;

        const double fps = sr / (double) hop;

        auto acAt = [&] (double lag) -> double
        {
            const int l0 = (int) lag;
            const double fr = lag - (double) l0;
            const int m = n - l0 - 2;
            if (m < 256)
                return 0.0;

            double s = 0.0;
            for (int i = 0; i < m; ++i)
                s += (double) e[(size_t) i] * ((1.0 - fr) * (double) e[(size_t) (i + l0)] + fr * (double) e[(size_t) (i + l0 + 1)]);
            return s / (double) m;
        };

        auto score = [&] (double bpm) -> double
        {
            const double lag = 60.0 / bpm * fps;
            double s = 0.0;
            for (int k = 1; k <= 4; ++k)
                s += acAt ((double) k * lag);
            return s;
        };

        // Pass 1: +-4 % in 0.1 BPM steps; pass 2: +-0.2 BPM in 0.01 steps; then parabolic interpolation
        double best = startBpm, bestS = -1.0e30;
        for (double bp = startBpm * 0.96; bp <= startBpm * 1.04; bp += 0.1)
        {
            const double sc = score (bp);
            if (sc > bestS) { bestS = sc; best = bp; }
        }

        double best2 = best;
        bestS = -1.0e30;
        for (int i = -20; i <= 20; ++i)
        {
            const double bp = best + 0.01 * (double) i;
            const double sc = score (bp);
            if (sc > bestS) { bestS = sc; best2 = bp; }
        }

        const double yl = score (best2 - 0.01), yr = score (best2 + 0.01);
        const double den = yl - 2.0 * bestS + yr;
        double refined = best2;
        if (den < 0.0)
            refined += 0.01 * juce::jlimit (-1.0, 1.0, 0.5 * (yl - yr) / den);

        res.bpm = refined;
        res.score = bestS;
        res.scoreAtStart = score (startBpm);
        return res;
    }

    // Krumhansl-Schmuckler: correlation of the chroma with the 24 key profiles (0-11 major, 12-23 minor)
    void scoreKeys (const std::array<double, 12>& chroma, std::array<double, 24>& outCorr)
    {
        static const double kMajor[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double kMinor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        double mc = 0.0;
        for (double c : chroma)
            mc += c;
        mc /= 12.0;

        for (int mode = 0; mode < 2; ++mode)
        {
            const double* prof = mode == 0 ? kMajor : kMinor;
            double mp = 0.0;
            for (int i = 0; i < 12; ++i)
                mp += prof[i];
            mp /= 12.0;

            for (int t = 0; t < 12; ++t)
            {
                double num = 0.0, dc = 0.0, dp = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const double c = chroma[(size_t) ((t + i) % 12)] - mc;
                    const double p = prof[i] - mp;
                    num += c * p;
                    dc += c * c;
                    dp += p * p;
                }
                outCorr[(size_t) (t + 12 * mode)] = (dc > 0.0 && dp > 0.0) ? num / std::sqrt (dc * dp) : 0.0;
            }
        }
    }

    // New chroma variants (FFT 16384, hop 8192, 80-2000 Hz, up to 90 s from the middle):
    // out[0]: mean per semitone range instead of sum over all bins (every octave weighs the same)
    // out[1]: like out[0], each frame normalised to sum 1, very quiet frames (< 30 % of the median) skipped
    // out[2]: like out[1], then harmonic deconvolution (non-negative least squares with a harmonic template)
    bool computeChromaVariants (const juce::AudioBuffer<float>& buf, int a, int b, double sr,
                                std::array<std::array<double, 12>, 3>& out, int& framesUsed,
                                double& winStartS, double& winEndS)
    {
        constexpr int order = 14, size = 1 << order, hop = size / 2;
        const int maxLen = (int) (90.0 * sr);
        int s0 = a, s1 = b;
        if (b - a > maxLen)
        {
            s0 = a + (b - a - maxLen) / 2;
            s1 = s0 + maxLen;
        }
        if (s1 - s0 < size * 2)
            return false;

        winStartS = (double) s0 / sr;
        winEndS = (double) s1 / sr;

        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) size * 2), window ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1));

        const int numCh = buf.getNumChannels();
        std::vector<std::array<double, 12>> frames;

        for (int pos = s0; pos + size <= s1; pos += hop)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* d = buf.getReadPointer (ch) + pos;
                for (int i = 0; i < size; ++i)
                    data[(size_t) i] += d[i] / (float) numCh;
            }
            for (int i = 0; i < size; ++i)
                data[(size_t) i] *= window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (data.data());

            std::array<double, 128> sumP {}, cntP {};
            for (int k = 1; k < size / 2; ++k)
            {
                const double f = (double) k * sr / (double) size;
                if (f < 80.0) continue;
                if (f > 2000.0) break;

                const int midi = juce::jlimit (0, 127, (int) std::lround (69.0 + 12.0 * std::log2 (f / 440.0)));
                sumP[(size_t) midi] += std::sqrt ((double) data[(size_t) k]);
                cntP[(size_t) midi] += 1.0;
            }

            std::array<double, 12> fc {};
            for (int p = 0; p < 128; ++p)
                if (cntP[(size_t) p] > 0.0)
                    fc[(size_t) (p % 12)] += sumP[(size_t) p] / cntP[(size_t) p];
            frames.push_back (fc);
        }

        if (frames.empty())
            return false;

        std::vector<double> totals;
        for (const auto& fc : frames)
        {
            double t = 0.0;
            for (double c : fc)
                t += c;
            totals.push_back (t);
        }
        std::vector<double> sorted = totals;
        std::sort (sorted.begin(), sorted.end());
        const double gate = 0.3 * sorted[sorted.size() / 2];

        out = {};
        framesUsed = 0;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            for (size_t c = 0; c < 12; ++c)
                out[0][c] += frames[i][c];

            if (totals[i] <= 0.0 || totals[i] < gate)
                continue;

            for (size_t c = 0; c < 12; ++c)
                out[1][c] += frames[i][c] / totals[i];
            ++framesUsed;
        }

        if (framesUsed == 0)
            return false;

        // Harmonic deconvolution: each root class r produces energy at r (partials 1,2,4,8), r+7 (3,6), r+4 (5), r+10 (7)
        // with decreasing weight 0.8^(h-1). Solve for x >= 0 with T*x ~ chroma (multiplicative updates).
        double T[12][12] = {};
        for (int r = 0; r < 12; ++r)
            for (int h = 1; h <= 8; ++h)
                T[(r + (int) std::lround (12.0 * std::log2 ((double) h))) % 12][r] += std::pow (0.8, (double) (h - 1));

        double meanC = 0.0;
        for (double c : out[1])
            meanC += c;
        meanC /= 12.0;

        double x[12];
        for (int i = 0; i < 12; ++i)
            x[i] = meanC > 0.0 ? meanC : 1.0;

        for (int it = 0; it < 300; ++it)
        {
            double Tx[12] = {};
            for (int i = 0; i < 12; ++i)
                for (int r = 0; r < 12; ++r)
                    Tx[i] += T[i][r] * x[r];

            double next[12];
            for (int r = 0; r < 12; ++r)
            {
                double num = 0.0, den = 1.0e-12;
                for (int i = 0; i < 12; ++i)
                {
                    num += T[i][r] * out[1][(size_t) i];
                    den += T[i][r] * Tx[i];
                }
                next[r] = x[r] * num / den;
            }
            for (int r = 0; r < 12; ++r)
                x[r] = next[r];
        }

        for (int i = 0; i < 12; ++i)
            out[2][(size_t) i] = x[i];

        return true;
    }

    // Key: chroma from FFT frames (up to 40 s from the middle of the selection), Krumhansl-Schmuckler profiles.
    // For hardstyle/gabber (distorted kicks) this is only a rough approximation - the confidence shows it.
    void estimateKey (const juce::AudioBuffer<float>& buf, int a, int b, double sr, int& keyOut, float& confOut)
    {
        keyOut = -1;
        confOut = 0.0f;

        const int order = 13, size = 1 << order;
        const int maxLen = (int) (40.0 * sr);
        int s0 = a, s1 = b;
        if (b - a > maxLen)
        {
            s0 = a + (b - a - maxLen) / 2;
            s1 = s0 + maxLen;
        }
        if (s1 - s0 < size * 2)
            return;

        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) size * 2), window ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1));

        const int numCh = buf.getNumChannels();
        std::array<double, 12> chroma {};
        std::array<double, 5> bandMass {};   // chroma share per frequency band (diagnostics only)
        int frameCount = 0;

        for (int pos = s0; pos + size <= s1; pos += size / 2)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* d = buf.getReadPointer (ch) + pos;
                for (int i = 0; i < size; ++i)
                    data[(size_t) i] += d[i] / (float) numCh;
            }
            for (int i = 0; i < size; ++i)
                data[(size_t) i] *= window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (data.data());
            ++frameCount;

            for (int k = 1; k < size / 2; ++k)
            {
                const double f = (double) k * sr / (double) size;
                if (f < 70.0) continue;
                if (f > 2000.0) break;

                const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
                const int pc = (((int) std::lround (midi)) % 12 + 12) % 12;
                const double v = std::sqrt ((double) data[(size_t) k]);
                chroma[(size_t) pc] += v;
                bandMass[(size_t) (f < 140.0 ? 0 : f < 280.0 ? 1 : f < 560.0 ? 2 : f < 1120.0 ? 3 : 4)] += v;
            }
        }

        static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        auto corr = [&] (const double* prof, int tonic) -> double
        {
            double mc = 0.0, mp = 0.0;
            for (int i = 0; i < 12; ++i) { mc += chroma[(size_t) i]; mp += prof[i]; }
            mc /= 12.0;
            mp /= 12.0;

            double num = 0.0, dc = 0.0, dp = 0.0;
            for (int i = 0; i < 12; ++i)
            {
                const double c = chroma[(size_t) ((tonic + i) % 12)] - mc;
                const double p = prof[i] - mp;
                num += c * p;
                dc += c * c;
                dp += p * p;
            }
            return (dc > 0.0 && dp > 0.0) ? num / std::sqrt (dc * dp) : 0.0;
        };

        std::array<double, 24> allCorr {};
        double best = -2.0;
        int bestKey = -1;
        for (int t = 0; t < 12; ++t)
        {
            const double cM = corr (major, t);
            allCorr[(size_t) t] = cM;
            if (cM > best) { best = cM; bestKey = t; }
            const double cm = corr (minor, t);
            allCorr[(size_t) t + 12] = cm;
            if (cm > best) { best = cm; bestKey = t + 12; }
        }

        if (kDebugDump)
        {
            double mx = 0.0, tot = 0.0;
            for (double c : chroma)
                mx = std::max (mx, c);
            for (double m : bandMass)
                tot += m;

            std::fprintf (stderr, "[SG-KEY] window %.1f-%.1f s, %d frames\n", (double) s0 / sr, (double) s1 / sr, frameCount);
            std::fprintf (stderr, "[SG-KEY] Chroma (max=1):");
            for (int i = 0; i < 12; ++i)
                std::fprintf (stderr, " %s=%.2f", pcNames[i], mx > 0.0 ? chroma[(size_t) i] / mx : 0.0);
            std::fprintf (stderr, "\n");

            if (tot > 0.0)
                std::fprintf (stderr, "[SG-KEY] share per band: 70-140Hz %.0f%%  140-280 %.0f%%  280-560 %.0f%%  560-1120 %.0f%%  1120-2000 %.0f%%\n",
                              100.0 * bandMass[0] / tot, 100.0 * bandMass[1] / tot, 100.0 * bandMass[2] / tot,
                              100.0 * bandMass[3] / tot, 100.0 * bandMass[4] / tot);

            std::array<int, 24> order {};
            for (int i = 0; i < 24; ++i)
                order[(size_t) i] = i;
            std::sort (order.begin(), order.end(), [&] (int p, int q) { return allCorr[(size_t) p] > allCorr[(size_t) q]; });
            for (int i = 0; i < 5; ++i)
            {
                const int kk = order[(size_t) i];
                std::fprintf (stderr, "[SG-KEY]   #%d %s %s  r=%.3f\n", i + 1, pcNames[kk % 12], kk >= 12 ? "min" : "maj", allCorr[(size_t) kk]);
            }
        }

        if (kKeyVariant > 0)
        {
            std::array<std::array<double, 12>, 3> var {};
            int used = 0;
            double w0 = 0.0, w1 = 0.0;
            if (computeChromaVariants (buf, a, b, sr, var, used, w0, w1))
            {
                static const char* const label[3] = { "Halbton-Mittel", "+Frame-Norm", "+Oberton-Entfaltung" };
                if (kDebugDump)
                    std::fprintf (stderr, "[SG-KEY] new variants: window %.1f-%.1f s, %d frames used with gate, chosen: variant %d\n", w0, w1, used, kKeyVariant);

                for (size_t v = 0; v < 3; ++v)
                {
                    std::array<double, 24> cv {};
                    scoreKeys (var[v], cv);

                    std::array<int, 24> ord {};
                    for (int i = 0; i < 24; ++i)
                        ord[(size_t) i] = i;
                    std::sort (ord.begin(), ord.end(), [&] (int p, int q) { return cv[(size_t) p] > cv[(size_t) q]; });

                    if (kDebugDump)
                    {
                        double mx = 0.0;
                        for (double c : var[v])
                            mx = std::max (mx, c);
                        std::fprintf (stderr, "[SG-KEY] V%d %-20s Chroma:", (int) v + 1, label[v]);
                        for (int i = 0; i < 12; ++i)
                            std::fprintf (stderr, " %s=%.2f", pcNames[i], mx > 0.0 ? var[v][(size_t) i] / mx : 0.0);
                        std::fprintf (stderr, "\n[SG-KEY]    ");
                        for (int i = 0; i < 3; ++i)
                        {
                            const int kk = ord[(size_t) i];
                            std::fprintf (stderr, " #%d %s %s r=%.3f ", i + 1, pcNames[kk % 12], kk >= 12 ? "min" : "maj", cv[(size_t) kk]);
                        }
                        std::fprintf (stderr, "\n");
                    }

                    if ((int) v == kKeyVariant - 1)
                    {
                        bestKey = ord[0];
                        best = cv[(size_t) ord[0]];
                    }
                }
            }
        }

        keyOut = bestKey;
        confOut = (float) juce::jmax (0.0, best);
    }
}

//==============================================================================
// Background thread: pre-renders pitch/speed for the current selection (debounced, 300 ms of quiet).
class SampleGrabberProcessor::Renderer : public juce::Thread
{
public:
    explicit Renderer (SampleGrabberProcessor& p) : juce::Thread ("stretch-render"), owner (p) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (60);

            if (! owner.renderDirty.load())
                continue;
            if (juce::Time::getMillisecondCounter() - owner.lastChangeMs.load() < 150)
                continue;

            owner.renderBusy = true;     // busy first, then clear dirty: isRendering() stays true throughout
            owner.renderDirty = false;
            owner.doRender();
            owner.renderBusy = false;
        }
    }

private:
    SampleGrabberProcessor& owner;
};

//==============================================================================
std::shared_ptr<SampleGrabberProcessor::RenderedAudio>
SampleGrabberProcessor::renderStretched (const juce::AudioBuffer<float>& src, int a, int b, double sr,
                                         float pitchSemitones, double speed,
                                         const std::function<bool()>& shouldAbort)
{
    const int channels = src.getNumChannels();
    const int L = b - a;
    constexpr int chunk = 4096;

    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault (channels, (float) sr);
    st.setTransposeSemitones (pitchSemitones);
    st.reset();

    const int inLat = st.inputLatency();
    const int outLat = st.outputLatency();
    const int padIn = inLat + (int) std::ceil ((double) outLat * speed) + 2048;
    const int totalIn = ((L + padIn + chunk - 1) / chunk) * chunk;
    const int rawLen = (int) std::ceil ((double) totalIn / speed) + 64;

    juce::AudioBuffer<float> raw (channels, rawLen);
    raw.clear();
    juce::AudioBuffer<float> in (channels, chunk);
    std::vector<const float*> inPtr ((size_t) channels);
    std::vector<float*> outPtr ((size_t) channels);

    int inPos = 0, outPos = 0;
    while (inPos < totalIn)
    {
        if (shouldAbort && shouldAbort())
            return nullptr;

        for (int c = 0; c < channels; ++c)
        {
            float* d = in.getWritePointer (c);
            const float* s = src.getReadPointer (c);
            for (int i = 0; i < chunk; ++i)
            {
                const int idx = inPos + i;
                d[i] = idx < L ? s[a + idx] : 0.0f;
            }
            inPtr[(size_t) c] = d;
        }

        const int target = (int) std::lround ((double) (inPos + chunk) / speed);
        const int outN = juce::jmin (target - outPos, rawLen - outPos);

        for (int c = 0; c < channels; ++c)
            outPtr[(size_t) c] = raw.getWritePointer (c) + outPos;

        if (outN > 0)
        {
            st.process (inPtr.data(), chunk, outPtr.data(), outN);
            outPos += outN;
        }
        inPos += chunk;
    }

    // The output lags behind the input: skip outputLatency + inputLatency/speed samples
    const int skip = outLat + (int) std::lround ((double) inLat / speed);
    const int outLen = juce::jmax (1, (int) std::lround ((double) L / speed));

    auto r = std::make_shared<RenderedAudio>();
    r->buffer.setSize (channels, outLen);
    r->buffer.clear();

    const int avail = juce::jmax (0, juce::jmin (outLen, outPos - skip));
    if (avail > 0)
        for (int c = 0; c < channels; ++c)
            r->buffer.copyFrom (c, 0, raw, c, skip, avail);

    r->regionStart = a;
    r->regionEnd = b;
    r->speed = speed;
    return r;
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout SampleGrabberProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Gain: left stop (gainMinDb) = silent, skew chosen so that 0 dB sits exactly in the middle (norm value 0.5).
    // setSkewForCentre works with (value - start) / (end - start), so negative lower bounds are no problem.
    // The state stores real dB values: old sessions (e.g. -24 dB from the earlier range) still fit.
    {
        juce::NormalisableRange<float> gainRange (SampleGrabberProcessor::gainMinDb, SampleGrabberProcessor::gainMaxDb, 0.1f);
        gainRange.setSkewForCentre (0.0f);

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "gain", 1 }, "Gain", gainRange, 0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("dB")
                .withStringFromValueFunction ([] (float v, int)
                {
                    return v <= SampleGrabberProcessor::gainMinDb + 0.05f ? juce::String ("-inf") : juce::String (v, 1);
                })
                .withValueFromStringFunction ([] (const juce::String& t)
                {
                    return t.trim().containsIgnoreCase ("inf") ? SampleGrabberProcessor::gainMinDb : t.getFloatValue();
                })));
    }

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "pitch", 1 }, "Pitch",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "speed", 1 }, "Speed",
        juce::NormalisableRange<float> (50.0f, 200.0f, 0.1f), 100.0f));

    // Fades in % of the region (pad or selection), new IDs: old ms values ("fadeIn"/"fadeOut") are not read as %.
    // Skew: 10 % at the knob centre -> the lower half of the travel covers the short, frequently used fades finely.
    {
        juce::NormalisableRange<float> fadeRange (0.0f, SampleGrabberProcessor::fadeMaxPct, 0.1f);
        fadeRange.setSkewForCentre (10.0f);

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "fadeInPct", 1 }, "Fade In", fadeRange, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "fadeOutPct", 1 }, "Fade Out", fadeRange, 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Curve shape: exponent p = 2^(shape/50), 0 = linear
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "fadeShape", 1 }, "Fade Shape",
            juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f));
    }

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "loop", 1 }, "Loop", false));

    return layout;
}

SampleGrabberProcessor::SampleGrabberProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    formats.registerBasicFormats();   // WAV, AIFF, FLAC, OGG, ...

    gainParam    = apvts.getRawParameterValue ("gain");
    pitchParam   = apvts.getRawParameterValue ("pitch");
    speedParam   = apvts.getRawParameterValue ("speed");
    fadeInParam    = apvts.getRawParameterValue ("fadeInPct");
    fadeOutParam   = apvts.getRawParameterValue ("fadeOutPct");
    fadeShapeParam = apvts.getRawParameterValue ("fadeShape");
    loopParam    = apvts.getRawParameterValue ("loop");

    gainSmoothed.reset (hostRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (gainFromDb (gainParam->load()));

    apvts.addParameterListener ("pitch", this);
    apvts.addParameterListener ("speed", this);

    renderer = std::make_unique<Renderer> (*this);
    renderer->startThread();

    graveyardTimer.startTimer (1000);   // empty the graveyard once per second on the message thread
}

SampleGrabberProcessor::~SampleGrabberProcessor()
{
    apvts.removeParameterListener ("pitch", this);
    apvts.removeParameterListener ("speed", this);
    graveyardTimer.stopTimer();
    renderer->stopThread (5000);
}

void SampleGrabberProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    gainSmoothed.reset (sampleRate, 0.02);   // ~20 ms smoothing for gain changes
    gainSmoothed.setCurrentAndTargetValue (gainFromDb (gainParam->load()));
}

void SampleGrabberProcessor::retire (std::shared_ptr<juce::AudioBuffer<float>> old)
{
    if (old == nullptr)
        return;

    const std::lock_guard<std::mutex> gl (graveyardMutex);
    for (const auto& g : graveSamples)
        if (g == old)
            return;   // already queued (e.g. live again after BACK): duplicate entries would never reach use_count 1
    graveSamples.push_back (std::move (old));
}

void SampleGrabberProcessor::retire (std::shared_ptr<RenderedAudio> old)
{
    if (old == nullptr)
        return;

    const std::lock_guard<std::mutex> gl (graveyardMutex);
    for (const auto& g : graveRenders)
        if (g == old)
            return;
    graveRenders.push_back (std::move (old));
}

void SampleGrabberProcessor::sweepGraveyard()
{
    // Entries with use_count 1 hang only here: nobody else can get them anymore (sample/rendered no longer point
    // to them, the audio thread reads only those two). Only relink under the mutex, release outside.
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> deadSamples;
    std::vector<std::shared_ptr<RenderedAudio>> deadRenders;
    {
        const std::lock_guard<std::mutex> gl (graveyardMutex);

        auto collect = [] (auto& graves, auto& dead)
        {
            for (size_t i = 0; i < graves.size();)
            {
                if (graves[i].use_count() == 1)
                {
                    dead.push_back (std::move (graves[i]));
                    graves.erase (graves.begin() + (std::ptrdiff_t) i);
                }
                else
                {
                    ++i;
                }
            }
        };
        collect (graveSamples, deadSamples);
        collect (graveRenders, deadRenders);
    }

    // use_count() reads relaxed: fence so that the last reads by other threads (before their release) are visibly complete
    if (! deadSamples.empty() || ! deadRenders.empty())
        std::atomic_thread_fence (std::memory_order_acquire);

    deadSamples.clear();   // the big buffers drop here: message thread, no lock held
    deadRenders.clear();
}

void SampleGrabberProcessor::parameterChanged (const juce::String& parameterID, float)
{
    if (parameterID == "pitch" || parameterID == "speed")
        markRenderDirty();
}

void SampleGrabberProcessor::markRenderDirty()
{
    lastChangeMs = juce::Time::getMillisecondCounter();
    renderDirty = true;
}

void SampleGrabberProcessor::doRender()
{
    auto buf = getSnapshot();
    if (buf == nullptr)
        return;

    const auto reg = getRegion (buf->getNumSamples());
    const float pitch = pitchParam->load();
    const double speed = (double) speedParam->load() / 100.0;
    const bool neutral = std::abs (pitch) < 0.005f && std::abs (speed - 1.0) < 0.0005;

    if (neutral || reg.second - reg.first < 2048)
    {
        std::shared_ptr<RenderedAudio> old;
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            old = std::move (rendered);   // nothing to process -> play directly from the original
            rendered = nullptr;
        }
        if (old != nullptr)
            ++renderVersion;
        retire (std::move (old));   // the audio thread may still hold it as voiceRender
        return;
    }

    auto r = renderStretched (*buf, reg.first, reg.second, fileRate.load(), pitch, speed,
                              [this] { return renderDirty.load() || renderer->threadShouldExit(); });
    if (r == nullptr)
        return;   // aborted: a newer request is already queued

    std::shared_ptr<RenderedAudio> old;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (sample != buf)
            return;   // sample was switched in the meantime: render no longer fits, r is dropped outside the lock (renderer thread)
        old = std::move (rendered);
        rendered = r;
    }
    ++renderVersion;
    retire (std::move (old));
}

//==============================================================================
std::shared_ptr<juce::AudioBuffer<float>> SampleGrabberProcessor::getSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return sample;
}

bool SampleGrabberProcessor::hasSample() const
{
    return getSnapshot() != nullptr;
}

int SampleGrabberProcessor::getNumSamples() const
{
    auto s = getSnapshot();
    return s != nullptr ? s->getNumSamples() : 0;
}

std::pair<int, int> SampleGrabberProcessor::getRegion (int len) const
{
    const int a = juce::jlimit (0, len - 1, (int) std::lround ((double) trimStart.load() * len));
    const int e = juce::jlimit (a + 1, len, (int) std::lround ((double) trimEnd.load() * len));
    return { a, e };
}

juce::int64 SampleGrabberProcessor::getPeakKey() const
{
    const bool neutral = std::abs (pitchParam->load()) < 0.005f && std::abs (speedParam->load() - 100.0f) < 0.05f;
    return ((juce::int64) (juce::uint32) selectionVersion.load() << 32)
           ^ ((juce::int64) (juce::uint32) renderVersion.load() << 1)
           ^ (neutral ? 1 : 0);
}

float SampleGrabberProcessor::computeSelectionPeak() const
{
    std::shared_ptr<juce::AudioBuffer<float>> buf;
    std::shared_ptr<RenderedAudio> rnd;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        buf = sample;
        rnd = rendered;
    }
    if (buf == nullptr)
        return 0.0f;

    // Same choice as processBlock/exportRegion: render only if pitch/speed are active and it covers the selection.
    // While the render is still computing, the original applies (afterwards renderVersion changes -> recompute).
    const auto reg = getRegion (buf->getNumSamples());
    const bool neutral = std::abs (pitchParam->load()) < 0.005f && std::abs (speedParam->load() - 100.0f) < 0.05f;

    if (! neutral && rnd != nullptr && rnd->regionStart == reg.first && rnd->regionEnd == reg.second)
        return rnd->buffer.getNumSamples() > 0 ? rnd->buffer.getMagnitude (0, rnd->buffer.getNumSamples()) : 0.0f;

    return buf->getMagnitude (reg.first, reg.second - reg.first);
}

std::array<int, SampleGrabberProcessor::numPads + 1>
SampleGrabberProcessor::computeBoundsFor (const juce::AudioBuffer<float>& b) const
{
    const auto reg = getRegion (b.getNumSamples());
    const auto mode = getSliceMode();

    if (mode == SliceMode::beat)
    {
        std::array<int, numPads + 1> beatBounds {};
        if (makeBeatBounds (fluxCache, reg.first, reg.second, fileRate.load(), (double) bpmValue.load(), beatBounds))
            return beatBounds;

        // no tempo known or no clear beat: like GRID
        return makeBounds (fluxCache, reg.first, reg.second, fileRate.load(), false);
    }

    return makeBounds (fluxCache, reg.first, reg.second, fileRate.load(), mode == SliceMode::transient);
}

void SampleGrabberProcessor::recomputeSlices()
{
    auto s = getSnapshot();
    if (s == nullptr)
        return;

    const auto bounds = computeBoundsFor (*s);
    logSlices (bounds, fileRate.load(), (int) getSliceMode(), false, (double) bpmValue.load());
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        sliceBounds = bounds;
    }
    ++sliceVersion;
}

void SampleGrabberProcessor::pushHistory()
{
    auto cur = getSnapshot();
    if (cur == nullptr)
        return;

    HistoryEntry e;
    e.sample = cur;
    e.flux = fluxCache;
    e.file = loadedFile;
    e.sampleRate = fileRate.load();
    e.bitDepth = fileBitDepth.load();
    e.trimStart = trimStart.load();
    e.trimEnd = trimEnd.load();
    e.sliceMode = sliceMode.load();
    e.bpmOctave = bpmOctave.load();
    e.bpm = bpmValue.load();
    e.key = keyValue.load();
    e.keyConfidence = keyConfidence.load();
    history.push_back (std::move (e));

    // Limit: depth, then RAM. The newest entry is always kept, even if it alone exceeds the RAM limit.
    auto bytesOf = [] (const HistoryEntry& h)
    {
        return h.sample != nullptr ? (size_t) h.sample->getNumChannels() * (size_t) h.sample->getNumSamples() * sizeof (float) : (size_t) 0;
    };

    while ((int) history.size() > maxHistoryDepth)
        history.erase (history.begin());

    size_t total = 0;
    for (const auto& h : history)
        total += bytesOf (h);
    while (history.size() > 1 && total > maxHistoryBytes)
    {
        total -= bytesOf (history.front());
        history.erase (history.begin());
    }
}

void SampleGrabberProcessor::installSample (std::shared_ptr<juce::AudioBuffer<float>> newSample, const juce::File& file, bool forceLog)
{
    const auto bounds = computeBoundsFor (*newSample);   // slices for the new sample, before it goes live
    logSlices (bounds, fileRate.load(), (int) getSliceMode(), forceLog, (double) bpmValue.load());

    stopRequested.store (true);   // running playback would otherwise point into the old buffer (short fade-out instead of a click)
    std::shared_ptr<juce::AudioBuffer<float>> oldSample;
    std::shared_ptr<RenderedAudio> oldRender;
    {
        // swap only under the SpinLock; the old pointers then go to the graveyard (never release under the lock)
        const juce::SpinLock::ScopedLockType sl (lock);
        oldSample = std::exchange (sample, std::move (newSample));
        oldRender = std::exchange (rendered, nullptr);
        sliceBounds = bounds;
    }
    retire (std::move (oldSample));
    retire (std::move (oldRender));
    ++sliceVersion;
    ++selectionVersion;
    ++renderVersion;
    loadedFile = file;
    markRenderDirty();
}

bool SampleGrabberProcessor::goBack()
{
    if (history.empty())
        return false;

    HistoryEntry e = std::move (history.back());
    history.pop_back();

    fileRate = e.sampleRate;
    fileBitDepth = e.bitDepth;
    trimStart = e.trimStart;
    trimEnd = e.trimEnd;
    pendingTrigger = -1;
    fluxCache = std::move (e.flux);
    sliceMode = e.sliceMode;
    bpmOctave.store (e.bpmOctave);
    bpmValue = e.bpm;                  // valid immediately (BEAT grid); the analysis then runs again via selectionVersion
    keyValue = e.key;
    keyConfidence = e.keyConfidence;

    installSample (std::move (e.sample), e.file, true);
    return true;
}

bool SampleGrabberProcessor::loadSample (const juce::File& file, bool recordHistory)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0
        || reader->lengthInSamples > (juce::int64) (std::numeric_limits<int>::max() / 2))
        return false;

    auto fresh = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin<unsigned int> (2, reader->numChannels),
                                                              (int) reader->lengthInSamples);
    reader->read (fresh.get(), 0, fresh->getNumSamples(), 0, true, true);

    // Only now, once reading succeeded, save the previous state (before the fields below are overwritten)
    if (recordHistory)
        pushHistory();

    fileRate = reader->sampleRate;
    fileBitDepth = (int) reader->bitsPerSample;
    trimStart = 0.0f;
    trimEnd = 1.0f;
    pendingTrigger = -1;

    fluxCache = computeFlux (*fresh);
    bpmOctave.store (0);   // new file: reset tempo octave
    bpmValue = 0.0f;       // old tempo does not apply to the new file (BEAT grid needs the new tempo)

    installSample (std::move (fresh), file, true);
    return true;
}

void SampleGrabberProcessor::setTrim (float startNorm, float endNorm, bool recompute)
{
    startNorm = juce::jlimit (0.0f, 1.0f, startNorm);
    endNorm = juce::jlimit (0.0f, 1.0f, endNorm);
    if (endNorm < startNorm)
        std::swap (startNorm, endNorm);

    trimStart = startNorm;
    trimEnd = endNorm;
    ++selectionVersion;
    markRenderDirty();

    if (recompute)
        recomputeSlices();
}

void SampleGrabberProcessor::setSliceMode (SliceMode m)
{
    sliceMode = (int) m;
    gLastSliceLogKey = -1;   // diagnostics: always log after a mode change
    recomputeSlices();
}

std::array<float, SampleGrabberProcessor::numPads + 1> SampleGrabberProcessor::getSliceBoundsNorm() const
{
    std::array<float, numPads + 1> out {};
    std::shared_ptr<juce::AudioBuffer<float>> s;
    std::array<int, numPads + 1> b {};
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        s = sample;
        b = sliceBounds;
    }
    if (s == nullptr || s->getNumSamples() <= 0)
        return out;

    const float len = (float) s->getNumSamples();
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (float) b[i] / len;
    return out;
}

std::vector<SampleGrabberProcessor::PeakColumn>
SampleGrabberProcessor::computePeaks (int columns, float startNorm, float endNorm) const
{
    std::vector<PeakColumn> out;
    auto s = getSnapshot();
    if (s == nullptr || columns <= 0)
        return out;

    const int n = s->getNumSamples();
    const int r0 = juce::jlimit (0, n - 1, (int) ((double) startNorm * (double) n));
    const int r1 = juce::jlimit (r0 + 1, n, (int) ((double) endNorm * (double) n));
    const juce::int64 span = (juce::int64) (r1 - r0);
    const int numCh = s->getNumChannels();
    out.resize ((size_t) columns);

    for (int c = 0; c < columns; ++c)
    {
        const int a = r0 + (int) (span * c / columns);
        const int b = juce::jlimit (a + 1, r1, r0 + (int) (span * (c + 1) / columns));
        float mn = 0.0f, mx = 0.0f;
        double sq = 0.0;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* d = s->getReadPointer (ch);
            for (int i = a; i < b; ++i)
            {
                const float v = d[i];
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
                sq += (double) v * (double) v;
            }
        }

        out[(size_t) c] = { mn, mx, (float) std::sqrt (sq / (double) ((b - a) * numCh)) };
    }
    return out;
}

bool SampleGrabberProcessor::canShiftBpmOctave (int delta) const
{
    const float cur = bpmValue.load();
    if (cur <= 0.0f || delta == 0)
        return false;

    const int next = bpmOctave.load() + delta;
    if (next < -2 || next > 2)
        return false;

    const double shifted = (double) cur * std::pow (2.0, (double) delta);
    return shifted >= 30.0 && shifted <= 400.0;   // same limits as makeBeatBounds, otherwise silent GRID fallback
}

void SampleGrabberProcessor::shiftBpmOctave (int delta)
{
    if (! canShiftBpmOctave (delta))
        return;

    const float cur = bpmValue.load();
    const int before = bpmOctave.load();
    const int next = before + delta;

    bpmOctave.store (next);
    bpmValue.store (cur * std::pow (2.0f, (float) (next - before)));
    if (getSliceMode() == SliceMode::beat)
        recomputeSlices();   // BEAT depends on the tempo
}

void SampleGrabberProcessor::analyse()
{
    auto s = getSnapshot();
    if (s == nullptr)
    {
        bpmValue = 0.0f;
        keyValue = -1;
        keyConfidence = 0.0f;
        return;
    }

    const double sr = fileRate.load();
    const auto reg = getRegion (s->getNumSamples());

    if (kDebugDump)
    {
        static bool stampPrinted = false;
        if (! stampPrinted)
        {
            stampPrinted = true;
            std::fprintf (stderr, "[SG] Build %s %s\n", __DATE__, __TIME__);   // shows which build is actually running
        }
    }

    // BPM: at most 60 s from the middle of the selection
    int h0 = reg.first / fluxHop;
    int h1 = juce::jmin (reg.second / fluxHop, (int) fluxCache.size());
    const int maxFrames = (int) (60.0 * sr / (double) fluxHop);
    if (h1 - h0 > maxFrames)
    {
        h0 += (h1 - h0 - maxFrames) / 2;
        h1 = h0 + maxFrames;
    }
    if (kDebugDump)
        std::fprintf (stderr, "[SG] analyse: %s, selection %.1f-%.1f s of %.1f s, SR %.0f\n",
                      loadedFile.getFileName().toRawUTF8(), (double) reg.first / sr, (double) reg.second / sr,
                      (double) s->getNumSamples() / sr, sr);

    // Short selection: the tempo is a property of the whole track. It is therefore determined from the middle of the
    // whole file (many more periods -> more stable and accurate than from a few seconds).
    if (kGlobalBpmForShortSelection && (reg.second - reg.first) < (int) (45.0 * sr))
    {
        h0 = 0;
        h1 = (int) fluxCache.size();
        if (h1 - h0 > maxFrames)
        {
            h0 += (h1 - h0 - maxFrames) / 2;
            h1 = h0 + maxFrames;
        }
    }

    float bpm = estimateBpm (fluxCache, h0, h1, sr);
    if (kUseFineBpm && bpm > 0.0f)
    {
        const int f0 = h0 * fluxHop, f1 = h1 * fluxHop;
        const auto fine = refineBpm (*s, f0, f1, sr, (double) bpm);
        if (fine.bpm > 0.0)
        {
            if (kDebugDump)
            {
                std::fprintf (stderr, "[SG-BPM] fine: coarse %.2f -> fine %.3f BPM (%+.2f %%), score %.5f, at coarse value %.5f\n",
                              (double) bpm, fine.bpm, 100.0 * (fine.bpm / (double) bpm - 1.0), fine.score, fine.scoreAtStart);
                for (double f : { 0.5, 2.0 })
                {
                    const double ob = (double) bpm * f;
                    if (ob < 60.0 || ob > 250.0)
                        continue;
                    const auto o = refineBpm (*s, f0, f1, sr, ob);
                    if (o.bpm > 0.0)
                        std::fprintf (stderr, "[SG-BPM]   fine octave x%.1f: %.3f BPM, score %.5f\n", f, o.bpm, o.score);
                }
            }
            bpm = (float) fine.bpm;
        }
    }
    const float shown = bpm > 0.0f ? bpm * std::pow (2.0f, (float) bpmOctave.load()) : 0.0f;
    bpmValue = shown;
    if (getSliceMode() == SliceMode::beat)
        recomputeSlices();   // BEAT depends on the tempo
    if (kDebugDump)
        std::fprintf (stderr, "[SG-BPM] display: %.3f BPM (octave %+d)\n", (double) shown, bpmOctave.load());

    int key = -1;
    float conf = 0.0f;
    estimateKey (*s, reg.first, reg.second, sr, key, conf);
    keyValue = key;
    keyConfidence = conf;
}

//==============================================================================
void SampleGrabberProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    std::shared_ptr<juce::AudioBuffer<float>> buf;
    std::shared_ptr<RenderedAudio> rnd;
    std::array<int, numPads + 1> bounds {};
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        buf = sample;
        rnd = rendered;
        bounds = sliceBounds;
    }

    int trigger = pendingTrigger.exchange (-1);
    const bool stopNow = stopRequested.exchange (false);
    const bool pauseNow = pauseRequested.exchange (false);
    const bool retriggerNow = retriggerRequested.exchange (false);   // always consume, even if it is discarded below

    // TODO: sample-accurate triggers (currently: start at block start)
    // Raw bytes instead of meta.getMessage(): MidiMessage may allocate for SysEx. Note-on with velocity 0 = note-off (like isNoteOn()).
    for (const auto meta : midi)
    {
        if (meta.numBytes >= 3 && (meta.data[0] & 0xf0) == 0x90 && meta.data[2] != 0)
        {
            const int pad = (int) meta.data[1] - firstPadNote;
            if (pad >= 0 && pad < numPads)
                trigger = pad;
        }
    }

    // Gain target per block; smoothing only while a voice is audibly running (otherwise jump straight to the target)
    const float targetGain = gainFromDb (gainParam->load());

    if (buf == nullptr)
    {
        voicePlaying = false;
        activePad = -1;
        playheadNorm = -1.0f;
        gainSmoothed.setCurrentAndTargetValue (targetGain);
        return;
    }

    const int len = buf->getNumSamples();

    // Selection changed (mouse released): restart running playback with the new bounds.
    // Paused/stopped: do nothing. A trigger from this block (MIDI/pad) takes priority.
    // Stop/pause in this block or a running stop/pause fade-out also take priority (otherwise the trigger block would cancel them).
    if (retriggerNow && ! stopNow && ! pauseNow && rampThen == RampThen::none
        && voicePlaying && ! paused && voiceTrigger >= 0 && trigger < 0)
    {
        const bool emptyPad = voiceTrigger < numPads && bounds[(size_t) voiceTrigger + 1] - bounds[(size_t) voiceTrigger] < 2;
        if (emptyPad)
        {
            rampDir = -1;   // the pad has no onset in the new layout -> fade out and end
            rampThen = RampThen::stop;
        }
        else
        {
            trigger = voiceTrigger;
        }
    }
    if (stopNow && voicePlaying)
    {
        if (paused)
        {
            voicePlaying = false;   // end immediately from pause
            paused = false;
            pausedFlag = false;
        }
        else
        {
            rampDir = -1;           // short fade-out (5 ms) instead of a hard cut
            rampThen = RampThen::stop;
        }
    }
    else if (pauseNow && voicePlaying && ! paused && rampThen != RampThen::stop)
    {
        rampDir = -1;
        rampThen = RampThen::pause;
    }

    // PLAY from pause: resume instead of restarting
    if (trigger == fullRegionPad && paused && voicePlaying)
    {
        paused = false;
        pausedFlag = false;
        rampGain = 0.0f;
        rampDir = +1;
        rampThen = RampThen::none;
        trigger = -1;
    }

    // Empty pad (no onset for it): do nothing so running playback is not interrupted
    if (trigger >= 0 && trigger < numPads && bounds[(size_t) trigger + 1] - bounds[(size_t) trigger] < 2)
        trigger = -1;

    if (trigger >= 0 && trigger <= numPads)
    {
        const bool whole = trigger >= numPads;
        const int srcStart = juce::jlimit (0, len - 1, whole ? bounds.front() : bounds[(size_t) trigger]);
        const int srcEnd   = juce::jlimit (0, len,     whole ? bounds.back()  : bounds[(size_t) trigger + 1]);

        const bool neutral = std::abs (pitchParam->load()) < 0.005f && std::abs (speedParam->load() - 100.0f) < 0.05f;

        voiceUsesRender = false;
        voiceRender.reset();

        if (! neutral && rnd != nullptr && rnd->regionStart == bounds.front() && rnd->regionEnd == bounds.back())
        {
            voiceRender = rnd;
            voiceUsesRender = true;

            const int rl = rnd->buffer.getNumSamples();
            auto mapPos = [&] (int p)
            {
                return juce::jlimit (0, rl, (int) std::lround ((double) (p - rnd->regionStart) / rnd->speed));
            };
            voiceStart = mapPos (srcStart);
            voiceEnd = mapPos (srcEnd);
        }
        else
        {
            voiceStart = srcStart;
            voiceEnd = srcEnd;
        }

        voicePos = (double) voiceStart;
        voicePlaying = (voiceEnd - voiceStart) > 1;
        voiceWrapped = false;   // click guard at the start only in the first pass
        gainSmoothed.setCurrentAndTargetValue (targetGain);   // new voice starts without a ramp from the old value
        paused = false;
        pausedFlag = false;
        rampGain = 1.0f;
        rampDir = 0;
        rampThen = RampThen::none;
        xfadeLeft = 0;
        xfadeRender.reset();
        voiceSrcStart = srcStart;
        voiceSrcEnd = srcEnd;
        voiceTrigger = trigger;
        activePad = (voicePlaying && ! whole) ? trigger : -1;
    }

    if (! voicePlaying)
    {
        activePad = -1;
        playheadNorm = -1.0f;
        voiceUsesRender = false;
        voiceRender.reset();   // only decrement the count, release happens in the graveyard (message thread)
        gainSmoothed.setCurrentAndTargetValue (targetGain);
        return;
    }

    if (paused)
    {
        pausedFlag = true;   // frozen: position and playhead stay put
        gainSmoothed.setCurrentAndTargetValue (targetGain);   // resuming fades in via rampGain anyway
        return;
    }

    gainSmoothed.setTargetValue (targetGain);

    // Pitch/speed change during playback: take the new render and continue at the same position
    {
        const bool neutralNow = std::abs (pitchParam->load()) < 0.005f && std::abs (speedParam->load() - 100.0f) < 0.05f;
        bool wantRender = voiceUsesRender;
        std::shared_ptr<RenderedAudio> wanted = voiceRender;

        if (neutralNow)
        {
            wantRender = false;
            wanted = nullptr;
        }
        else if (rnd != nullptr && rnd->regionStart == bounds.front() && rnd->regionEnd == bounds.back())
        {
            wantRender = true;
            wanted = rnd;
        }

        if (wantRender != voiceUsesRender || (wantRender && wanted != voiceRender))
        {
            const double curSrc = voiceUsesRender && voiceRender != nullptr
                                      ? (double) voiceRender->regionStart + voicePos * voiceRender->speed
                                      : voicePos;

            xfadeRender = voiceRender;
            xfadeFromOriginal = ! voiceUsesRender;
            xfadePos = voicePos;
            xfadeEnd = voiceEnd;
            xfadeTotal = juce::jmax (1, (int) (0.012 * hostRate));
            xfadeLeft = xfadeTotal;

            voiceRender = wanted;
            voiceUsesRender = wantRender;

            if (voiceUsesRender && voiceRender != nullptr)
            {
                const int rl = voiceRender->buffer.getNumSamples();
                auto mapPos = [&] (double p)
                {
                    return juce::jlimit (0, rl, (int) std::lround ((p - (double) voiceRender->regionStart) / voiceRender->speed));
                };
                voiceStart = mapPos ((double) voiceSrcStart);
                voiceEnd = mapPos ((double) voiceSrcEnd);
                voicePos = juce::jlimit ((double) voiceStart, (double) juce::jmax (voiceStart, voiceEnd - 1),
                                         (curSrc - (double) voiceRender->regionStart) / voiceRender->speed);
            }
            else
            {
                voiceStart = voiceSrcStart;
                voiceEnd = voiceSrcEnd;
                voicePos = juce::jlimit ((double) voiceStart, (double) juce::jmax (voiceStart, voiceEnd - 1), curSrc);
            }
        }
    }

    const juce::AudioBuffer<float>& src = (voiceUsesRender && voiceRender != nullptr) ? voiceRender->buffer : *buf;
    const int srcLen = src.getNumSamples();
    voiceEnd = juce::jmin (voiceEnd, srcLen);   // in case a shorter sample was loaded in between

    const double fr = fileRate.load();
    const double rate = fr / hostRate;   // pitch/speed are already baked into the pre-rendered material
    const bool loop = loopParam->load() > 0.5f;
    // Fades as fraction of the region in samples of the current source (original or render, both at fileRate)
    const double fadeInFrac  = (double) fadeInParam->load()  * 0.01;
    const double fadeOutFrac = (double) fadeOutParam->load() * 0.01;
    const double fadeExp     = fadeExponent ((double) fadeShapeParam->load());
    const double clickS      = clickGuardSeconds * fr;
    const double clickOutS   = loop ? 0.0 : clickS;   // no click guard at the loop wrap (seamless)
    const float rampStep = (float) (1.0 / juce::jmax (1.0, 0.005 * hostRate));
    const int srcCh = src.getNumChannels();
    const int numOut = buffer.getNumChannels();

    // Old source for the crossfade after a switch original <-> render
    const juce::AudioBuffer<float>* oldSrc = nullptr;
    if (xfadeLeft > 0)
    {
        if (xfadeFromOriginal)
            oldSrc = buf.get();
        else if (xfadeRender != nullptr)
            oldSrc = &xfadeRender->buffer;
    }
    const int oldEnd = oldSrc != nullptr ? juce::jmin (xfadeEnd, oldSrc->getNumSamples()) : 0;
    const int oldCh = oldSrc != nullptr ? oldSrc->getNumChannels() : 0;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        if (voicePos >= (double) voiceEnd)
        {
            if (loop && voiceEnd - voiceStart > 1)
            {
                // fmod instead of subtracting once: with rate > region length voicePos would otherwise lie further behind voiceEnd
                const double regLen = (double) (voiceEnd - voiceStart);
                voicePos = (double) voiceStart + std::fmod (voicePos - (double) voiceStart, regLen);
                if (voicePos < (double) voiceStart || voicePos >= (double) voiceEnd)
                    voicePos = (double) voiceStart;   // rounding at the boundary
                voiceWrapped = true;
            }
            else
            {
                voicePlaying = false;
                break;
            }
        }

        // Fade-in/out (%) in every pass, also in the loop. Click guard only at real boundaries:
        // start only in the first pass, end only without loop (loop wrap stays seamless)
        float env = fadeEnvelope (voicePos, (double) voiceStart, (double) voiceEnd, fadeInFrac, fadeOutFrac, fadeExp,
                                  voiceWrapped ? 0.0 : clickS, clickOutS)
                    * gainSmoothed.getNextValue();

        if (rampDir != 0)
        {
            rampGain += (float) rampDir * rampStep;

            if (rampDir < 0 && rampGain <= 0.0f)
            {
                rampGain = 0.0f;
                rampDir = 0;

                if (rampThen == RampThen::stop)
                {
                    voicePlaying = false;
                }
                else
                {
                    paused = true;
                    pausedFlag = true;
                }

                rampThen = RampThen::none;
                break;
            }

            if (rampDir > 0 && rampGain >= 1.0f)
            {
                rampGain = 1.0f;
                rampDir = 0;
            }
        }
        env *= rampGain;

        const float t = xfadeLeft > 0 ? 1.0f - (float) xfadeLeft / (float) xfadeTotal : 1.0f;

        for (int ch = 0; ch < numOut; ++ch)
        {
            float s = readInterpolated (src, juce::jmin (ch, srcCh - 1), voicePos, voiceEnd) * env;

            if (oldSrc != nullptr && xfadeLeft > 0)
            {
                const float o = (xfadePos >= 0.0 && xfadePos < (double) oldEnd - 1.0)
                                    ? readInterpolated (*oldSrc, juce::jmin (ch, oldCh - 1), xfadePos, oldEnd) * env
                                    : 0.0f;
                s = s * t + o * (1.0f - t);
            }

            buffer.addSample (ch, i, s);
        }

        voicePos += rate;
        if (xfadeLeft > 0)
        {
            --xfadeLeft;
            xfadePos += rate;
        }
    }

    if (xfadeLeft <= 0)
        xfadeRender.reset();   // release the old source as soon as the crossfade is done

    if (voicePlaying)
    {
        const double srcPos = voiceUsesRender && voiceRender != nullptr
                                  ? (double) voiceRender->regionStart + voicePos * voiceRender->speed
                                  : voicePos;
        playheadNorm = (float) (srcPos / (double) len);
    }
    else
    {
        playheadNorm = -1.0f;
        activePad = -1;
        rampDir = 0;
        rampGain = 1.0f;
        rampThen = RampThen::none;
        xfadeLeft = 0;
        xfadeRender.reset();
        voiceUsesRender = false;
        voiceRender.reset();   // a replaced render should not stay in RAM after stop until the next pad
    }
}

//==============================================================================
juce::File SampleGrabberProcessor::exportRegion (int pad)
{
    std::shared_ptr<juce::AudioBuffer<float>> buf;
    std::shared_ptr<RenderedAudio> rnd;
    std::array<int, numPads + 1> bounds {};
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        buf = sample;
        rnd = rendered;
        bounds = sliceBounds;
    }
    if (buf == nullptr)
        return {};

    const int len = buf->getNumSamples();
    const bool whole = (pad < 0 || pad >= numPads);
    const int srcStart = juce::jlimit (0, len - 1, whole ? bounds.front() : bounds[(size_t) pad]);
    const int srcEnd   = juce::jlimit (0, len,     whole ? bounds.back()  : bounds[(size_t) pad + 1]);
    if (srcEnd - srcStart < 2)
        return {};

    const double fr = fileRate.load();
    const float pitch = pitchParam->load();
    const double speed = (double) speedParam->load() / 100.0;
    const bool neutral = std::abs (pitch) < 0.005f && std::abs (speed - 1.0) < 0.0005;

    // Pitch/speed: use the existing render, otherwise compute synchronously now
    std::shared_ptr<RenderedAudio> r;
    if (! neutral)
    {
        if (rnd != nullptr && rnd->regionStart == bounds.front() && rnd->regionEnd == bounds.back())
            r = rnd;
        else if (bounds.back() - bounds.front() >= 2048)
            r = renderStretched (*buf, bounds.front(), bounds.back(), fr, pitch, speed, {});
    }

    const juce::AudioBuffer<float>& src = r != nullptr ? r->buffer : *buf;
    int s = srcStart, e = srcEnd;
    if (r != nullptr)
    {
        const int rl = r->buffer.getNumSamples();
        auto mapPos = [&] (int p) { return juce::jlimit (0, rl, (int) std::lround ((double) (p - r->regionStart) / r->speed)); };
        s = mapPos (srcStart);
        e = mapPos (srcEnd);
    }
    if (e - s < 2)
        return {};

    const float gain = gainFromDb (gainParam->load());   // constant (no smoothing), silent at the left stop
    // Same envelope as in processBlock (same region in source or render space)
    const double fadeInFrac  = (double) fadeInParam->load()  * 0.01;
    const double fadeOutFrac = (double) fadeOutParam->load() * 0.01;
    const double fadeExp     = fadeExponent ((double) fadeShapeParam->load());
    const double clickS      = clickGuardSeconds * fr;   // export: click guard at both ends

    juce::AudioBuffer<float> out (src.getNumChannels(), e - s);
    for (int i = 0; i < e - s; ++i)
    {
        const float env = fadeEnvelope ((double) (s + i), (double) s, (double) e, fadeInFrac, fadeOutFrac, fadeExp, clickS, clickS) * gain;
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.setSample (ch, i, src.getSample (ch, s + i) * env);
    }

    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("SampleGrabber").getChildFile ("export");
    dir.createDirectory();

    const auto base = loadedFile.getFileNameWithoutExtension().substring (0, 40);
    const auto name = base + (whole ? juce::String ("_sel") : "_pad" + juce::String (pad + 1).paddedLeft ('0', 2)) + ".wav";
    auto file = dir.getChildFile (name);
    file.deleteFile();

    std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
    if (os == nullptr)
        return {};

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (os.get(), fr, (unsigned int) out.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return {};

    os.release();   // the writer now owns the stream
    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    writer.reset();

    return file;
}

//==============================================================================
void SampleGrabberProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("samplePath", loadedFile.getFullPathName(), nullptr);
    state.setProperty ("trimStart", (double) trimStart.load(), nullptr);
    state.setProperty ("trimEnd", (double) trimEnd.load(), nullptr);
    state.setProperty ("sliceMode", sliceMode.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SampleGrabberProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto state = juce::ValueTree::fromXml (*xml);
        if (! state.isValid())
            return;

        // Remove old ms fades (from before the % fades): APVTS ignores unknown IDs but they would be saved forever
        for (int i = state.getNumChildren(); --i >= 0;)
        {
            const auto id = state.getChild (i).getProperty ("id").toString();
            if (id == "fadeIn" || id == "fadeOut")
                state.removeChild (i, nullptr);
        }

        apvts.replaceState (state);
        sliceMode = (int) state.getProperty ("sliceMode", (int) SliceMode::transient);

        const juce::File f (state.getProperty ("samplePath").toString());
        if (f.existsAsFile() && loadSample (f, false))
        {
            history.clear();   // new plugin state: the old BACK stack no longer belongs to it
            setTrim ((float) (double) state.getProperty ("trimStart", 0.0),
                     (float) (double) state.getProperty ("trimEnd", 1.0));
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* SampleGrabberProcessor::createEditor()
{
    return new SampleGrabberEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SampleGrabberProcessor();
}
