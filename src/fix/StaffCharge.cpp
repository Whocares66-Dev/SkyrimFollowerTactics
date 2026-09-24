#include "fix/StaffCharge.h"

#include "game/Addresses.h"
#include "game/Log.h"

#include <array>
#include <cstring>
#include <span>

namespace ft::fix
{
namespace
{

namespace addr = game::addr;

// The cost function as each site called it before us: the engine's own, or
// another mod's hook on the same call, which then keeps working and gets
// the actor too. Set once at install, before either site is rewritten.
using CostFn = float (*)(RE::MagicItem *, RE::Actor *);
std::array<CostFn, 2> g_before{};

// What the two sites hand the cost function: the enchantment in rcx, a null
// caster in rdx. The stubs below add the inventory in r8, and the call goes
// on to what the site called before, with the inventory's actor as the
// caster.
template <std::size_t Site>
float StaffUseCost(RE::MagicItem *enchantment, RE::Actor *caster, RE::CombatInventory *inventory)
{
    RE::NiPointer<RE::Actor> actor;
    if (auto *controller = inventory ? inventory->parentController : nullptr;
        controller && controller->inventory == inventory)
        actor = controller->attackerHandle.get();
    return g_before[Site](enchantment, actor ? actor.get() : caster);
}

// A stub between a site's call and StaffUseCost: the inventory into r8,
// then a jump. Placed in the trampoline's memory. `load` is the move of
// the inventory into r8, as the site keeps it.
void *MakeStub(std::span<const std::uint8_t> load, std::uintptr_t target)
{
    constexpr std::array<std::uint8_t, 2> kMovRax{0x48, 0xB8}; // mov rax, imm64
    constexpr std::array<std::uint8_t, 2> kJmpRax{0xFF, 0xE0}; // jmp rax
    const std::size_t size = load.size() + kMovRax.size() + sizeof(target) + kJmpRax.size();
    auto *code = static_cast<std::uint8_t *>(SKSE::GetTrampoline().allocate(size));
    std::uint8_t *at = code;
    std::memcpy(at, load.data(), load.size());
    at += load.size();
    std::memcpy(at, kMovRax.data(), kMovRax.size());
    at += kMovRax.size();
    std::memcpy(at, &target, sizeof(target));
    at += sizeof(target);
    std::memcpy(at, kJmpRax.data(), kJmpRax.size());
    return code;
}

// Wrap one site's call: whatever it called before is kept and called on,
// with the actor. Only a call instruction (E8) is taken; anything else at
// the site is another patch's shape, and is left alone.
template <std::size_t Site>
bool Rewrite(const char *what, REL::RelocationID function, REL::VariantOffset offset,
             std::span<const std::uint8_t> load)
{
    const REL::Relocation<std::uintptr_t> site{function, offset};
    const REL::Relocation<std::uintptr_t> cost{addr::kCalculateMagickaCost};
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(site.address());
    if (bytes[0] != 0xE8)
    {
        log::fix.warn("staff charge: the {} site at {:X} is not a call; left alone", what, site.address());
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    const std::uintptr_t before = site.address() + 5 + static_cast<std::intptr_t>(rel);
    g_before[Site] = reinterpret_cast<CostFn>(before);
    SKSE::GetTrampoline().write_call<5>(
        site.address(),
        reinterpret_cast<std::uintptr_t>(MakeStub(load, reinterpret_cast<std::uintptr_t>(&StaffUseCost<Site>))));
    if (before != cost.address())
        log::fix.info("staff charge: the {} call already went to {:X}, not the cost function; wrapped, and called on",
                      what, before);
    return true;
}

} // namespace

void InstallStaffChargeFix()
{
    if (REL::Module::IsVR() || SKSE::GetTrampoline().empty())
    {
        log::fix.warn("staff charge: not installed here; a staff below its unscaled cost stays out of the AI's list");
        return;
    }
    // SetItemCount keeps the inventory in rbp at its call.
    constexpr std::array<std::uint8_t, 3> kFromRbp{0x49, 0x89, 0xE8}; // mov r8, rbp
    // The rebuild keeps it on its frame, at [rbp + disp32].
    std::array<std::uint8_t, 7> fromFrame{0x4C, 0x8B, 0x85, 0, 0, 0, 0}; // mov r8, [rbp + disp32]
    const auto disp = static_cast<std::int32_t>(addr::kRebuildInventoryFrame.offset());
    std::memcpy(fromFrame.data() + 3, &disp, sizeof(disp));

    const bool count = Rewrite<0>("item count", addr::kSetItemCount, addr::kSetItemCountCostCall, kFromRbp);
    const bool rebuild = Rewrite<1>("rebuild", addr::kRebuildInventory, addr::kRebuildInventoryCostCall, fromFrame);
    log::fix.info("staff charge: the AI's count of a staff asks its cost with perks ({} of 2 sites)",
                  static_cast<int>(count) + static_cast<int>(rebuild));
}

} // namespace ft::fix
