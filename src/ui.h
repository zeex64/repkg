#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace repkg::ui {

inline constexpr std::uint32_t kScreenClass = 0x808047B7U;
inline constexpr std::uint32_t kHierarchyClass = 0x8080496AU;
inline constexpr std::uint32_t kWidgetTableClass = 0x80804825U;
inline constexpr std::uint16_t kRootParent = 0x7FFFU;

struct Hierarchy {
    std::uint32_t tag{};
    std::uint32_t entryIndex{};
    std::uint32_t widgetTableTag{};
    std::uint16_t nodeCount{};
    std::vector<std::pair<std::uint16_t, std::uint16_t>> edges;
    std::vector<std::byte> header;
};

struct View {
    std::uint32_t nameHash{};
    std::string name;
    Hierarchy hierarchy;
};

struct Layout {
    std::string screenName;
    std::uint32_t screenTag{};
    std::uint32_t screenEntry{};
    std::vector<std::byte> screenTemplate;
    struct LocalizedStringsReference {
        std::uint32_t tag{};
        std::uint32_t word{};
        std::size_t offset{};
    };
    std::vector<LocalizedStringsReference> localizedStrings;
    std::vector<View> views;
};

struct CompiledEntry {
    std::uint32_t entryIndex{};
    std::string role;
    std::vector<std::byte> data;
};

struct WidgetArrayItem {
    std::size_t offset{};
    std::vector<std::byte> data;
};

struct WidgetArray {
    std::size_t fieldOffset{};
    std::size_t dataOffset{};
    std::uint32_t itemClass{};
    std::size_t itemSize{};
    std::string name;
    std::vector<WidgetArrayItem> items;
};

struct WidgetTable {
    std::uint32_t tag{};
    std::uint32_t entryIndex{};
    std::vector<std::byte> objectTemplate;
    std::vector<WidgetArray> arrays;
};

struct ScreenReference {
    std::uint32_t nameHash{};
    std::uint32_t hierarchyTag{};
    std::size_t nameHashOffset{};
    std::size_t hierarchyTagOffset{};
};

[[nodiscard]] bool parse_screen(std::span<const std::byte> bytes,
                                std::vector<ScreenReference>& references,
                                std::vector<Layout::LocalizedStringsReference>& localizedStrings) noexcept;
[[nodiscard]] bool parse_hierarchy(std::span<const std::byte> bytes,
                                   Hierarchy& hierarchy) noexcept;
[[nodiscard]] std::string known_view_name(std::uint32_t hash);
[[nodiscard]] bool parse_widget_table(std::span<const std::byte> bytes,
                                      WidgetTable& table) noexcept;
void write_layout(const std::filesystem::path& path, const Layout& layout);
void write_widget_table(const std::filesystem::path& path,
                        const WidgetTable& table);
[[nodiscard]] std::vector<CompiledEntry>
compile_layout(const std::filesystem::path& path,
               std::uint16_t expectedPackageId,
               std::size_t entryCount);
[[nodiscard]] CompiledEntry
compile_widget_table(const std::filesystem::path& path,
                     std::uint16_t expectedPackageId,
                     std::size_t entryCount);

} // namespace repkg::ui
