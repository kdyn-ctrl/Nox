#pragma once

#include <chrono>
#include <vector>
#include <cstring>
#include <cstdlib>

namespace nox {

// US market closed on these holidays (hardcoded for 2024-2027).
// IMPORTANT: This list is tuned per FEEDBACK_HARDCODE_NOTHING_TUNABLE.
// To override, set TRADING_DAY_SKIP_DATES=YYYY-MM-DD,YYYY-MM-DD in .env
static std::vector<std::string> get_market_holidays() {
    std::vector<std::string> holidays = {
        // 2024
        "2024-01-01", // New Year's Day
        "2024-01-15", // MLK Day
        "2024-02-19", // Presidents Day
        "2024-03-29", // Good Friday
        "2024-05-27", // Memorial Day
        "2024-06-19", // Juneteenth
        "2024-07-04", // Independence Day
        "2024-09-02", // Labor Day
        "2024-11-28", // Thanksgiving
        "2024-12-25", // Christmas

        // 2025
        "2025-01-01", // New Year's Day
        "2025-01-20", // MLK Day
        "2025-02-17", // Presidents Day
        "2025-04-18", // Good Friday
        "2025-05-26", // Memorial Day
        "2025-06-19", // Juneteenth
        "2025-07-04", // Independence Day
        "2025-09-01", // Labor Day
        "2025-11-27", // Thanksgiving
        "2025-12-25", // Christmas

        // 2026
        "2026-01-01", // New Year's Day
        "2026-01-19", // MLK Day
        "2026-02-16", // Presidents Day
        "2026-04-03", // Good Friday
        "2026-05-25", // Memorial Day
        "2026-06-19", // Juneteenth
        "2026-07-04", // Independence Day (observed, assuming Fri)
        "2026-09-07", // Labor Day
        "2026-11-26", // Thanksgiving
        "2026-12-25", // Christmas

        // 2027
        "2027-01-01", // New Year's Day
        "2027-01-18", // MLK Day
        "2027-02-15", // Presidents Day
        "2027-03-26", // Good Friday
        "2027-05-31", // Memorial Day
        "2027-06-18", // Juneteenth
        "2027-07-05", // Independence Day (observed, Mon)
        "2027-09-06", // Labor Day
        "2027-11-25", // Thanksgiving
        "2027-12-24", // Christmas Eve (market closes early at 13:00, treat as off)
        "2027-12-25", // Christmas
    };

    // Allow env var override: TRADING_DAY_SKIP_DATES=YYYY-MM-DD,YYYY-MM-DD
    const char* env_skip = std::getenv("TRADING_DAY_SKIP_DATES");
    if (env_skip) {
        std::string skip_str(env_skip);
        size_t pos = 0;
        while (pos < skip_str.size()) {
            size_t comma = skip_str.find(',', pos);
            if (comma == std::string::npos) comma = skip_str.size();
            std::string date = skip_str.substr(pos, comma - pos);
            // Trim whitespace
            date.erase(0, date.find_first_not_of(" \t"));
            date.erase(date.find_last_not_of(" \t") + 1);
            if (!date.empty()) {
                holidays.push_back(date);
            }
            pos = comma + 1;
        }
    }

    return holidays;
}

// Returns true if today is a US market trading day (not a weekend or holiday).
// This gates options signals, equity signals, scout protocol, market scanner, and IV collection.
// To temporarily disable all signal generation on weekdays, set TRADING_DAYS_ENABLED=false in .env
static bool is_trading_day() {
    // Check env var override
    const char* enabled = std::getenv("TRADING_DAYS_ENABLED");
    if (enabled && std::string(enabled) == "false") {
        return false;
    }

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&tt, &utc);

    // Skip weekends (0=Sunday, 6=Saturday)
    if (utc.tm_wday == 0 || utc.tm_wday == 6) {
        return false;
    }

    // Skip holidays
    char date_buf[11];
    snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    std::string today(date_buf);

    const auto& holidays = get_market_holidays();
    for (const auto& holiday : holidays) {
        if (today == holiday) {
            return false;
        }
    }

    return true;
}

} // namespace nox
