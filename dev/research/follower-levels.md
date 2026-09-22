# Follower levels, as the load order has them

Read 2026-09-21 through houseCARL (`housecarl_cross_plugin_query`, type Npc, referencing `PotentialFollowerFaction` 05C84D:Skyrim.esm, plus Dawnguard's Serana), against the MO2 load order at `C:\modding\MO2`. Every row's winner is its own plugin: nothing in this load order overrides them. Thirsk's rieklings, in the faction but not followers, are left out.

The engine's level for a *PC Level Mult* NPC is `player level x multiplier`, rounded down, then held between the minimum and maximum, 0 meaning no limit (`TESActorBaseData::GetLevel`, 1.6.1170 ID 14384; the multiplier is stored in thousandths). It is worked out on every call.

| Name | EditorID | FormID | Level | Min | Max | Plugin |
|---|---|---|---|---:|---:|---|
| Adelaisa Vendicci | Adelaisa | 01411D:Skyrim.esm | x0.9 | 6 | 25 | Skyrim.esm |
| Aela the Huntress | AelaTheHuntress | 01A696:Skyrim.esm | x1 | 8 | 50 | Skyrim.esm |
| Agmaer | DLC1Agmaer | 00336E:Dawnguard.esm | x1 | 5 | 25 | Dawnguard.esm |
| Ahtar | Ahtar | 01325F:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Annekke Crag-Jumper | Annekke | 013666:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Aranea Ienith | DA01Aranea | 028AD0:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Argis the Bulwark | HousecarlMarkarth | 0A2C8C:Skyrim.esm | x1 | 10 | 50 | Skyrim.esm |
| Athis | Athis | 01A6D5:Skyrim.esm | x1 | 5 | 25 | Skyrim.esm |
| Beleval | DLC1Beleval | 01541C:Dawnguard.esm | x1 | 5 | 25 | Dawnguard.esm |
| Belrand | HirelingBelrand | 0B9981:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Benor | Benor | 0135E8:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Borgakh the Steel Heart | Borgakh | 019959:Skyrim.esm | x1 | 10 | 30 | Skyrim.esm |
| Brelyna Maryon | BrelynaMaryon | 01C196:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Calder | HousecarlWindhelm | 0A2C90:Skyrim.esm | x1 | 10 | 50 | Skyrim.esm |
| Celann | DLC1Celann | 01541E:Dawnguard.esm | x1 | 10 | none | Dawnguard.esm |
| Cosnach | Cosnach | 013390:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Derkeethus | Derkeethus | 01403E:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Durak | DLC1Durak | 01541D:Dawnguard.esm | x1 | 10 | none | Dawnguard.esm |
| Eola | Eola | 01990F:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Erandur | Erandur | 02427D:Skyrim.esm | x1.5 | 6 | 50 | Skyrim.esm |
| Erik the Slayer | HirelingErikTheSlayer | 065657:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Faendal | Faendal | 013480:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Farkas | Farkas | 01A692:Skyrim.esm | x1 | 8 | 50 | Skyrim.esm |
| Frea | DLC2Frea | 017934:Dragonborn.esm | x1 | 20 | none | Dragonborn.esm |
| Ghorbash the Iron Hand | Ghorbash | 013B81:Skyrim.esm | x1 | 10 | 30 | Skyrim.esm |
| Gogh | ccBGSSSE040_EncGoblinFollower | 000815:ccbgssse040-advobgobs.esl | fixed 6 |  |  | ccbgssse040-advobgobs.esl |
| Golldir | Golldir | 019FE8:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Gregor | BYOHHousecarlPale | 00521E:HearthFires.esm | x1 | 10 | 50 | HearthFires.esm |
| Hand Ethra Mavandas | ccASVSSE001_HandMavandes | 00083F:ccasvsse001-almsivi.esm | x1 | 15 | 100 | ccasvsse001-almsivi.esm |
| Hand Kydren Indobar | ccASVSSE001_HandIndobar | 00083E:ccasvsse001-almsivi.esm | x1.2 | 20 | 100 | ccasvsse001-almsivi.esm |
| Illia | dunDarklightIllia | 048C2F:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Ingjard | DLC1Ingjard | 01541B:Dawnguard.esm | x1 | 10 | none | Dawnguard.esm |
| Iona | HousecarlRiften | 0A2C91:Skyrim.esm | x1 | 10 | 50 | Skyrim.esm |
| J'zargo | Jzargo | 01C195:Skyrim.esm | x1 | 6 | none | Skyrim.esm |
| Jenassa | HirelingJenassa | 0B9982:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Jordis the Sword-Maiden | HousecarlSolitude | 0A2C8F:Skyrim.esm | x1 | 10 | 50 | Skyrim.esm |
| Kharjo | Kharjo | 01B1D2:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Lob | Lob | 019E1E:Skyrim.esm | x1 | 10 | 30 | Skyrim.esm |
| Lydia | HousecarlWhiterun | 0A2C8E:Skyrim.esm | x1 | 6 | 50 | Skyrim.esm |
| Marcurio | HirelingMarcurio | 0B9980:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Mjoll the Lioness | Mjoll | 01336B:Skyrim.esm | x1.25 | 10 | 40 | Skyrim.esm |
| Njada Stonearm | NjadaStonearm | 01A6D9:Skyrim.esm | x1 | 5 | 25 | Skyrim.esm |
| Ogol | Ogol | 019E22:Skyrim.esm | x1 | 10 | 30 | Skyrim.esm |
| Onmund | Onmund | 01C194:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Ralis Sedarys | DLC2dunKolbjornRalis | 0179C7:Dragonborn.esm | x1 | 15 | 60 | Dragonborn.esm |
| Rayya | BYOHHousecarlFalkreath | 005215:HearthFires.esm | x1 | 10 | 50 | HearthFires.esm |
| Ria | Ria | 01A6D7:Skyrim.esm | x1 | 5 | 25 | Skyrim.esm |
| Roggi Knot-Beard | Roggi | 01403F:Skyrim.esm | x0.75 | 6 | 20 | Skyrim.esm |
| Rulnik Wind-Strider | ccKRTSSE001_Rulnik | 00080A:cckrtsse001_altar.esl | x1 | 15 | 75 | cckrtsse001_altar.esl |
| Serana | DLC1Serana | 002B6C:Dawnguard.esm | x1 | 12 | 50 | Dawnguard.esm |
| Stenvar | HirelingStenvar | 0B9983:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Sven | Sven | 01347F:Skyrim.esm | x0.75 | 6 | 20 | Skyrim.esm |
| Talvas Fathryon | DLC2Talvas | 017777:Dragonborn.esm | fixed 25 |  |  | Dragonborn.esm |
| Teldryn Sero | DLC2RRTeldrynSero | 038560:Dragonborn.esm | x1 | 10 | 60 | Dragonborn.esm |
| Torvar | Torvar | 01A6DB:Skyrim.esm | x1 | 5 | 25 | Skyrim.esm |
| Ugor | Ugor | 019E1A:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Uthgerd the Unbroken | Uthgerd | 0918E2:Skyrim.esm | x1 | 6 | 30 | Skyrim.esm |
| Valdimar | BYOHHousecarlHjaalmarch | 00521B:HearthFires.esm | x1 | 10 | 50 | HearthFires.esm |
| Vesparth the Toe | ccASVSSE001_HandVesparth | 000841:ccasvsse001-almsivi.esm | x1.2 | 20 | 100 | ccasvsse001-almsivi.esm |
| Vilkas | Vilkas | 01A694:Skyrim.esm | x1 | 8 | 50 | Skyrim.esm |
| Viriya | ccBGSSSE001_Viriya | 0008F6:ccbgssse001-fish.esm | fixed 16 |  |  | ccbgssse001-fish.esm |
| Vorstag | HirelingVorstag | 0B997F:Skyrim.esm | x1 | 10 | 40 | Skyrim.esm |
| Watchman Sindras | ccASVSSE001_HandTherethi | 00083D:ccasvsse001-almsivi.esm | x1.2 | 15 | 100 | ccasvsse001-almsivi.esm |
