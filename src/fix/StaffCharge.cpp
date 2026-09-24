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

// What the two sites hand the cost function: the enchantment in rcx, a null
// caster in rdx. The stubs below add the inventory in r8.
float StaffUseCost(RE::MagicItem *enchantment, RE::Actor *caster, RE::CombatInventory *inventory)
{
    if (!enchantment)
        return 0.0f;
    RE::NiPointer<RE::Actor> actor;
    if (auto *controller = inventory ? inventory->parentController : nullptr;
        controller && controller->inventory == inventory)
        actor = controller->attackerHandle.get();
    return enchantment->CalculateMagickaCost(actor ? actor.get() : caster);
}

// A stub between a site's call and StaffUseCost: the inventory into r8,
// then a jump. Placed in the trampoline's memory. `load` is the move of
// the inventory into r8, as the site keeps it.
void *MakeStub(std::span<const std::uint8_t> load)
{
    constexpr std::array<std::uint8_t, 2> kMovRax{0x48, 0xB8}; // mov rax, imm64
    constexpr std::array<std::uint8_t, 2> kJmpRax{0xFF, 0xE0}; // jmp rax
    const auto target = reinterpret_cast<std::uintptr_t>(&StaffUseCost);
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

// Rewrite one site's call, if it is still the plain call to the cost
// function: another mod may have changed it.
bool Rewrite(const char *what, REL::RelocationID function, REL::VariantOffset offset,
             std::span<const std::uint8_t> load)
{
    const REL::Relocation<std::uintptr_t> site{function, offset};
    const REL::Relocation<std::uintptr_t> cost{addr::kCalculateMagickaCost};
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(site.address());
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    if (bytes[0] != 0xE8 || site.address() + 5 + rel != cost.address())
    {
        log::fix.warn("staff charge: the {} call at {:X} is not the plain call to the cost function; left alone", what,
                      site.address());
        return false;
    }
    SKSE::GetTrampoline().write_call<5>(site.address(), reinterpret_cast<std::uintptr_t>(MakeStub(load)));
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

    const bool count = Rewrite("item count", addr::kSetItemCount, addr::kSetItemCountCostCall, kFromRbp);
    const bool rebuild = Rewrite("rebuild", addr::kRebuildInventory, addr::kRebuildInventoryCostCall, fromFrame);
    log::fix.info("staff charge: the AI's count of a staff asks its cost with perks ({} of 2 sites)",
                  static_cast<int>(count) + static_cast<int>(rebuild));
}

} // namespace ft::fix
