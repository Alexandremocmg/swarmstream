// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Leitor de JSON mínimo dos testes do sw-pack (o projeto não tem dependências de terceiros).
// Confere com SW_CHECK: entrada malformada derruba o teste.
#pragma once

#include "check.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace swarmstream::test {

// ---------------------------------------------------------------- JSON mínimo

struct Json
{
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> members;

    [[nodiscard]] const Json& at(const std::string& key) const
    {
        const auto found = members.find(key);
        SW_CHECK(kind == Kind::Object && found != members.end());
        return found->second;
    }
    [[nodiscard]] bool has(const std::string& key) const
    {
        return kind == Kind::Object && members.count(key) != 0;
    }
    [[nodiscard]] std::size_t as_size() const
    {
        SW_CHECK(kind == Kind::Number && number >= 0 &&
                 number == static_cast<double>(static_cast<std::uint64_t>(number)));
        return static_cast<std::size_t>(number);
    }
    [[nodiscard]] const std::string& as_string() const
    {
        SW_CHECK(kind == Kind::String);
        return text;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view source) : source_(source) {}

    Json parse()
    {
        Json value = parse_value();
        skip_space();
        SW_CHECK(position_ == source_.size());
        return value;
    }

private:
    void skip_space()
    {
        while (position_ < source_.size() &&
               std::string_view(" \t\r\n").find(source_[position_]) != std::string_view::npos) {
            ++position_;
        }
    }
    char peek()
    {
        skip_space();
        SW_CHECK(position_ < source_.size());
        return source_[position_];
    }
    void expect(char c)
    {
        SW_CHECK(peek() == c);
        ++position_;
    }
    bool take_literal(std::string_view literal)
    {
        if (source_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (true) {
            SW_CHECK(position_ < source_.size());
            const char c = source_[position_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            SW_CHECK(position_ < source_.size());
            const char escaped = source_[position_++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': append_utf8(out, parse_hex4()); break;
            default: SW_CHECK(false && "escape de JSON invalido");
            }
        }
    }

    // Escape unicode de 4 digitos hexa: so o plano basico (sem pares substitutos), o bastante
    // para o texto dos vetores.
    std::uint32_t parse_hex4()
    {
        static constexpr std::string_view kDigits = "0123456789abcdef";
        SW_CHECK(position_ + 4 <= source_.size());
        std::uint32_t value = 0;
        for (int k = 0; k < 4; ++k) {
            const char digit =
                static_cast<char>(std::tolower(static_cast<unsigned char>(source_[position_++])));
            const std::size_t found = kDigits.find(digit);
            SW_CHECK(found != std::string_view::npos);
            value = value * 16 + static_cast<std::uint32_t>(found);
        }
        SW_CHECK(value < 0xD800 || value > 0xDFFF);
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    Json parse_value()
    {
        Json value;
        const char c = peek();
        if (c == '{') {
            ++position_;
            value.kind = Json::Kind::Object;
            if (peek() == '}') {
                ++position_;
                return value;
            }
            while (true) {
                std::string key = parse_string();
                expect(':');
                value.members.emplace(std::move(key), parse_value());
                if (peek() == ',') {
                    ++position_;
                    continue;
                }
                expect('}');
                return value;
            }
        }
        if (c == '[') {
            ++position_;
            value.kind = Json::Kind::Array;
            if (peek() == ']') {
                ++position_;
                return value;
            }
            while (true) {
                value.items.push_back(parse_value());
                if (peek() == ',') {
                    ++position_;
                    continue;
                }
                expect(']');
                return value;
            }
        }
        if (c == '"') {
            value.kind = Json::Kind::String;
            value.text = parse_string();
            return value;
        }
        const bool is_true = take_literal("true");
        if (is_true || take_literal("false")) {
            value.kind = Json::Kind::Bool;
            value.boolean = is_true;
            return value;
        }
        if (take_literal("null")) {
            return value;
        }
        const std::size_t start = position_;
        while (position_ < source_.size() &&
               std::string_view("+-.eE0123456789").find(source_[position_]) !=
                   std::string_view::npos) {
            ++position_;
        }
        SW_CHECK(position_ > start);
        value.kind = Json::Kind::Number;
        value.number =
            std::strtod(std::string(source_.substr(start, position_ - start)).c_str(), nullptr);
        return value;
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

} // namespace swarmstream::test
