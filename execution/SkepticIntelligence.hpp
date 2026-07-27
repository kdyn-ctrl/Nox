#ifndef SKEPTIC_INTELLIGENCE_HPP
#define SKEPTIC_INTELLIGENCE_HPP

// SkepticIntelligence — the decision layer that finally makes the Skeptic
// engine's non-contradiction workstreams influence real trades.
//
// Before this, only WS1 (the Contradiction Vector) fed sizing — it halved the
// Kelly budget on a sentiment-vs-skew contradiction (see
// OptionsSignalGenerator::fetchWS1KellyMultiplier). WS2 (alt-macro / physical
// supply) and WS3 (insider Form 4 clusters) were computed by the
// america-data-engine and exposed on /macro/alt and /insider/clusters, but
// NOTHING in execution consumed them — so the Skeptic "hadn't done anything"
// beyond the weekend report. This header closes that gap, and adds the new
// China information-lag feed (/china/lag) as a first-class conviction input.
//
// DESIGN: this file is PURE — no HTTP, no JSON, no I/O — the same testable
// shape as PortfolioRiskManager.hpp. OptionsSignalGenerator does the network
// fetch + parse into these plain structs and calls decide(); the aggregation
// math lives here so it can be unit-tested without a broker or a live data
// engine (test/test_skeptic_intelligence.cpp).
//
// It composes MULTIPLICATIVELY with the existing WS1 Kelly cut rather than
// replacing it — WS1 keeps owning contradiction; this owns insider + alt-macro
// + china. Fail-open is the caller's job: a dead feed parses to an empty
// Inputs, which decide() maps to size_mult=1.0, suppress=false (a no-op).
//
// Everything tunable is env-sourced with a fake-safe default (per CLAUDE.md's
// "hardcode nothing tunable" rule) — see Knobs::fromEnv().

#include <algorithm>
#include <cstdlib>
#include <string>

namespace nox::skeptic {

enum class Dir { Bullish, Bearish, Neutral };

// WS3 — insider Form 4 buy-cluster for this ticker (from /insider/clusters).
struct InsiderInput {
    bool has_cluster   = false;
    int  insider_count = 0;
};

// WS2 — alt-macro physical-supply verdict, already resolved to whether it
// applies to THIS ticker (the caller matched the ticker against a region's
// tickers[] list) and which oil-price direction the physical data implies.
struct AltMacroInput {
    bool applies                    = false;
    Dir  bias                       = Dir::Neutral; // supply-constrained=Bullish, released=Bearish
    double strength                 = 0.0;          // |physical_stress|, 0..1
    bool text_contradicts_physical  = false;        // headline disagreed with the ships
};

// China information-lag verdict for this ticker (from /china/lag). `fresh`
// means the driving macro release is still inside the lag window the US
// session is presumed not to have fully priced yet — the actual edge.
struct ChinaLagInput {
    bool applies    = false;
    Dir  bias       = Dir::Neutral;
    double strength = 0.0;   // 0..1 conviction from the release magnitude
    bool fresh      = false; // release still within the unpriced lag window
    std::string release;     // e.g. "caixin_pmi" — for the audit detail line
};

struct Inputs {
    InsiderInput  insider;
    AltMacroInput alt_macro;
    ChinaLagInput china;
};

struct Knobs {
    // Insider (WS3): a genuine officer/director buy-cluster is a conviction
    // signal. Boosts an aligned (bullish) trade, cuts a directly-opposed
    // (bearish) one — insiders buying while we're short is exactly the kind of
    // disagreement the Skeptic exists to flag.
    double insider_boost         = 1.25;
    int    insider_min_execs     = 2;    // need >= this many distinct insiders to act
    double insider_conflict_cut  = 0.75;

    // Alt-macro (WS2): physical supply direction. Aligned → modest boost;
    // opposed → cut, harder when the headline contradicts the physical data.
    double altmacro_align_boost  = 1.15;
    double altmacro_oppose_cut   = 0.60;
    double altmacro_contradict_cut = 0.45; // opposed AND text_contradicts_physical

    // China lag: the information-lag edge. A fresh, unpriced release aligned
    // with the trade is the highest-conviction input here; opposed is the
    // strongest cut (we'd be trading into a move the tape hasn't shown yet).
    // RULE-D5 (2026-07-17 audit burndown Track 3, H3): WS8's premise —
    // surprise-vs-consensus direction and "unpriced by the US session" — is
    // unmeasured (level-vs-50 only, no consensus figure; the WS7 media-lag
    // proxy only matches BABA). Boost power stays OFF by default until that
    // premise can be measured; the feed still logs every verdict either way.
    double china_align_boost     = 1.30;
    double china_fresh_extra     = 1.15; // extra factor when the release is still fresh
    bool   china_boost_enabled   = false;
    // RULE-D4 (H2): a STALE release must never move sizing at all, boost or
    // cut — only a `fresh` release (still inside the unpriced lag window) may
    // touch `mult`. A stale-but-aligned/opposed verdict is logged only.
    double china_oppose_cut      = 0.50;

    // Combined clamp + suppression.
    double size_mult_min         = 0.40;
    double size_mult_max         = 1.60;
    bool   suppress_enabled      = true;
    // Suppress a new entry only when the combined multiplier collapses at or
    // below this AND at least one feed is a HARD opposition (china-fresh-opposed
    // or alt-macro text-contradicts-physical-opposed). A merely soft cut never
    // suppresses — it just sizes down, per the signal-driven philosophy.
    double suppress_threshold    = 0.50;

    static double envd(const char* name, double dflt) {
        if (const char* v = std::getenv(name)) { try { return std::stod(v); } catch (...) {} }
        return dflt;
    }
    static int envi(const char* name, int dflt) {
        if (const char* v = std::getenv(name)) { try { return std::stoi(v); } catch (...) {} }
        return dflt;
    }
    static bool envb(const char* name, bool dflt) {
        if (const char* v = std::getenv(name)) {
            std::string s(v);
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s == "true" || s == "1" || s == "yes";
        }
        return dflt;
    }

    static Knobs fromEnv() {
        Knobs k;
        k.insider_boost          = envd("SKEPTIC_INSIDER_BOOST",          k.insider_boost);
        k.insider_min_execs      = envi("SKEPTIC_INSIDER_MIN_EXECS",      k.insider_min_execs);
        k.insider_conflict_cut   = envd("SKEPTIC_INSIDER_CONFLICT_CUT",   k.insider_conflict_cut);
        k.altmacro_align_boost   = envd("SKEPTIC_ALTMACRO_ALIGN_BOOST",   k.altmacro_align_boost);
        k.altmacro_oppose_cut    = envd("SKEPTIC_ALTMACRO_OPPOSE_CUT",    k.altmacro_oppose_cut);
        k.altmacro_contradict_cut= envd("SKEPTIC_ALTMACRO_CONTRADICT_CUT",k.altmacro_contradict_cut);
        k.china_align_boost      = envd("SKEPTIC_CHINA_ALIGN_BOOST",      k.china_align_boost);
        k.china_fresh_extra      = envd("SKEPTIC_CHINA_FRESH_EXTRA",      k.china_fresh_extra);
        k.china_boost_enabled    = envb("SKEPTIC_CHINA_BOOST_ENABLED",    k.china_boost_enabled);
        k.china_oppose_cut       = envd("SKEPTIC_CHINA_OPPOSE_CUT",       k.china_oppose_cut);
        k.size_mult_min          = envd("SKEPTIC_SIZE_MULT_MIN",          k.size_mult_min);
        k.size_mult_max          = envd("SKEPTIC_SIZE_MULT_MAX",          k.size_mult_max);
        k.suppress_enabled       = envb("SKEPTIC_SUPPRESS_ENABLED",       k.suppress_enabled);
        k.suppress_threshold     = envd("SKEPTIC_SUPPRESS_THRESHOLD",     k.suppress_threshold);
        return k;
    }
};

struct Decision {
    double      size_mult = 1.0;
    bool        suppress  = false;
    std::string reason;   // short slug for signal_events (e.g. "skeptic_boost", "suppressed_skeptic")
    std::string detail;   // human-readable breakdown for the log/alert
};

namespace detail {
inline bool aligned(Dir signal, Dir feed) {
    return feed != Dir::Neutral && signal == feed;
}
inline bool opposed(Dir signal, Dir feed) {
    return feed != Dir::Neutral && signal != Dir::Neutral && signal != feed;
}
} // namespace detail

// Pure aggregation. `signal_dir` is the trade's directional bias
// (STRADDLE/STRANGLE/REVERSE_IRON_CONDOR are non-directional → pass Neutral,
// which makes every align/oppose test false so only the neutral-safe insider
// magnitude can nudge sizing, never suppress).
inline Decision decide(Dir signal_dir, const Inputs& in, const Knobs& k) {
    Decision d;
    double mult = 1.0;
    std::string detail;
    bool hard_opposition = false;

    // ── WS3 insider ────────────────────────────────────────────────────────
    if (in.insider.has_cluster && in.insider.insider_count >= k.insider_min_execs) {
        if (signal_dir == Dir::Bullish) {
            mult *= k.insider_boost;
            detail += "insider_cluster_buy(+" + std::to_string(in.insider.insider_count) + ")→boost; ";
        } else if (signal_dir == Dir::Bearish) {
            mult *= k.insider_conflict_cut;
            detail += "insider_cluster_buy_vs_short→cut; ";
        }
        // Neutral (vol play): insiders buying is directional info a straddle
        // can't use — leave sizing unchanged, just note it.
        else {
            detail += "insider_cluster_buy(neutral_trade,no_size_change); ";
        }
    }

    // ── WS2 alt-macro physical supply ────────────────────────────────────────
    if (in.alt_macro.applies && in.alt_macro.bias != Dir::Neutral) {
        if (detail::aligned(signal_dir, in.alt_macro.bias)) {
            mult *= k.altmacro_align_boost;
            detail += "altmacro_aligned→boost; ";
        } else if (detail::opposed(signal_dir, in.alt_macro.bias)) {
            if (in.alt_macro.text_contradicts_physical) {
                mult *= k.altmacro_contradict_cut;
                hard_opposition = true;
                detail += "altmacro_text_contradicts_physical_opposed→hard_cut; ";
            } else {
                mult *= k.altmacro_oppose_cut;
                detail += "altmacro_opposed→cut; ";
            }
        }
    }

    // ── China information-lag ────────────────────────────────────────────────
    // RULE-D4: a STALE release never touches `mult` (boost or cut) — only a
    // FRESH one may. RULE-D5: the aligned/boost side additionally stays a
    // logging-only no-op unless SKEPTIC_CHINA_BOOST_ENABLED=true, since WS8's
    // "unpriced surprise" premise is unmeasured.
    if (in.china.applies && in.china.bias != Dir::Neutral) {
        if (detail::aligned(signal_dir, in.china.bias)) {
            if (in.china.fresh && k.china_boost_enabled) {
                mult *= k.china_align_boost * k.china_fresh_extra;
                detail += "china_lag_aligned_FRESH(" + in.china.release + ")→boost; ";
            } else {
                detail += "china_lag_aligned(" + in.china.release +
                          (in.china.fresh ? ",FRESH" : ",STALE") +
                          ")→logged_only(no_size_change); ";
            }
        } else if (detail::opposed(signal_dir, in.china.bias)) {
            if (in.china.fresh) {
                mult *= k.china_oppose_cut;
                hard_opposition = true; // fresh unpriced move against us
                detail += "china_lag_opposed_FRESH(" + in.china.release + ")→cut; ";
            } else {
                detail += "china_lag_opposed_STALE(" + in.china.release + ")→logged_only(no_size_change); ";
            }
        }
    }

    // Clamp before the suppression test so the threshold compares against the
    // same number that would actually size the trade.
    double clamped = std::max(k.size_mult_min, std::min(k.size_mult_max, mult));

    if (k.suppress_enabled && hard_opposition && clamped <= k.suppress_threshold) {
        d.suppress  = true;
        d.size_mult = clamped;
        d.reason    = "suppressed_skeptic";
        d.detail    = detail.empty() ? "no signals" : detail;
        return d;
    }

    d.size_mult = clamped;
    d.suppress  = false;
    if (detail.empty()) {
        d.reason = "skeptic_neutral";
        d.detail = "no actionable skeptic signals";
    } else {
        d.reason = (clamped > 1.0 + 1e-9) ? "skeptic_boost"
                 : (clamped < 1.0 - 1e-9) ? "skeptic_cut"
                 : "skeptic_neutral";
        d.detail = detail;
    }
    return d;
}

} // namespace nox::skeptic

#endif // SKEPTIC_INTELLIGENCE_HPP
