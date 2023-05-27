/*
* Copyright (C) 2018 AzerothCore <http://www.azerothcore.org>
* Copyright (C) 2012 CVMagic <http://www.trinitycore.org/f/topic/6551-vas-autobalance/>
* Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
* Copyright (C) 1985-2010 KalCorp  <http://vasserver.dyndns.org/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
* Script Name: AutoBalance
* Original Authors: KalCorp and Vaughner
* Maintainer(s): AzerothCore
* Original Script Name: AutoBalance
* Description: This script is intended to scale based on number of players,
* instance mobs & world bosses' level, health, mana, and damage.
*/

#include "Configuration/Config.h"
#include "Unit.h"
#include "Chat.h"
#include "Creature.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "MapMgr.h"
#include "World.h"
#include "Map.h"
#include "ScriptMgr.h"
#include "Language.h"
#include <vector>
#include "AutoBalance.h"
#include "ScriptMgrMacros.h"
#include "Group.h"
#include "Log.h"

#if AC_COMPILER == AC_COMPILER_GNU
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace Acore::ChatCommands;

ABScriptMgr* ABScriptMgr::instance()
{
    static ABScriptMgr instance;
    return &instance;
}

bool ABScriptMgr::OnBeforeModifyAttributes(Creature *creature, uint32 & instancePlayerCount)
{
    auto ret = IsValidBoolScript<ABModuleScript>([&](ABModuleScript* script)
    {
        return !script->OnBeforeModifyAttributes(creature, instancePlayerCount);
    });

    if (ret && *ret)
    {
        return false;
    }

    return true;
}

bool ABScriptMgr::OnAfterDefaultMultiplier(Creature *creature, float& defaultMultiplier)
{
    auto ret = IsValidBoolScript<ABModuleScript>([&](ABModuleScript* script)
    {
        return !script->OnAfterDefaultMultiplier(creature, defaultMultiplier);
    });

    if (ret && *ret)
    {
        return false;
    }

    return true;
}

bool ABScriptMgr::OnBeforeUpdateStats(Creature* creature, uint32& scaledHealth, uint32& scaledMana, float& damageMultiplier, uint32& newBaseArmor)
{
    auto ret = IsValidBoolScript<ABModuleScript>([&](ABModuleScript* script)
    {
        return !script->OnBeforeUpdateStats(creature, scaledHealth, scaledMana, damageMultiplier, newBaseArmor);
    });

    if (ret && *ret)
    {
        return false;
    }

    return true;
}

ABModuleScript::ABModuleScript(const char* name)
    : ModuleScript(name)
{
    ScriptRegistry<ABModuleScript>::AddScript(this);
}


class AutoBalanceCreatureInfo : public DataMap::Base
{
public:
    AutoBalanceCreatureInfo() {}
    AutoBalanceCreatureInfo(uint32 count, float dmg, float hpRate, float manaRate, float armorRate, uint8 selLevel) :
    instancePlayerCount(count),selectedLevel(selLevel), DamageMultiplier(dmg),HealthMultiplier(hpRate),ManaMultiplier(manaRate),ArmorMultiplier(armorRate) {}
    uint32 instancePlayerCount = 0;
    uint8 selectedLevel = 0;
    // this is used to detect creatures that update their entry
    uint32 entry = 0;
    float DamageMultiplier = 1.0f;
    float HealthMultiplier = 1.0f;
    float ManaMultiplier = 1.0f;
    float ArmorMultiplier = 1.0f;
    float CCDurationMultiplier = 1.0f;
};

class AutoBalanceMapInfo : public DataMap::Base
{
public:
    AutoBalanceMapInfo() {}
    AutoBalanceMapInfo(uint32 count, uint8 selLevel, bool enabled) : playerCount(count),mapLevel(selLevel),enabled(enabled) {}
    uint32 playerCount = 0;
    uint8 mapLevel = 0;
    bool enabled = false;
};

class AutoBalanceStatModifiers : public DataMap::Base
{
public:
    AutoBalanceStatModifiers() {}
    AutoBalanceStatModifiers(float global, float health, float mana, float armor, float damage, float ccduration) :
        global(global), health(health), mana(mana), armor(armor), damage(damage), ccduration(ccduration) {}
    float global;
    float health;
    float mana;
    float armor;
    float damage;
    float ccduration;
};

class AutoBalanceInflectionPointSettings : public DataMap::Base
{
public:
    AutoBalanceInflectionPointSettings() {}
    AutoBalanceInflectionPointSettings(float value, float curveFloor, float curveCeiling) :
        value(value), curveFloor(curveFloor), curveCeiling(curveCeiling) {}
    float value;
    float curveFloor;
    float curveCeiling;
};

// The map values correspond with the .AutoBalance.XX.Name entries in the configuration file.
static std::map<int, int> forcedCreatureIds;
static std::map<uint32, uint8> enabledDungeonIds;
static std::map<uint32, AutoBalanceInflectionPointSettings> dungeonOverrides;
static std::map<uint32, AutoBalanceInflectionPointSettings> bossOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierBossOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierCreatureOverrides;
// cheaphack for difficulty server-wide.
// Another value TODO in player class for the party leader's value to determine dungeon difficulty.
static int8 PlayerCountDifficultyOffset, LevelScaling, higherOffset, lowerOffset;
static uint32 rewardRaid, rewardDungeon, MinPlayerReward;
static bool Announcement;
static bool LevelEndGameBoost, PlayerChangeNotify, LevelUseDb, rewardEnabled, DungeonScaleDownXP, DungeonScaleDownMoney;
static float MinHPModifier, MinManaModifier, MinDamageModifier, MinCCDurationModifier, MaxCCDurationModifier;

// Enable.*
static bool EnableGlobal;
static bool Enable5M, Enable10M, Enable15M, Enable20M, Enable25M, Enable40M;
static bool Enable5MHeroic, Enable10MHeroic, Enable25MHeroic;
static bool EnableOtherNormal, EnableOtherHeroic;

// InflectionPoint*
static float InflectionPoint, InflectionPointCurveFloor, InflectionPointCurveCeiling, InflectionPointBoss;
static float InflectionPointHeroic, InflectionPointHeroicCurveFloor, InflectionPointHeroicCurveCeiling, InflectionPointHeroicBoss;
static float InflectionPointRaid, InflectionPointRaidCurveFloor, InflectionPointRaidCurveCeiling, InflectionPointRaidBoss;
static float InflectionPointRaidHeroic, InflectionPointRaidHeroicCurveFloor, InflectionPointRaidHeroicCurveCeiling, InflectionPointRaidHeroicBoss;

static float InflectionPointRaid10M, InflectionPointRaid10MCurveFloor, InflectionPointRaid10MCurveCeiling, InflectionPointRaid10MBoss;
static float InflectionPointRaid10MHeroic, InflectionPointRaid10MHeroicCurveFloor, InflectionPointRaid10MHeroicCurveCeiling, InflectionPointRaid10MHeroicBoss;
static float InflectionPointRaid15M, InflectionPointRaid15MCurveFloor, InflectionPointRaid15MCurveCeiling, InflectionPointRaid15MBoss;
static float InflectionPointRaid20M, InflectionPointRaid20MCurveFloor, InflectionPointRaid20MCurveCeiling, InflectionPointRaid20MBoss;
static float InflectionPointRaid25M, InflectionPointRaid25MCurveFloor, InflectionPointRaid25MCurveCeiling, InflectionPointRaid25MBoss;
static float InflectionPointRaid25MHeroic, InflectionPointRaid25MHeroicCurveFloor, InflectionPointRaid25MHeroicCurveCeiling, InflectionPointRaid25MHeroicBoss;
static float InflectionPointRaid40M, InflectionPointRaid40MCurveFloor, InflectionPointRaid40MCurveCeiling, InflectionPointRaid40MBoss;

// StatModifier*
static float StatModifier_Global, StatModifier_Health, StatModifier_Mana, StatModifier_Armor, StatModifier_Damage, StatModifier_CCDuration;
static float StatModifierHeroic_Global, StatModifierHeroic_Health, StatModifierHeroic_Mana, StatModifierHeroic_Armor, StatModifierHeroic_Damage, StatModifierHeroic_CCDuration;
static float StatModifierRaid_Global, StatModifierRaid_Health, StatModifierRaid_Mana, StatModifierRaid_Armor, StatModifierRaid_Damage, StatModifierRaid_CCDuration;
static float StatModifierRaidHeroic_Global, StatModifierRaidHeroic_Health, StatModifierRaidHeroic_Mana, StatModifierRaidHeroic_Armor, StatModifierRaidHeroic_Damage, StatModifierRaidHeroic_CCDuration;

static float StatModifierRaid10M_Global, StatModifierRaid10M_Health, StatModifierRaid10M_Mana, StatModifierRaid10M_Armor, StatModifierRaid10M_Damage, StatModifierRaid10M_CCDuration;
static float StatModifierRaid10MHeroic_Global, StatModifierRaid10MHeroic_Health, StatModifierRaid10MHeroic_Mana, StatModifierRaid10MHeroic_Armor, StatModifierRaid10MHeroic_Damage, StatModifierRaid10MHeroic_CCDuration;
static float StatModifierRaid15M_Global, StatModifierRaid15M_Health, StatModifierRaid15M_Mana, StatModifierRaid15M_Armor, StatModifierRaid15M_Damage, StatModifierRaid15M_CCDuration;
static float StatModifierRaid20M_Global, StatModifierRaid20M_Health, StatModifierRaid20M_Mana, StatModifierRaid20M_Armor, StatModifierRaid20M_Damage, StatModifierRaid20M_CCDuration;
static float StatModifierRaid25M_Global, StatModifierRaid25M_Health, StatModifierRaid25M_Mana, StatModifierRaid25M_Armor, StatModifierRaid25M_Damage, StatModifierRaid25M_CCDuration;
static float StatModifierRaid25MHeroic_Global, StatModifierRaid25MHeroic_Health, StatModifierRaid25MHeroic_Mana, StatModifierRaid25MHeroic_Armor, StatModifierRaid25MHeroic_Damage, StatModifierRaid25MHeroic_CCDuration;
static float StatModifierRaid40M_Global, StatModifierRaid40M_Health, StatModifierRaid40M_Mana, StatModifierRaid40M_Armor, StatModifierRaid40M_Damage, StatModifierRaid40M_CCDuration;

// StatModifier* (Boss)
static float StatModifier_Boss_Global, StatModifier_Boss_Health, StatModifier_Boss_Mana, StatModifier_Boss_Armor, StatModifier_Boss_Damage, StatModifier_Boss_CCDuration;
static float StatModifierHeroic_Boss_Global, StatModifierHeroic_Boss_Health, StatModifierHeroic_Boss_Mana, StatModifierHeroic_Boss_Armor, StatModifierHeroic_Boss_Damage, StatModifierHeroic_Boss_CCDuration;
static float StatModifierRaid_Boss_Global, StatModifierRaid_Boss_Health, StatModifierRaid_Boss_Mana, StatModifierRaid_Boss_Armor, StatModifierRaid_Boss_Damage, StatModifierRaid_Boss_CCDuration;
static float StatModifierRaidHeroic_Boss_Global, StatModifierRaidHeroic_Boss_Health, StatModifierRaidHeroic_Boss_Mana, StatModifierRaidHeroic_Boss_Armor, StatModifierRaidHeroic_Boss_Damage, StatModifierRaidHeroic_Boss_CCDuration;

static float StatModifierRaid10M_Boss_Global, StatModifierRaid10M_Boss_Health, StatModifierRaid10M_Boss_Mana, StatModifierRaid10M_Boss_Armor, StatModifierRaid10M_Boss_Damage, StatModifierRaid10M_Boss_CCDuration;
static float StatModifierRaid10MHeroic_Boss_Global, StatModifierRaid10MHeroic_Boss_Health, StatModifierRaid10MHeroic_Boss_Mana, StatModifierRaid10MHeroic_Boss_Armor, StatModifierRaid10MHeroic_Boss_Damage, StatModifierRaid10MHeroic_Boss_CCDuration;
static float StatModifierRaid15M_Boss_Global, StatModifierRaid15M_Boss_Health, StatModifierRaid15M_Boss_Mana, StatModifierRaid15M_Boss_Armor, StatModifierRaid15M_Boss_Damage, StatModifierRaid15M_Boss_CCDuration;
static float StatModifierRaid20M_Boss_Global, StatModifierRaid20M_Boss_Health, StatModifierRaid20M_Boss_Mana, StatModifierRaid20M_Boss_Armor, StatModifierRaid20M_Boss_Damage, StatModifierRaid20M_Boss_CCDuration;
static float StatModifierRaid25M_Boss_Global, StatModifierRaid25M_Boss_Health, StatModifierRaid25M_Boss_Mana, StatModifierRaid25M_Boss_Armor, StatModifierRaid25M_Boss_Damage, StatModifierRaid25M_Boss_CCDuration;
static float StatModifierRaid25MHeroic_Boss_Global, StatModifierRaid25MHeroic_Boss_Health, StatModifierRaid25MHeroic_Boss_Mana, StatModifierRaid25MHeroic_Boss_Armor, StatModifierRaid25MHeroic_Boss_Damage, StatModifierRaid25MHeroic_Boss_CCDuration;
static float StatModifierRaid40M_Boss_Global, StatModifierRaid40M_Boss_Health, StatModifierRaid40M_Boss_Mana, StatModifierRaid40M_Boss_Armor, StatModifierRaid40M_Boss_Damage, StatModifierRaid40M_Boss_CCDuration;

int GetValidDebugLevel()
{
    int debugLevel = sConfigMgr->GetOption<uint32>("AutoBalance.DebugLevel", 2);

    if ((debugLevel < 0) || (debugLevel > 3))
        {
        return 1;
        }
    return debugLevel;
}

void LoadEnabledDungeons(std::string dungeonIdString) // Used for reading the string from the configuration file for selecting dungeons to scale
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string pairOne, pairTwo;
        std::stringstream dungeonPairStream(delimitedValue);
        dungeonPairStream>>pairOne>>pairTwo;
        auto dungeonMapId = atoi(pairOne.c_str());
        auto minPlayers = atoi(pairTwo.c_str());
        enabledDungeonIds[dungeonMapId] = minPlayers;
    }
}

std::map<uint32, AutoBalanceInflectionPointSettings> LoadInflectionPointOverrides(std::string dungeonIdString) // Used for reading the string from the configuration file for selecting dungeons to override
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::map<uint32, AutoBalanceInflectionPointSettings> overrideMap;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string val1, val2, val3, val4;
        std::stringstream dungeonPairStream(delimitedValue);
        dungeonPairStream >> val1 >> val2 >> val3 >> val4;

        auto dungeonMapId = atoi(val1.c_str());

        // Replace any missing values with -1
        if (val2.empty()) { val2 = "-1"; }
        if (val3.empty()) { val3 = "-1"; }
        if (val4.empty()) { val4 = "-1"; }

        AutoBalanceInflectionPointSettings ipSettings = AutoBalanceInflectionPointSettings(
            atof(val2.c_str()),
            atof(val3.c_str()),
            atof(val4.c_str())
        );

        overrideMap[dungeonMapId] = ipSettings;
    }

    return overrideMap;
}

std::map<uint32, AutoBalanceStatModifiers> LoadStatModifierOverrides(std::string dungeonIdString) // Used for reading the string from the configuration file for per-dungeon stat modifiers
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::map<uint32, AutoBalanceStatModifiers> overrideMap;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string val1, val2, val3, val4, val5, val6, val7;
        std::stringstream dungeonStream(delimitedValue);
        dungeonStream >> val1 >> val2 >> val3 >> val4 >> val5 >> val6 >> val7;

        auto dungeonMapId = atoi(val1.c_str());

        // Replace any missing values with -1
        if (val2.empty()) { val2 = "-1"; }
        if (val3.empty()) { val3 = "-1"; }
        if (val4.empty()) { val4 = "-1"; }
        if (val5.empty()) { val5 = "-1"; }
        if (val6.empty()) { val6 = "-1"; }
        if (val7.empty()) { val7 = "-1"; }

        AutoBalanceStatModifiers statSettings = AutoBalanceStatModifiers(
            atof(val2.c_str()),
            atof(val3.c_str()),
            atof(val4.c_str()),
            atof(val5.c_str()),
            atof(val6.c_str()),
            atof(val7.c_str())
        );

        overrideMap[dungeonMapId] = statSettings;
    }

    return overrideMap;
}


bool isEnabledDungeon(uint32 dungeonId)
{
    return (enabledDungeonIds.find(dungeonId) != enabledDungeonIds.end());
}

bool perDungeonScalingEnabled()
{
    return (!enabledDungeonIds.empty());
}

bool hasDungeonOverride(uint32 dungeonId)
{
    return (dungeonOverrides.find(dungeonId) != dungeonOverrides.end());
}

bool hasBossOverride(uint32 dungeonId)
{
    return (bossOverrides.find(dungeonId) != bossOverrides.end());
}

bool hasStatModifierOverride(uint32 dungeonId)
{
    return (statModifierOverrides.find(dungeonId) != statModifierOverrides.end());
}

bool hasStatModifierBossOverride(uint32 dungeonId)
{
    return (statModifierBossOverrides.find(dungeonId) != statModifierBossOverrides.end());
}

bool hasStatModifierCreatureOverride(uint32 creatureId)
{
    return (statModifierCreatureOverrides.find(creatureId) != statModifierCreatureOverrides.end());
}

bool ShouldMapBeEnabled(Map* map)
{
    if (map->IsDungeon() || map->IsRaid())
    {
        // get the current instance map
        auto instanceMap = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()));

        // if there wasn't one, then we're not in an instance
        if (!instanceMap)
        {
            return false;
        }

        // get the max player count for the instance
        auto maxPlayerCount = instanceMap->GetMaxPlayers();

        // if the player count is less than 1, then we're not in an instance
        if (maxPlayerCount < 1)
        {
            return false;
        }

        // use the configuration variables to determine if this instance type/size should have scaling enabled
        if (instanceMap->IsHeroic())
        {
            switch (maxPlayerCount)
            {
                case 5:
                    return Enable5MHeroic;
                case 10:
                    return Enable10MHeroic;
                case 25:
                    return Enable25MHeroic;
                default:
                    return EnableOtherHeroic;
            }
        }
        else
        {
            switch (maxPlayerCount)
            {
                case 5:
                    return Enable5M;
                case 10:
                    return Enable10M;
                case 15:
                    return Enable15M;
                case 20:
                    return Enable20M;
                case 25:
                    return Enable25M;
                case 40:
                    return Enable40M;
                default:
                    return EnableOtherNormal;
            }
        }
    }
    else
    {
        // we're not in a dungeon or a raid, we never scale
        return false;
    }
}

void LoadForcedCreatureIdsFromString(std::string creatureIds, int forcedPlayerCount) // Used for reading the string from the configuration file to for those creatures who need to be scaled for XX number of players.
{
    std::string delimitedValue;
    std::stringstream creatureIdsStream;

    creatureIdsStream.str(creatureIds);
    while (std::getline(creatureIdsStream, delimitedValue, ',')) // Process each Creature ID in the string, delimited by the comma - ","
    {
        int creatureId = atoi(delimitedValue.c_str());
        if (creatureId >= 0)
        {
            forcedCreatureIds[creatureId] = forcedPlayerCount;
        }
    }
}

int GetForcedNumPlayers(int creatureId)
{
    if (forcedCreatureIds.find(creatureId) == forcedCreatureIds.end()) // Don't want the forcedCreatureIds map to blowup to a massive empty array
    {
        return -1;
    }
    return forcedCreatureIds[creatureId];
}


void getAreaLevel(Map *map, uint8 areaid, uint8 &min, uint8 &max) {
    LFGDungeonEntry const* dungeon = GetLFGDungeon(map->GetId(), map->GetDifficulty());
    if (dungeon && (map->IsDungeon() || map->IsRaid())) {
        min  = dungeon->MinLevel;
        max  = dungeon->TargetLevel ? dungeon->TargetLevel : dungeon->MaxLevel;
    }

    if (!min && !max)
    {
        AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaid);
        if (areaEntry && areaEntry->area_level > 0) {
            min = areaEntry->area_level;
            max = areaEntry->area_level;
        }
    }
}

class AutoBalance_WorldScript : public WorldScript
{
    public:
    AutoBalance_WorldScript()
        : WorldScript("AutoBalance_WorldScript")
    {
    }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        SetInitialWorldSettings();
    }
    void OnStartup() override
    {
    }

    void SetInitialWorldSettings()
    {
        forcedCreatureIds.clear();
        enabledDungeonIds.clear();
        dungeonOverrides.clear();
        bossOverrides.clear();
        statModifierOverrides.clear();
        statModifierBossOverrides.clear();
        statModifierCreatureOverrides.clear();
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID40", ""), 40);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID25", ""), 25);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID10", ""), 10);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID5", ""), 5);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID2", ""), 2);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.DisabledID", ""), 0);
        LoadEnabledDungeons(sConfigMgr->GetOption<std::string>("AutoBalance.PerDungeonPlayerCounts", ""));

        // Overrides
        if (sConfigMgr->GetOption<float>("AutoBalance.PerDungeonScaling", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.PerDungeonScaling` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        dungeonOverrides = LoadInflectionPointOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.InflectionPoint.PerInstance",sConfigMgr->GetOption<std::string>("AutoBalance.PerDungeonScaling", "", false), false)
        ); // `AutoBalance.PerDungeonScaling` for backwards compatibility

        if (sConfigMgr->GetOption<float>("AutoBalance.PerDungeonBossScaling", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.PerDungeonBossScaling` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        bossOverrides = LoadInflectionPointOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.InflectionPoint.Boss.PerInstance", sConfigMgr->GetOption<std::string>("AutoBalance.PerDungeonBossScaling", "", false), false)
        ); // `AutoBalance.PerDungeonBossScaling` for backwards compatibility

        statModifierOverrides = LoadStatModifierOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.StatModifier.PerInstance", "", false)
        );

        statModifierBossOverrides = LoadStatModifierOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.StatModifier.Boss.PerInstance", "", false)
        );

        statModifierCreatureOverrides = LoadStatModifierOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.StatModifier.PerCreature", "", false)
        );

        // AutoBalance.Enable.*
        // Deprecated setting warning
        if (sConfigMgr->GetOption<int>("AutoBalance.enable", -1, false) != -1)
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.enable` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");

        EnableGlobal = sConfigMgr->GetOption<bool>("AutoBalance.Enable.Global", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false)); // `AutoBalance.enable` for backwards compatibility

        Enable5M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.5M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable10M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.10M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable15M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.15M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable20M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.20M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable25M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.25M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable40M = sConfigMgr->GetOption<bool>("AutoBalance.Enable.40M", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        EnableOtherNormal = sConfigMgr->GetOption<bool>("AutoBalance.Enable.OtherNormal", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));

        Enable5MHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.5MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable10MHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.5MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        Enable25MHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.5MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));
        EnableOtherHeroic = sConfigMgr->GetOption<bool>("AutoBalance.Enable.5MHeroic", sConfigMgr->GetOption<bool>("AutoBalance.enable", 1, false));

        // Deprecated setting warning
        if (sConfigMgr->GetOption<int>("AutoBalance.DungeonsOnly", -1, false) != -1)
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.DungeonsOnly` defined in `AutoBalance.conf`. This variable has been removed and has no effect. Please see `AutoBalance.conf.dist` for more details.");

        // Misc Settings
        // TODO: Organize and standardize variable names
        LevelEndGameBoost = sConfigMgr->GetOption<bool>("AutoBalance.LevelEndGameBoost", 1);
        PlayerChangeNotify = sConfigMgr->GetOption<bool>("AutoBalance.PlayerChangeNotify", 1);
        LevelUseDb = sConfigMgr->GetOption<bool>("AutoBalance.levelUseDbValuesWhenExists", 1);
        rewardEnabled = sConfigMgr->GetOption<bool>("AutoBalance.reward.enable", 1);
        DungeonScaleDownXP = sConfigMgr->GetOption<bool>("AutoBalance.DungeonScaleDownXP", 0);
        DungeonScaleDownMoney = sConfigMgr->GetOption<bool>("AutoBalance.DungeonScaleDownMoney", 0);

        LevelScaling = sConfigMgr->GetOption<uint32>("AutoBalance.levelScaling", 1);
        PlayerCountDifficultyOffset = sConfigMgr->GetOption<uint32>("AutoBalance.playerCountDifficultyOffset", 0);
        higherOffset = sConfigMgr->GetOption<uint32>("AutoBalance.levelHigherOffset", 3);
        lowerOffset = sConfigMgr->GetOption<uint32>("AutoBalance.levelLowerOffset", 0);
        rewardRaid = sConfigMgr->GetOption<uint32>("AutoBalance.reward.raidToken", 49426);
        rewardDungeon = sConfigMgr->GetOption<uint32>("AutoBalance.reward.dungeonToken", 47241);
        MinPlayerReward = sConfigMgr->GetOption<float>("AutoBalance.reward.MinPlayerReward", 1);

        // InflectionPoint*
        // warn the console if deprecated values are detected
        if (sConfigMgr->GetOption<float>("AutoBalance.BossInflectionMult", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.BossInflectionMult` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");

        InflectionPoint =                           sConfigMgr->GetOption<float>("AutoBalance.InflectionPoint", 0.5f, false);
        InflectionPointCurveFloor =                 sConfigMgr->GetOption<float>("AutoBalance.InflectionPoint.CurveFloor", 0.0f, false);
        InflectionPointCurveCeiling =               sConfigMgr->GetOption<float>("AutoBalance.InflectionPoint.CurveCeiling", 1.0f, false);
        InflectionPointBoss =                       sConfigMgr->GetOption<float>("AutoBalance.InflectionPoint.BossModifier", sConfigMgr->GetOption<float>("AutoBalance.BossInflectionMult", 1.0f, false), false); // `AutoBalance.BossInflectionMult` for backwards compatibility

        InflectionPointHeroic =                     sConfigMgr->GetOption<float>("AutoBalance.InflectionPointHeroic", 0.5f, false);
        InflectionPointHeroicCurveFloor =           sConfigMgr->GetOption<float>("AutoBalance.InflectionPointHeroic.CurveFloor", 0.0f, false);
        InflectionPointHeroicCurveCeiling =         sConfigMgr->GetOption<float>("AutoBalance.InflectionPointHeroic.CurveCeiling", 1.0f, false);
        InflectionPointHeroicBoss =                 sConfigMgr->GetOption<float>("AutoBalance.InflectionPointHeroic.BossModifier", sConfigMgr->GetOption<float>("AutoBalance.BossInflectionMult", 1.0f, false), false); // `AutoBalance.BossInflectionMult` for backwards compatibility

        InflectionPointRaid =                       sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid", 0.5f, false);
        InflectionPointRaidCurveFloor =             sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid.CurveFloor", 0.0f, false);
        InflectionPointRaidCurveCeiling =           sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid.CurveCeiling", 1.0f, false);
        InflectionPointRaidBoss =                   sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid.BossModifier", sConfigMgr->GetOption<float>("AutoBalance.BossInflectionMult", 1.0f, false), false); // `AutoBalance.BossInflectionMult` for backwards compatibility

        InflectionPointRaidHeroic =                 sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaidHeroic", 0.5f, false);
        InflectionPointRaidHeroicCurveFloor =       sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaidHeroic.CurveFloor", 0.0f, false);
        InflectionPointRaidHeroicCurveCeiling =     sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaidHeroic.CurveCeiling", 1.0f, false);
        InflectionPointRaidHeroicBoss =             sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaidHeroic.BossModifier", sConfigMgr->GetOption<float>("AutoBalance.BossInflectionMult", 1.0f, false), false); // `AutoBalance.BossInflectionMult` for backwards compatibility

        InflectionPointRaid10M =                    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10M", InflectionPointRaid, false);
        InflectionPointRaid10MCurveFloor =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10M.CurveFloor", InflectionPointRaidCurveFloor, false);
        InflectionPointRaid10MCurveCeiling =        sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10M.CurveCeiling", InflectionPointRaidCurveCeiling, false);
        InflectionPointRaid10MBoss =                sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10M.BossModifier", InflectionPointRaidBoss, false);

        InflectionPointRaid10MHeroic =              sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10MHeroic", InflectionPointRaidHeroic, false);
        InflectionPointRaid10MHeroicCurveFloor =    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10MHeroic.CurveFloor", InflectionPointRaidHeroicCurveFloor, false);
        InflectionPointRaid10MHeroicCurveCeiling =  sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10MHeroic.CurveCeiling", InflectionPointRaidHeroicCurveCeiling, false);
        InflectionPointRaid10MHeroicBoss =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid10MHeroic.BossModifier", InflectionPointRaidHeroicBoss, false);

        InflectionPointRaid15M =                    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid15M", InflectionPointRaid, false);
        InflectionPointRaid15MCurveFloor =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid15M.CurveFloor", InflectionPointRaidCurveFloor, false);
        InflectionPointRaid15MCurveCeiling =        sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid15M.CurveCeiling", InflectionPointRaidCurveCeiling, false);
        InflectionPointRaid15MBoss =                sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid15M.BossModifier", InflectionPointRaidBoss, false);

        InflectionPointRaid20M =                    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid20M", InflectionPointRaid, false);
        InflectionPointRaid20MCurveFloor =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid20M.CurveFloor", InflectionPointRaidCurveFloor, false);
        InflectionPointRaid20MCurveCeiling =        sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid20M.CurveCeiling", InflectionPointRaidCurveCeiling, false);
        InflectionPointRaid20MBoss =                sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid20M.BossModifier", InflectionPointRaidBoss, false);

        InflectionPointRaid25M =                    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25M", InflectionPointRaid, false);
        InflectionPointRaid25MCurveFloor =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25M.CurveFloor", InflectionPointRaidCurveFloor, false);
        InflectionPointRaid25MCurveCeiling =        sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25M.CurveCeiling", InflectionPointRaidCurveCeiling, false);
        InflectionPointRaid25MBoss =                sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25M.BossModifier", InflectionPointRaidBoss, false);

        InflectionPointRaid25MHeroic =              sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25MHeroic", InflectionPointRaidHeroic, false);
        InflectionPointRaid25MHeroicCurveFloor =    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25MHeroic.CurveFloor", InflectionPointRaidHeroicCurveFloor, false);
        InflectionPointRaid25MHeroicCurveCeiling =  sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25MHeroic.CurveCeiling", InflectionPointRaidHeroicCurveCeiling, false);
        InflectionPointRaid25MHeroicBoss =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid25MHeroic.BossModifier", InflectionPointRaidHeroicBoss, false);

        InflectionPointRaid40M =                    sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid40M", InflectionPointRaid, false);
        InflectionPointRaid40MCurveFloor =          sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid40M.CurveFloor", InflectionPointRaidCurveFloor, false);
        InflectionPointRaid40MCurveCeiling =        sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid40M.CurveCeiling", InflectionPointRaidCurveCeiling, false);
        InflectionPointRaid40MBoss =                sConfigMgr->GetOption<float>("AutoBalance.InflectionPointRaid40M.BossModifier", InflectionPointRaidBoss, false);

        // StatModifier*
        // warn the console if deprecated values are detected
        if (sConfigMgr->GetOption<float>("AutoBalance.rate.global", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.rate.global` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        if (sConfigMgr->GetOption<float>("AutoBalance.rate.health", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.rate.health` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        if (sConfigMgr->GetOption<float>("AutoBalance.rate.mana", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.rate.mana` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        if (sConfigMgr->GetOption<float>("AutoBalance.rate.armor", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.rate.armor` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        if (sConfigMgr->GetOption<float>("AutoBalance.rate.damage", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.rate.damage` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");

        // 5-player dungeons
        StatModifier_Global =                       sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifier_Health =                       sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifier_Mana =                         sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifier_Armor =                        sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifier_Damage =                       sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifier_CCDuration =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifier.CCDuration", -1.0f, false);

        StatModifier_Boss_Global =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifier_Boss_Health =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifier_Boss_Mana =                    sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifier_Boss_Armor =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifier_Boss_Damage =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifier_Boss_CCDuration =              sConfigMgr->GetOption<float>("AutoBalance.StatModifier.Boss.CCDuration", -1.0f, false);

        // 5-player heroic dungeons
        StatModifierHeroic_Global =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierHeroic_Health =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierHeroic_Mana =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierHeroic_Armor =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierHeroic_Damage =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierHeroic_CCDuration =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.CCDuration", -1.0f, false);

        StatModifierHeroic_Boss_Global =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierHeroic_Boss_Health =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierHeroic_Boss_Mana =              sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierHeroic_Boss_Armor =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierHeroic_Boss_Damage =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierHeroic_Boss_CCDuration =        sConfigMgr->GetOption<float>("AutoBalance.StatModifierHeroic.Boss.CCDuration", -1.0f, false);

        // Default for all raids
        StatModifierRaid_Global =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierRaid_Health =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierRaid_Mana =                     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierRaid_Armor =                    sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierRaid_Damage =                   sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierRaid_CCDuration =               sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.CCDuration", -1.0f, false);

        StatModifierRaid_Boss_Global =              sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierRaid_Boss_Health =              sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierRaid_Boss_Mana =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierRaid_Boss_Armor =               sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierRaid_Boss_Damage =              sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierRaid_Boss_CCDuration =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid.Boss.CCDuration", -1.0f, false);

        // Default for all heroic raids
        StatModifierRaidHeroic_Global =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierRaidHeroic_Health =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierRaidHeroic_Mana =               sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierRaidHeroic_Armor =              sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierRaidHeroic_Damage =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierRaidHeroic_CCDuration =         sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.CCDuration", -1.0f, false);

        StatModifierRaidHeroic_Boss_Global =        sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.Global", sConfigMgr->GetOption<float>("AutoBalance.rate.global", 1.0f, false), false); // `AutoBalance.rate.global` for backwards compatibility
        StatModifierRaidHeroic_Boss_Health =        sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.Health", sConfigMgr->GetOption<float>("AutoBalance.rate.health", 1.0f, false), false); // `AutoBalance.rate.health` for backwards compatibility
        StatModifierRaidHeroic_Boss_Mana =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.Mana", sConfigMgr->GetOption<float>("AutoBalance.rate.mana", 1.0f, false), false); // `AutoBalance.rate.mana` for backwards compatibility
        StatModifierRaidHeroic_Boss_Armor =         sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.Armor", sConfigMgr->GetOption<float>("AutoBalance.rate.armor", 1.0f, false), false); // `AutoBalance.rate.armor` for backwards compatibility
        StatModifierRaidHeroic_Boss_Damage =        sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.Damage", sConfigMgr->GetOption<float>("AutoBalance.rate.damage", 1.0f, false), false); // `AutoBalance.rate.damage` for backwards compatibility
        StatModifierRaidHeroic_Boss_CCDuration =    sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaidHeroic.Boss.CCDuration", -1.0f, false);

        // 10-player raids
        StatModifierRaid10M_Global =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Global", StatModifierRaid_Global, false);
        StatModifierRaid10M_Health =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Health", StatModifierRaid_Health, false);
        StatModifierRaid10M_Mana =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Mana", StatModifierRaid_Mana, false);
        StatModifierRaid10M_Armor =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Armor", StatModifierRaid_Armor, false);
        StatModifierRaid10M_Damage =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Damage", StatModifierRaid_Damage, false);
        StatModifierRaid10M_CCDuration =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.CCDuration", StatModifierRaid_CCDuration, false);

        StatModifierRaid10M_Boss_Global =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.Global", StatModifierRaid_Boss_Global, false);
        StatModifierRaid10M_Boss_Health =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.Health", StatModifierRaid_Boss_Health, false);
        StatModifierRaid10M_Boss_Mana =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.Mana", StatModifierRaid_Boss_Mana, false);
        StatModifierRaid10M_Boss_Armor =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.Armor", StatModifierRaid_Boss_Armor, false);
        StatModifierRaid10M_Boss_Damage =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.Damage", StatModifierRaid_Boss_Damage, false);
        StatModifierRaid10M_Boss_CCDuration =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10M.Boss.CCDuration", StatModifierRaid_Boss_CCDuration, false);

        // 10-player heroic raids
        StatModifierRaid10MHeroic_Global =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Global", StatModifierRaidHeroic_Global, false);
        StatModifierRaid10MHeroic_Health =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Health", StatModifierRaidHeroic_Health, false);
        StatModifierRaid10MHeroic_Mana =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Mana", StatModifierRaidHeroic_Mana, false);
        StatModifierRaid10MHeroic_Armor =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Armor", StatModifierRaidHeroic_Armor, false);
        StatModifierRaid10MHeroic_Damage =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Damage", StatModifierRaidHeroic_Damage, false);
        StatModifierRaid10MHeroic_CCDuration =      sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.CCDuration", StatModifierRaidHeroic_CCDuration, false);

        StatModifierRaid10MHeroic_Boss_Global =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.Global", StatModifierRaidHeroic_Boss_Global, false);
        StatModifierRaid10MHeroic_Boss_Health =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.Health", StatModifierRaidHeroic_Boss_Health, false);
        StatModifierRaid10MHeroic_Boss_Mana =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.Mana", StatModifierRaidHeroic_Boss_Mana, false);
        StatModifierRaid10MHeroic_Boss_Armor =      sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.Armor", StatModifierRaidHeroic_Boss_Armor, false);
        StatModifierRaid10MHeroic_Boss_Damage =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.Damage", StatModifierRaidHeroic_Boss_Damage, false);
        StatModifierRaid10MHeroic_Boss_CCDuration = sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid10MHeroic.Boss.CCDuration", StatModifierRaidHeroic_Boss_CCDuration, false);

        // 15-player raids
        StatModifierRaid15M_Global =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Global", StatModifierRaid_Global, false);
        StatModifierRaid15M_Health =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Health", StatModifierRaid_Health, false);
        StatModifierRaid15M_Mana =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Mana", StatModifierRaid_Mana, false);
        StatModifierRaid15M_Armor =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Armor", StatModifierRaid_Armor, false);
        StatModifierRaid15M_Damage =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Damage", StatModifierRaid_Damage, false);
        StatModifierRaid15M_CCDuration =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.CCDuration", StatModifierRaid_CCDuration, false);

        StatModifierRaid15M_Boss_Global =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.Global", StatModifierRaid_Boss_Global, false);
        StatModifierRaid15M_Boss_Health =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.Health", StatModifierRaid_Boss_Health, false);
        StatModifierRaid15M_Boss_Mana =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.Mana", StatModifierRaid_Boss_Mana, false);
        StatModifierRaid15M_Boss_Armor =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.Armor", StatModifierRaid_Boss_Armor, false);
        StatModifierRaid15M_Boss_Damage =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.Damage", StatModifierRaid_Boss_Damage, false);
        StatModifierRaid15M_Boss_CCDuration =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid15M.Boss.CCDuration", StatModifierRaid_Boss_CCDuration, false);

        // 20-player raids
        StatModifierRaid20M_Global =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Global", StatModifierRaid_Global, false);
        StatModifierRaid20M_Health =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Health", StatModifierRaid_Health, false);
        StatModifierRaid20M_Mana =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Mana", StatModifierRaid_Mana, false);
        StatModifierRaid20M_Armor =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Armor", StatModifierRaid_Armor, false);
        StatModifierRaid20M_Damage =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Damage", StatModifierRaid_Damage, false);
        StatModifierRaid20M_CCDuration =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.CCDuration", StatModifierRaid_CCDuration, false);

        StatModifierRaid20M_Boss_Global =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.Global", StatModifierRaid_Boss_Global, false);
        StatModifierRaid20M_Boss_Health =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.Health", StatModifierRaid_Boss_Health, false);
        StatModifierRaid20M_Boss_Mana =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.Mana", StatModifierRaid_Boss_Mana, false);
        StatModifierRaid20M_Boss_Armor =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.Armor", StatModifierRaid_Boss_Armor, false);
        StatModifierRaid20M_Boss_Damage =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.Damage", StatModifierRaid_Boss_Damage, false);
        StatModifierRaid20M_Boss_CCDuration =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid20M.Boss.CCDuration", StatModifierRaid_Boss_CCDuration, false);

        // 25-player raids
        StatModifierRaid25M_Global =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Global", StatModifierRaid_Global, false);
        StatModifierRaid25M_Health =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Health", StatModifierRaid_Health, false);
        StatModifierRaid25M_Mana =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Mana", StatModifierRaid_Mana, false);
        StatModifierRaid25M_Armor =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Armor", StatModifierRaid_Armor, false);
        StatModifierRaid25M_Damage =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Damage", StatModifierRaid_Damage, false);
        StatModifierRaid25M_CCDuration =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.CCDuration", StatModifierRaid_CCDuration, false);

        StatModifierRaid25M_Boss_Global =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.Global", StatModifierRaid_Boss_Global, false);
        StatModifierRaid25M_Boss_Health =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.Health", StatModifierRaid_Boss_Health, false);
        StatModifierRaid25M_Boss_Mana =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.Mana", StatModifierRaid_Boss_Mana, false);
        StatModifierRaid25M_Boss_Armor =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.Armor", StatModifierRaid_Boss_Armor, false);
        StatModifierRaid25M_Boss_Damage =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.Damage", StatModifierRaid_Boss_Damage, false);
        StatModifierRaid25M_Boss_CCDuration =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25M.Boss.CCDuration", StatModifierRaid_Boss_CCDuration, false);

        // 25-player heroic raids
        StatModifierRaid25MHeroic_Global =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Global", StatModifierRaidHeroic_Global, false);
        StatModifierRaid25MHeroic_Health =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Health", StatModifierRaidHeroic_Health, false);
        StatModifierRaid25MHeroic_Mana =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Mana", StatModifierRaidHeroic_Mana, false);
        StatModifierRaid25MHeroic_Armor =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Armor", StatModifierRaidHeroic_Armor, false);
        StatModifierRaid25MHeroic_Damage =          sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Damage", StatModifierRaidHeroic_Damage, false);
        StatModifierRaid25MHeroic_CCDuration =      sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.CCDuration", StatModifierRaidHeroic_CCDuration, false);

        StatModifierRaid25MHeroic_Boss_Global =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.Global", StatModifierRaidHeroic_Boss_Global, false);
        StatModifierRaid25MHeroic_Boss_Health =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.Health", StatModifierRaidHeroic_Boss_Health, false);
        StatModifierRaid25MHeroic_Boss_Mana =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.Mana", StatModifierRaidHeroic_Boss_Mana, false);
        StatModifierRaid25MHeroic_Boss_Armor =      sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.Armor", StatModifierRaidHeroic_Boss_Armor, false);
        StatModifierRaid25MHeroic_Boss_Damage =     sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.Damage", StatModifierRaidHeroic_Boss_Damage, false);
        StatModifierRaid25MHeroic_Boss_CCDuration = sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid25MHeroic.Boss.CCDuration", StatModifierRaidHeroic_Boss_CCDuration, false);

        // 40-player raids
        StatModifierRaid40M_Global =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Global", StatModifierRaid_Global, false);
        StatModifierRaid40M_Health =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Health", StatModifierRaid_Health, false);
        StatModifierRaid40M_Mana =                  sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Mana", StatModifierRaid_Mana, false);
        StatModifierRaid40M_Armor =                 sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Armor", StatModifierRaid_Armor, false);
        StatModifierRaid40M_Damage =                sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Damage", StatModifierRaid_Damage, false);
        StatModifierRaid40M_CCDuration =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.CCDuration", StatModifierRaid_CCDuration, false);

        StatModifierRaid40M_Boss_Global =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.Global", StatModifierRaid_Boss_Global, false);
        StatModifierRaid40M_Boss_Health =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.Health", StatModifierRaid_Boss_Health, false);
        StatModifierRaid40M_Boss_Mana =             sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.Mana", StatModifierRaid_Boss_Mana, false);
        StatModifierRaid40M_Boss_Armor =            sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.Armor", StatModifierRaid_Boss_Armor, false);
        StatModifierRaid40M_Boss_Damage =           sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.Damage", StatModifierRaid_Boss_Damage, false);
        StatModifierRaid40M_Boss_CCDuration =       sConfigMgr->GetOption<float>("AutoBalance.StatModifierRaid40M.Boss.CCDuration", StatModifierRaid_Boss_CCDuration, false);

        // Modifier Min/Max
        MinHPModifier = sConfigMgr->GetOption<float>("AutoBalance.MinHPModifier", 0.1f);
        MinManaModifier = sConfigMgr->GetOption<float>("AutoBalance.MinManaModifier", 0.01f);
        MinDamageModifier = sConfigMgr->GetOption<float>("AutoBalance.MinDamageModifier", 0.01f);
        MinCCDurationModifier = sConfigMgr->GetOption<float>("AutoBalance.MinCCDurationModifier", 0.25f);
        MaxCCDurationModifier = sConfigMgr->GetOption<float>("AutoBalance.MaxCCDurationModifier", 1.0f);

        // Announcement
        Announcement = sConfigMgr->GetOption<bool>("AutoBalanceAnnounce.enable", true);

    }
};

class AutoBalance_PlayerScript : public PlayerScript
{
    public:
        AutoBalance_PlayerScript()
            : PlayerScript("AutoBalance_PlayerScript")
        {
        }

        void OnLogin(Player *Player) override
        {
            if (EnableGlobal && Announcement) {
                ChatHandler(Player->GetSession()).SendSysMessage("This server is running the |cff4CFF00AutoBalance |rmodule.");
            }
        }

        virtual void OnLevelChanged(Player* player, uint8 /*oldlevel*/) override
        {
            if (!EnableGlobal || !player)
                return;

            if (LevelScaling == 0)
                return;

            AutoBalanceMapInfo *mapABInfo=player->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (mapABInfo->mapLevel < player->getLevel())
                mapABInfo->mapLevel = player->getLevel();


        }

        void OnGiveXP(Player* player, uint32& amount, Unit* victim, uint8 /*xpSource*/) override
        {
            Map* map = player->GetMap();
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (victim && DungeonScaleDownXP && mapABInfo->enabled)
            {
                Map* map = player->GetMap();

                if (map->IsDungeon())
                {
                    // Ensure that the players always get the same XP, even when entering the dungeon alone
                    auto maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                    auto currentPlayerCount = map->GetPlayersCountExceptGMs();
                    amount *= (float)currentPlayerCount / maxPlayerCount;
                }
            }
        }

        void OnBeforeLootMoney(Player* player, Loot* loot) override
        {
            Map* map = player->GetMap();
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (DungeonScaleDownMoney && mapABInfo->enabled)
            {
                Map* map = player->GetMap();

                if (map->IsDungeon())
                {
                    // Ensure that the players always get the same money, even when entering the dungeon alone
                    auto maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                    auto currentPlayerCount = map->GetPlayersCountExceptGMs();
                    loot->gold = uint32(loot->gold * ((float)currentPlayerCount / (float)maxPlayerCount));
                }
            }

        }
};

class AutoBalance_UnitScript : public UnitScript
{
    public:
    AutoBalance_UnitScript()
        : UnitScript("AutoBalance_UnitScript", true)
    {
    }

    uint32 DealDamage(Unit* AttackerUnit, Unit *playerVictim, uint32 damage, DamageEffectType /*damagetype*/) override
    {
        return _Modifer_DealDamage(playerVictim, AttackerUnit, damage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void ModifyHealReceived(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = _Modifer_DealDamage(target, attacker, damage);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override {
        // Only if this aura has a duration
        if (aura->GetDuration() > 0 || aura->GetMaxDuration() > 0)
        {
            uint32 auraDuration = _Modifier_CCDuration(unit, aura->GetCaster(), aura);

            // only update if we decided to change it
            if (auraDuration != (float)aura->GetDuration())
            {
                aura->SetMaxDuration(auraDuration);
                aura->SetDuration(auraDuration);
            }
        }
    }

    uint32 _Modifer_DealDamage(Unit* target, Unit* attacker, uint32 damage)
    {
        // check that we're enabled globally, else return the original damage
        if (!EnableGlobal)
            return damage;

        // make sure we have an attacker, that its not a player, and that the attacker is in the world, else return the original damage
        if (!attacker || attacker->GetTypeId() == TYPEID_PLAYER || !attacker->IsInWorld())
            return damage;

        // make sure we're in an instance, else return the original damage
        if (
            !(
                (target->GetMap()->IsDungeon() && attacker->GetMap()->IsDungeon()) ||
                (target->GetMap()->IsBattleground() && attacker->GetMap()->IsBattleground())
            )
           )
            return damage;

        // get the map's info to see if we're enabled
        AutoBalanceMapInfo *targetMapInfo = target->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        AutoBalanceMapInfo *attackerMapInfo = attacker->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        // if either the target or the attacker's maps are not enabled, return the original damage
        if (!targetMapInfo->enabled || !attackerMapInfo->enabled)
            return damage;

        // get the current creature's damage multiplier
        float damageMultiplier = attacker->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->DamageMultiplier;

        // if it's the default of 1.0, return the original damage
        if (damageMultiplier == 1)
            return damage;

        // if the attacker is under the control of the player, return the original damage
        if ((attacker->IsHunterPet() || attacker->IsPet() || attacker->IsSummon()) && attacker->IsControlledByPlayer())
            return damage;

        // we are good to go, return the original damage times the multiplier
        return damage * damageMultiplier;
    }

    uint32 _Modifier_CCDuration(Unit* target, Unit* caster, Aura* aura)
    {
        // store the original duration of the aura
        float originalDuration = (float)aura->GetDuration();

        // check that we're enabled globally, else return the original duration
        if (!EnableGlobal)
            return originalDuration;

        // if the target isn't a player or the caster is a player, return the original duration
        if (!target->IsPlayer() || caster->IsPlayer())
            return originalDuration;

        // make sure we're in an instance, else return the original duration
        if (
            !(
                (target->GetMap()->IsDungeon() && caster->GetMap()->IsDungeon()) ||
                (target->GetMap()->IsBattleground() && caster->GetMap()->IsBattleground())
            )
           )
            return originalDuration;

        // get the current creature's CC duration multiplier
        float ccDurationMultiplier = caster->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->CCDurationMultiplier;

        // if it's the default of 1.0, return the original damage
        if (ccDurationMultiplier == 1)
            return originalDuration;

        // if the aura was cast by a pet or summon, return the original duration
        if ((caster->IsHunterPet() || caster->IsPet() || caster->IsSummon()) && caster->IsControlledByPlayer())
            return originalDuration;

        // only if this aura is a CC
        if (
            aura->HasEffectType(SPELL_AURA_MOD_CHARM)          ||
            aura->HasEffectType(SPELL_AURA_MOD_CONFUSE)        ||
            aura->HasEffectType(SPELL_AURA_MOD_DISARM)         ||
            aura->HasEffectType(SPELL_AURA_MOD_FEAR)           ||
            aura->HasEffectType(SPELL_AURA_MOD_PACIFY)         ||
            aura->HasEffectType(SPELL_AURA_MOD_POSSESS)        ||
            aura->HasEffectType(SPELL_AURA_MOD_SILENCE)        ||
            aura->HasEffectType(SPELL_AURA_MOD_STUN)           ||
            aura->HasEffectType(SPELL_AURA_MOD_SPEED_SLOW_ALL)
            )
        {
            return originalDuration * ccDurationMultiplier;
        }
        else
        {
            return originalDuration;
        }
    }
};


class AutoBalance_AllMapScript : public AllMapScript
{
    public:
    AutoBalance_AllMapScript()
        : AllMapScript("AutoBalance_AllMapScript")
        {
        }

        void OnPlayerEnterAll(Map* map, Player* player)
        {
            if (!EnableGlobal)
                return;

            if (player->IsGameMaster())
                return;

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // determine if the map should be enabled for scaling based on the current settings
            mapABInfo->enabled = ShouldMapBeEnabled(map);

            // always check level, even if not conf enabled
            // because we can enable at runtime and we need this information
            if (player) {
                if (player->getLevel() > mapABInfo->mapLevel)
                    mapABInfo->mapLevel = player->getLevel();
            } else {
                Map::PlayerList const &playerList = map->GetPlayers();
                if (!playerList.IsEmpty())
                {
                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        if (Player* playerHandle = playerIteration->GetSource())
                        {
                            if (!playerHandle->IsGameMaster() && playerHandle->getLevel() > mapABInfo->mapLevel)
                                mapABInfo->mapLevel = playerHandle->getLevel();
                        }
                    }
                }
            }

            //mapABInfo->playerCount++; //(maybe we've to found a safe solution to avoid player recount each time)
            mapABInfo->playerCount = map->GetPlayersCountExceptGMs();

            if (PlayerChangeNotify && mapABInfo->enabled)
            {
                if (map->GetEntry()->IsDungeon() && player)
                {
                    Map::PlayerList const &playerList = map->GetPlayers();
                    if (!playerList.IsEmpty())
                    {
                        for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                        {
                            if (Player* playerHandle = playerIteration->GetSource())
                            {
                                ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                                auto instanceMap = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()));

                                std::string instanceDifficulty; if (instanceMap->IsHeroic()) instanceDifficulty = "Heroic"; else instanceDifficulty = "Normal";

                                chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s enters %s (%u-player %s). Player count set to %u (Player Difficulty Offset = %u) |r", player->GetName().c_str(), map->GetMapName(), instanceMap->GetMaxPlayers(), instanceDifficulty, mapABInfo->playerCount + PlayerCountDifficultyOffset, PlayerCountDifficultyOffset);
                            }
                        }
                    }
                }
            }
        }

        void OnPlayerLeaveAll(Map* map, Player* player)
        {
            if (!EnableGlobal)
                return;

            if (player->IsGameMaster())
                return;

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // determine if the map should be enabled for scaling based on the current settings
            mapABInfo->enabled = ShouldMapBeEnabled(map);

            //mapABInfo->playerCount--;// (maybe we've to found a safe solution to avoid player recount each time)
            // mapABInfo->playerCount = map->GetPlayersCountExceptGMs();
            //pklloveyou天鹿:
            if (map->GetEntry() && map->GetEntry()->IsDungeon())
            {
                bool AutoBalanceCheck = false;
                Map::PlayerList const& pl = map->GetPlayers();
                for (Map::PlayerList::const_iterator itr = pl.begin(); itr != pl.end(); ++itr)
                {
                    if (Player* plr = itr->GetSource())
                    {
                        if (plr->IsInCombat())
                            AutoBalanceCheck = true;
                    }
                }
                //pklloveyou天鹿:
                if (AutoBalanceCheck)
                {
                    for (Map::PlayerList::const_iterator itr = pl.begin(); itr != pl.end(); ++itr)
                    {
                        if (Player* plr = itr->GetSource())
                        {
                            plr->GetSession()->SendNotification("|cff4cff00%s|rAccess can be unlocked during non-combat", map->GetMapName());
                            plr->GetSession()->SendNotification("|cffffffff[%s]|rThe player left during the battle|cff4cff00%s|rInstance elastic lock", player->GetName().c_str(), map->GetMapName());
                        }
                    }
                }
                else
                {
                    //mapABInfo->playerCount--;
                    mapABInfo->playerCount = map->GetPlayersCountExceptGMs() - 1;
                }
            }

            // always check level, even if not conf enabled
            // because we can enable at runtime and we need this information
            if (!mapABInfo->playerCount) {
                mapABInfo->mapLevel = 0;
                return;
            }

            if (PlayerChangeNotify && mapABInfo->enabled)
            {
                if (map->GetEntry()->IsDungeon() && player)
                {
                    Map::PlayerList const &playerList = map->GetPlayers();
                    if (!playerList.IsEmpty())
                    {
                        for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                        {
                            if (Player* playerHandle = playerIteration->GetSource())
                            {
                                ChatHandler chatHandle = ChatHandler(playerHandle->GetSession());
                                chatHandle.PSendSysMessage("|cffFF0000 [-AutoBalance]|r|cffFF8000 %s left the Instance %s. Auto setting player count to %u (Player Difficulty Offset = %u) |r", player->GetName().c_str(), map->GetMapName(), mapABInfo->playerCount, PlayerCountDifficultyOffset);
                            }
                        }
                    }
                }
            }
        }
};

class AutoBalance_AllCreatureScript : public AllCreatureScript
{
public:
    AutoBalance_AllCreatureScript()
        : AllCreatureScript("AutoBalance_AllCreatureScript")
    {
    }

    void Creature_SelectLevel(const CreatureTemplate* /*creatureTemplate*/, Creature* creature) override
    {
        ModifyCreatureAttributes(creature, true);
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        ModifyCreatureAttributes(creature);
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        ModifyCreatureAttributes(creature, true);
    }


    bool checkLevelOffset(uint8 selectedLevel, uint8 targetLevel) {
        return selectedLevel && ((targetLevel >= selectedLevel && targetLevel <= (selectedLevel + higherOffset) ) || (targetLevel <= selectedLevel && targetLevel >= (selectedLevel - lowerOffset)));
    }

    void ModifyCreatureAttributes(Creature* creature, bool resetSelLevel = false)
    {
        // make sure that we're enabled globally
        if (!EnableGlobal)
            return;

        // make sure we have a creature and that it's assigned to a map
        if (!creature || !creature->GetMap())
            return;

        // check to make sure that the creature's map is enabled for scaling
        AutoBalanceMapInfo *mapABInfo=creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
        if (!mapABInfo->enabled)
            return;

        // if this isn't a dungeon or a battleground, make no changes
        if (!(creature->GetMap()->IsDungeon() || creature->GetMap()->IsBattleground()))
            return;

        // if this is a pet or summon, make no changes
        if (((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer()))
        {
            return;
        }

        // if a map level is not set, make no changes
        if (!mapABInfo->mapLevel)
            return;

        CreatureTemplate const *creatureTemplate = creature->GetCreatureTemplate();

        InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));
        uint32 mapId = instanceMap->GetEntry()->MapID;
        if (perDungeonScalingEnabled() && !isEnabledDungeon(mapId))
        {
            return;
        }
        uint32 maxNumberOfPlayers = instanceMap->GetMaxPlayers();
        int forcedNumPlayers = GetForcedNumPlayers(creatureTemplate->Entry);

        if (forcedNumPlayers > 0)
            maxNumberOfPlayers = forcedNumPlayers; // Force maxNumberOfPlayers to be changed to match the Configuration entries ForcedID2, ForcedID5, ForcedID10, ForcedID20, ForcedID25, ForcedID40
        else if (forcedNumPlayers == 0)
            return; // forcedNumPlayers 0 means that the creature is contained in DisabledID -> no scaling

        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        // force resetting selected level.
        // this is also a "workaround" to fix bug of not recalculated
        // attributes when UpdateEntry has been used.
        // TODO: It's better and faster to implement a core hook
        // in that position and force a recalculation then
        if ((creatureABInfo->entry != 0 && creatureABInfo->entry != creature->GetEntry()) || resetSelLevel) {
            creatureABInfo->selectedLevel = 0; // force a recalculation
        }

        if (!creature->IsAlive())
            return;

        uint32 curCount=mapABInfo->playerCount + PlayerCountDifficultyOffset;
        if (perDungeonScalingEnabled())
        {
            curCount = adjustCurCount(curCount, mapId);
        }

        uint8 bonusLevel = creatureTemplate->rank == CREATURE_ELITE_WORLDBOSS ? 3 : 0;
        // already scaled
        if (creatureABInfo->selectedLevel > 0) {
            if (LevelScaling) {
                if (checkLevelOffset(mapABInfo->mapLevel + bonusLevel, creature->getLevel()) &&
                    checkLevelOffset(creatureABInfo->selectedLevel, creature->getLevel()) &&
                    creatureABInfo->instancePlayerCount == curCount) {
                    return;
                }
            } else if (creatureABInfo->instancePlayerCount == curCount) {
                    return;
            }
        }

        creatureABInfo->instancePlayerCount = curCount;

        if (!creatureABInfo->instancePlayerCount) // no players in map, do not modify attributes
            return;

        if (!sABScriptMgr->OnBeforeModifyAttributes(creature, creatureABInfo->instancePlayerCount))
            return;

        uint8 originalLevel = creatureTemplate->maxlevel;

        uint8 level = mapABInfo->mapLevel;

        uint8 areaMinLvl, areaMaxLvl;
        getAreaLevel(creature->GetMap(), creature->GetAreaId(), areaMinLvl, areaMaxLvl);

        // avoid level changing for critters and special creatures (spell summons etc.) in instances
        if (originalLevel <= 1 && areaMinLvl >= 5)
            return;

        if (LevelScaling && creature->GetMap()->IsDungeon() && !checkLevelOffset(level, originalLevel)) {  // change level only within the offsets and when in dungeon/raid
            if (level != creatureABInfo->selectedLevel || creatureABInfo->selectedLevel != creature->getLevel()) {
                // keep bosses +3 level
                creatureABInfo->selectedLevel = level + bonusLevel;
                creature->SetLevel(creatureABInfo->selectedLevel);
            }
        } else {
            creatureABInfo->selectedLevel = creature->getLevel();
        }

        creatureABInfo->entry = creature->GetEntry();

        bool useDefStats = false;
        if (LevelUseDb && creature->getLevel() >= creatureTemplate->minlevel && creature->getLevel() <= creatureTemplate->maxlevel)
            useDefStats = true;

        CreatureBaseStats const* origCreatureStats = sObjectMgr->GetCreatureBaseStats(originalLevel, creatureTemplate->unit_class);
        CreatureBaseStats const* creatureStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->selectedLevel, creatureTemplate->unit_class);

        uint32 baseHealth = origCreatureStats->GenerateHealth(creatureTemplate);
        uint32 baseMana = origCreatureStats->GenerateMana(creatureTemplate);
        uint32 scaledHealth = 0;
        uint32 scaledMana = 0;

        // Note: InflectionPoint handle the number of players required to get 50% health.
        //       you'd adjust this to raise or lower the hp modifier for per additional player in a non-whole group.
        //
        //       diff modify the rate of percentage increase between
        //       number of players. Generally the closer to the value of 1 you have this
        //       the less gradual the rate will be. For example in a 5 man it would take 3
        //       total players to face a mob at full health.
        //
        //       The +1 and /2 values raise the TanH function to a positive range and make
        //       sure the modifier never goes above the value or 1.0 or below 0.
        //
        //       curveFloor and curveCeiling squishes the curve by adjusting the curve start and end points.
        //       This allows for better control over high and low player count scaling.

        float defaultMultiplier;
        float curveFloor;
        float curveCeiling;

        //
        // Inflection Point
        //
        float inflectionValue  = (float)maxNumberOfPlayers;

        if (instanceMap->IsHeroic())
        {
            switch (maxNumberOfPlayers)
            {
			    case 1:
			    case 2:
			    case 3:
			    case 4:
			    case 5:
                    inflectionValue *= InflectionPointHeroic;
                    curveFloor = InflectionPointHeroicCurveFloor;
                    curveCeiling = InflectionPointHeroicCurveCeiling;
                    break;
                case 10:
                    inflectionValue *= InflectionPointRaid10MHeroic;
                    curveFloor = InflectionPointRaid10MHeroicCurveFloor;
                    curveCeiling = InflectionPointRaid10MHeroicCurveCeiling;
                    break;
                case 25:
                    inflectionValue *= InflectionPointRaid25MHeroic;
                    curveFloor = InflectionPointRaid25MHeroicCurveFloor;
                    curveCeiling = InflectionPointRaid25MHeroicCurveCeiling;
                    break;
                default:
                    inflectionValue *= InflectionPointRaidHeroic;
                    curveFloor = InflectionPointRaidHeroicCurveFloor;
                    curveCeiling = InflectionPointRaidHeroicCurveCeiling;
            }
        }
        else
        {
            switch (maxNumberOfPlayers)
            {
			    case 1:
			    case 2:
			    case 3:
			    case 4:
			    case 5:
                    inflectionValue *= InflectionPoint;
                    curveFloor = InflectionPointCurveFloor;
                    curveCeiling = InflectionPointCurveCeiling;
                    break;
                case 10:
                    inflectionValue *= InflectionPointRaid10M;
                    curveFloor = InflectionPointRaid10MCurveFloor;
                    curveCeiling = InflectionPointRaid10MCurveCeiling;
                    break;
                case 15:
                    inflectionValue *= InflectionPointRaid15M;
                    curveFloor = InflectionPointRaid15MCurveFloor;
                    curveCeiling = InflectionPointRaid15MCurveCeiling;
                    break;
                case 20:
                    inflectionValue *= InflectionPointRaid20M;
                    curveFloor = InflectionPointRaid20MCurveFloor;
                    curveCeiling = InflectionPointRaid20MCurveCeiling;
                    break;
                case 25:
                    inflectionValue *= InflectionPointRaid25M;
                    curveFloor = InflectionPointRaid25MCurveFloor;
                    curveCeiling = InflectionPointRaid25MCurveCeiling;
                    break;
                case 40:
                    inflectionValue *= InflectionPointRaid40M;
                    curveFloor = InflectionPointRaid40MCurveFloor;
                    curveCeiling = InflectionPointRaid40MCurveCeiling;
                    break;
                default:
                    inflectionValue *= InflectionPointRaid;
                    curveFloor = InflectionPointRaidCurveFloor;
                    curveCeiling = InflectionPointRaidCurveCeiling;
            }
        }

        // Per map ID overrides alter the above settings, if set
        if (hasDungeonOverride(mapId))
        {
            AutoBalanceInflectionPointSettings* myInflectionPointOverrides = &dungeonOverrides[mapId];

            // Alter the inflectionValue according to the override, if set
            if (myInflectionPointOverrides->value != -1)
            {
                inflectionValue  = (float)maxNumberOfPlayers; // Starting over
                inflectionValue *= myInflectionPointOverrides->value;
            }

            if (myInflectionPointOverrides->curveFloor != -1)   { curveFloor =    myInflectionPointOverrides->curveFloor;   }
            if (myInflectionPointOverrides->curveCeiling != -1) { curveCeiling =  myInflectionPointOverrides->curveCeiling; }
        }

        //
        // Boss Inflection Point
        //
        if (creature->IsDungeonBoss()) {

            float bossInflectionPointMultiplier;

            // Determine the correct boss inflection multiplier
            if (instanceMap->IsHeroic())
            {
                switch (maxNumberOfPlayers)
                {
			        case 1:
			        case 2:
			        case 3:
			        case 4:
			        case 5:
                        bossInflectionPointMultiplier = InflectionPointHeroicBoss;
                        break;
                    case 10:
                        bossInflectionPointMultiplier = InflectionPointRaid10MHeroicBoss;
                        break;
                    case 25:
                        bossInflectionPointMultiplier = InflectionPointRaid25MHeroicBoss;
                        break;
                    default:
                        bossInflectionPointMultiplier = InflectionPointRaidHeroicBoss;
                }
            }
            else
            {
                switch (maxNumberOfPlayers)
                {
			        case 1:
			        case 2:
			        case 3:
			        case 4:
			        case 5:
                        bossInflectionPointMultiplier = InflectionPointBoss;
                        break;
                    case 10:
                        bossInflectionPointMultiplier = InflectionPointRaid10MBoss;
                        break;
                    case 15:
                        bossInflectionPointMultiplier = InflectionPointRaid15MBoss;
                        break;
                    case 20:
                        bossInflectionPointMultiplier = InflectionPointRaid20MBoss;
                        break;
                    case 25:
                        bossInflectionPointMultiplier = InflectionPointRaid25MBoss;
                        break;
                    case 40:
                        bossInflectionPointMultiplier = InflectionPointRaid40MBoss;
                        break;
                    default:
                        bossInflectionPointMultiplier = InflectionPointRaidBoss;
                }
            }

            // Per map ID overrides alter the above settings, if set
            if (hasBossOverride(mapId))
            {
                AutoBalanceInflectionPointSettings* myBossOverrides = &bossOverrides[mapId];

                // If set, alter the inflectionValue according to the override
                if (myBossOverrides->value != -1)
                {
                    inflectionValue *= myBossOverrides->value;
                }
                // Otherwise, calculate using the value determined by instance type
                else
                {
                    inflectionValue *= bossInflectionPointMultiplier;
                }
            }
            // No override, use the value determined by the instance type
            else
            {
                inflectionValue *= bossInflectionPointMultiplier;
            }
        }

        //
        // Stat Modifiers
        //

        // Calculate stat modifiers
        float statMod_global, statMod_health, statMod_mana, statMod_armor, statMod_damage, statMod_ccDuration;
        float statMod_boss_global, statMod_boss_health, statMod_boss_mana, statMod_boss_armor, statMod_boss_damage, statMod_boss_ccDuration;

        // Apply the per-instance-type modifiers first
		if (instanceMap->IsHeroic())
		{
			switch (maxNumberOfPlayers)
			{
			    case 1:
			    case 2:
			    case 3:
			    case 4:
			    case 5:
			        statMod_global = StatModifierHeroic_Global;
			        statMod_health = StatModifierHeroic_Health;
			        statMod_mana = StatModifierHeroic_Mana;
			        statMod_armor = StatModifierHeroic_Armor;
			        statMod_damage = StatModifierHeroic_Damage;
			        statMod_ccDuration = StatModifierHeroic_CCDuration;

			        statMod_boss_global = StatModifierHeroic_Boss_Global;
			        statMod_boss_health = StatModifierHeroic_Boss_Health;
			        statMod_boss_mana = StatModifierHeroic_Boss_Mana;
			        statMod_boss_armor = StatModifierHeroic_Boss_Armor;
			        statMod_boss_damage = StatModifierHeroic_Boss_Damage;
			        statMod_boss_ccDuration = StatModifierHeroic_Boss_CCDuration;
			        break;
			    case 10:
                    statMod_global = StatModifierRaid10MHeroic_Global;
                    statMod_health = StatModifierRaid10MHeroic_Health;
                    statMod_mana = StatModifierRaid10MHeroic_Mana;
                    statMod_armor = StatModifierRaid10MHeroic_Armor;
                    statMod_damage = StatModifierRaid10MHeroic_Damage;
                    statMod_ccDuration = StatModifierRaid10MHeroic_CCDuration;

                    statMod_boss_global = StatModifierRaid10MHeroic_Boss_Global;
                    statMod_boss_health = StatModifierRaid10MHeroic_Boss_Health;
                    statMod_boss_mana = StatModifierRaid10MHeroic_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid10MHeroic_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid10MHeroic_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid10MHeroic_Boss_CCDuration;
			        break;
			    case 25:
                    statMod_global = StatModifierRaid25MHeroic_Global;
                    statMod_health = StatModifierRaid25MHeroic_Health;
                    statMod_mana = StatModifierRaid25MHeroic_Mana;
                    statMod_armor = StatModifierRaid25MHeroic_Armor;
                    statMod_damage = StatModifierRaid25MHeroic_Damage;
                    statMod_ccDuration = StatModifierRaid25MHeroic_CCDuration;

                    statMod_boss_global = StatModifierRaid25MHeroic_Boss_Global;
                    statMod_boss_health = StatModifierRaid25MHeroic_Boss_Health;
                    statMod_boss_mana = StatModifierRaid25MHeroic_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid25MHeroic_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid25MHeroic_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid25MHeroic_Boss_CCDuration;
                    break;
			    default:
                    statMod_global = StatModifierRaidHeroic_Global;
                    statMod_health = StatModifierRaidHeroic_Health;
                    statMod_mana = StatModifierRaidHeroic_Mana;
                    statMod_armor = StatModifierRaidHeroic_Armor;
                    statMod_damage = StatModifierRaidHeroic_Damage;
                    statMod_ccDuration = StatModifierRaidHeroic_CCDuration;

                    statMod_boss_global = StatModifierRaidHeroic_Global;
                    statMod_boss_health = StatModifierRaidHeroic_Health;
                    statMod_boss_mana = StatModifierRaidHeroic_Mana;
                    statMod_boss_armor = StatModifierRaidHeroic_Armor;
                    statMod_boss_damage = StatModifierRaidHeroic_Damage;
                    statMod_boss_ccDuration = StatModifierRaidHeroic_Boss_CCDuration;
			}
		}
		else
		{
			switch (maxNumberOfPlayers)
			{
			    case 1:
			    case 2:
			    case 3:
			    case 4:
			    case 5:
			        statMod_global = StatModifier_Global;
			        statMod_health = StatModifier_Health;
			        statMod_mana = StatModifier_Mana;
			        statMod_armor = StatModifier_Armor;
			        statMod_damage = StatModifier_Damage;
			        statMod_ccDuration = StatModifier_CCDuration;

			        statMod_boss_global = StatModifier_Boss_Global;
			        statMod_boss_health = StatModifier_Boss_Health;
			        statMod_boss_mana = StatModifier_Boss_Mana;
			        statMod_boss_armor = StatModifier_Boss_Armor;
			        statMod_boss_damage = StatModifier_Boss_Damage;
			        statMod_boss_ccDuration = StatModifier_Boss_CCDuration;
			        break;
			    case 10:
                    statMod_global = StatModifierRaid10M_Global;
                    statMod_health = StatModifierRaid10M_Health;
                    statMod_mana = StatModifierRaid10M_Mana;
                    statMod_armor = StatModifierRaid10M_Armor;
                    statMod_damage = StatModifierRaid10M_Damage;
                    statMod_ccDuration = StatModifierRaid10M_CCDuration;

                    statMod_boss_global = StatModifierRaid10M_Boss_Global;
                    statMod_boss_health = StatModifierRaid10M_Boss_Health;
                    statMod_boss_mana = StatModifierRaid10M_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid10M_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid10M_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid10M_Boss_CCDuration;
                    break;
			    case 15:
                    statMod_global = StatModifierRaid15M_Global;
                    statMod_health = StatModifierRaid15M_Health;
                    statMod_mana = StatModifierRaid15M_Mana;
                    statMod_armor = StatModifierRaid15M_Armor;
                    statMod_damage = StatModifierRaid15M_Damage;
                    statMod_ccDuration = StatModifierRaid15M_CCDuration;

                    statMod_boss_global = StatModifierRaid15M_Boss_Global;
                    statMod_boss_health = StatModifierRaid15M_Boss_Health;
                    statMod_boss_mana = StatModifierRaid15M_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid15M_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid15M_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid15M_Boss_CCDuration;
                    break;
			    case 20:
                    statMod_global = StatModifierRaid20M_Global;
                    statMod_health = StatModifierRaid20M_Health;
                    statMod_mana = StatModifierRaid20M_Mana;
                    statMod_armor = StatModifierRaid20M_Armor;
                    statMod_damage = StatModifierRaid20M_Damage;
                    statMod_ccDuration = StatModifierRaid20M_CCDuration;

                    statMod_boss_global = StatModifierRaid20M_Boss_Global;
                    statMod_boss_health = StatModifierRaid20M_Boss_Health;
                    statMod_boss_mana = StatModifierRaid20M_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid20M_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid20M_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid20M_Boss_CCDuration;
                    break;
			    case 25:
                    statMod_global = StatModifierRaid25M_Global;
                    statMod_health = StatModifierRaid25M_Health;
                    statMod_mana = StatModifierRaid25M_Mana;
                    statMod_armor = StatModifierRaid25M_Armor;
                    statMod_damage = StatModifierRaid25M_Damage;
                    statMod_ccDuration = StatModifierRaid25M_CCDuration;

                    statMod_boss_global = StatModifierRaid25M_Boss_Global;
                    statMod_boss_health = StatModifierRaid25M_Boss_Health;
                    statMod_boss_mana = StatModifierRaid25M_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid25M_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid25M_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid25M_Boss_CCDuration;
                    break;
			    case 40:
                    statMod_global = StatModifierRaid40M_Global;
                    statMod_health = StatModifierRaid40M_Health;
                    statMod_mana = StatModifierRaid40M_Mana;
                    statMod_armor = StatModifierRaid40M_Armor;
                    statMod_damage = StatModifierRaid40M_Damage;
                    statMod_ccDuration = StatModifierRaid40M_CCDuration;

                    statMod_boss_global = StatModifierRaid40M_Boss_Global;
                    statMod_boss_health = StatModifierRaid40M_Boss_Health;
                    statMod_boss_mana = StatModifierRaid40M_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid40M_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid40M_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid40M_Boss_CCDuration;
                    break;
			    default:
                    statMod_global = StatModifierRaid_Global;
                    statMod_health = StatModifierRaid_Health;
                    statMod_mana = StatModifierRaid_Mana;
                    statMod_armor = StatModifierRaid_Armor;
                    statMod_damage = StatModifierRaid_Damage;
                    statMod_ccDuration = StatModifierRaid_CCDuration;

                    statMod_boss_global = StatModifierRaid_Boss_Global;
                    statMod_boss_health = StatModifierRaid_Boss_Health;
                    statMod_boss_mana = StatModifierRaid_Boss_Mana;
                    statMod_boss_armor = StatModifierRaid_Boss_Armor;
                    statMod_boss_damage = StatModifierRaid_Boss_Damage;
                    statMod_boss_ccDuration = StatModifierRaid_Boss_CCDuration;
			}
		}

        // Boss modifiers
        if (creature->IsDungeonBoss())
        {
            // Start with the settings determined above
            // AutoBalance.StatModifier*.Boss.<stat>
            if (creature->IsDungeonBoss())
            {
                statMod_global = statMod_boss_global;
                statMod_health = statMod_boss_health;
                statMod_mana = statMod_boss_mana;
                statMod_armor = statMod_boss_armor;
                statMod_damage = statMod_boss_damage;
                statMod_ccDuration = statMod_boss_ccDuration;
            }

            // Per-instance boss overrides
            // AutoBalance.StatModifier.Boss.PerInstance
            if (creature->IsDungeonBoss() && hasStatModifierBossOverride(mapId))
            {
                AutoBalanceStatModifiers* myStatModifierBossOverrides = &statModifierBossOverrides[mapId];

                if (myStatModifierBossOverrides->global != -1)      { statMod_global =      myStatModifierBossOverrides->global;      }
                if (myStatModifierBossOverrides->health != -1)      { statMod_health =      myStatModifierBossOverrides->health;      }
                if (myStatModifierBossOverrides->mana != -1)        { statMod_mana =        myStatModifierBossOverrides->mana;        }
                if (myStatModifierBossOverrides->armor != -1)       { statMod_armor =       myStatModifierBossOverrides->armor;       }
                if (myStatModifierBossOverrides->damage != -1)      { statMod_damage =      myStatModifierBossOverrides->damage;      }
                if (myStatModifierBossOverrides->ccduration != -1)  { statMod_ccDuration =  myStatModifierBossOverrides->ccduration;  }
            }
        }
        // Non-boss modifiers
        else
        {
            // Per-instance non-boss overrides
            // AutoBalance.StatModifier.PerInstance
            if (hasStatModifierOverride(mapId))
            {
                AutoBalanceStatModifiers* myStatModifierOverrides = &statModifierOverrides[mapId];

                if (myStatModifierOverrides->global != -1)      { statMod_global =      myStatModifierOverrides->global;      }
                if (myStatModifierOverrides->health != -1)      { statMod_health =      myStatModifierOverrides->health;      }
                if (myStatModifierOverrides->mana != -1)        { statMod_mana =        myStatModifierOverrides->mana;        }
                if (myStatModifierOverrides->armor != -1)       { statMod_armor =       myStatModifierOverrides->armor;       }
                if (myStatModifierOverrides->damage != -1)      { statMod_damage =      myStatModifierOverrides->damage;      }
                if (myStatModifierOverrides->ccduration != -1)  { statMod_ccDuration =  myStatModifierOverrides->ccduration;  }
            }
        }

        // Per-creature modifiers applied last
        // AutoBalance.StatModifier.PerCreature
        if (hasStatModifierCreatureOverride(creatureTemplate->Entry))
        {
            AutoBalanceStatModifiers* myCreatureOverrides = &statModifierCreatureOverrides[creatureTemplate->Entry];

            if (myCreatureOverrides->global != -1)      { statMod_global =      myCreatureOverrides->global;      }
            if (myCreatureOverrides->health != -1)      { statMod_health =      myCreatureOverrides->health;      }
            if (myCreatureOverrides->mana != -1)        { statMod_mana =        myCreatureOverrides->mana;        }
            if (myCreatureOverrides->armor != -1)       { statMod_armor =       myCreatureOverrides->armor;       }
            if (myCreatureOverrides->damage != -1)      { statMod_damage =      myCreatureOverrides->damage;      }
            if (myCreatureOverrides->ccduration != -1)  { statMod_ccDuration =  myCreatureOverrides->ccduration;  }
        }

        // #maththings
        float diff = ((float)maxNumberOfPlayers/5)*1.5f;

        // For math reasons that I do not understand, curveCeiling needs to be adjusted to bring the actual multiplier
        // closer to the curveCeiling setting. Create an adjustment based on how much the ceiling should be changed at
        // the max players multiplier.
        float curveCeilingAdjustment = curveCeiling / (((tanh(((float)maxNumberOfPlayers - inflectionValue) / diff) + 1.0f) / 2.0f) * (curveCeiling - curveFloor) + curveFloor);

        // Adjust the multiplier based on the configured floor and ceiling values, plus the ceiling adjustment we just calculated
        defaultMultiplier = ((tanh(((float)creatureABInfo->instancePlayerCount - inflectionValue) / diff) + 1.0f) / 2.0f) * (curveCeiling * curveCeilingAdjustment - curveFloor) + curveFloor;

        if (!sABScriptMgr->OnAfterDefaultMultiplier(creature, defaultMultiplier))
            return;

        //
        //  Health Scaling
        //

        creatureABInfo->HealthMultiplier = defaultMultiplier * statMod_global * statMod_health;

        if (creatureABInfo->HealthMultiplier <= MinHPModifier)
        {
            creatureABInfo->HealthMultiplier = MinHPModifier;
        }

        float hpStatsRate  = 1.0f;
        if (!useDefStats && LevelScaling) {
            float newBaseHealth = 0;
            if (level <= 60)
                newBaseHealth=creatureStats->BaseHealth[0];
            else if(level <= 70)
                newBaseHealth=creatureStats->BaseHealth[1];
            else {
                newBaseHealth=creatureStats->BaseHealth[2];
                // special increasing for end-game contents
                if (LevelEndGameBoost)
                    newBaseHealth *= creatureABInfo->selectedLevel >= 75 && originalLevel < 75 ? float(creatureABInfo->selectedLevel-70) * 0.3f : 1;
            }

            float newHealth =  newBaseHealth * creatureTemplate->ModHealth;

            // allows health to be different with creatures that originally
            // differentiate their health by different level instead of multiplier field.
            // expecially in dungeons. The health reduction decrease if original level is similar to the area max level
            if (originalLevel >= areaMinLvl && originalLevel < areaMaxLvl) {
                float reduction = newHealth / float(areaMaxLvl-areaMinLvl) * (float(areaMaxLvl-originalLevel)*0.3f); // never more than 30%
                if (reduction > 0 && reduction < newHealth)
                    newHealth -= reduction;
            }

            hpStatsRate = newHealth / float(baseHealth);
        }

        creatureABInfo->HealthMultiplier *= hpStatsRate;

        scaledHealth = round(((float) baseHealth * creatureABInfo->HealthMultiplier) + 1.0f);

        //Getting the list of Classes in this group - this will be used later on to determine what additional scaling will be required based on the ratio of tank/dps/healer
        //GetPlayerClassList(creature, playerClassList); // Update playerClassList with the list of all the participating Classes

        //
        //  Mana Scaling
        //
        float manaStatsRate  = 1.0f;
        if (!useDefStats && LevelScaling) {
            float newMana =  creatureStats->GenerateMana(creatureTemplate);
            manaStatsRate = newMana/float(baseMana);
        }

        creatureABInfo->ManaMultiplier =  defaultMultiplier * manaStatsRate * statMod_global * statMod_mana;

        if (creatureABInfo->ManaMultiplier <= MinManaModifier)
        {
            creatureABInfo->ManaMultiplier = MinManaModifier;
        }

        scaledMana = round(baseMana * creatureABInfo->ManaMultiplier);

        //
        //  Armor Scaling
        //
        creatureABInfo->ArmorMultiplier = defaultMultiplier * statMod_global * statMod_armor;
        uint32 newBaseArmor= round(creatureABInfo->ArmorMultiplier * (useDefStats || !LevelScaling ? origCreatureStats->GenerateArmor(creatureTemplate) : creatureStats->GenerateArmor(creatureTemplate)));

        //
        //  Damage Scaling
        //
        float damageMul = defaultMultiplier * statMod_global * statMod_damage;

        // Can not be less then Min_D_Mod
        if (damageMul <= MinDamageModifier)
        {
            damageMul = MinDamageModifier;
        }

        if (!useDefStats && LevelScaling) {
            float origDmgBase = origCreatureStats->GenerateBaseDamage(creatureTemplate);
            float newDmgBase = 0;
            if (level <= 60)
                newDmgBase=creatureStats->BaseDamage[0];
            else if(level <= 70)
                newDmgBase=creatureStats->BaseDamage[1];
            else {
                newDmgBase=creatureStats->BaseDamage[2];
                // special increasing for end-game contents
                if (LevelEndGameBoost && maxNumberOfPlayers <= 5) {
                    newDmgBase *= creatureABInfo->selectedLevel >= 75 && originalLevel < 75 ? float(creatureABInfo->selectedLevel-70) * 0.3f : 1;
                }
            }

            damageMul *= newDmgBase/origDmgBase;
        }

        //
        // Crowd Control Debuff Duration Scaling
        //
        float ccDurationMul;
        if (statMod_ccDuration != -1.0f)
        {
            ccDurationMul = defaultMultiplier * statMod_ccDuration;

            // Min/Max checking
            if (ccDurationMul < MinCCDurationModifier)
            {
                ccDurationMul = MinCCDurationModifier;
            }
            else if (ccDurationMul > MaxCCDurationModifier)
            {
                ccDurationMul = MaxCCDurationModifier;
            }
        }
        else
        {
            ccDurationMul = 1.0f;
        }

        //
        //  Apply New Values
        //
        if (!sABScriptMgr->OnBeforeUpdateStats(creature, scaledHealth, scaledMana, damageMul, newBaseArmor))
            return;

        uint32 prevMaxHealth = creature->GetMaxHealth();
        uint32 prevMaxPower = creature->GetMaxPower(POWER_MANA);
        uint32 prevHealth = creature->GetHealth();
        uint32 prevPower = creature->GetPower(POWER_MANA);

        Powers pType= creature->getPowerType();

        creature->SetArmor(newBaseArmor);
        creature->SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, (float)newBaseArmor);
        creature->SetCreateHealth(scaledHealth);
        creature->SetMaxHealth(scaledHealth);
        creature->ResetPlayerDamageReq();
        creature->SetCreateMana(scaledMana);
        creature->SetMaxPower(POWER_MANA, scaledMana);
        creature->SetModifierValue(UNIT_MOD_ENERGY, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_RAGE, BASE_VALUE, (float)100.0f);
        creature->SetModifierValue(UNIT_MOD_HEALTH, BASE_VALUE, (float)scaledHealth);
        creature->SetModifierValue(UNIT_MOD_MANA, BASE_VALUE, (float)scaledMana);
        creatureABInfo->DamageMultiplier = damageMul;
        creatureABInfo->CCDurationMultiplier = ccDurationMul;

        uint32 scaledCurHealth=prevHealth && prevMaxHealth ? float(scaledHealth)/float(prevMaxHealth)*float(prevHealth) : 0;
        uint32 scaledCurPower=prevPower && prevMaxPower  ? float(scaledMana)/float(prevMaxPower)*float(prevPower) : 0;

        creature->SetHealth(scaledCurHealth);
        if (pType == POWER_MANA)
            creature->SetPower(POWER_MANA, scaledCurPower);
        else
            creature->setPowerType(pType); // fix creatures with different power types

        creature->UpdateAllStats();
    }

private:
    uint32 adjustCurCount(uint32 inputCount, uint32 dungeonId)
    {
        uint8 minPlayers = enabledDungeonIds[dungeonId];
        return inputCount < minPlayers ? minPlayers : inputCount;
    }
};
class AutoBalance_CommandScript : public CommandScript
{
public:
    AutoBalance_CommandScript() : CommandScript("AutoBalance_CommandScript") { }

    std::vector<ChatCommand> GetCommands() const
    {
        static std::vector<ChatCommand> ABCommandTable =
        {
            { "setoffset",        SEC_GAMEMASTER,                        true, &HandleABSetOffsetCommand,                 "Sets the global Player Difficulty Offset for instances. Example: (You + offset(1) = 2 player difficulty)." },
            { "getoffset",        SEC_GAMEMASTER,                        true, &HandleABGetOffsetCommand,                 "Shows current global player offset value." },
            { "checkmap",         SEC_GAMEMASTER,                        true, &HandleABCheckMapCommand,                  "Run a check for current map/instance, it can help in case you're testing autobalance with GM." },
            { "mapstat",          SEC_GAMEMASTER,                        true, &HandleABMapStatsCommand,                  "Shows current autobalance information for this map" },
            { "creaturestat",     SEC_GAMEMASTER,                        true, &HandleABCreatureStatsCommand,             "Shows current autobalance information for selected creature." },
        };

        static std::vector<ChatCommand> commandTable =
        {
            { "autobalance",     SEC_GAMEMASTER,                            false, NULL,                      "", ABCommandTable },
            { "ab",              SEC_GAMEMASTER,                            false, NULL,                      "", ABCommandTable },
        };
        return commandTable;
    }

    static bool HandleABSetOffsetCommand(ChatHandler* handler, const char* args)
    {
        if (!*args)
        {
            handler->PSendSysMessage(".autobalance setoffset #");
            handler->PSendSysMessage("Sets the Player Difficulty Offset for instances. Example: (You + offset(1) = 2 player difficulty).");
            return false;
        }
        char* offset = strtok((char*)args, " ");
        int32 offseti = -1;

        if (offset)
        {
            offseti = (uint32)atoi(offset);
            handler->PSendSysMessage("Changing Player Difficulty Offset to %i.", offseti);
            PlayerCountDifficultyOffset = offseti;
            return true;
        }
        else
            handler->PSendSysMessage("Error changing Player Difficulty Offset! Please try again.");
        return false;
    }

    static bool HandleABGetOffsetCommand(ChatHandler* handler, const char* /*args*/)
    {
        handler->PSendSysMessage("Current Player Difficulty Offset = %i", PlayerCountDifficultyOffset);
        return true;
    }

    static bool HandleABCheckMapCommand(ChatHandler* handler, const char* args)
    {
        Player *pl = handler->getSelectedPlayer();

        if (!pl)
        {
            handler->SendSysMessage(LANG_SELECT_PLAYER_OR_PET);
            handler->SetSentErrorMessage(true);
            return false;
        }

        AutoBalanceMapInfo *mapABInfo=pl->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        mapABInfo->playerCount = pl->GetMap()->GetPlayersCountExceptGMs();

        Map::PlayerList const &playerList = pl->GetMap()->GetPlayers();
        uint8 level = 0;
        if (!playerList.IsEmpty())
        {
            for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
            {
                if (Player* playerHandle = playerIteration->GetSource())
                {
                    if (playerHandle->getLevel() > level)
                        mapABInfo->mapLevel = level = playerHandle->getLevel();
                }
            }
        }

        HandleABMapStatsCommand(handler, args);

        return true;
    }

    static bool HandleABMapStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Player *pl = handler->getSelectedPlayer();

        if (!pl)
        {
            handler->SendSysMessage(LANG_SELECT_PLAYER_OR_PET);
            handler->SetSentErrorMessage(true);
            return false;
        }

        AutoBalanceMapInfo *mapABInfo=pl->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        handler->PSendSysMessage("Players on map: %u", mapABInfo->playerCount);
        handler->PSendSysMessage("Max level of players in this map: %u", mapABInfo->mapLevel);

        return true;
    }

    static bool HandleABCreatureStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Creature* target = handler->getSelectedCreature();

        if (!target)
        {
            handler->SendSysMessage(LANG_SELECT_CREATURE);
            handler->SetSentErrorMessage(true);
            return false;
        }

        AutoBalanceCreatureInfo *creatureABInfo=target->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        handler->PSendSysMessage("Instance player Count: %u", creatureABInfo->instancePlayerCount);
        handler->PSendSysMessage("Selected level: %u", creatureABInfo->selectedLevel);
        handler->PSendSysMessage("Health multiplier: %.6f", creatureABInfo->HealthMultiplier);
        handler->PSendSysMessage("Mana multiplier: %.6f", creatureABInfo->ManaMultiplier);
        handler->PSendSysMessage("Armor multiplier: %.6f", creatureABInfo->ArmorMultiplier);
        handler->PSendSysMessage("Damage multiplier: %.6f", creatureABInfo->DamageMultiplier);
        handler->PSendSysMessage("CC Duration multiplier: %.6f", creatureABInfo->CCDurationMultiplier);

        return true;

    }
};

class AutoBalance_GlobalScript : public GlobalScript {
public:
    AutoBalance_GlobalScript() : GlobalScript("AutoBalance_GlobalScript") { }

    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type,  uint32 /*creditEntry*/, Unit* source, Difficulty /*difficulty_fixed*/, DungeonEncounterList const* /*encounters*/, uint32 /*dungeonCompleted*/, bool updated) override {
        //if (!dungeonCompleted)
        //    return;

        if (!rewardEnabled || !updated)
            return;

        if (map->GetPlayersCountExceptGMs() < MinPlayerReward)
            return;

        AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        uint8 areaMinLvl, areaMaxLvl;
        getAreaLevel(map, source->GetAreaId(), areaMinLvl, areaMaxLvl);

        // skip if it's not a pre-wotlk dungeon/raid and if it's not scaled
        if (!LevelScaling || lowerOffset >= 10 || mapABInfo->mapLevel <= 70 || areaMinLvl > 70
            // skip when not in dungeon or not kill credit
            || type != ENCOUNTER_CREDIT_KILL_CREATURE || !map->IsDungeon())
            return;

        Map::PlayerList const &playerList = map->GetPlayers();

        if (playerList.IsEmpty())
            return;

        uint32 reward = map->ToInstanceMap()->GetMaxPlayers() > 5 ? rewardRaid : rewardDungeon;
        if (!reward)
            return;

        //instanceStart=0, endTime;
        uint8 difficulty = map->GetDifficulty();

        for (Map::PlayerList::const_iterator itr = playerList.begin(); itr != playerList.end(); ++itr)
        {
            if (!itr->GetSource() || itr->GetSource()->IsGameMaster() || itr->GetSource()->getLevel() < DEFAULT_MAX_LEVEL)
                continue;

            itr->GetSource()->AddItem(reward, 1 + difficulty); // difficulty boost
        }
    }
};



void AddAutoBalanceScripts()
{
    new AutoBalance_WorldScript();
    new AutoBalance_PlayerScript();
    new AutoBalance_UnitScript();
    new AutoBalance_AllCreatureScript();
    new AutoBalance_AllMapScript();
    new AutoBalance_CommandScript();
    new AutoBalance_GlobalScript();
}
