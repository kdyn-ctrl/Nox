#pragma once

// =============================================================================
// csv_parser.hpp — Historical Data Ingestion & Indicator Pre-computation
// =============================================================================
// Responsibilities:
//   1. Parse a combined SPY + VIX daily CSV file from disk.
//   2. Validate every row for data integrity (zero prices, low > high, etc.).
//   3. Run three sequential computation passes to populate ATR14, SMA200, RSI14.
//
// Expected CSV column order (no extra spaces, header row required):
//   Date,High,Low,Close,Volume,VIX_Close
//
// Volume is optional — if the column is missing or zero the volume MA
// will be left at 0.0 and the volume gate will be automatically disabled.
//
// All computed fields are populated in-place on the loaded vector.
// Rows that fail integrity checks are marked valid=false and are skipped
// by the simulation loop — they are NOT removed so that index alignment
// is preserved for rolling-window calculations.
// =============================================================================

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>

// -----------------------------------------------------------------------------
// DailyDataPoint
// Represents one calendar day of market data after all indicator passes.
// -----------------------------------------------------------------------------
struct DailyDataPoint {
    std::string date;       // "YYYY-MM-DD" — used for date-range filtering
    double high       = 0.0;
    double low        = 0.0;
    double close      = 0.0;
    double vix_close  = 0.0;
    double volume    = 0.0;  // Daily share volume (0 if not present in CSV)

    // --- Computed by the three indicator passes ---
    double atr14   = 0.0;   // 14-period Wilder ATR
    double sma200  = 0.0;   // 200-period Simple Moving Average of Close
    double rsi14   = 50.0;  // 14-period Wilder RSI (default 50 = neutral)
    double vol_ma50 = 0.0;  // 50-period Simple Moving Average of Volume

    // Row integrity flag — set to false on any parse or sanity failure.
    // The simulation loop must skip rows where valid == false.
    bool valid = false;
};

// -----------------------------------------------------------------------------
// CSVParser
// Static utility class — call CSVParser::load(filepath) to get the full
// processed dataset ready for the simulation loop.
// -----------------------------------------------------------------------------
class CSVParser {
public:

    // -------------------------------------------------------------------------
    // load()
    // Entry point. Parses the file, validates each row, then runs all three
    // indicator computation passes before returning the completed dataset.
    // Throws std::runtime_error on file-open failure or empty dataset.
    // -------------------------------------------------------------------------
    static std::vector<DailyDataPoint> load(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("[PARSER] Cannot open file: " + filepath);
        }

        std::vector<DailyDataPoint> data;
        std::string line;
        int line_num = 0;

        // Skip header row
        std::getline(file, line);
        ++line_num;

        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }

            DailyDataPoint point;
            std::stringstream ss(line);
            std::string token;

            try {
                // Column 1: Date — normalize M/D/YYYY or MM/DD/YYYY → YYYY-MM-DD
                // so that lexicographic comparison works correctly for date filtering.
                std::string raw_date;
                std::getline(ss, raw_date, ',');
                point.date = normalize_date(raw_date);

                // Column 2: High
                std::getline(ss, token, ',');
                point.high = std::stod(token);

                // Column 3: Low
                std::getline(ss, token, ',');
                point.low = std::stod(token);

                // Column 4: Close (Adjusted)
                std::getline(ss, token, ',');
                point.close = std::stod(token);

                // Column 5: Volume (optional) OR VIX Close
                // Detect whether a Volume column is present by checking if a 6th
                // token exists. If only 5 columns, treat col-5 as VIX directly.
                std::string col5;
                std::getline(ss, col5, ',');

                std::string col6;
                std::getline(ss, col6);
                col6.erase(col6.find_last_not_of(" \t\r\n") + 1);

                if (!col6.empty()) {
                    // 6-column format: Date,High,Low,Close,Volume,VIX_Close
                    point.volume    = std::stod(col5);
                    point.vix_close = std::stod(col6);
                } else {
                    // 5-column format: Date,High,Low,Close,VIX_Close (no volume)
                    col5.erase(col5.find_last_not_of(" \t\r\n") + 1);
                    point.volume    = 0.0;
                    point.vix_close = std::stod(col5);
                }

                // --- Integrity Checks ---
                if (point.close <= 0.0) {
                    std::cerr << "[PARSER][WARN] Line " << line_num
                              << " (" << point.date << "): SPY Close is zero or negative. Row skipped." << std::endl;
                    point.valid = false;
                } else if (point.high <= 0.0 || point.low <= 0.0) {
                    std::cerr << "[PARSER][WARN] Line " << line_num
                              << " (" << point.date << "): High or Low is zero or negative. Row skipped." << std::endl;
                    point.valid = false;
                } else if (point.low > point.high) {
                    std::cerr << "[PARSER][WARN] Line " << line_num
                              << " (" << point.date << "): Low > High (data corruption). Row skipped." << std::endl;
                    point.valid = false;
                } else if (point.vix_close <= 0.0) {
                    std::cerr << "[PARSER][WARN] Line " << line_num
                              << " (" << point.date << "): VIX_Close is zero or negative. Row skipped." << std::endl;
                    point.valid = false;
                } else {
                    point.valid = true;
                }

            } catch (const std::exception& e) {
                std::cerr << "[PARSER][WARN] Line " << line_num
                          << " parse error: " << e.what() << " — row skipped. Content: [" << line << "]" << std::endl;
                point.valid = false;
            }

            data.push_back(point);
        }

        if (data.empty()) {
            throw std::runtime_error("[PARSER] No data loaded from: " + filepath);
        }

        int valid_count = 0;
        for (const auto& d : data) if (d.valid) ++valid_count;

        std::cerr << "[PARSER] Loaded " << data.size() << " rows ("
                  << valid_count << " valid, "
                  << (data.size() - valid_count) << " skipped) from: "
                  << filepath << std::endl;

        if (valid_count < 250) {
            throw std::runtime_error("[PARSER] Insufficient valid rows (" +
                std::to_string(valid_count) + ") to compute SMA-200. Need at least 250.");
        }

        // Run the three indicator computation passes in order.
        // ATR must be computed before SMA and RSI since it's independent.
        // SMA and RSI are also independent of each other.
        compute_atr14(data);
        compute_sma200(data);
        compute_rsi14(data);
        compute_vol_ma50(data);

        return data;
    }

private:

    // -------------------------------------------------------------------------
    // normalize_date()
    // Converts M/D/YYYY or MM/DD/YYYY to YYYY-MM-DD so that all date-range
    // comparisons in the simulation loop work correctly via lexicographic sort.
    // If the string is already in YYYY-MM-DD format it is returned unchanged.
    // -------------------------------------------------------------------------
    static std::string normalize_date(const std::string& raw) {
        // Already ISO format (YYYY-MM-DD): first separator is at index 4
        if (raw.size() >= 8 && raw[4] == '-') {
            return raw;
        }

        // Expect M/D/YYYY or MM/DD/YYYY separated by '/'
        std::stringstream ss(raw);
        std::string month_s, day_s, year_s;
        if (!std::getline(ss, month_s, '/') ||
            !std::getline(ss, day_s,   '/') ||
            !std::getline(ss, year_s)) {
            return raw; // Unrecognized format — return as-is
        }

        int month = std::stoi(month_s);
        int day   = std::stoi(day_s);
        int year  = std::stoi(year_s);

        std::ostringstream out;
        out << std::setw(4) << std::setfill('0') << year  << '-'
            << std::setw(2) << std::setfill('0') << month << '-'
            << std::setw(2) << std::setfill('0') << day;
        return out.str();
    }

    // -------------------------------------------------------------------------
    // compute_atr14()
    // Wilder's 14-period Average True Range.
    //
    // True Range = max( High-Low, |High-PrevClose|, |Low-PrevClose| )
    // Seed:     Simple average of TR for first 14 periods.
    // Smoothed: ATR = ((ATR_prev * 13) + TR_current) / 14
    //
    // Days with valid == false carry forward the previous ATR so rolling
    // state is not broken for subsequent calculations.
    // -------------------------------------------------------------------------
    static void compute_atr14(std::vector<DailyDataPoint>& data) {
        const int period = 14;

        for (size_t i = 0; i < data.size(); ++i) {
            // Day 0 has no previous close — ATR is undefined.
            if (i == 0) {
                data[i].atr14 = 0.0;
                continue;
            }

            // If the current row is invalid, carry forward the previous ATR.
            if (!data[i].valid) {
                data[i].atr14 = data[i - 1].atr14;
                continue;
            }

            double prev_close = data[i - 1].close;
            double tr = std::max({ data[i].high - data[i].low,
                                   std::abs(data[i].high - prev_close),
                                   std::abs(data[i].low  - prev_close) });

            // Not enough history yet to seed the first average.
            if (i < static_cast<size_t>(period)) {
                data[i].atr14 = 0.0;
                continue;
            }

            // Seed: compute the simple average of TR for the first 14 bars.
            if (i == static_cast<size_t>(period)) {
                double sum = 0.0;
                for (int j = 1; j <= period; ++j) {
                    double pc = data[j - 1].close;
                    sum += std::max({ data[j].high - data[j].low,
                                      std::abs(data[j].high - pc),
                                      std::abs(data[j].low  - pc) });
                }
                data[i].atr14 = sum / period;
            } else {
                // Wilder's exponential smoothing for all subsequent bars.
                data[i].atr14 = ((data[i - 1].atr14 * (period - 1)) + tr) / period;
            }
        }

        std::cerr << "[PARSER] ATR14 computation complete." << std::endl;
    }

    // -------------------------------------------------------------------------
    // compute_sma200()
    // 200-period Simple Moving Average of the adjusted Close price.
    //
    // RULE-001 mirror: averages ONLY the last 200 bars — not all available bars.
    // Days where fewer than 200 valid closes precede them are left at 0.0.
    // The simulation loop uses sma200 == 0.0 as the warm-up guard signal.
    // -------------------------------------------------------------------------
    static void compute_sma200(std::vector<DailyDataPoint>& data) {
        const int period = 200;

        for (size_t i = 0; i < data.size(); ++i) {
            if (i < static_cast<size_t>(period - 1)) {
                data[i].sma200 = 0.0;
                continue;
            }

            double sum        = 0.0;
            int    valid_seen = 0;

            for (int j = 0; j < period; ++j) {
                const auto& row = data[i - j];
                if (row.valid) {
                    sum += row.close;
                    ++valid_seen;
                }
            }

            // If any of the 200 lookback rows were invalid, the SMA is
            // unreliable. Mark it 0.0 so the simulation warm-up guard
            // skips this day.
            data[i].sma200 = (valid_seen == period) ? (sum / period) : 0.0;
        }

        std::cerr << "[PARSER] SMA200 computation complete." << std::endl;
    }

    // -------------------------------------------------------------------------
    // compute_rsi14()
    // 14-period Relative Strength Index using Wilder's smoothing method.
    //
    // Seed:     Simple average of gains and losses for first 14 changes.
    // Smoothed: avg_gain = ((prev_avg_gain * 13) + current_gain) / 14
    //
    // Default value of 50.0 (neutral) is used before enough history exists.
    // -------------------------------------------------------------------------
    static void compute_rsi14(std::vector<DailyDataPoint>& data) {
        const int period = 14;

        if (data.size() < static_cast<size_t>(period + 1)) {
            std::cerr << "[PARSER][WARN] Not enough rows to compute RSI14." << std::endl;
            return;
        }

        // Seed: compute initial average gain and loss over first `period` changes.
        double avg_gain = 0.0;
        double avg_loss = 0.0;

        for (int i = 1; i <= period; ++i) {
            double change = data[i].close - data[i - 1].close;
            if (change > 0.0) avg_gain += change;
            else              avg_loss += std::abs(change);
        }
        avg_gain /= period;
        avg_loss /= period;

        // Set the seeded RSI value at index `period`.
        if (avg_loss == 0.0) {
            data[period].rsi14 = 100.0;
        } else {
            double rs = avg_gain / avg_loss;
            data[period].rsi14 = 100.0 - (100.0 / (1.0 + rs));
        }

        // Wilder's smoothing for all bars after the seed.
        for (size_t i = period + 1; i < data.size(); ++i) {
            if (!data[i].valid) {
                // Carry forward the previous RSI on invalid rows.
                data[i].rsi14 = data[i - 1].rsi14;
                continue;
            }

            double change = data[i].close - data[i - 1].close;
            double gain   = (change > 0.0) ? change        : 0.0;
            double loss   = (change < 0.0) ? std::abs(change) : 0.0;

            avg_gain = ((avg_gain * (period - 1)) + gain) / period;
            avg_loss = ((avg_loss * (period - 1)) + loss) / period;

            if (avg_loss == 0.0) {
                data[i].rsi14 = 100.0;
            } else {
                double rs = avg_gain / avg_loss;
                data[i].rsi14 = 100.0 - (100.0 / (1.0 + rs));
            }
        }

        std::cerr << "[PARSER] RSI14 computation complete." << std::endl;
    }

    // -------------------------------------------------------------------------
    // compute_vol_ma50()
    // 50-period Simple Moving Average of daily Volume.
    // Left at 0.0 if volume data is absent (all volume values are zero).
    // The simulation loop treats vol_ma50 == 0.0 as "gate disabled".
    // -------------------------------------------------------------------------
    static void compute_vol_ma50(std::vector<DailyDataPoint>& data) {
        const int period = 50;

        // Check if volume data is present at all.
        bool has_volume = false;
        for (const auto& d : data) {
            if (d.volume > 0.0) { has_volume = true; break; }
        }
        if (!has_volume) {
            std::cerr << "[PARSER] No volume data found — vol_ma50 gate disabled." << std::endl;
            return;
        }

        for (size_t i = 0; i < data.size(); ++i) {
            if (i < static_cast<size_t>(period - 1)) {
                data[i].vol_ma50 = 0.0;
                continue;
            }

            double sum        = 0.0;
            int    valid_seen = 0;
            for (int j = 0; j < period; ++j) {
                const auto& row = data[i - j];
                if (row.valid && row.volume > 0.0) {
                    sum += row.volume;
                    ++valid_seen;
                }
            }
            data[i].vol_ma50 = (valid_seen == period) ? (sum / period) : 0.0;
        }

        std::cerr << "[PARSER] Vol_MA50 computation complete." << std::endl;
    }
};
