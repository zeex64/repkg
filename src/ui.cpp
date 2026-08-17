#include "ui.h"

#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>

#include "json.h"
#include "package.h"

namespace repkg::ui {
namespace {

template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> bytes, std::size_t offset,
                           T& value) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

template <typename T>
void write_le(std::vector<std::byte>& bytes, std::size_t offset, T value,
              std::string_view label) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw Error(std::string(label) + " points outside the UI template");
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

[[nodiscard]] std::string hex(std::uint64_t value, unsigned width) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(width)
           << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::string hex_bytes(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const unsigned value = std::to_integer<unsigned>(bytes[index]);
        output[index * 2] = digits[value >> 4U];
        output[index * 2 + 1] = digits[value & 0xFU];
    }
    return output;
}

[[nodiscard]] unsigned nibble(char value) noexcept {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
    return 16;
}

[[nodiscard]] std::vector<std::byte> parse_hex_bytes(const json::Value& value,
                                                     std::string_view label) {
    const std::string& text = value.string(label);
    if ((text.size() & 1U) != 0) throw Error(std::string(label) + " must have an even length");
    std::vector<std::byte> output(text.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const unsigned high = nibble(text[index * 2]);
        const unsigned low = nibble(text[index * 2 + 1]);
        if (high > 15 || low > 15) throw Error(std::string(label) + " is not hexadecimal");
        output[index] = static_cast<std::byte>((high << 4U) | low);
    }
    return output;
}

[[nodiscard]] std::uint64_t parse_unsigned(const json::Value& value,
                                           std::string_view label,
                                           std::uint64_t maximum) {
    std::uint64_t result = 0;
    if (value.is_integer()) {
        const std::int64_t signedValue = value.integer(label);
        if (signedValue < 0) throw Error(std::string(label) + " must be unsigned");
        result = static_cast<std::uint64_t>(signedValue);
    } else if (value.is_string()) {
        const std::string& text = value.string(label);
        if (text.empty() || text.front() == '-') {
            throw Error(std::string(label) + " must be unsigned");
        }
        std::size_t consumed = 0;
        try {
            result = std::stoull(text, &consumed, 0);
        } catch (const std::exception&) {
            throw Error(std::string(label) + " is not an unsigned integer");
        }
        if (consumed != text.size()) {
            throw Error(std::string(label) + " has trailing characters");
        }
    } else {
        throw Error(std::string(label) + " must be an integer or quoted 0x value");
    }
    if (result > maximum) throw Error(std::string(label) + " exceeds its field width");
    return result;
}

[[nodiscard]] std::string escape(std::string_view input) {
    std::string output;
    for (const unsigned char value : input) {
        switch (value) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    std::ostringstream encoded;
                    encoded << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<unsigned>(value);
                    output += encoded.str();
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    return output;
}

[[nodiscard]] std::uint32_t u32(const json::Value& value, std::string_view label) {
    return static_cast<std::uint32_t>(parse_unsigned(value, label, 0xFFFFFFFFULL));
}

[[nodiscard]] bool float_widget_class(std::uint32_t classId) noexcept {
    return classId == 0x80804693U || classId == 0x80804694U;
}

[[nodiscard]] std::string float_text(std::uint32_t bits) {
    if ((bits & 0x7F800000U) == 0x7F800000U) {
        return "bits:" + hex(bits, 8);
    }
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<float>::max_digits10)
           << std::bit_cast<float>(bits);
    return output.str();
}

[[nodiscard]] std::uint32_t parse_float_text(const json::Value& value,
                                             std::string_view label) {
    const std::string& text = value.string(label);
    if (text.starts_with("bits:")) {
        json::Value encoded(std::string(text.substr(5)));
        return u32(encoded, label);
    }
    std::size_t consumed = 0;
    float parsed = 0;
    try {
        parsed = std::stof(text, &consumed);
    } catch (const std::exception&) {
        throw Error(std::string(label) + " is not a float32 value");
    }
    if (consumed != text.size()) {
        throw Error(std::string(label) + " has trailing characters");
    }
    return std::bit_cast<std::uint32_t>(parsed);
}

[[nodiscard]] std::size_t widget_item_size(std::uint32_t classId) noexcept {
    switch (classId) {
        case 0x80804607U: return 12;
        case 0x80804618U: return 4;
        case 0x80804622U: return 16;
        case 0x8080462AU: return 64;
        case 0x80804688U: return 16;
        case 0x8080468BU: return 2;
        case 0x8080468DU: return 1;
        case 0x80804690U: return 4;
        case 0x80804692U: return 4;
        case 0x80804693U: return 16;
        case 0x80804694U: return 4;
        case 0x80804695U: return 8;
        case 0x80804696U: return 4;
        case 0x808046D4U: return 24;
        case 0x808046D8U: return 56;
        case 0x808046F3U: return 24;
        case 0x808046F5U: return 32;
        case 0x808046F7U: return 48;
        case 0x808046FBU: return 16;
        case 0x80804837U: return 24;
        case 0x80804858U: return 24;
        case 0x808048D1U: return 112;
        case 0x808048D7U: return 32;
        case 0x808048DFU: return 32;
        case 0x8080490FU: return 12;
        case 0x80804943U: return 16;
        case 0x80804949U: return 4;
        default: return 0;
    }
}

[[nodiscard]] std::string root_widget_array_name(std::size_t offset) {
    switch (offset) {
        case 8: return "root_523CB520";
        case 24: return "objects";
        case 40: return "root_9296B694";
        case 56: return "components";
        case 72: return "overlays";
        case 88: return "root_AAA08769";
        case 104: return "root_B886C949";
        case 120: return "root_54CAE4A4";
        case 136: return "root_2452834D";
        default: return {};
    }
}

[[nodiscard]] std::string nested_widget_array_name(std::uint32_t parentClass,
                                                    std::size_t relativeOffset) {
    if (parentClass == 0x8080462AU && relativeOffset == 32) return "object_98FCD168";
    if (parentClass == 0x8080462AU && relativeOffset == 48) return "animations";
    if (parentClass == 0x808046D4U && relativeOffset == 8) return "component_77A4F489";
    if (parentClass == 0x808046D8U && relativeOffset == 48) return "conversion";
    if (parentClass == 0x808046F3U && relativeOffset == 8) return "components";
    if (parentClass == 0x808046F5U && relativeOffset == 8) return "properties";
    if (parentClass == 0x808046F7U && relativeOffset == 32) return "property_8502FB24";
    if (parentClass == 0x80804943U && relativeOffset == 0) return "elements";
    return {};
}

void name_widget_arrays(std::vector<WidgetArray>& arrays) {
    for (WidgetArray& array : arrays) {
        array.name = root_widget_array_name(array.fieldOffset);
        if (!array.name.empty()) continue;
        for (const WidgetArray& parent : arrays) {
            bool found = false;
            for (const WidgetArrayItem& item : parent.items) {
                if (array.fieldOffset >= item.offset
                    && array.fieldOffset < item.offset + parent.itemSize) {
                    array.name = nested_widget_array_name(
                        parent.itemClass, array.fieldOffset - item.offset);
                    found = !array.name.empty();
                    if (found) break;
                }
            }
            if (found) break;
        }
        if (array.name.empty()) {
            std::ostringstream name;
            if (array.itemClass == 0x80804694U) name << "float_values_at_0x";
            else if (array.itemClass == 0x80804693U) name << "vector4_values_at_0x";
            else name << "array_at_0x";
            name << std::uppercase << std::hex << array.fieldOffset;
            array.name = name.str();
        }
    }
}

} // namespace

bool parse_screen(std::span<const std::byte> bytes,
                  std::vector<ScreenReference>& references,
                  std::vector<Layout::LocalizedStringsReference>& localizedStrings) noexcept {
    references.clear();
    localizedStrings.clear();
    std::uint64_t totalSize = 0;
    std::uint64_t count = 0;
    std::uint64_t relative = 0;
    if (!read_le(bytes, 0, totalSize) || totalSize != bytes.size()
        || !read_le(bytes, 0x10, count) || count > 4096
        || !read_le(bytes, 0x18, relative)) return false;
    const std::uint64_t itemStart64 = 0x28ULL + relative;
    if (itemStart64 > bytes.size() || count > (bytes.size() - itemStart64) / 24) return false;
    const std::size_t itemStart = static_cast<std::size_t>(itemStart64);
    references.reserve(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t nameHash = 0;
        std::uint64_t innerCount = 0;
        std::uint64_t innerRelative = 0;
        const std::size_t item = itemStart + index * 24;
        if (!read_le(bytes, item, nameHash) || nameHash > 0xFFFFFFFFULL
            || !read_le(bytes, item + 8, innerCount) || innerCount != 1
            || !read_le(bytes, item + 16, innerRelative)) return false;
        const std::uint64_t innerTarget64 = item + 0x20ULL + innerRelative;
        std::uint32_t fnvBase = 0;
        std::uint32_t tag = 0;
        if (innerTarget64 > std::numeric_limits<std::size_t>::max()
            || !read_le(bytes, static_cast<std::size_t>(innerTarget64), fnvBase)
            || !read_le(bytes, static_cast<std::size_t>(innerTarget64) + 4, tag)
            || fnvBase != 0x811C9DC5U || tag < kTagBase) return false;
        references.push_back(ScreenReference{static_cast<std::uint32_t>(nameHash), tag,
                                              item,
                                              static_cast<std::size_t>(innerTarget64) + 4});
    }
    std::uint64_t stringsCount = 0;
    std::uint64_t stringsRelative = 0;
    if (!read_le(bytes, 0x20, stringsCount) || stringsCount > 4096
        || !read_le(bytes, 0x28, stringsRelative)) return false;
    const std::uint64_t stringsTarget64 = 0x38ULL + stringsRelative;
    if (stringsTarget64 > bytes.size()
        || stringsCount > (bytes.size() - static_cast<std::size_t>(stringsTarget64)) / 8) {
        return false;
    }
    for (std::size_t index = 0; index < stringsCount; ++index) {
        const std::size_t offset = static_cast<std::size_t>(stringsTarget64) + index * 8;
        Layout::LocalizedStringsReference value;
        value.offset = offset;
        if (!read_le(bytes, offset, value.tag) || value.tag < kTagBase
            || !read_le(bytes, offset + 4, value.word)) return false;
        localizedStrings.push_back(value);
    }
    return true;
}

bool parse_hierarchy(std::span<const std::byte> bytes, Hierarchy& hierarchy) noexcept {
    std::uint64_t totalSize = 0;
    std::uint64_t count = 0;
    std::uint64_t relative = 0;
    std::uint16_t nodeA = 0;
    std::uint16_t nodeB = 0;
    std::uint64_t capacity = 0;
    std::uint64_t itemClass = 0;
    if (bytes.size() < 0x40 || !read_le(bytes, 0, totalSize) || totalSize != bytes.size()
        || !read_le(bytes, 8, count) || count > 0xFFFF
        || !read_le(bytes, 0x10, relative) || relative != 0x20
        || !read_le(bytes, 0x18, nodeA) || !read_le(bytes, 0x1A, nodeB)
        || nodeA != nodeB || !read_le(bytes, 0x1C, hierarchy.widgetTableTag)
        || !read_le(bytes, 0x30, capacity) || capacity != count
        || !read_le(bytes, 0x38, itemClass) || itemClass != 0x80804616ULL
        || bytes.size() != 0x40 + count * 4) return false;
    hierarchy.nodeCount = nodeA;
    hierarchy.header.assign(bytes.begin(), bytes.begin() + 0x40);
    hierarchy.edges.clear();
    hierarchy.edges.reserve(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        std::uint16_t parent = 0;
        std::uint16_t child = 0;
        if (!read_le(bytes, 0x40 + index * 4, parent)
            || !read_le(bytes, 0x42 + index * 4, child)) return false;
        if (child >= hierarchy.nodeCount
            || (parent != kRootParent && parent >= hierarchy.nodeCount)) return false;
        hierarchy.edges.emplace_back(parent, child);
    }
    return true;
}

std::string known_view_name(std::uint32_t hash) {
    static const std::map<std::uint32_t, std::string_view> names{
        {0x2F9EB846U, "matchmaking_loading"},
        {0x404C2310U, "orbit_matchmaking_totem"},
        {0x494CE5B3U, "orbit_waiting"},
        {0x91E7DA3EU, "orbit_matchmaking"},
        {0xE6B3AF4EU, "orbit_director"},
        {0xFFE50823U, "matchmaking_guided_game"},
    };
    const auto found = names.find(hash);
    return found == names.end() ? "view_" + hex(hash, 8).substr(2)
                                : std::string(found->second);
}

bool parse_widget_table(std::span<const std::byte> bytes,
                        WidgetTable& table) noexcept {
    std::uint64_t totalSize = 0;
    if (bytes.size() < 0x158 || !read_le(bytes, 0, totalSize)
        || totalSize != bytes.size()) return false;
    table.objectTemplate.assign(bytes.begin(), bytes.end());
    table.arrays.clear();
    for (std::size_t field = 8; field + 16 <= bytes.size(); field += 8) {
        std::uint64_t count = 0;
        std::uint64_t relative = 0;
        if (!read_le(bytes, field, count) || count == 0 || count > 0x100000
            || !read_le(bytes, field + 8, relative)) continue;
        if (relative > std::numeric_limits<std::uint64_t>::max() - field - 0x18ULL) {
            continue;
        }
        const std::uint64_t dataOffset64 = field + 0x18ULL + relative;
        if (dataOffset64 < 0x20 || dataOffset64 > bytes.size()) continue;
        const std::size_t dataOffset = static_cast<std::size_t>(dataOffset64);
        std::uint32_t marker = 0;
        std::uint64_t capacity = 0;
        std::uint64_t itemClass64 = 0;
        if (!read_le(bytes, dataOffset - 0x14, marker) || marker != 0x80809FBDU
            || !read_le(bytes, dataOffset - 0x10, capacity) || capacity != count
            || !read_le(bytes, dataOffset - 8, itemClass64)
            || itemClass64 > 0xFFFFFFFFULL) continue;
        const std::uint32_t itemClass = static_cast<std::uint32_t>(itemClass64);
        const std::size_t itemSize = widget_item_size(itemClass);
        if (itemSize == 0 || count > (bytes.size() - dataOffset) / itemSize) continue;
        WidgetArray array;
        array.fieldOffset = field;
        array.dataOffset = dataOffset;
        array.itemClass = itemClass;
        array.itemSize = itemSize;
        array.items.reserve(static_cast<std::size_t>(count));
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t itemOffset = dataOffset + index * itemSize;
            array.items.push_back(WidgetArrayItem{
                itemOffset,
                std::vector<std::byte>(bytes.begin() + itemOffset,
                                       bytes.begin() + itemOffset + itemSize)});
        }
        table.arrays.push_back(std::move(array));
    }
    name_widget_arrays(table.arrays);
    return true;
}

void write_layout(const std::filesystem::path& path, const Layout& layout) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Error("could not create UI layout JSON: " + path.string());
    output << "{\n"
           << "  \"format\": \"repkg.ui_layout.v1\",\n"
           << "  \"screen\": {\n"
           << "    \"name\": \"" << escape(layout.screenName) << "\",\n"
           << "    \"tag\": \"" << hex(layout.screenTag, 8) << "\",\n"
           << "    \"entry_index\": " << layout.screenEntry << ",\n"
           << "    \"class_id\": \"" << hex(kScreenClass, 8) << "\",\n"
           << "    \"localized_strings\": [";
    for (std::size_t index = 0; index < layout.localizedStrings.size(); ++index) {
        const auto& value = layout.localizedStrings[index];
        if (index != 0) output << ", ";
        output << "{\"tag\": \"" << hex(value.tag, 8)
               << "\", \"word\": \"" << hex(value.word, 8) << "\"}";
    }
    output << "],\n"
           << "    \"template_hex\": \"" << hex_bytes(layout.screenTemplate) << "\"\n"
           << "  },\n"
           << "  \"views\": [";
    for (std::size_t index = 0; index < layout.views.size(); ++index) {
        const View& view = layout.views[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\n"
               << "      \"name\": \"" << escape(view.name) << "\",\n"
               << "      \"name_hash\": \"" << hex(view.nameHash, 8) << "\",\n"
               << "      \"hierarchy_tag\": \"" << hex(view.hierarchy.tag, 8) << "\",\n"
               << "      \"hierarchy_entry\": " << view.hierarchy.entryIndex << ",\n"
               << "      \"hierarchy_class_id\": \"" << hex(kHierarchyClass, 8) << "\",\n"
               << "      \"widget_table_tag\": \""
               << hex(view.hierarchy.widgetTableTag, 8) << "\",\n"
               << "      \"node_count\": " << view.hierarchy.nodeCount << ",\n"
               << "      \"header_hex\": \"" << hex_bytes(view.hierarchy.header) << "\",\n"
               << "      \"edges\": [";
        for (std::size_t edge = 0; edge < view.hierarchy.edges.size(); ++edge) {
            const auto [parent, child] = view.hierarchy.edges[edge];
            output << (edge == 0 ? "\n" : ",\n") << "        {\"parent\": ";
            if (parent == kRootParent) output << "null";
            else output << parent;
            output << ", \"child\": " << child << "}";
        }
        output << (view.hierarchy.edges.empty() ? "" : "\n      ") << "]\n"
               << "    }";
    }
    output << (layout.views.empty() ? "" : "\n  ") << "]\n}\n";
    if (!output) throw Error("failed while writing UI layout JSON: " + path.string());
}

void write_widget_table(const std::filesystem::path& path,
                        const WidgetTable& table) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Error("could not create UI widget-table JSON: " + path.string());
    output << "{\n"
           << "  \"format\": \"repkg.ui_widget_table.v1\",\n"
           << "  \"tag\": \"" << hex(table.tag, 8) << "\",\n"
           << "  \"entry_index\": " << table.entryIndex << ",\n"
           << "  \"class_id\": \"" << hex(kWidgetTableClass, 8) << "\",\n"
           << "  \"template_hex\": \"" << hex_bytes(table.objectTemplate) << "\",\n"
           << "  \"arrays\": [";
    for (std::size_t arrayIndex = 0; arrayIndex < table.arrays.size(); ++arrayIndex) {
        const WidgetArray& array = table.arrays[arrayIndex];
        output << (arrayIndex == 0 ? "\n" : ",\n")
               << "    {\n"
               << "      \"name\": \"" << escape(array.name) << "\",\n"
               << "      \"field_offset\": \"" << hex(array.fieldOffset, 4) << "\",\n"
               << "      \"data_offset\": \"" << hex(array.dataOffset, 4) << "\",\n"
               << "      \"item_class\": \"" << hex(array.itemClass, 8) << "\",\n"
               << "      \"item_size\": " << array.itemSize << ",\n"
               << "      \"items\": [";
        for (std::size_t itemIndex = 0; itemIndex < array.items.size(); ++itemIndex) {
            const WidgetArrayItem& item = array.items[itemIndex];
            output << (itemIndex == 0 ? "\n" : ",\n")
                   << "        {\"offset\": \"" << hex(item.offset, 4) << "\", ";
            const std::size_t wordCount = item.data.size() / 4;
            if (float_widget_class(array.itemClass)) {
                output << "\"floats_le\": [";
                for (std::size_t word = 0; word < wordCount; ++word) {
                    std::uint32_t value = 0;
                    std::memcpy(&value, item.data.data() + word * 4, 4);
                    if (word != 0) output << ", ";
                    output << "{\"bits\": \"" << hex(value, 8)
                           << "\", \"value\": \"" << float_text(value) << "\"}";
                }
            } else {
                output << "\"words_le\": [";
                for (std::size_t word = 0; word < wordCount; ++word) {
                    std::uint32_t value = 0;
                    std::memcpy(&value, item.data.data() + word * 4, 4);
                    if (word != 0) output << ", ";
                    output << "\"" << hex(value, 8) << "\"";
                }
            }
            output << "], \"tail_hex\": \""
                   << hex_bytes(std::span<const std::byte>(item.data).subspan(wordCount * 4))
                   << "\"}";
        }
        output << (array.items.empty() ? "" : "\n      ") << "]\n"
               << "    }";
    }
    output << (table.arrays.empty() ? "" : "\n  ") << "]\n}\n";
    if (!output) throw Error("failed while writing UI widget-table JSON: " + path.string());
}

std::vector<CompiledEntry> compile_layout(const std::filesystem::path& path,
                                          std::uint16_t expectedPackageId,
                                          std::size_t entryCount) {
    json::Value document;
    try {
        document = json::parse(read_text(path));
    } catch (const json::Error& error) {
        throw Error(path.string() + ": " + error.what());
    }
    if (document.require("format").string("UI layout format") != "repkg.ui_layout.v1") {
        throw Error("unsupported UI layout format");
    }
    const json::Value& screen = document.require("screen");
    (void)screen.object("UI screen");
    if (u32(screen.require("class_id"), "UI screen class_id") != kScreenClass) {
        throw Error("UI screen class_id is not the supported screen class");
    }
    const std::uint32_t screenTag = u32(screen.require("tag"), "UI screen tag");
    const std::uint32_t screenEntry = u32(screen.require("entry_index"), "UI screen entry_index");
    if (tag_package(screenTag) != expectedPackageId || tag_entry(screenTag) != screenEntry
        || screenEntry >= entryCount) throw Error("UI screen tag/entry mapping is invalid");
    std::vector<std::byte> root = parse_hex_bytes(screen.require("template_hex"),
                                                  "UI screen template_hex");
    std::vector<ScreenReference> originalReferences;
    std::vector<Layout::LocalizedStringsReference> originalStrings;
    if (!parse_screen(root, originalReferences, originalStrings)) {
        throw Error("UI screen template is not a supported 0x808047B7 payload");
    }
    const auto& rawViews = document.require("views").array("UI views");
    if (rawViews.size() != originalReferences.size()) {
        throw Error("UI view count cannot yet be changed; it must match the template");
    }
    if (const json::Value* strings = screen.find("localized_strings")) {
        const auto& values = strings->array("localized_strings");
        if (values.size() != originalStrings.size()) {
            throw Error("localized_strings count cannot yet be changed");
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            write_le(root, originalStrings[index].offset,
                     u32(values[index].require("tag"), "localized_strings tag"),
                     "localized_strings tag");
            write_le(root, originalStrings[index].offset + 4,
                     u32(values[index].require("word"), "localized_strings word"),
                     "localized_strings word");
        }
    } else {
        if (originalStrings.size() != 1) {
            throw Error("UI screen requires the localized_strings array");
        }
        write_le(root, originalStrings[0].offset,
                 u32(screen.require("localized_strings_tag"), "localized_strings_tag"),
                 "localized_strings_tag");
    }

    std::vector<CompiledEntry> output;
    output.reserve(rawViews.size() + 1);
    std::map<std::uint32_t, bool> usedEntries;
    usedEntries.emplace(screenEntry, true);
    for (std::size_t index = 0; index < rawViews.size(); ++index) {
        const json::Value& view = rawViews[index];
        (void)view.object("UI view");
        const std::uint32_t nameHash = u32(view.require("name_hash"), "UI view name_hash");
        const std::uint32_t hierarchyTag = u32(view.require("hierarchy_tag"),
                                               "UI view hierarchy_tag");
        const std::uint32_t hierarchyEntry = u32(view.require("hierarchy_entry"),
                                                 "UI view hierarchy_entry");
        if (u32(view.require("hierarchy_class_id"), "UI hierarchy class_id")
                != kHierarchyClass
            || tag_package(hierarchyTag) != expectedPackageId
            || tag_entry(hierarchyTag) != hierarchyEntry || hierarchyEntry >= entryCount
            || !usedEntries.emplace(hierarchyEntry, true).second) {
            throw Error("UI hierarchy tag/entry mapping is invalid or duplicated");
        }
        write_le(root, originalReferences[index].nameHashOffset, nameHash,
                 "UI view name_hash");
        write_le(root, originalReferences[index].hierarchyTagOffset, hierarchyTag,
                 "UI hierarchy tag");

        std::vector<std::byte> hierarchy = parse_hex_bytes(view.require("header_hex"),
                                                           "UI hierarchy header_hex");
        if (hierarchy.size() != 0x40) throw Error("UI hierarchy header_hex must encode 64 bytes");
        const std::uint16_t nodeCount = static_cast<std::uint16_t>(parse_unsigned(
            view.require("node_count"), "UI hierarchy node_count", 0xFFFF));
        const auto& edges = view.require("edges").array("UI hierarchy edges");
        if (edges.size() > 0xFFFF) throw Error("UI hierarchy has too many edges");
        hierarchy.resize(0x40 + edges.size() * 4);
        write_le(hierarchy, 0, static_cast<std::uint64_t>(hierarchy.size()),
                 "UI hierarchy size");
        write_le(hierarchy, 8, static_cast<std::uint64_t>(edges.size()),
                 "UI hierarchy edge count");
        write_le(hierarchy, 0x18, nodeCount, "UI hierarchy node_count");
        write_le(hierarchy, 0x1A, nodeCount, "UI hierarchy node_count");
        write_le(hierarchy, 0x1C, u32(view.require("widget_table_tag"),
                                      "UI widget_table_tag"), "UI widget_table_tag");
        write_le(hierarchy, 0x30, static_cast<std::uint64_t>(edges.size()),
                 "UI hierarchy capacity");
        for (std::size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
            const json::Value& edge = edges[edgeIndex];
            (void)edge.object("UI hierarchy edge");
            const json::Value& parentValue = edge.require("parent");
            const std::uint16_t parent = parentValue.is_null() ? kRootParent
                : static_cast<std::uint16_t>(parse_unsigned(parentValue, "UI edge parent", 0xFFFF));
            const std::uint16_t child = static_cast<std::uint16_t>(parse_unsigned(
                edge.require("child"), "UI edge child", 0xFFFF));
            write_le(hierarchy, 0x40 + edgeIndex * 4, parent, "UI edge parent");
            write_le(hierarchy, 0x42 + edgeIndex * 4, child, "UI edge child");
        }
        Hierarchy validation;
        if (!parse_hierarchy(hierarchy, validation)) {
            throw Error("compiled UI hierarchy failed structural validation");
        }
        output.push_back(CompiledEntry{hierarchyEntry,
                                       "hierarchy:" + hex(hierarchyTag, 8),
                                       std::move(hierarchy)});
    }
    std::vector<ScreenReference> validationReferences;
    std::vector<Layout::LocalizedStringsReference> validationStrings;
    if (!parse_screen(root, validationReferences, validationStrings)) {
        throw Error("compiled UI screen failed structural validation");
    }
    output.insert(output.begin(), CompiledEntry{screenEntry, "screen", std::move(root)});
    return output;
}

CompiledEntry compile_widget_table(const std::filesystem::path& path,
                                   std::uint16_t expectedPackageId,
                                   std::size_t entryCount) {
    json::Value document;
    try {
        document = json::parse(read_text(path));
    } catch (const json::Error& error) {
        throw Error(path.string() + ": " + error.what());
    }
    if (document.require("format").string("UI widget-table format")
            != "repkg.ui_widget_table.v1"
        || u32(document.require("class_id"), "UI widget-table class_id")
            != kWidgetTableClass) {
        throw Error("unsupported UI widget-table format or class");
    }
    const std::uint32_t tag = u32(document.require("tag"), "UI widget-table tag");
    const std::uint32_t entryIndex = u32(document.require("entry_index"),
                                         "UI widget-table entry_index");
    if (tag_package(tag) != expectedPackageId || tag_entry(tag) != entryIndex
        || entryIndex >= entryCount) {
        throw Error("UI widget-table tag/entry mapping is invalid");
    }
    std::vector<std::byte> bytes = parse_hex_bytes(document.require("template_hex"),
                                                   "UI widget-table template_hex");
    WidgetTable parsed;
    if (!parse_widget_table(bytes, parsed)) {
        throw Error("UI widget-table template is not a supported 0x80804825 payload");
    }
    const auto& arrays = document.require("arrays").array("UI widget-table arrays");
    if (arrays.size() != parsed.arrays.size()) {
        throw Error("UI widget-table array count cannot yet be changed");
    }
    for (std::size_t arrayIndex = 0; arrayIndex < arrays.size(); ++arrayIndex) {
        const json::Value& array = arrays[arrayIndex];
        const WidgetArray& original = parsed.arrays[arrayIndex];
        if (parse_unsigned(array.require("field_offset"), "widget array field_offset",
                           SIZE_MAX) != original.fieldOffset
            || parse_unsigned(array.require("data_offset"), "widget array data_offset",
                              SIZE_MAX) != original.dataOffset
            || u32(array.require("item_class"), "widget array item_class")
                   != original.itemClass
            || parse_unsigned(array.require("item_size"), "widget array item_size", SIZE_MAX)
                   != original.itemSize) {
            throw Error("UI widget-table array metadata cannot yet be changed");
        }
        const auto& items = array.require("items").array("UI widget-table items");
        if (items.size() != original.items.size()) {
            throw Error("UI widget-table item counts cannot yet be changed");
        }
        for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
            const json::Value& item = items[itemIndex];
            const WidgetArrayItem& originalItem = original.items[itemIndex];
            if (parse_unsigned(item.require("offset"), "widget item offset", SIZE_MAX)
                    != originalItem.offset) {
                throw Error("UI widget-table item offsets cannot yet be changed");
            }
            const std::size_t expectedWords = original.itemSize / 4;
            if (float_widget_class(original.itemClass)) {
                const auto& values = item.require("floats_le").array("widget item floats_le");
                if (values.size() != expectedWords) {
                    throw Error("UI widget-table float count does not match item_size");
                }
                for (std::size_t word = 0; word < values.size(); ++word) {
                    const json::Value& scalar = values[word];
                    const std::uint32_t bits = u32(scalar.require("bits"), "widget float bits");
                    const std::uint32_t floatBits = parse_float_text(
                        scalar.require("value"), "widget float value");
                    std::uint32_t originalBits = 0;
                    std::memcpy(&originalBits, originalItem.data.data() + word * 4, 4);
                    std::uint32_t selected = bits;
                    if (bits == originalBits && floatBits != originalBits) selected = floatBits;
                    else if (bits != originalBits && floatBits != originalBits
                             && bits != floatBits) {
                        throw Error("widget float bits and value were edited inconsistently");
                    }
                    write_le(bytes, originalItem.offset + word * 4, selected,
                             "widget float value");
                }
            } else {
                const auto& words = item.require("words_le").array("widget item words_le");
                if (words.size() != expectedWords) {
                    throw Error("UI widget-table item word count does not match item_size");
                }
                for (std::size_t word = 0; word < words.size(); ++word) {
                    write_le(bytes, originalItem.offset + word * 4,
                             u32(words[word], "widget item word"), "widget item word");
                }
            }
            const std::vector<std::byte> tail = parse_hex_bytes(
                item.require("tail_hex"), "widget item tail_hex");
            if (tail.size() != original.itemSize % 4) {
                throw Error("UI widget-table item tail size does not match item_size");
            }
            if (!tail.empty()) {
                std::memcpy(bytes.data() + originalItem.offset + expectedWords * 4,
                            tail.data(), tail.size());
            }
        }
    }
    WidgetTable validation;
    if (!parse_widget_table(bytes, validation)
        || validation.arrays.size() != parsed.arrays.size()) {
        throw Error("compiled UI widget table failed structural validation");
    }
    return CompiledEntry{entryIndex, "widget_table", std::move(bytes)};
}

} // namespace repkg::ui
