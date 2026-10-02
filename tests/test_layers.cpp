// The pins in layers, as situations: the player's own beneath an idle
// rule's, beneath a fight's opening, beneath a combat rule's; what each
// request does across the books, what is left when each layer lets go, and
// what the save holds through all of it (core/Loadout.h, Layer;
// core/Watchdog.h, Layers).
//
// Two ways in. The first drives the books alone, each request made by hand
// in the layer the game would make it in. The second drives a follower's
// whole turn -- the production DecideTurn, both lists, real conditions --
// with the action decided applied to the books and the lapse after it, in
// the order game/Tactics.cpp takes them.
//
// No Skyrim, no SKSE, no CommonLibSSE.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Coordinator.h"
#include "core/Watchdog.h"

#include <cstdint>
#include <vector>

using namespace ft;

namespace
{

constexpr std::uint32_t kDagger = 0x1397E;
constexpr std::uint32_t kSword = 0x12EB7;
constexpr std::uint32_t kBow = 0x13985;
constexpr std::uint32_t kShield = 0x12EB6;
constexpr std::uint32_t kTorch = 0x1D4EC;
constexpr std::uint32_t kCuirass = 0x13911;

Holdable Held(std::uint32_t form, Grip grip)
{
    Holdable h;
    h.form = form;
    h.kind = Kind::Weapon;
    h.grip = grip;
    return h;
}

const Holdable kTheDagger = Held(kDagger, Grip::Either);
const Holdable kTheSword = Held(kSword, Grip::Either);
const Holdable kTheBow = Held(kBow, Grip::Both);
const Holdable kTheShield = Held(kShield, Grip::LeftOnly);
const Holdable kTheTorch = Held(kTorch, Grip::LeftOnly);

Holdable Cuirass()
{
    Holdable h;
    h.form = kCuirass;
    h.kind = Kind::Armor;
    h.slots = BipedSlot(32);
    return h;
}

// One follower's books, with each request made in a layer as the game
// makes it (game/Pins.cpp: Wear, ReleaseKind, LapsePins, EnforcePins).
struct Books
{
    std::vector<Pin> pins; // the book in force
    Layers layers;

    void Pin(Layer layer, const Holdable &thing, Hand hand = Hand::None)
    {
        [[maybe_unused]] const auto displaced =
            layers.Apply(pins, layer, PinRequest::Pin, thing, HandsFor(thing.grip, hand), false, true);
    }
    void Unequip(Layer layer, Kind kind, Hand hands = Hand::None)
    {
        [[maybe_unused]] const auto released = layers.Release(pins, layer, kind, hands, BodyPart::All);
    }
    // A list's turn, with what its rules hold.
    Settled Idle(const Wants &wants = {})
    {
        return layers.Lapse(pins, false, wants, true).value_or(Settled{});
    }
    Settled Combat(const Wants &wants = {})
    {
        return layers.Lapse(pins, true, wants, true).value_or(Settled{});
    }
    Settled Fight(bool fighting)
    {
        return layers.Note(pins, fighting).value_or(Settled{});
    }

    // Pinned in force, in exactly these hands.
    [[nodiscard]] bool Has(std::uint32_t form, Hand hands = Hand::None) const
    {
        const ft::Pin *pin = FindPin(pins, form);
        return pin && pin->hands == hands;
    }
    [[nodiscard]] bool Saved(std::uint32_t form) const
    {
        return FindPin(layers.Saved(), form) != nullptr;
    }
};

Wants Holding(std::initializer_list<Pin> pins)
{
    Wants wants;
    wants.pins = pins;
    return wants;
}

} // namespace

TEST_CASE("idle over normal: a rule's pin shadows the player's, and gives it back", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Normal, kTheShield);
    lydia.Pin(Layer::Normal, Cuirass());

    // Night: the rule's torch takes the shield's hand. The shield is out of
    // force and still the player's.
    lydia.Pin(Layer::Idle, kTheTorch);
    REQUIRE(lydia.Has(kTorch, Hand::Left));
    REQUIRE_FALSE(lydia.Has(kShield, Hand::Left));
    REQUIRE(lydia.Has(kCuirass));
    REQUIRE(lydia.Saved(kShield));
    REQUIRE_FALSE(lydia.Saved(kTorch));
    REQUIRE_FALSE(lydia.Idle(Holding({{kTheTorch, Hand::Left}})).Changed());

    // Day: the torch makes way and the shield is back in its hand.
    const Settled day = lydia.Idle();
    REQUIRE(day.released.size() == 1);
    REQUIRE(day.released[0].form == kTorch);
    REQUIRE(day.released[0].takeOff);
    REQUIRE(day.restored.size() == 1);
    REQUIRE(lydia.Has(kShield, Hand::Left));
    REQUIRE(lydia.Has(kCuirass));
    REQUIRE(lydia.pins.size() == 2);
}

TEST_CASE("the player's click under a holding idle rule acts at once, and the rule takes the hand again", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Idle, kTheTorch);

    // The panel pins the shield: in force now, whatever the rule says.
    lydia.Pin(Layer::Normal, kTheShield);
    REQUIRE(lydia.Has(kShield, Hand::Left));
    REQUIRE_FALSE(lydia.Has(kTorch, Hand::Left));
    // The rule still holds, and its turn does not make its pin for it...
    REQUIRE_FALSE(lydia.Idle(Holding({{kTheTorch, Hand::Left}})).Changed());
    REQUIRE(lydia.Has(kShield, Hand::Left));
    // ...its own equip does, over the click, which is kept.
    lydia.Pin(Layer::Idle, kTheTorch);
    REQUIRE(lydia.Has(kTorch, Hand::Left));
    REQUIRE(lydia.Saved(kShield));

    // And the click is what is left when the rule lets go.
    REQUIRE(lydia.Idle().restored.size() == 1);
    REQUIRE(lydia.Has(kShield, Hand::Left));
}

TEST_CASE("fight over idle over normal, and back down one layer at a time", "[layers]")
{
    // The player's dagger; idle rules put a sword over it and a torch in
    // the other hand.
    Books lydia;
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    lydia.Pin(Layer::Idle, kTheSword, Hand::Right);
    lydia.Pin(Layer::Idle, kTheTorch);
    REQUIRE(lydia.Has(kSword, Hand::Right));
    REQUIRE(lydia.Has(kTorch, Hand::Left));
    REQUIRE(lydia.pins.size() == 2);

    // A fight: the idle rules are not asked in it, and their pins stand
    // though nothing says it holds them.
    REQUIRE_FALSE(lydia.Fight(true).Changed());
    REQUIRE_FALSE(lydia.layers.Lapse(lydia.pins, false, {}, true));
    REQUIRE(lydia.pins.size() == 2);

    // A combat rule's bow takes both hands, over both.
    lydia.Pin(Layer::Fight, kTheBow);
    REQUIRE(lydia.Has(kBow, Hand::Both));
    REQUIRE(lydia.pins.size() == 1);
    REQUIRE_FALSE(lydia.Combat(Holding({{kTheBow, Hand::Both}})).Changed());

    // The rule lets go mid-fight: the idle layer comes back, not the
    // player's dagger beneath it.
    const Settled lapsed = lydia.Combat();
    REQUIRE(lapsed.released.size() == 1);
    REQUIRE(lapsed.released[0].takeOff);
    REQUIRE(lapsed.restored.size() == 2);
    REQUIRE(lydia.Has(kSword, Hand::Right));
    REQUIRE(lydia.Has(kTorch, Hand::Left));
    REQUIRE(FindPin(lydia.pins, kDagger) == nullptr);

    // Pinned again, and this time the fight ends over it: the same.
    lydia.Pin(Layer::Fight, kTheBow);
    const Settled after = lydia.Fight(false);
    REQUIRE(after.released.size() == 1);
    REQUIRE(after.restored.size() == 2);
    REQUIRE(lydia.Has(kSword, Hand::Right));
    REQUIRE(lydia.Has(kTorch, Hand::Left));
    // Out of the fight the combat list has nothing to let go.
    REQUIRE_FALSE(lydia.layers.Lapse(lydia.pins, true, {}, true));

    // The torch rule stops holding: let go where it is, nothing beneath it.
    const Settled dawn = lydia.Idle(Holding({{kTheSword, Hand::Right}}));
    REQUIRE(dawn.released.size() == 1);
    REQUIRE(dawn.released[0].form == kTorch);
    REQUIRE_FALSE(dawn.released[0].takeOff);
    REQUIRE(dawn.restored.empty());

    // Then the sword rule: the player's dagger at last.
    const Settled last = lydia.Idle();
    REQUIRE(last.released[0].form == kSword);
    REQUIRE(last.released[0].takeOff);
    REQUIRE(lydia.Has(kDagger, Hand::Right));
    REQUIRE(lydia.pins.size() == 1);
}

TEST_CASE("the opening lasts the fight, under the standing rules and no longer than it", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Normal, Cuirass());
    REQUIRE_FALSE(lydia.Fight(true).Changed());

    // Combat start: the bow.
    lydia.Pin(Layer::Opening, kTheBow);
    REQUIRE(lydia.Has(kBow, Hand::Both));
    // No standing rule holds anything, and the bow is not theirs to drop.
    REQUIRE_FALSE(lydia.Combat().Changed());

    // The enemy closes: a standing rule's sword, over the bow.
    lydia.Pin(Layer::Fight, kTheSword, Hand::Right);
    REQUIRE(lydia.Has(kSword, Hand::Right));
    REQUIRE(FindPin(lydia.pins, kBow) == nullptr);
    REQUIRE_FALSE(lydia.Combat(Holding({{kTheSword, Hand::Right}})).Changed());

    // It draws off: the sword makes way for the bow.
    const Settled back = lydia.Combat();
    REQUIRE(back.released[0].form == kSword);
    REQUIRE(back.released[0].takeOff);
    REQUIRE(lydia.Has(kBow, Hand::Both));

    // The fight ends: the bow is let go in place, the player's cuirass was
    // never anyone else's.
    const Settled over = lydia.Fight(false);
    REQUIRE(over.released.size() == 1);
    REQUIRE(over.released[0].form == kBow);
    REQUIRE_FALSE(over.released[0].takeOff);
    REQUIRE(lydia.pins.size() == 1);
    REQUIRE(lydia.Has(kCuirass));
}

TEST_CASE("the player's click mid-fight is in every book, and outlasts every layer over it", "[layers]")
{
    Books lydia;
    REQUIRE_FALSE(lydia.Fight(true).Changed());
    lydia.Pin(Layer::Opening, kTheBow);
    lydia.Pin(Layer::Fight, kTheSword, Hand::Right);

    // The panel pins the dagger in the hand the rule's sword holds: at
    // once, and over the opening's bow, which wanted that hand too and
    // whose rule will not speak again.
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    REQUIRE(lydia.Has(kDagger, Hand::Right));
    REQUIRE(lydia.pins.size() == 1);
    REQUIRE(lydia.Saved(kDagger));

    // The sword rule still holds and pins over it again; when it lets go,
    // the dagger is what comes back, not the bow.
    lydia.Pin(Layer::Fight, kTheSword, Hand::Right);
    REQUIRE(lydia.Has(kSword, Hand::Right));
    REQUIRE(lydia.Combat().restored.size() == 1);
    REQUIRE(lydia.Has(kDagger, Hand::Right));
    REQUIRE(FindPin(lydia.pins, kBow) == nullptr);

    // Through the fight's end, and an idle turn with nothing held.
    REQUIRE_FALSE(lydia.Fight(false).Changed());
    REQUIRE_FALSE(lydia.Idle().Changed());
    REQUIRE(lydia.Has(kDagger, Hand::Right));
}

TEST_CASE("a Combat end rule's pin is the player's own: it outlasts the idle rules and is saved", "[layers]")
{
    Books lydia;
    REQUIRE_FALSE(lydia.Fight(true).Changed());
    lydia.Pin(Layer::Fight, kTheBow);
    REQUIRE(lydia.Fight(false).Changed());

    // The fight is over, and the rule says: the sword.
    lydia.Pin(Layer::Normal, kTheSword, Hand::Right);
    REQUIRE(lydia.Saved(kSword));
    // No idle rule holds it, and none needs to.
    REQUIRE_FALSE(lydia.Idle().Changed());
    REQUIRE(lydia.Has(kSword, Hand::Right));

    // An idle rule's bow goes over it and gives it back, as over any pin
    // of the player's.
    lydia.Pin(Layer::Idle, kTheBow);
    REQUIRE(lydia.Has(kBow, Hand::Both));
    REQUIRE(lydia.Idle().restored.size() == 1);
    REQUIRE(lydia.Has(kSword, Hand::Right));
}

TEST_CASE("an Unequip lasts as its layer does: an idle rule's while it holds, the player's own for good", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    lydia.Pin(Layer::Normal, kTheShield);

    // An idle rule bares the right hand: the dagger's pin is out of force,
    // still the player's, and kept out while the rule holds.
    lydia.Unequip(Layer::Idle, Kind::Weapon, Hand::Right);
    REQUIRE(FindPin(lydia.pins, kDagger) == nullptr);
    REQUIRE(lydia.Has(kShield, Hand::Left));
    REQUIRE(lydia.Saved(kDagger));
    Wants bare;
    bare.holes.push_back({Kind::Weapon, Hand::Right, BodyPart::All});
    REQUIRE_FALSE(lydia.Idle(bare).Changed());
    // The rule stops holding: back it comes.
    REQUIRE(lydia.Idle().restored.size() == 1);
    REQUIRE(lydia.Has(kDagger, Hand::Right));

    // A Combat end rule's Unequip is the player's own book changed.
    lydia.Unequip(Layer::Normal, Kind::Weapon, Hand::Left);
    REQUIRE_FALSE(lydia.Saved(kShield));
    REQUIRE_FALSE(lydia.Idle().Changed());
    REQUIRE(FindPin(lydia.pins, kShield) == nullptr);
    REQUIRE(lydia.Has(kDagger, Hand::Right));
}

TEST_CASE("a Combat start Unequip keeps the hand free for the fight, and the pin comes back after it", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    REQUIRE_FALSE(lydia.Fight(true).Changed());

    lydia.Unequip(Layer::Opening, Kind::Weapon, Hand::Right);
    REQUIRE(lydia.pins.empty());
    // No standing rule holds the hand, and the dagger stays out of it.
    REQUIRE_FALSE(lydia.Combat().Changed());
    // A standing rule's sword comes and goes over the bare hand: let go in
    // place, with nothing to make way for.
    lydia.Pin(Layer::Fight, kTheSword, Hand::Right);
    const Settled lapsed = lydia.Combat();
    REQUIRE(lapsed.released.size() == 1);
    REQUIRE_FALSE(lapsed.released[0].takeOff);
    REQUIRE(lapsed.restored.empty());

    // The fight ends, and the dagger is the player's again.
    REQUIRE(lydia.Fight(false).restored.size() == 1);
    REQUIRE(lydia.Has(kDagger, Hand::Right));
}

TEST_CASE("the save holds the player's own, whatever lies over it and whenever it is written", "[layers]")
{
    Books lydia;
    const auto saved = [&] { return lydia.layers.Saved().size(); };
    lydia.Pin(Layer::Normal, Cuirass());
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    REQUIRE(saved() == 2);

    lydia.Pin(Layer::Idle, kTheTorch);
    lydia.Pin(Layer::Idle, kTheSword, Hand::Right); // over the dagger
    REQUIRE(saved() == 2);
    REQUIRE(lydia.Saved(kDagger));

    REQUIRE_FALSE(lydia.Fight(true).Changed());
    lydia.Pin(Layer::Opening, kTheBow);
    lydia.Pin(Layer::Fight, kTheSword, Hand::Right);
    REQUIRE(saved() == 2);
    REQUIRE(lydia.Saved(kDagger));
    REQUIRE(lydia.Saved(kCuirass));

    // Taken back from that save, the book is the player's own and no more;
    // the rules that hold make their pins again.
    Books loaded;
    for (const Pin &pin : lydia.layers.Saved())
        loaded.layers.Adopt(loaded.pins, pin.thing, pin.hands);
    REQUIRE(loaded.pins.size() == 2);
    REQUIRE(loaded.Has(kDagger, Hand::Right));
    REQUIRE(loaded.Has(kCuirass));
    REQUIRE_FALSE(loaded.Idle().Changed());
}

TEST_CASE("a shadowed pin whose thing is gone does not come back", "[layers]")
{
    // The player's shield under the rule's torch, and the shield sold.
    Books lydia;
    lydia.Pin(Layer::Normal, kTheShield);
    lydia.Pin(Layer::Idle, kTheTorch);
    lydia.layers.Forget(kTheShield);
    REQUIRE_FALSE(lydia.Saved(kShield));

    const Settled day = lydia.Idle();
    REQUIRE(day.released.size() == 1);
    REQUIRE_FALSE(day.released[0].takeOff);
    REQUIRE(day.restored.empty());
    REQUIRE(lydia.pins.empty());

    // The same of a form no plugin defines any more, in a fight's books.
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    REQUIRE_FALSE(lydia.Fight(true).Changed());
    lydia.Pin(Layer::Opening, kTheBow);
    lydia.layers.ForgetIf([](const Pin &pin) { return pin.thing.form == kDagger || pin.thing.form == kBow; });
    REQUIRE(lydia.layers.Saved().empty());
    REQUIRE(lydia.layers.fight.before.empty());
    REQUIRE(lydia.layers.fight.opening.empty());
}

TEST_CASE("what a layer's pin lies over: the book its displaced pins are still held in", "[layers]")
{
    Books lydia;
    lydia.Pin(Layer::Normal, kTheDagger, Hand::Right);
    // Out of a fight: the player's own under an idle rule, nothing under
    // the player's own, and no fight to lie over.
    REQUIRE(lydia.layers.Beneath(Layer::Idle) == &lydia.layers.idle.normal);
    REQUIRE(lydia.layers.Beneath(Layer::Normal) == nullptr);
    REQUIRE(lydia.layers.Beneath(Layer::Opening) == nullptr);
    REQUIRE(lydia.layers.Beneath(Layer::Fight) == nullptr);

    REQUIRE_FALSE(lydia.Fight(true).Changed());
    REQUIRE(lydia.layers.Beneath(Layer::Opening) == &lydia.layers.fight.before);
    REQUIRE(lydia.layers.Beneath(Layer::Fight) == &lydia.layers.fight.opening);
    // The dagger is in both: what a Combat start rule or a standing one
    // displaces there is overridden, not lost.
    REQUIRE(FindPin(*lydia.layers.Beneath(Layer::Opening), kDagger) != nullptr);
    REQUIRE(FindPin(*lydia.layers.Beneath(Layer::Fight), kDagger) != nullptr);
}

// ---- A follower's whole turn.

namespace
{

// A follower under both lists, a turn at a time. The pin pass notes the
// fight's edges before the rules (game/Tactics.cpp, Tick: KeepPins comes
// first); the turn is the production DecideTurn; the action it decides is
// made in its rule's layer (Execute, by LayerOf); and the lapse follows the
// action (RunTurn). That order is the game's, copied here. Everything
// called is the core's own.
struct Follower
{
    ActorRules rules;
    ActorRun run;
    Snapshot base = test::Healthy();
    Books books;
    double now{100.0};
    bool fighting{false};
    bool down{false}; // bleeding out: held
    bool idleOn{true};

    Follower()
    {
        rules.idle.moment = Moment::Idle;
        base.inCombat = false;
        base.timeOfDay = TimeKind::Night;
        for (const Holdable &thing : {kTheDagger, kTheSword, kTheBow, kTheShield, kTheTorch})
            base.loadout.push_back(thing);
    }

    TickResult Tick()
    {
        now += 0.5;
        [[maybe_unused]] const Settled edge = books.Fight(fighting);

        TickFacts facts;
        facts.now.fighting = fighting;
        facts.now.held = down;
        facts.now.idleEnabled = idleOn;
        facts.now.idleHasRules = !rules.idle.rules.empty();
        const auto snapshot = [&](Moment) {
            Snapshot s = base;
            s.now = now;
            s.inCombat = fighting;
            s.pins = books.pins;
            s.worn = books.pins;
            return s;
        };
        TickResult turn = DecideTurn(run, rules, facts, now, snapshot);
        if (turn.Fired())
        {
            const Action &a = turn.decision.step->action;
            const Layer layer = LayerOf(*turn.plan.list, turn.decision.rule.predicate);
            REQUIRE(IsEquip(a.kind));
            if (a.form == 0)
                books.Unequip(layer, KindOf(a.kind), TakesHand(a.kind) ? a.hand : Hand::None);
            else
                books.Pin(layer, *FindHoldable(base.loadout, a.form), a.hand);
            NoteOutcome(run, turn.decision, ActionOutcome::Performed);
        }
        if (turn.holds) [[maybe_unused]]
            const auto lapsed =
                books.layers.Lapse(books.pins, turn.holds->list == Moment::Combat, turn.holds->wants, true);
        return turn;
    }
};

Rule Equip(std::uint32_t form, Hand hand)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::EquipWeapon;
    r.FirstAction().form = form;
    r.FirstAction().hand = hand;
    return r;
}

// IF Self: Time is Night THEN equip the torch -- issue #21's rule.
Rule TorchAtNight()
{
    Rule r = Equip(kTorch, Hand::Left);
    r.predicate = PredicateKind::Time;
    r.timeKind = TimeKind::Night;
    return r;
}

} // namespace

TEST_CASE("a torch lit by a night rule is let go at dawn", "[layers][turn]")
{
    // Issue #21. Until the rule's pin lapsed with its condition the torch
    // stayed pinned through the day, and the watchdog put it back every
    // tick against the engine putting it away.
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());

    REQUIRE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    // Through the night: done, held, nothing more to do.
    for (int i = 0; i < 4; ++i)
        REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    REQUIRE(lydia.books.layers.Saved().empty());

    // Dawn: no pin, and the engine is left to put the torch away.
    lydia.base.timeOfDay = TimeKind::Morning;
    REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.pins.empty());
    // And dusk again: the rule makes its pin again.
    lydia.base.timeOfDay = TimeKind::Night;
    REQUIRE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
}

TEST_CASE("the player's shield under the night rule's torch is back at dawn", "[layers][turn]")
{
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());
    lydia.books.Pin(Layer::Normal, kTheShield);

    REQUIRE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    REQUIRE(FindPin(lydia.books.pins, kShield) == nullptr);

    lydia.base.timeOfDay = TimeKind::Morning;
    REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kShield, Hand::Left));
    REQUIRE(lydia.books.pins.size() == 1);
}

TEST_CASE("an idle list switched off lets its pins go; a follower held keeps them", "[layers][turn]")
{
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());
    REQUIRE(lydia.Tick().Fired());

    // Bleeding out: nothing is asked, and nothing changes.
    lydia.down = true;
    REQUIRE_FALSE(lydia.Tick());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    lydia.down = false;

    // The list switched off: its rules hold nothing.
    lydia.idleOn = false;
    REQUIRE_FALSE(lydia.Tick());
    REQUIRE(lydia.books.pins.empty());

    // Its last rule deleted does the same.
    lydia.idleOn = true;
    REQUIRE(lydia.Tick().Fired());
    lydia.rules.idle.rules.clear();
    REQUIRE_FALSE(lydia.Tick());
    REQUIRE(lydia.books.pins.empty());
}

TEST_CASE("a night, a fight in it, and the morning after: every layer in turn", "[layers][turn]")
{
    // Idle: the torch at night. Combat: the bow at the start; the sword
    // while the enemy is fresh. The player's own: a dagger.
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());
    Rule opening = Equip(kBow, Hand::Both);
    opening.predicate = PredicateKind::CombatBegins;
    Rule close = Equip(kSword, Hand::Right);
    close.subject = SubjectKind::Enemy;
    close.predicate = PredicateKind::HealthPctAbove;
    close.conditionArg = 0.5f;
    lydia.rules.combat.rules = {opening, close};
    lydia.books.Pin(Layer::Normal, kTheDagger, Hand::Right);
    const auto &pins = lydia.books.pins;

    // Night: the torch beside the dagger.
    REQUIRE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    REQUIRE(lydia.books.Has(kDagger, Hand::Right));

    // The fight's first turn is the Combat start rule's: the bow, over both.
    lydia.fighting = true;
    lydia.base.enemies.push_back({0x101, {100.0f, 100.0f}, 200.0f});
    TickResult turn = lydia.Tick();
    REQUIRE(turn.plan.began);
    REQUIRE(turn.decision.actionForm() == kBow);
    REQUIRE(lydia.books.Has(kBow, Hand::Both));
    REQUIRE(pins.size() == 1);

    // Then the standing rule: the sword over the bow, the left hand free.
    turn = lydia.Tick();
    REQUIRE(turn.decision.actionForm() == kSword);
    REQUIRE(lydia.books.Has(kSword, Hand::Right));
    REQUIRE(pins.size() == 1);
    REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kSword, Hand::Right));

    // The enemy is hurt: the sword rule lets go, and the opening's bow is
    // back. Neither the idle torch nor the player's dagger is, mid-fight.
    lydia.base.enemies[0].health = {40.0f, 100.0f};
    REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kBow, Hand::Both));
    REQUIRE(pins.size() == 1);
    // What a save written now would hold: the dagger, and no rule's pin.
    REQUIRE(lydia.books.layers.Saved().size() == 1);
    REQUIRE(lydia.books.Saved(kDagger));

    // Day breaks during the fight, and then the fight ends: what the fight
    // found comes back whole, the night's torch with it.
    lydia.base.timeOfDay = TimeKind::Morning;
    lydia.fighting = false;
    lydia.base.enemies.clear();
    turn = lydia.Tick();
    REQUIRE(turn.plan.ended);
    REQUIRE_FALSE(turn.holds);
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    REQUIRE(lydia.books.Has(kDagger, Hand::Right));

    // The idle list's next turn finds it morning, and the torch is let go.
    REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(pins.size() == 1);
    REQUIRE(lydia.books.Has(kDagger, Hand::Right));
}

TEST_CASE("a Combat end rule's equip stays through the idle turns after it", "[layers][turn]")
{
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());
    Rule after = Equip(kSword, Hand::Right);
    after.predicate = PredicateKind::CombatEnds;
    lydia.rules.combat.rules = {after};
    lydia.base.timeOfDay = TimeKind::Morning;

    lydia.fighting = true;
    REQUIRE_FALSE(lydia.Tick().Fired());
    lydia.fighting = false;
    const TickResult farewell = lydia.Tick();
    REQUIRE(farewell.plan.ended);
    REQUIRE(farewell.decision.actionForm() == kSword);
    REQUIRE(lydia.books.Has(kSword, Hand::Right));
    REQUIRE(lydia.books.Saved(kSword));

    // The idle list's turns: no idle rule holds the sword, and it stays.
    for (int i = 0; i < 3; ++i)
        REQUIRE_FALSE(lydia.Tick().Fired());
    REQUIRE(lydia.books.Has(kSword, Hand::Right));
}

TEST_CASE("two idle rules for one hand: the one above holds it, and the one beneath takes over when it lets go",
          "[layers][turn]")
{
    // The torch at night, else the shield. Dusk and dawn change which rule
    // holds the hand, and neither change puts anything on in between.
    Follower lydia;
    lydia.rules.idle.rules.push_back(TorchAtNight());
    lydia.rules.idle.rules.push_back(Equip(kShield, Hand::Left));

    REQUIRE(lydia.Tick().decision.actionForm() == kTorch);
    REQUIRE_FALSE(lydia.Tick().Fired()); // the shield rule is outranked
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));

    // Dawn: the torch lapses in place, and the shield rule's own equip
    // takes the hand.
    lydia.base.timeOfDay = TimeKind::Morning;
    TickResult turn = lydia.Tick();
    REQUIRE(turn.decision.actionForm() == kShield);
    REQUIRE(lydia.books.Has(kShield, Hand::Left));
    REQUIRE(lydia.books.pins.size() == 1);

    // Dusk: the torch rule is above, and takes it back.
    lydia.base.timeOfDay = TimeKind::Night;
    turn = lydia.Tick();
    REQUIRE(turn.decision.actionForm() == kTorch);
    REQUIRE(lydia.books.Has(kTorch, Hand::Left));
    REQUIRE(lydia.books.pins.size() == 1);
}
