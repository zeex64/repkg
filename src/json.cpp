#include "json.h"

#include <charconv>
#include <cctype>
#include <limits>

namespace repkg::json {
namespace {

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    [[nodiscard]] Value root() {
        Value value = parse_value(0);
        whitespace();
        if (position_ != input_.size()) {
            fail("unexpected trailing JSON data");
        }
        return value;
    }

private:
    static constexpr unsigned kMaxDepth = 64;

    [[noreturn]] void fail(std::string_view message) const {
        throw Error(std::string(message) + " at byte " + std::to_string(position_));
    }

    void whitespace() noexcept {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) noexcept {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    [[nodiscard]] bool literal(std::string_view expected) noexcept {
        whitespace();
        if (input_.substr(position_, expected.size()) != expected) {
            return false;
        }
        position_ += expected.size();
        return true;
    }

    [[nodiscard]] static unsigned hex(char value) {
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
        return 0xFFFFFFFFU;
    }

    static void append_utf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    [[nodiscard]] std::string parse_string() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '"') {
            fail("expected JSON string");
        }
        ++position_;
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return output;
            if (value < 0x20) fail("unescaped control byte in JSON string");
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) fail("truncated JSON escape");
            const char escape = input_[position_++];
            switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    if (input_.size() - position_ < 4) fail("truncated Unicode escape");
                    unsigned codepoint = 0;
                    for (unsigned index = 0; index < 4; ++index) {
                        const unsigned digit = hex(input_[position_++]);
                        if (digit > 15) fail("invalid Unicode escape");
                        codepoint = (codepoint << 4U) | digit;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
                        fail("surrogate escapes are not supported in manifests");
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: fail("invalid JSON escape");
            }
        }
        fail("unterminated JSON string");
    }

    [[nodiscard]] Value parse_number() {
        whitespace();
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            fail("invalid JSON integer");
        }
        if (input_[position_] == '0') {
            ++position_;
        } else {
            while (position_ < input_.size()
                   && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size()
            && (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
            fail("manifest numbers must be integers");
        }
        std::int64_t output = 0;
        const auto token = input_.substr(start, position_ - start);
        const auto converted = std::from_chars(token.data(), token.data() + token.size(), output);
        if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size()) {
            fail("JSON integer is outside signed 64-bit range");
        }
        return Value(output);
    }

    [[nodiscard]] Value parse_array(unsigned depth) {
        expect('[');
        Value::Array output;
        if (consume(']')) return Value(std::move(output));
        for (;;) {
            output.push_back(parse_value(depth + 1));
            if (consume(']')) break;
            expect(',');
        }
        return Value(std::move(output));
    }

    [[nodiscard]] Value parse_object(unsigned depth) {
        expect('{');
        Value::Object output;
        if (consume('}')) return Value(std::move(output));
        for (;;) {
            std::string key = parse_string();
            expect(':');
            if (!output.emplace(key, parse_value(depth + 1)).second) {
                fail("duplicate JSON object member");
            }
            if (consume('}')) break;
            expect(',');
        }
        return Value(std::move(output));
    }

    [[nodiscard]] Value parse_value(unsigned depth) {
        if (depth >= kMaxDepth) fail("JSON nesting is too deep");
        whitespace();
        if (position_ >= input_.size()) fail("expected JSON value");
        switch (input_[position_]) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Value(parse_string());
            case 't': if (literal("true")) return Value(true); break;
            case 'f': if (literal("false")) return Value(false); break;
            case 'n': if (literal("null")) return Value(nullptr); break;
            default:
                if (input_[position_] == '-'
                    || std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                    return parse_number();
                }
        }
        fail("invalid JSON value");
    }

    std::string_view input_;
    std::size_t position_{};
};

[[noreturn]] void wrong_type(std::string_view label, std::string_view wanted) {
    throw Error(std::string(label) + " must be " + std::string(wanted));
}

} // namespace

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_integer() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Value::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Value::boolean(std::string_view label) const {
    if (!is_bool()) wrong_type(label, "a boolean");
    return std::get<bool>(storage_);
}

std::int64_t Value::integer(std::string_view label) const {
    if (!is_integer()) wrong_type(label, "an integer");
    return std::get<std::int64_t>(storage_);
}

const std::string& Value::string(std::string_view label) const {
    if (!is_string()) wrong_type(label, "a string");
    return std::get<std::string>(storage_);
}

const Value::Array& Value::array(std::string_view label) const {
    if (!is_array()) wrong_type(label, "an array");
    return std::get<Array>(storage_);
}

const Value::Object& Value::object(std::string_view label) const {
    if (!is_object()) wrong_type(label, "an object");
    return std::get<Object>(storage_);
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    const auto& object = std::get<Object>(storage_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

const Value& Value::require(std::string_view key) const {
    const Value* value = find(key);
    if (value == nullptr) throw Error("manifest requires '" + std::string(key) + "'");
    return *value;
}

Value parse(std::string_view text) { return Parser(text).root(); }

} // namespace repkg::json
