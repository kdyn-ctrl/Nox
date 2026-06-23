#pragma once

#include <cmath>
#include <stdexcept>
#include <string>

namespace nox::options {

// ─── Enumerations ────────────────────────────────────────────────────────────

enum class OptionType { Call, Put };

// ─── Structures ──────────────────────────────────────────────────────────────

struct OptionContract {
    std::string symbol;           // e.g. "AAPL"
    double      strike;           // strike price K
    double      underlying;       // current spot price S
    double      expiry;           // time to expiration in years (T)
    double      risk_free_rate;   // continuously compounded risk-free rate r
    double      volatility;       // annualised implied volatility σ (initial estimate)
    OptionType  type;
};

struct OptionGreeks {
    double price;
    double delta;
    double gamma;
    double theta;              // expressed as daily decay (divided by 365)
    double vega;               // per 1% move in vol
    double rho;                // per 1% move in rate
    double implied_volatility; // solved IV if computed via pricing; otherwise σ from contract
};

// ─── Internal math helpers ───────────────────────────────────────────────────

namespace detail {

// Standard normal PDF
inline double norm_pdf(double x) noexcept {
    constexpr double inv_sqrt2pi = 0.3989422804014327;
    return inv_sqrt2pi * std::exp(-0.5 * x * x);
}

// Standard normal CDF — Abramowitz & Stegun 26.2.17 (max error ~7.5e-8)
inline double norm_cdf(double x) noexcept {
    constexpr double a1 =  0.319381530;
    constexpr double a2 = -0.356563782;
    constexpr double a3 =  1.781477937;
    constexpr double a4 = -1.821255978;
    constexpr double a5 =  1.330274429;
    constexpr double p  =  0.2316419;

    double t = 1.0 / (1.0 + p * std::abs(x));
    double poly = t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5))));
    double cdf  = 1.0 - norm_pdf(x) * poly;
    return (x >= 0.0) ? cdf : 1.0 - cdf;
}

// Compute d1 and d2 — shared across all Greeks
inline void compute_d1_d2(const OptionContract& c,
                          double sigma,
                          double& d1,
                          double& d2)
{
    if (c.expiry <= 0.0)
        throw std::domain_error("OptionEngine: expiry must be > 0");
    if (sigma <= 0.0)
        throw std::domain_error("OptionEngine: volatility must be > 0");
    if (c.strike <= 0.0 || c.underlying <= 0.0)
        throw std::domain_error("OptionEngine: strike and underlying must be > 0");

    const double sqrt_T = std::sqrt(c.expiry);
    d1 = (std::log(c.underlying / c.strike)
          + (c.risk_free_rate + 0.5 * sigma * sigma) * c.expiry)
         / (sigma * sqrt_T);
    d2 = d1 - sigma * sqrt_T;
}

} // namespace detail

// ─── Black-Scholes pricing ────────────────────────────────────────────────────

inline double bs_price(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);

    const double disc = std::exp(-c.risk_free_rate * c.expiry);

    if (c.type == OptionType::Call) {
        return c.underlying * detail::norm_cdf(d1)
               - c.strike   * disc * detail::norm_cdf(d2);
    } else {
        return c.strike   * disc * detail::norm_cdf(-d2)
               - c.underlying * detail::norm_cdf(-d1);
    }
}

// ─── Greeks ──────────────────────────────────────────────────────────────────

// Delta: ∂V/∂S
inline double bs_delta(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);
    if (c.type == OptionType::Call)
        return detail::norm_cdf(d1);
    else
        return detail::norm_cdf(d1) - 1.0;
}

// Gamma: ∂²V/∂S² — identical for calls and puts
inline double bs_gamma(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);
    return detail::norm_pdf(d1)
           / (c.underlying * sigma * std::sqrt(c.expiry));
}

// Theta: -∂V/∂T, returned as daily decay (÷365)
inline double bs_theta(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);

    const double sqrt_T  = std::sqrt(c.expiry);
    const double disc    = std::exp(-c.risk_free_rate * c.expiry);
    const double pdf_d1  = detail::norm_pdf(d1);

    double annual_theta;
    if (c.type == OptionType::Call) {
        annual_theta =
            -(c.underlying * pdf_d1 * sigma) / (2.0 * sqrt_T)
            - c.risk_free_rate * c.strike * disc * detail::norm_cdf(d2);
    } else {
        annual_theta =
            -(c.underlying * pdf_d1 * sigma) / (2.0 * sqrt_T)
            + c.risk_free_rate * c.strike * disc * detail::norm_cdf(-d2);
    }
    return annual_theta / 365.0;
}

// Vega: ∂V/∂σ — identical for calls and puts; scaled to 1% vol move
inline double bs_vega(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);
    return c.underlying * detail::norm_pdf(d1) * std::sqrt(c.expiry) * 0.01;
}

// Rho: ∂V/∂r, scaled to 1% rate move
inline double bs_rho(const OptionContract& c, double sigma) {
    double d1, d2;
    detail::compute_d1_d2(c, sigma, d1, d2);

    const double disc = std::exp(-c.risk_free_rate * c.expiry);
    if (c.type == OptionType::Call) {
        return c.strike * c.expiry * disc * detail::norm_cdf(d2)  * 0.01;
    } else {
        return -c.strike * c.expiry * disc * detail::norm_cdf(-d2) * 0.01;
    }
}

// ─── Implied Volatility (Newton-Raphson) ─────────────────────────────────────

// Solves σ such that bs_price(c, σ) == market_price.
// Returns NaN if the solver fails to converge.
inline double implied_volatility(const OptionContract& c,
                                 double market_price,
                                 double sigma_init  = 0.20,
                                 int    max_iters   = 100,
                                 double tolerance   = 1e-7)
{
    double sigma = sigma_init;
    for (int i = 0; i < max_iters; ++i) {
        double price = bs_price(c, sigma);
        double diff  = price - market_price;
        if (std::abs(diff) < tolerance)
            return sigma;

        // Vega as raw ∂V/∂σ (not the 1%-scaled version)
        double d1, d2;
        detail::compute_d1_d2(c, sigma, d1, d2);
        double vega_raw = c.underlying * detail::norm_pdf(d1) * std::sqrt(c.expiry);

        if (std::abs(vega_raw) < 1e-12)
            return std::numeric_limits<double>::quiet_NaN();

        sigma -= diff / vega_raw;
        if (sigma <= 0.0) sigma = 1e-6;  // keep σ positive
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// ─── Full Greeks snapshot ────────────────────────────────────────────────────

// Computes the complete Greeks bundle for a contract.
// If solve_iv is true, the contract's volatility field is treated as a
// market price and IV is solved first; otherwise it is used directly as σ.
inline OptionGreeks compute_greeks(const OptionContract& c, bool solve_iv = false) {
    double sigma = c.volatility;

    if (solve_iv) {
        sigma = implied_volatility(c, c.volatility);
        if (std::isnan(sigma))
            throw std::runtime_error("OptionEngine: IV solver did not converge");
    }

    OptionGreeks g{};
    g.price             = bs_price(c, sigma);
    g.delta             = bs_delta(c, sigma);
    g.gamma             = bs_gamma(c, sigma);
    g.theta             = bs_theta(c, sigma);
    g.vega              = bs_vega(c, sigma);
    g.rho               = bs_rho(c, sigma);
    g.implied_volatility = sigma;
    return g;
}

} // namespace nox::options
