#include "PluginFile.h"

#include <algorithm>
#include <cstring>

namespace ft
{
namespace
{

// A little-endian cursor over bytes that reads nothing past the end: a
// read that would is refused and leaves the cursor spent.
class Cursor
{
  public:
    explicit Cursor(std::span<const std::uint8_t> bytes) noexcept : m_bytes(bytes)
    {
    }

    [[nodiscard]] bool Ok() const noexcept
    {
        return m_ok;
    }
    [[nodiscard]] std::size_t Left() const noexcept
    {
        return m_ok ? m_bytes.size() - m_at : 0;
    }

    template <class T> [[nodiscard]] T Read() noexcept
    {
        T value{};
        if (!Take(sizeof(T)))
            return value;
        std::memcpy(&value, m_bytes.data() + m_at - sizeof(T), sizeof(T));
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> Bytes(std::size_t n) noexcept
    {
        if (!Take(n))
            return {};
        return m_bytes.subspan(m_at - n, n);
    }

    // A script's string: a 16-bit length, then that many bytes.
    [[nodiscard]] std::string WString()
    {
        const auto n = Read<std::uint16_t>();
        const auto bytes = Bytes(n);
        return {bytes.begin(), bytes.end()};
    }

    bool Skip(std::size_t n) noexcept
    {
        return Take(n);
    }

  private:
    bool Take(std::size_t n) noexcept
    {
        if (!m_ok || m_bytes.size() - m_at < n)
        {
            m_ok = false;
            return false;
        }
        m_at += n;
        return true;
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_at{0};
    bool m_ok{true};
};

bool Same(const std::array<char, 4> &a, std::string_view b) noexcept
{
    return b.size() == 4 && std::equal(a.begin(), a.end(), b.begin());
}

} // namespace

bool PluginHeader::IsGroup() const noexcept
{
    return Is("GRUP");
}

bool PluginHeader::Is(std::string_view t) const noexcept
{
    return Same(type, t);
}

bool PluginHeader::Labelled(std::string_view t) const noexcept
{
    return IsGroup() && Same(label, t);
}

std::optional<PluginHeader> ReadPluginHeader(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() < kPluginHeaderSize)
        return std::nullopt;
    PluginHeader h;
    std::memcpy(h.type.data(), bytes.data(), 4);
    std::memcpy(&h.size, bytes.data() + 4, 4);
    std::memcpy(&h.flags, bytes.data() + 8, 4);
    std::memcpy(h.label.data(), bytes.data() + 8, 4);
    std::memcpy(&h.formID, bytes.data() + 12, 4);
    return h;
}

std::vector<std::string> PluginMasters(std::span<const std::uint8_t> headerData)
{
    std::vector<std::string> masters;
    Cursor at(headerData);
    while (at.Left() >= 6)
    {
        const auto type = at.Bytes(4);
        const auto size = at.Read<std::uint16_t>();
        const auto data = at.Bytes(size);
        if (!at.Ok())
            break;
        if (std::equal(type.begin(), type.end(), "MAST"))
        {
            // A zero-terminated name.
            const auto end = std::find(data.begin(), data.end(), std::uint8_t{0});
            masters.emplace_back(data.begin(), end);
        }
    }
    return masters;
}

std::vector<PluginRecord> RecordsIn(std::span<const std::uint8_t> group)
{
    std::vector<PluginRecord> records;
    const auto head = ReadPluginHeader(group);
    if (!head || !head->IsGroup())
        return records;
    const std::size_t end = (std::min<std::size_t>)(group.size(), head->size);
    std::size_t at = kPluginHeaderSize;
    // A group whose size is less than its own header ends before `at`.
    while (at <= end)
    {
        const auto h = ReadPluginHeader(group.subspan(at, end - at));
        if (!h)
            break;
        if (h->IsGroup())
        {
            if (h->size < kPluginHeaderSize || end - at < h->size)
                break;
            at += h->size;
            continue;
        }
        if (end - at - kPluginHeaderSize < h->size)
            break;
        records.push_back({h->formID, h->flags, group.subspan(at + kPluginHeaderSize, h->size)});
        at += kPluginHeaderSize + h->size;
    }
    return records;
}

std::span<const std::uint8_t> Subrecord(std::span<const std::uint8_t> data, std::string_view type)
{
    Cursor at(data);
    std::uint32_t large = 0;
    while (at.Left() >= 6)
    {
        const auto tag = at.Bytes(4);
        const std::uint16_t size = at.Read<std::uint16_t>();
        const bool xxxx = std::equal(tag.begin(), tag.end(), "XXXX");
        const auto body = at.Bytes(large != 0 ? large : size);
        if (!at.Ok())
            break;
        large = 0;
        if (xxxx && body.size() == 4)
        {
            std::memcpy(&large, body.data(), 4);
            continue;
        }
        if (type.size() == 4 && std::equal(tag.begin(), tag.end(), type.begin()))
            return body;
    }
    return {};
}

std::vector<ScriptObjectProperty> ScriptObjectProperties(std::span<const std::uint8_t> vmad)
{
    std::vector<ScriptObjectProperty> out;
    Cursor at(vmad);
    const auto version = at.Read<std::int16_t>();
    const auto objectFormat = at.Read<std::int16_t>();
    const auto scripts = at.Read<std::uint16_t>();
    // An Object value: the form and an alias, in the order the format
    // says; 2 puts the form last.
    const auto object = [&]() -> std::uint32_t {
        if (objectFormat == 1)
        {
            const auto form = at.Read<std::uint32_t>();
            at.Skip(4);
            return form;
        }
        at.Skip(4);
        return at.Read<std::uint32_t>();
    };
    for (std::uint16_t s = 0; s < scripts && at.Ok(); ++s)
    {
        const std::string script = at.WString();
        if (version >= 4)
            at.Skip(1); // the script's status
        const auto properties = at.Read<std::uint16_t>();
        for (std::uint16_t p = 0; p < properties && at.Ok(); ++p)
        {
            const std::string property = at.WString();
            const auto type = at.Read<std::uint8_t>();
            if (version >= 4)
                at.Skip(1); // the property's status
            switch (type)
            {
            case 1: // Object
                if (const auto form = object(); at.Ok())
                    out.push_back({script, property, form});
                break;
            case 2: // String
                (void)at.WString();
                break;
            case 3: // Int
            case 4: // Float
                at.Skip(4);
                break;
            case 5: // Bool
                at.Skip(1);
                break;
            case 11: { // Object array
                const auto count = at.Read<std::uint32_t>();
                for (std::uint32_t i = 0; i < count && at.Ok(); ++i)
                    if (const auto form = object(); at.Ok())
                        out.push_back({script, property, form});
                break;
            }
            case 12: { // String array
                const auto count = at.Read<std::uint32_t>();
                for (std::uint32_t i = 0; i < count && at.Ok(); ++i)
                    (void)at.WString();
                break;
            }
            case 13: // Int array
            case 14: // Float array
                at.Skip(std::size_t{4} * at.Read<std::uint32_t>());
                break;
            case 15: // Bool array
                at.Skip(at.Read<std::uint32_t>());
                break;
            default:
                return out;
            }
        }
    }
    return out;
}

} // namespace ft
