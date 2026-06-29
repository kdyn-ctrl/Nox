#pragma once

// Shared Telegram notification utility for all Nox C++ services.
//
// PREREQUISITE: the including translation unit must define
// CPPHTTPLIB_OPENSSL_SUPPORT and include "httplib.h" before this header.
// Both execution/main.cpp and analyst/main.cpp already do this.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace nox {

class TelegramNotifier {
public:
    // Instance form: construct with explicit credentials (caller owns them).
    TelegramNotifier(const std::string& token, const std::string& chat_id)
        : token_(token), chat_id_(chat_id) {}

    void send(const std::string& msg) const {
        post_chunked(token_, chat_id_, msg);
    }

    // Static form: reads TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID from environment
    // on every call. Use when no persistent instance is warranted.
    static void sendMessage(const std::string& msg) {
        const char* tok = std::getenv("TELEGRAM_BOT_TOKEN");
        const char* cid = std::getenv("TELEGRAM_CHAT_ID");
        if (!tok || !cid || !*tok || !*cid) {
            std::cerr << "[TELEGRAM] Missing BOT_TOKEN or CHAT_ID — message dropped.\n";
            return;
        }
        post_chunked(tok, cid, msg);
    }

private:
    std::string token_;
    std::string chat_id_;

    // Telegram hard-limits messages to 4096 chars. We chunk at 4000 to leave
    // room for parse_mode formatting overhead and walk back to a valid UTF-8
    // boundary so we never split a multi-byte codepoint mid-sequence.
    static void post_chunked(const std::string& token,
                              const std::string& chat_id,
                              const std::string& msg) {
        if (token.empty() || chat_id.empty()) return;

        httplib::Client cli("https://api.telegram.org");
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(10));

        const std::string path = "/bot" + token + "/sendMessage";
        constexpr size_t MAX_CHUNK = 4000;

        for (size_t offset = 0; offset < msg.size(); ) {
            size_t len = std::min(MAX_CHUNK, msg.size() - offset);

            // Walk back to the start of any partial UTF-8 continuation byte
            // so we never send a broken multi-byte sequence to Telegram.
            size_t safe = offset + len;
            while (safe > offset &&
                   (static_cast<unsigned char>(msg[safe]) & 0xC0) == 0x80)
                --safe;
            len = safe - offset;
            if (len == 0) len = std::min(MAX_CHUNK, msg.size() - offset); // ASCII fallback

            const std::string chunk = msg.substr(offset, len);
            offset += len;

            // Build a minimal JSON body without pulling nlohmann into this header.
            const std::string body =
                "{\"chat_id\":\"" + chat_id +
                "\",\"text\":\""  + json_escape(chunk) +
                "\",\"parse_mode\":\"Markdown\"}";

            auto res = cli.Post(path.c_str(), body, "application/json");
            if (!res || res->status != 200) {
                std::cerr << "[TELEGRAM] Delivery failed. Status: "
                          << (res ? std::to_string(res->status) : "timeout") << '\n';
            }
        }
    }

    // Minimal JSON string escaper — only handles characters Telegram messages
    // realistically contain. Full RFC 8259 compliance isn't needed here.
    static std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 16);
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }
};

} // namespace nox
