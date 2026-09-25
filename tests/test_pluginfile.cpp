// A plugin file's bytes: headers, masters, records, subrecords and the forms
// a script's properties name.

#include <catch2/catch_test_macros.hpp>

#include "core/PluginFile.h"

#include <cstring>

using namespace ft;

namespace
{

// Little-endian bytes, built the way a plugin lays them out.
struct Bytes
{
    std::vector<std::uint8_t> b;

    Bytes &u8(std::uint8_t v)
    {
        b.push_back(v);
        return *this;
    }
    Bytes &u16(std::uint16_t v)
    {
        return u8(static_cast<std::uint8_t>(v & 0xFF)).u8(static_cast<std::uint8_t>(v >> 8));
    }
    Bytes &u32(std::uint32_t v)
    {
        return u16(static_cast<std::uint16_t>(v & 0xFFFF)).u16(static_cast<std::uint16_t>(v >> 16));
    }
    Bytes &tag(std::string_view t)
    {
        for (const char c : t)
            u8(static_cast<std::uint8_t>(c));
        return *this;
    }
    Bytes &wstr(std::string_view s)
    {
        u16(static_cast<std::uint16_t>(s.size()));
        return tag(s);
    }
    Bytes &raw(const Bytes &other)
    {
        b.insert(b.end(), other.b.begin(), other.b.end());
        return *this;
    }
    Bytes &sub(std::string_view type, const Bytes &data)
    {
        return tag(type).u16(static_cast<std::uint16_t>(data.b.size())).raw(data);
    }
    Bytes &record(std::string_view type, std::uint32_t formID, std::uint32_t flags, const Bytes &data)
    {
        return tag(type).u32(static_cast<std::uint32_t>(data.b.size())).u32(flags).u32(formID).u32(0).u32(0).raw(data);
    }
    Bytes &group(std::string_view label, const Bytes &contents)
    {
        return tag("GRUP")
            .u32(static_cast<std::uint32_t>(contents.b.size() + kPluginHeaderSize))
            .tag(label)
            .u32(0)
            .u32(0)
            .u32(0)
            .raw(contents);
    }
    [[nodiscard]] std::span<const std::uint8_t> span() const
    {
        return b;
    }
};

Bytes Zstring(std::string_view s)
{
    Bytes out;
    out.tag(s).u8(0);
    return out;
}

} // namespace

TEST_CASE("a plugin's header names its masters in order", "[pluginfile]")
{
    Bytes hedr;
    hedr.u32(0).u32(0).u32(0);
    Bytes data;
    data.u32(0).u32(0);
    Bytes header;
    header.sub("HEDR", hedr).sub("MAST", Zstring("Skyrim.esm")).sub("DATA", data);
    header.sub("MAST", Zstring("Update.esm")).sub("DATA", data);
    const auto masters = PluginMasters(header.span());
    REQUIRE(masters == std::vector<std::string>{"Skyrim.esm", "Update.esm"});

    Bytes file;
    file.record("TES4", 0, 0, header);
    const auto h = ReadPluginHeader(file.span());
    REQUIRE(h);
    REQUIRE(h->Is("TES4"));
    REQUIRE_FALSE(h->IsGroup());
    REQUIRE(h->size == header.b.size());
    REQUIRE_FALSE(ReadPluginHeader(file.span().first(10)));
}

TEST_CASE("a group's records are read in order, a group within skipped", "[pluginfile]")
{
    Bytes one;
    one.sub("EDID", Zstring("First"));
    Bytes two;
    two.sub("EDID", Zstring("Second"));
    Bytes inner;
    inner.record("MGEF", 0x0300, 0, one);
    Bytes contents;
    contents.record("MGEF", 0x01000800, 0, one).group("XXXX", inner).record("MGEF", 0x01000801, kRecordCompressed, two);
    Bytes top;
    top.group("MGEF", contents);

    const auto h = ReadPluginHeader(top.span());
    REQUIRE(h->Labelled("MGEF"));
    const auto records = RecordsIn(top.span());
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].formID == 0x01000800);
    REQUIRE_FALSE(records[0].Compressed());
    REQUIRE(records[1].formID == 0x01000801);
    REQUIRE(records[1].Compressed());
    REQUIRE(records[1].data.size() == two.b.size());

    // Cut short: what fits is read, and no more.
    REQUIRE(RecordsIn(top.span().first(top.b.size() - 3)).size() == 1);
    REQUIRE(RecordsIn(one.span()).empty());

    // A group claiming less than its own header holds nothing, whatever follows.
    Bytes corrupt;
    corrupt.tag("GRUP").u32(10).tag("MGEF").u32(0).u32(0).u32(0).raw(contents);
    REQUIRE(RecordsIn(corrupt.span()).empty());
}

TEST_CASE("a subrecord is found by type, an XXXX giving its size", "[pluginfile]")
{
    Bytes vmad;
    vmad.u32(0xDEADBEEF);
    Bytes data;
    data.sub("EDID", Zstring("BLO_BloodSacrificeFFSelf")).sub("VMAD", vmad);
    const auto found = Subrecord(data.span(), "VMAD");
    REQUIRE(found.size() == 4);
    REQUIRE(Subrecord(data.span(), "DATA").empty());

    // A field too large for sixteen bits: XXXX carries its size, and its
    // own size field is zero.
    Bytes big;
    for (int i = 0; i < 70000; ++i)
        big.u8(static_cast<std::uint8_t>(i));
    Bytes size;
    size.u32(static_cast<std::uint32_t>(big.b.size()));
    Bytes large;
    large.sub("XXXX", size).tag("NVNM").u16(0).raw(big).sub("VMAD", vmad);
    REQUIRE(Subrecord(large.span(), "NVNM").size() == big.b.size());
    REQUIRE(Subrecord(large.span(), "VMAD").size() == 4);
}

TEST_CASE("a script's properties name forms, in either object format", "[pluginfile]")
{
    // Blood Sacrifice's power effect: dar_simpletogglescript, its
    // TriggerSpell0 the ability, among properties of every other kind.
    Bytes vmad;
    vmad.u16(5).u16(2).u16(1);
    vmad.wstr("dar_simpletogglescript").u8(0).u16(7);
    vmad.wstr("Count").u8(3).u8(1).u32(2);
    vmad.wstr("Label").u8(2).u8(1).wstr("on");
    vmad.wstr("TriggerSpell0").u8(1).u8(1).u16(0).u16(0xFFFF).u32(0x0100098E);
    vmad.wstr("Rate").u8(4).u8(1).u32(0);
    vmad.wstr("Also").u8(11).u8(1).u32(2).u16(0).u16(0xFFFF).u32(0x00012FCD).u16(0).u16(0xFFFF).u32(0x01000900);
    vmad.wstr("Flags").u8(15).u8(1).u32(3).u8(1).u8(0).u8(1);
    vmad.wstr("Enabled").u8(5).u8(1).u8(1);

    const auto forms = ScriptObjectProperties(vmad.span());
    REQUIRE(forms.size() == 3);
    REQUIRE(forms[0].script == "dar_simpletogglescript");
    REQUIRE(forms[0].property == "TriggerSpell0");
    REQUIRE(forms[0].formID == 0x0100098E);
    REQUIRE(forms[1].property == "Also");
    REQUIRE(forms[1].formID == 0x00012FCD);
    REQUIRE(forms[2].formID == 0x01000900);

    // Format 1: the form first, then the alias.
    Bytes old;
    old.u16(5).u16(1).u16(1).wstr("s").u8(0).u16(1).wstr("p").u8(1).u8(1).u32(0x0100098E).u16(0xFFFF).u16(0);
    const auto first = ScriptObjectProperties(old.span());
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].formID == 0x0100098E);

    // A type Skyrim's scripts do not have ends the read with what it had;
    // bytes cut short end it too.
    Bytes odd;
    odd.u16(5).u16(2).u16(1).wstr("s").u8(0).u16(2);
    odd.wstr("a").u8(1).u8(1).u16(0).u16(0xFFFF).u32(0x14);
    odd.wstr("b").u8(6).u8(1).u32(0);
    REQUIRE(ScriptObjectProperties(odd.span()).size() == 1);
    REQUIRE(ScriptObjectProperties(vmad.span().first(40)).empty());
    REQUIRE(ScriptObjectProperties({}).empty());
}
