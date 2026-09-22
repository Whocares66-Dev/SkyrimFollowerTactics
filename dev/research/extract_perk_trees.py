"""The 18 vanilla skill trees, read through houseCARL, as the data the core reads.

    python dev/research/extract_perk_trees.py --fetch   # query houseCARL (read-only), then convert
    python dev/research/extract_perk_trees.py           # convert the dumps already here

Fetching reads Skyrim.esm's ActorValueInformation records for the 18 skills
and every perk their trees name, following each perk's NextPerk chain, into
avif-raw.json and perks-raw.json (large, not committed). Converting writes:

  perk-trees.json   every node and rank, conditions and effects as read
  perk-trees.txt    the same, readable: one line per node
  ../../tests/progression/data/vanilla-perks.json
                    what the tests read (tests/progression/PerkData.h): each node's
                    place and ranks, conditions as the core models them

Nothing is written outside this repository and no plugin is modified; the
houseCARL tools used are read_record and batch_record_detail, both read-only.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
HOUSECARL = r"C:\modding\houseCARL\housecarl\server\housecarl-mcp.exe"
MO2 = r"C:\modding\MO2"

SKILLS = {  # AVIF local id -> the core's skill key
    "00044C": "OneHanded",
    "00044D": "TwoHanded",
    "00044E": "Archery",
    "00044F": "Block",
    "000450": "Smithing",
    "000451": "HeavyArmor",
    "000452": "LightArmor",
    "000453": "Pickpocket",
    "000454": "Lockpicking",
    "000455": "Sneak",
    "000456": "Alchemy",
    "000457": "Speech",
    "000458": "Alteration",
    "000459": "Conjuration",
    "00045A": "Destruction",
    "00045B": "Illusion",
    "00045C": "Restoration",
    "00045D": "Enchanting",
}
SKILL_AV = {
    k: i + 6
    for i, k in enumerate(
        [
            "OneHanded",
            "TwoHanded",
            "Archery",
            "Block",
            "Smithing",
            "HeavyArmor",
            "LightArmor",
            "Pickpocket",
            "Lockpicking",
            "Sneak",
            "Alchemy",
            "Speech",
            "Alteration",
            "Conjuration",
            "Destruction",
            "Illusion",
            "Restoration",
            "Enchanting",
        ]
    )
}

# BGSEntryPoint's numbering (CommonLibSSE-NG, RE/B/BGSEntryPoint.h), by the
# names houseCARL (Mutagen) gives them. Where the two spell a name
# differently, both are listed.
ENTRY_POINTS = [
    "CalculateWeaponDamage",
    "CalculateMyCriticalHitChance",
    "CalculateMyCriticalHitDamage",
    "CalculateMineExplodeChance",
    "AdjustLimbDamage",
    "AdjustBookSkillPoints",
    "ModRecoveredHealth",
    "GetShouldAttack",
    "ModBuyPrices",
    "AddLeveledListOnDeath",
    "GetMaxCarryWeight",
    "ModAddictionChance",
    "ModAddictionDuration",
    "ModPositiveChemDuration",
    "Activate",
    "IgnoreRunningDuringDetection",
    "IgnoreBrokenLock",
    "ModEnemyCriticalHitChance",
    "ModSneakAttackMult",
    "ModMaxPlaceableMines",
    "ModBowZoom",
    "ModRecoverArrowChance",
    "ModSkillUse",
    "ModTelekinesisDistance",
    "ModTelekinesisDamageMult",
    "ModTelekinesisDamage",
    "ModBashingDamage",
    "ModPowerAttackStamina",
    "ModPowerAttackDamage",
    "ModSpellMagnitude",
    "ModSpellDuration",
    "ModSecondaryValueWeight",
    "ModArmorWeight",
    "ModIncomingStagger",
    "ModTargetStagger",
    "ModAttackDamage",
    "ModIncomingDamage",
    "ModTargetDamageResistance",
    "ModSpellCost",
    "ModPercentBlocked",
    "ModShieldDeflectArrowChance",
    "ModIncomingSpellMagnitude",
    "ModIncomingSpellDuration",
    "ModPlayerIntimidation",
    "ModPlayerReputation",
    "ModFavorPoints",
    "ModBribeAmount",
    "ModDetectionLight",
    "ModDetectionMovement",
    "ModSoulGemRecharge",
    "SetSweepAttack",
    "ApplyCombatHitSpell",
    "ApplyBashingSpell",
    "ApplyReanimateSpell",
    "SetBooleanGraphVariable",
    "ModSpellCastingSoundEvent",
    "ModPickpocketChance",
    "ModDetectionSneakSkill",
    "ModFallingDamage",
    "ModLockpickSweetSpot",
    "ModSellPrices",
    "CanPickpocketEquippedItem",
    "ModLockpickLevelAllowed",
    "SetLockpickStartingArc",
    "SetProgressionPicking",
    "MakeLockpicksUnbreakable",
    "ModAlchemyEffectiveness",
    "ApplyWeaponSwingSpell",
    "ModCommandedActorLimit",
    "ApplySneakingSpell",
    "ModPlayerMagicSlowdown",
    "ModWardMagickaAbsorptionPct",
    "ModInitialIngredientEffectsLearned",
    "PurifyAlchemyIngredients",
    "FilterActivation",
    "CanDualCastSpell",
    "ModTemperingHealth",
    "ModEnchantmentPower",
    "ModSoulPctCapturedToWeapon",
    "ModSoulGemEnchanting",
    "ModNumberAppliedEnchantmentsAllowed",
    "SetActivateLabel",
    "ModShoutOK",
    "ModPoisonDoseCount",
    "ShouldApplyPlacedItem",
    "ModArmorRating",
    "ModLockpickingCrimeChance",
    "ModIngredientsHarvested",
    "ModSpellRange",
    "ModPotionsCreated",
    "ModLockpickingKeyRewardChance",
    "AllowMountActor",
]
EP_NUMBER = {name: i for i, name in enumerate(ENTRY_POINTS)}
EP_NUMBER.update(
    {
        "ModShieldDefectArrowChance": 40,
        "ModSoulPercentCapturedToWeapon": 78,
        "ModNumAppliedEnchantmentsAllowed": 80,
        "ModSpellRange_TargetLoc": 88,
    }
)
OPS = {
    "EqualTo": "==",
    "NotEqualTo": "!=",
    "GreaterThan": ">",
    "GreaterThanOrEqualTo": ">=",
    "LessThan": "<",
    "LessThanOrEqualTo": "<=",
}


class HouseCarl:
    def __init__(self):
        env = dict(
            os.environ,
            HouseCarl__Mo2InstanceDir=MO2,
            HOUSECARL_DATA_DIR=str(HERE / "housecarl-cache"),
        )
        self.proc = subprocess.Popen(
            [HOUSECARL],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            env=env,
        )
        self.next_id = 0
        self._call(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "progression-research", "version": "1"},
            },
        )
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, message):
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()

    def _call(self, method, params):
        self.next_id += 1
        self._send(dict(jsonrpc="2.0", id=self.next_id, method=method, params=params))
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("houseCARL exited")
            try:
                reply = json.loads(line)
            except ValueError:
                continue
            if reply.get("id") == self.next_id:
                return reply

    def tool(self, name, args):
        reply = self._call("tools/call", {"name": name, "arguments": args})
        return "".join(c.get("text", "") for c in reply["result"].get("content", []))

    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


def flat(record):
    return {f["path"]: f.get("value", f.get("note")) for f in record.get("fields", [])}


def fetch():
    hc = HouseCarl()
    try:
        avif = {}
        for local in SKILLS:
            avif[local] = hc.tool(
                "housecarl_read_record",
                {
                    "formid": local + ":Skyrim.esm",
                    "fields": ["PerkTree", "Name"],
                    "depth": 4,
                    "format": "json",
                    "max_chars": 300000,
                },
            )
        (HERE / "avif-raw.json").write_text(
            json.dumps(avif, indent=1, ensure_ascii=False), encoding="utf-8"
        )
        todo = set()
        for doc in avif.values():
            for key, value in flat(json.loads(doc)).items():
                if (
                    re.fullmatch(r"PerkTree\[\d+\]\.Perk", key)
                    and value
                    and ":" in value
                ):
                    todo.add(value)
        todo = sorted(todo)
        perks = {}
        while todo:
            batch, todo = todo[:25], todo[25:]
            reply = json.loads(
                hc.tool(
                    "housecarl_batch_record_detail",
                    {
                        "formids": batch,
                        "depth": 6,
                        "format": "json",
                        "max_chars": 2000000,
                    },
                )
            )
            for record in reply["records"]:
                perks[record["formid"]] = record
                nxt = flat(record).get("NextPerk")
                if (
                    nxt
                    and ":" in nxt
                    and nxt not in perks
                    and nxt not in todo
                    and nxt not in batch
                ):
                    todo.append(nxt)
        (HERE / "perks-raw.json").write_text(
            json.dumps(perks, indent=1, ensure_ascii=False), encoding="utf-8"
        )
    finally:
        hc.close()


def perk_info(record):
    f = flat(record)
    conds, effects = [], []
    i = 0
    while f"Conditions[{i}]" in f:
        p = f"Conditions[{i}]"
        conds.append(
            dict(
                fn=f.get(p + ".Data.Function"),
                av=f.get(p + ".Data.ActorValue"),
                perk=f.get(p + ".Data.Perk"),
                op=f.get(p + ".CompareOperator"),
                val=f.get(p + ".ComparisonValue"),
                flags=f.get(p + ".Flags"),
            )
        )
        i += 1
    i = 0
    while f"Effects[{i}]" in f:
        p = f"Effects[{i}]"
        effects.append(
            dict(
                kind=f.get(p),
                ep=f.get(p + ".EntryPoint"),
                ability=f.get(p + ".Ability"),
            )
        )
        i += 1
    return dict(
        formid=record["formid"],
        editorid=record.get("editorid"),
        name=f.get("Name"),
        desc=f.get("Description"),
        next=f.get("NextPerk"),
        conds=conds,
        effects=effects,
    )


def form_key(formid):  # '0BABE4:Skyrim.esm' -> 'Skyrim.esm|0BABE4'
    local, plugin = formid.split(":", 1)
    return f"{plugin}|{local.upper()}"


def convert():
    avif = json.loads((HERE / "avif-raw.json").read_text(encoding="utf-8"))
    raw = json.loads((HERE / "perks-raw.json").read_text(encoding="utf-8"))
    perks = {fid: perk_info(rec) for fid, rec in raw.items()}

    trees = {}
    for local, doc in avif.items():
        f = flat(json.loads(doc))
        nodes = {}
        i = 0
        while f"PerkTree[{i}]" in f:
            p = f"PerkTree[{i}]"
            children = []
            j = 0
            while f"{p}.ConnectionLineToIndices[{j}]" in f:
                children.append(int(f[f"{p}.ConnectionLineToIndices[{j}]"]))
                j += 1
            perk = f.get(p + ".Perk")
            index = int(f[p + ".Index"])
            nodes[index] = dict(
                index=index,
                perk=perk if perk and ":" in perk else None,
                x=float(f[p + ".HorizontalPosition"]),
                y=float(f[p + ".VerticalPosition"]),
                children=children,
            )
            i += 1
        trees[SKILLS[local]] = dict(avif=local + ":Skyrim.esm", nodes=nodes)
    (HERE / "perk-trees.json").write_text(
        json.dumps({"skills": trees, "perks": perks}, indent=1, ensure_ascii=False),
        encoding="utf-8",
    )

    # The core's format.
    out_nodes = []
    lines = []
    for skill, tree in trees.items():
        lines.append(f"===== {skill}")
        for index in sorted(tree["nodes"]):
            node = tree["nodes"][index]
            if not node["perk"]:
                continue
            ranks = []
            fid = node["perk"]
            entry_points, ability, quest = set(), False, False
            while fid and fid in perks:
                info = perks[fid]
                conditions = []
                for c in info["conds"]:
                    cond = {
                        "op": OPS.get(c["op"], "=="),
                        "value": float(c["val"] or 0),
                        "or": c["flags"] == "OR",
                    }
                    if c["fn"] in ("GetBaseActorValue", "GetActorValue"):
                        cond["fn"] = c["fn"]
                        cond["av"] = SKILL_AV.get(c["av"], -1)
                    elif c["fn"] == "HasPerk":
                        cond["fn"] = "HasPerk"
                        cond["perk"] = form_key(c["perk"])
                    else:
                        cond["fn"] = "Other"
                        cond["name"] = c["fn"]
                    conditions.append(cond)
                for e in info["effects"]:
                    if e["ep"]:
                        entry_points.add(EP_NUMBER[e["ep"]])
                    kind = e["kind"] or ""
                    ability = ability or "PerkAbilityEffect" in kind
                    quest = quest or "PerkQuestEffect" in kind
                ranks.append(
                    {
                        "form": form_key(fid),
                        "description": (info["desc"] or "").strip(),
                        "conditions": conditions,
                    }
                )
                nxt = info["next"]
                fid = nxt if nxt and ":" in nxt else None
            first = perks[node["perk"]]
            out_nodes.append(
                {
                    "skill": skill,
                    "name": first["name"],
                    "x": node["x"],
                    "y": node["y"],
                    "ranks": ranks,
                }
            )
            reqs = " > ".join(perks_rank_line(r) for r in ranks)
            lines.append(
                f"  {first['name']} ({first['editorid']}, {form_key(node['perk'])}): {reqs}"
                f"  eps={sorted(entry_points)}{' ability' if ability else ''}{' quest' if quest else ''}"
            )
    target = REPO / "tests" / "progression" / "data" / "vanilla-perks.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(
            {
                "source": "Skyrim.esm, read through houseCARL; see dev/research/extract_perk_trees.py",
                "nodes": out_nodes,
            },
            indent=1,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )
    (HERE / "perk-trees.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(
        f"{len(out_nodes)} nodes, {sum(len(n['ranks']) for n in out_nodes)} ranks -> {target}"
    )


def perks_rank_line(rank):
    parts = []
    for c in rank["conditions"]:
        if c["fn"] == "HasPerk":
            parts.append(
                f"HasPerk {c['perk']} {c['op']} {c['value']:g}{' OR' if c['or'] else ''}"
            )
        elif c["fn"] == "Other":
            parts.append(f"{c['name']}{' OR' if c['or'] else ''}")
        else:
            parts.append(
                f"AV{c['av']} {c['op']} {c['value']:g}{' OR' if c['or'] else ''}"
            )
    return "[" + "; ".join(parts) + "]"


if __name__ == "__main__":
    if "--fetch" in sys.argv:
        fetch()
    convert()
