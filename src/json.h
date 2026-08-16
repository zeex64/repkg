#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace repkg::json {

class Error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Value {
public:
    using Object = std::map<std::string, Value, std::less<>>;
    using Array = std::vector<Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;

    Value() = default;
    explicit Value(Storage storage) : storage_(std::move(storage)) {}

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;
    [[nodiscard]] bool boolean(std::string_view label) const;
    [[nodiscard]] std::int64_t integer(std::string_view label) const;
    [[nodiscard]] const std::string& string(std::string_view label) const;
    [[nodiscard]] const Array& array(std::string_view label) const;
    [[nodiscard]] const Object& object(std::string_view label) const;
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] const Value& require(std::string_view key) const;

private:
    Storage storage_{nullptr};
};

[[nodiscard]] Value parse(std::string_view text);

} // namespace repkg::json
