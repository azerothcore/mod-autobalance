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
#include "SharedDefines.h"
#include <chrono>

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

    uint64_t configTime;

    uint32 instancePlayerCount = 0;
    uint8 selectedLevel = 0;
    // this is used to detect creatures that update their entry
    uint32 entry = 0;
    float DamageMultiplier = 1.0f;
    float HealthMultiplier = 1.0f;
    float ManaMultiplier = 1.0f;
    float ArmorMultiplier = 1.0f;
    float CCDurationMultiplier = 1.0f;

    float XPModifier = 1.0f;
    float MoneyModifier = 1.0f;

    uint8 UnmodifiedLevel = 0;

    bool isActive = false;
    bool wasAliveNowDead = false;
    bool isInCreatureList = false;
};

class AutoBalanceMapInfo : public DataMap::Base
{
public:
    AutoBalanceMapInfo() {}

    uint64_t configTime = 1;

    uint32 playerCount = 0;
    uint32 adjustedPlayerCount = 0;
    uint32 minPlayers = 1;

    uint8 mapLevel = 0;
    uint8 lowestPlayerLevel = 0;
    uint8 highestPlayerLevel = 0;

    uint8 lfgMinLevel = 0;
    uint8 lfgTargetLevel = 80;
    uint8 lfgMaxLevel = 80;

    uint8 worldMultiplierTargetLevel = 1;
    float worldDamageHealingMultiplier = 1.0f;
    float worldHealthMultiplier = 1.0f;

    bool enabled = false;

    std::vector<Creature*> allMapCreatures;
    uint8 highestCreatureLevel = 0;
    uint8 lowestCreatureLevel = 0;
    float avgCreatureLevel;
    uint32 activeCreatureCount = 0;

    bool isLevelScalingEnabled;
    int levelScalingSkipHigherLevels, levelScalingSkipLowerLevels;
    int levelScalingDynamicCeiling, levelScalingDynamicFloor;
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

    std::time_t configTime;
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

class AutoBalanceLevelScalingDynamicLevelSettings: public DataMap::Base
{
public:
    AutoBalanceLevelScalingDynamicLevelSettings() {}
    AutoBalanceLevelScalingDynamicLevelSettings(int skipHigher, int skipLower, int ceiling, int floor) :
        skipHigher(skipHigher), skipLower(skipLower), ceiling(ceiling), floor(floor) {}
    int skipHigher;
    int skipLower;
    int ceiling;
    int floor;
};

enum ScalingMethod {
    AUTOBALANCE_SCALING_FIXED,
    AUTOBALANCE_SCALING_DYNAMIC
};

enum BaseValueType {
    AUTOBALANCE_HEALTH,
    AUTOBALANCE_DAMAGE_HEALING
};

// spell IDs that spend player health
static std::list<uint32> spellIdsThatSpendPlayerHealth =
{
    45529,      // Blood Tap
    2687,       // Bloodrage
    27869,      // Dark Rune
    16666,      // Demonic Rune
    755,        // Health Funnel (Rank 1)
    3698,       // Health Funnel (Rank 2)
    3699,       // Health Funnel (Rank 3)
    3700,       // Health Funnel (Rank 4)
    11693,      // Health Funnel (Rank 5)
    11694,      // Health Funnel (Rank 6)
    11695,      // Health Funnel (Rank 7)
    27259,      // Health Funnel (Rank 8)
    47856,      // Health Funnel (Rank 9)
    1454,       // Life Tap (Rank 1)
    1455,       // Life Tap (Rank 2)
    1456,       // Life Tap (Rank 3)
    11687,      // Life Tap (Rank 4)
    11688,      // Life Tap (Rank 5)
    11689,      // Life Tap (Rank 6)
    27222,      // Life Tap (Rank 7)
    57946,      // Life Tap (Rank 8)
    29858       // Soulshatter
};

// spacer used for logging
std::string SPACER = "------------------------------------------------";


// The map values correspond with the .AutoBalance.XX.Name entries in the configuration file.
static std::map<int, int> forcedCreatureIds;
static std::list<uint32> disabledDungeonIds;

static uint32 minPlayersNormal, minPlayersHeroic;
static std::map<uint32, uint8> minPlayersPerDungeonIdMap;
static std::map<uint32, uint8> minPlayersPerHeroicDungeonIdMap;

static std::map<uint32, AutoBalanceInflectionPointSettings> dungeonOverrides;
static std::map<uint32, AutoBalanceInflectionPointSettings> bossOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierBossOverrides;
static std::map<uint32, AutoBalanceStatModifiers> statModifierCreatureOverrides;
static std::map<uint8, AutoBalanceLevelScalingDynamicLevelSettings> levelScalingDynamicLevelOverrides;
static std::map<uint32, uint32> levelScalingDistanceCheckOverrides;
// cheaphack for difficulty server-wide.
// Another value TODO in player class for the party leader's value to determine dungeon difficulty.
static int8 PlayerCountDifficultyOffset;
static bool LevelScaling;
static int8 LevelScalingSkipHigherLevels, LevelScalingSkipLowerLevels;
static int8 LevelScalingDynamicLevelCeilingDungeons, LevelScalingDynamicLevelFloorDungeons, LevelScalingDynamicLevelCeilingRaids, LevelScalingDynamicLevelFloorRaids;
static int8 LevelScalingDynamicLevelCeilingHeroicDungeons, LevelScalingDynamicLevelFloorHeroicDungeons, LevelScalingDynamicLevelCeilingHeroicRaids, LevelScalingDynamicLevelFloorHeroicRaids;
static ScalingMethod LevelScalingMethod;
static uint32 rewardRaid, rewardDungeon, MinPlayerReward;
static bool Announcement;
static bool LevelScalingEndGameBoost, PlayerChangeNotify, rewardEnabled;
static float MinHPModifier, MinManaModifier, MinDamageModifier, MinCCDurationModifier, MaxCCDurationModifier;

// RewardScaling.*
static ScalingMethod RewardScalingMethod;
static bool RewardScalingXP, RewardScalingMoney;
static float RewardScalingXPModifier, RewardScalingMoneyModifier;

// Track the last time the config was reloaded
static uint64_t lastConfigTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

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

std::list<uint32> LoadDisabledDungeons(std::string dungeonIdString) // Used for reading the string from the configuration file for selectively disabling dungeons
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::list<uint32> dungeonIdList;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - ","
    {
        std::string valueOne;
        std::stringstream dungeonPairStream(delimitedValue);
        dungeonPairStream>>valueOne;
        auto dungeonMapId = atoi(valueOne.c_str());
        dungeonIdList.push_back(dungeonMapId);
    }

    return dungeonIdList;
}

std::map<uint32, uint8> LoadMinPlayersPerDungeonId(std::string minPlayersString) // Used for reading the string from the configuration file for per-dungeon minimum player count overrides
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::map<uint32, uint8> dungeonIdMap;

    dungeonIdStream.str(minPlayersString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string val1, val2;
        std::stringstream dungeonPairStream(delimitedValue);
        dungeonPairStream >> val1 >> val2;
        auto dungeonMapId = atoi(val1.c_str());
        auto minPlayers = atoi(val2.c_str());
        dungeonIdMap[dungeonMapId] = minPlayers;
    }

    return dungeonIdMap;
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

std::map<uint8, AutoBalanceLevelScalingDynamicLevelSettings> LoadDynamicLevelOverrides(std::string dungeonIdString) // Used for reading the string from the configuration file for per-dungeon dynamic level overrides
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::map<uint8, AutoBalanceLevelScalingDynamicLevelSettings> overrideMap;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string val1, val2, val3, val4, val5;
        std::stringstream dungeonStream(delimitedValue);
        dungeonStream >> val1 >> val2 >> val3 >> val4 >> val5;

        auto dungeonMapId = atoi(val1.c_str());

        // Replace any missing values with -1
        if (val2.empty()) { val2 = "-1"; }
        if (val3.empty()) { val3 = "-1"; }
        if (val4.empty()) { val3 = "-1"; }
        if (val5.empty()) { val3 = "-1"; }

        AutoBalanceLevelScalingDynamicLevelSettings dynamicLevelSettings = AutoBalanceLevelScalingDynamicLevelSettings(
            atoi(val2.c_str()),
            atoi(val3.c_str()),
            atoi(val4.c_str()),
            atoi(val5.c_str())
        );

        overrideMap[dungeonMapId] = dynamicLevelSettings;
    }

    return overrideMap;
}

std::map<uint32, uint32> LoadDistanceCheckOverrides(std::string dungeonIdString)
{
    std::string delimitedValue;
    std::stringstream dungeonIdStream;
    std::map<uint32, uint32> overrideMap;

    dungeonIdStream.str(dungeonIdString);
    while (std::getline(dungeonIdStream, delimitedValue, ',')) // Process each dungeon ID in the string, delimited by the comma - "," and then space " "
    {
        std::string val1, val2;
        std::stringstream dungeonStream(delimitedValue);
        dungeonStream >> val1 >> val2;

        auto dungeonMapId = atoi(val1.c_str());
        overrideMap[dungeonMapId] = atoi(val2.c_str());
    }

    return overrideMap;
}

bool isDungeonInDisabledDungeonIds(uint32 dungeonId)
{
    return (std::find(disabledDungeonIds.begin(), disabledDungeonIds.end(), dungeonId) != disabledDungeonIds.end());
}

bool isDungeonInMinPlayerMap(uint32 dungeonId, bool isHeroic)
{
    if (isHeroic) {
        return (minPlayersPerHeroicDungeonIdMap.find(dungeonId) != minPlayersPerHeroicDungeonIdMap.end());
    } else {
        return (minPlayersPerDungeonIdMap.find(dungeonId) != minPlayersPerDungeonIdMap.end());
    }
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

bool hasDynamicLevelOverride(uint32 dungeonId)
{
    return (levelScalingDynamicLevelOverrides.find(dungeonId) != levelScalingDynamicLevelOverrides.end());
}

bool hasLevelScalingDistanceCheckOverride(uint32 dungeonId)
{
    return (levelScalingDistanceCheckOverrides.find(dungeonId) != levelScalingDistanceCheckOverrides.end());
}

bool ShouldMapBeEnabled(Map* map)
{
    if (map->IsDungeon() || map->IsRaid())
    {
        // if globally disabled, return false
        if (!EnableGlobal)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}-\?\?) - Not enabled because EnableGlobal is false",
                      map->GetMapName(),
                      map->GetId()
            );
            return false;
        }
        
        // get the current instance map
        auto instanceMap = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()));

        // if there wasn't one, then we're not in an instance
        if (!instanceMap)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}) - Not enabled for the base map without an Instance Map",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );
            return false;
        }

        // if the player count is less than 1, then we're not in an instance
        if (instanceMap->GetMaxPlayers() < 1)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}, {}-player {}) - Not enabled because GetMaxPlayers < 1",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                      instanceMap->GetMaxPlayers(),
                      instanceMap->IsHeroic() ? "Heroic" : "Normal"
            );
            return false;
        }

        // if the Dungeon is disabled via configuration, do not enable it
        if (isDungeonInDisabledDungeonIds(map->GetId()))
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}, {}-player {}) - Not enabled because the map ID is disabled via configuration",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                      instanceMap->GetMaxPlayers(),
                      instanceMap->IsHeroic() ? "Heroic" : "Normal"
            );
            
            return false;
        }

        // use the configuration variables to determine if this instance type/size should have scaling enabled
        bool sizeDifficultyEnabled;
        if (instanceMap->IsHeroic())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Heroic Enables - 5:{} 10:{} 25:{} Other:{}",
                        Enable5MHeroic, Enable10MHeroic, Enable25MHeroic, EnableOtherHeroic);
            switch (instanceMap->GetMaxPlayers())
            {
                case 5:
                    sizeDifficultyEnabled = Enable5MHeroic;
                    break;
                case 10:
                    sizeDifficultyEnabled = Enable10MHeroic;
                    break;
                case 25:
                    sizeDifficultyEnabled = Enable25MHeroic;
                    break;
                default:
                    sizeDifficultyEnabled = EnableOtherHeroic;
                    break;
            }
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: Normal Enables - 5:{} 10:{} 15:{} 20:{} 25:{} 40:{} Other:{}",
                        Enable5M, Enable10M, Enable15M, Enable20M, Enable25M, Enable40M, EnableOtherNormal);
            switch (instanceMap->GetMaxPlayers())
            {
                case 5:
                    sizeDifficultyEnabled = Enable5M;
                    break;
                case 10:
                    sizeDifficultyEnabled = Enable10M;
                    break;
                case 15:
                    sizeDifficultyEnabled = Enable15M;
                    break;
                case 20:
                    sizeDifficultyEnabled = Enable20M;
                    break;
                case 25:
                    sizeDifficultyEnabled = Enable25M;
                    break;
                case 40:
                    sizeDifficultyEnabled = Enable40M;
                    break;
                default:
                    sizeDifficultyEnabled = EnableOtherNormal;
                    break;
            }
        }

        if (sizeDifficultyEnabled)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}, {}-player {}) - Enabled for AutoBalancing",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                      instanceMap->GetMaxPlayers(),
                      instanceMap->IsHeroic() ? "Heroic" : "Normal"
            );
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}, {}-player {}) - Not enabled because its size and difficulty are disabled via configuration",
                      map->GetMapName(),
                      map->GetId(),
                      map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                      instanceMap->GetMaxPlayers(),
                      instanceMap->IsHeroic() ? "Heroic" : "Normal"
            );
        }
        
        return sizeDifficultyEnabled;
    }
    else
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::ShouldMapBeEnabled: {} ({}{}) - Not enabled because we're not in an instance",
                    map->GetMapName(),
                    map->GetId(),
                    map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
        );
        return false;
        
        // we're not in a dungeon or a raid, we never scale
        return false;
    }
}

float getBaseExpansionValueForLevel(const float baseValues[3], uint8 targetLevel)
{
    // the database holds multiple base values depending on the expansion
    // this function returns the correct base value for the given level and
    // smooths the transition between expansions

    float vanillaValue = baseValues[0];
    float bcValue = baseValues[1];
    float wotlkValue = baseValues[2];

    float returnValue;

    // vanilla
    if (targetLevel <= 60)
    {
        returnValue = vanillaValue;
        //LOG_DEBUG("module.AutoBalance", "AutoBalance::getBaseExpansionValueForLevel: Returning Vanilla = {}", returnValue);
    }
    // transition from vanilla to BC
    else if (targetLevel < 63)
    {
        float vanillaMultiplier = (63 - targetLevel) / 3.0;
        float bcMultiplier = 1.0f - vanillaMultiplier;

        returnValue = (vanillaValue * vanillaMultiplier) + (bcValue * bcMultiplier);
        //LOG_DEBUG("module.AutoBalance", "AutoBalance::getBaseExpansionValueForLevel: Returning Vanilla/BC = {}", returnValue);
    }
    // BC
    else if (targetLevel <= 70)
    {
        returnValue = bcValue;
        //LOG_DEBUG("module.AutoBalance", "AutoBalance::getBaseExpansionValueForLevel: Returning BC = {}", returnValue);
    }
    // transition from BC to WotLK
    else if (targetLevel < 73)
    {
        float bcMultiplier = (73 - targetLevel) / 3.0f;
        float wotlkMultiplier = 1.0f - bcMultiplier;

        returnValue = (bcValue * bcMultiplier) + (wotlkValue * wotlkMultiplier);
        //LOG_DEBUG("module.AutoBalance", "AutoBalance::getBaseExpansionValueForLevel: Returning BC/WotLK = {}", returnValue);
    }
    // WotLK
    else
    {
        returnValue = wotlkValue;
        //LOG_DEBUG("module.AutoBalance", "AutoBalance::getBaseExpansionValueForLevel: Returning WotLK = {}", returnValue);
    }

    return returnValue;
}

uint32 getBaseExpansionValueForLevel(const uint32 baseValues[3], uint8 targetLevel)
{
    // convert baseValues from an array of uint32 to an array of float
    float floatBaseValues[3];
    for (int i = 0; i < 3; i++)
    {
        floatBaseValues[i] = (float)baseValues[i];
    }

    // return the result
    return getBaseExpansionValueForLevel(floatBaseValues, targetLevel);
}

AutoBalanceInflectionPointSettings getInflectionPointSettings (InstanceMap* instanceMap, bool isBoss = false)
{
    uint32 maxNumberOfPlayers = instanceMap->GetMaxPlayers();
    uint32 mapId = instanceMap->GetEntry()->MapID;

    float inflectionValue, curveFloor, curveCeiling;

    inflectionValue  = (float)maxNumberOfPlayers;

    //
    // Base Inflection Point
    //
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
    if (isBoss) {

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

    return AutoBalanceInflectionPointSettings(inflectionValue, curveFloor, curveCeiling);
}

AutoBalanceStatModifiers getStatModifiers (InstanceMap* instanceMap, Creature* creature = nullptr)
{
    // map variables
    uint32 maxNumberOfPlayers = instanceMap->GetMaxPlayers();
    uint32 mapId = instanceMap->GetEntry()->MapID;

    // get the creature's info if a creature was specified
    AutoBalanceCreatureInfo* creatureABInfo = nullptr;
    if (creature)
        creatureABInfo = creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

    // this will be the return value
    AutoBalanceStatModifiers statModifiers;

    // Apply the per-instance-type modifiers first
    // AutoBalance.StatModifier*(.Boss).<stat>
    if (instanceMap->IsHeroic()) // heroic
    {
        switch (maxNumberOfPlayers)
        {
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierHeroic_Boss_Global;
                    statModifiers.health = StatModifierHeroic_Boss_Health;
                    statModifiers.mana = StatModifierHeroic_Boss_Mana;
                    statModifiers.armor = StatModifierHeroic_Boss_Armor;
                    statModifiers.damage = StatModifierHeroic_Boss_Damage;
                    statModifiers.ccduration = StatModifierHeroic_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierHeroic_Global;
                    statModifiers.health = StatModifierHeroic_Health;
                    statModifiers.mana = StatModifierHeroic_Mana;
                    statModifiers.armor = StatModifierHeroic_Armor;
                    statModifiers.damage = StatModifierHeroic_Damage;
                    statModifiers.ccduration = StatModifierHeroic_CCDuration;
                }
                break;
            case 10:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaid10MHeroic_Boss_Global;
                    statModifiers.health = StatModifierRaid10MHeroic_Boss_Health;
                    statModifiers.mana = StatModifierRaid10MHeroic_Boss_Mana;
                    statModifiers.armor = StatModifierRaid10MHeroic_Boss_Armor;
                    statModifiers.damage = StatModifierRaid10MHeroic_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaid10MHeroic_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaid10MHeroic_Global;
                    statModifiers.health = StatModifierRaid10MHeroic_Health;
                    statModifiers.mana = StatModifierRaid10MHeroic_Mana;
                    statModifiers.armor = StatModifierRaid10MHeroic_Armor;
                    statModifiers.damage = StatModifierRaid10MHeroic_Damage;
                    statModifiers.ccduration = StatModifierRaid10MHeroic_CCDuration;
                }
                break;
            case 25:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaid25MHeroic_Boss_Global;
                    statModifiers.health = StatModifierRaid25MHeroic_Boss_Health;
                    statModifiers.mana = StatModifierRaid25MHeroic_Boss_Mana;
                    statModifiers.armor = StatModifierRaid25MHeroic_Boss_Armor;
                    statModifiers.damage = StatModifierRaid25MHeroic_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaid25MHeroic_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaid25MHeroic_Global;
                    statModifiers.health = StatModifierRaid25MHeroic_Health;
                    statModifiers.mana = StatModifierRaid25MHeroic_Mana;
                    statModifiers.armor = StatModifierRaid25MHeroic_Armor;
                    statModifiers.damage = StatModifierRaid25MHeroic_Damage;
                    statModifiers.ccduration = StatModifierRaid25MHeroic_CCDuration;
                }
                break;
            default:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaidHeroic_Boss_Global;
                    statModifiers.health = StatModifierRaidHeroic_Boss_Health;
                    statModifiers.mana = StatModifierRaidHeroic_Boss_Mana;
                    statModifiers.armor = StatModifierRaidHeroic_Boss_Armor;
                    statModifiers.damage = StatModifierRaidHeroic_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaidHeroic_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaidHeroic_Global;
                    statModifiers.health = StatModifierRaidHeroic_Health;
                    statModifiers.mana = StatModifierRaidHeroic_Mana;
                    statModifiers.armor = StatModifierRaidHeroic_Armor;
                    statModifiers.damage = StatModifierRaidHeroic_Damage;
                    statModifiers.ccduration = StatModifierRaidHeroic_CCDuration;
                }
        }
    }
    else // non-heroic
    {
        switch (maxNumberOfPlayers)
        {
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifier_Boss_Global;
                    statModifiers.health = StatModifier_Boss_Health;
                    statModifiers.mana = StatModifier_Boss_Mana;
                    statModifiers.armor = StatModifier_Boss_Armor;
                    statModifiers.damage = StatModifier_Boss_Damage;
                    statModifiers.ccduration = StatModifier_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifier_Global;
                    statModifiers.health = StatModifier_Health;
                    statModifiers.mana = StatModifier_Mana;
                    statModifiers.armor = StatModifier_Armor;
                    statModifiers.damage = StatModifier_Damage;
                    statModifiers.ccduration = StatModifier_CCDuration;
                }
                break;
            case 10:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaid10M_Boss_Global;
                    statModifiers.health = StatModifierRaid10M_Boss_Health;
                    statModifiers.mana = StatModifierRaid10M_Boss_Mana;
                    statModifiers.armor = StatModifierRaid10M_Boss_Armor;
                    statModifiers.damage = StatModifierRaid10M_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaid10M_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaid10M_Global;
                    statModifiers.health = StatModifierRaid10M_Health;
                    statModifiers.mana = StatModifierRaid10M_Mana;
                    statModifiers.armor = StatModifierRaid10M_Armor;
                    statModifiers.damage = StatModifierRaid10M_Damage;
                    statModifiers.ccduration = StatModifierRaid10M_CCDuration;
                }
                break;
            case 25:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaid25M_Boss_Global;
                    statModifiers.health = StatModifierRaid25M_Boss_Health;
                    statModifiers.mana = StatModifierRaid25M_Boss_Mana;
                    statModifiers.armor = StatModifierRaid25M_Boss_Armor;
                    statModifiers.damage = StatModifierRaid25M_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaid25M_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaid25M_Global;
                    statModifiers.health = StatModifierRaid25M_Health;
                    statModifiers.mana = StatModifierRaid25M_Mana;
                    statModifiers.armor = StatModifierRaid25M_Armor;
                    statModifiers.damage = StatModifierRaid25M_Damage;
                    statModifiers.ccduration = StatModifierRaid25M_CCDuration;
                }
                break;
            default:
                if (creature && creature->IsDungeonBoss())
                {
                    statModifiers.global = StatModifierRaid_Boss_Global;
                    statModifiers.health = StatModifierRaid_Boss_Health;
                    statModifiers.mana = StatModifierRaid_Boss_Mana;
                    statModifiers.armor = StatModifierRaid_Boss_Armor;
                    statModifiers.damage = StatModifierRaid_Boss_Damage;
                    statModifiers.ccduration = StatModifierRaid_Boss_CCDuration;
                }
                else
                {
                    statModifiers.global = StatModifierRaid_Global;
                    statModifiers.health = StatModifierRaid_Health;
                    statModifiers.mana = StatModifierRaid_Mana;
                    statModifiers.armor = StatModifierRaid_Armor;
                    statModifiers.damage = StatModifierRaid_Damage;
                    statModifiers.ccduration = StatModifierRaid_CCDuration;
                }
        }
    }

    // Per-Map Overrides
    // AutoBalance.StatModifier.Boss.PerInstance
    if (creature && creature->IsDungeonBoss() && hasStatModifierBossOverride(mapId))
    {
        AutoBalanceStatModifiers* myStatModifierBossOverrides = &statModifierBossOverrides[mapId];

        if (myStatModifierBossOverrides->global != -1)      { statModifiers.global =      myStatModifierBossOverrides->global;      }
        if (myStatModifierBossOverrides->health != -1)      { statModifiers.health =      myStatModifierBossOverrides->health;      }
        if (myStatModifierBossOverrides->mana != -1)        { statModifiers.mana =        myStatModifierBossOverrides->mana;        }
        if (myStatModifierBossOverrides->armor != -1)       { statModifiers.armor =       myStatModifierBossOverrides->armor;       }
        if (myStatModifierBossOverrides->damage != -1)      { statModifiers.damage =      myStatModifierBossOverrides->damage;      }
        if (myStatModifierBossOverrides->ccduration != -1)  { statModifiers.ccduration =  myStatModifierBossOverrides->ccduration;  }
    }
    // AutoBalance.StatModifier.PerInstance
    else if (hasStatModifierOverride(mapId))
    {
        AutoBalanceStatModifiers* myStatModifierOverrides = &statModifierOverrides[mapId];

        if (myStatModifierOverrides->global != -1)      { statModifiers.global =      myStatModifierOverrides->global;      }
        if (myStatModifierOverrides->health != -1)      { statModifiers.health =      myStatModifierOverrides->health;      }
        if (myStatModifierOverrides->mana != -1)        { statModifiers.mana =        myStatModifierOverrides->mana;        }
        if (myStatModifierOverrides->armor != -1)       { statModifiers.armor =       myStatModifierOverrides->armor;       }
        if (myStatModifierOverrides->damage != -1)      { statModifiers.damage =      myStatModifierOverrides->damage;      }
        if (myStatModifierOverrides->ccduration != -1)  { statModifiers.ccduration =  myStatModifierOverrides->ccduration;  }
    }

    // Per-creature modifiers applied last
    // AutoBalance.StatModifier.PerCreature
    if (creature && hasStatModifierCreatureOverride(creatureABInfo->entry))
    {
        AutoBalanceStatModifiers* myCreatureOverrides = &statModifierCreatureOverrides[creatureABInfo->entry];

        if (myCreatureOverrides->global != -1)      { statModifiers.global =      myCreatureOverrides->global;      }
        if (myCreatureOverrides->health != -1)      { statModifiers.health =      myCreatureOverrides->health;      }
        if (myCreatureOverrides->mana != -1)        { statModifiers.mana =        myCreatureOverrides->mana;        }
        if (myCreatureOverrides->armor != -1)       { statModifiers.armor =       myCreatureOverrides->armor;       }
        if (myCreatureOverrides->damage != -1)      { statModifiers.damage =      myCreatureOverrides->damage;      }
        if (myCreatureOverrides->ccduration != -1)  { statModifiers.ccduration =  myCreatureOverrides->ccduration;  }
    }

    return statModifiers;

}

float getDefaultMultiplier(Map* map, AutoBalanceInflectionPointSettings inflectionPointSettings)
{
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

    // get the max player count for the map
    uint32 maxNumberOfPlayers = map->ToInstanceMap()->GetMaxPlayers();

    // get the adjustedPlayerCount for this instance
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
    float adjustedPlayerCount = mapABInfo->adjustedPlayerCount;

    // #maththings
    float diff = ((float)maxNumberOfPlayers/5)*1.5f;

    // For math reasons that I do not understand, curveCeiling needs to be adjusted to bring the actual multiplier
    // closer to the curveCeiling setting. Create an adjustment based on how much the ceiling should be changed at
    // the max players multiplier.
    float curveCeilingAdjustment =
        inflectionPointSettings.curveCeiling /
        (((tanh(((float)maxNumberOfPlayers - inflectionPointSettings.value) / diff) + 1.0f) / 2.0f) *
        (inflectionPointSettings.curveCeiling - inflectionPointSettings.curveFloor) + inflectionPointSettings.curveFloor);

    // Adjust the multiplier based on the configured floor and ceiling values, plus the ceiling adjustment we just calculated
    float defaultMultiplier =
        ((tanh((adjustedPlayerCount - inflectionPointSettings.value) / diff) + 1.0f) / 2.0f) *
        (inflectionPointSettings.curveCeiling * curveCeilingAdjustment - inflectionPointSettings.curveFloor) +
        inflectionPointSettings.curveFloor;

    return defaultMultiplier;
}

float getWorldMultiplier(Map* map, BaseValueType baseValueType)
{
    // null check
    if (!map)
        return 1.0f;

    // if this isn't a dungeon, return 1.0f
    if (!(map->IsDungeon()))
        return 1.0f;

    // grab map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if the map isn't enabled, return 1.0f
    if (!mapABInfo->enabled)
        return 1.0f;

    // if there are no players on the map, return 1.0f
    if (map->GetPlayersCountExceptGMs() == 0)
        return 1.0f;

    // create some data variables
    InstanceMap* instanceMap = map->ToInstanceMap();
    uint8 avgMapCreatureLevelRounded = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);

    // get the inflection point settings for this map
    AutoBalanceInflectionPointSettings inflectionPointSettings = getInflectionPointSettings(instanceMap);

    // Generate the default multiplier before level scaling
    // This value is only based on the adjusted number of players in the instance
    float worldMultiplier = getDefaultMultiplier(map, inflectionPointSettings);

    LOG_DEBUG("module.AutoBalance",
        "AutoBalance::getWorldMultiplier: Map {} ({}) starting {} multiplier for {} player(s) before level scaling: {}.",
        map->GetMapName(),
        avgMapCreatureLevelRounded,
        baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
        mapABInfo->adjustedPlayerCount,
        worldMultiplier
    );

    // only scale based on level if level scaling is enabled and the instance's average creature level is not within the skip range
    if (LevelScaling &&
            (
                (mapABInfo->avgCreatureLevel > mapABInfo->highestPlayerLevel + mapABInfo->levelScalingSkipHigherLevels || mapABInfo->levelScalingSkipHigherLevels == 0) ||
                (mapABInfo->avgCreatureLevel < mapABInfo->highestPlayerLevel - mapABInfo->levelScalingSkipLowerLevels || mapABInfo->levelScalingSkipLowerLevels == 0)
            )
        )
    {
        mapABInfo->worldMultiplierTargetLevel = mapABInfo->highestPlayerLevel;
        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) {} level will be scaled to {}.",
            map->GetMapName(),
            avgMapCreatureLevelRounded,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            mapABInfo->worldMultiplierTargetLevel
        );

        // use creature base stats to determine how to level scale the damage multiplier
        CreatureBaseStats const* origMapBaseStats = sObjectMgr->GetCreatureBaseStats(avgMapCreatureLevelRounded, CLASS_WARRIOR);
        CreatureBaseStats const* adjustedMapBaseStats = sObjectMgr->GetCreatureBaseStats(mapABInfo->worldMultiplierTargetLevel, CLASS_WARRIOR);

        // The database holds multiple values for base damage, one for each expansion
        // This code will smooth transition between the different expansions based on the highest player level in the instance
        // Only do this if level scaling is enabled

        // Original Base Value
        float originalBaseValue;

        if (baseValueType == BaseValueType::AUTOBALANCE_HEALTH) // health
        {
            originalBaseValue = getBaseExpansionValueForLevel(
                origMapBaseStats->BaseHealth,
                avgMapCreatureLevelRounded
            );
        }
        else // damage
        {
            originalBaseValue = getBaseExpansionValueForLevel(
                origMapBaseStats->BaseDamage,
                avgMapCreatureLevelRounded
            );
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) original {} base is {}.",
            map->GetMapName(),
            avgMapCreatureLevelRounded,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            originalBaseValue
        );

        // New Base Value
        float newBaseValue;

        if (baseValueType == BaseValueType::AUTOBALANCE_HEALTH) // health
        {
            newBaseValue = getBaseExpansionValueForLevel(
                adjustedMapBaseStats->BaseHealth,
                mapABInfo->worldMultiplierTargetLevel
            );
        }
        else // damage
        {
            newBaseValue = getBaseExpansionValueForLevel(
                adjustedMapBaseStats->BaseDamage,
                mapABInfo->worldMultiplierTargetLevel
            );
        }

        // Handle LevelScalingEndGameBoost
        if (LevelScalingEndGameBoost)
        {
            if (
                baseValueType == BaseValueType::AUTOBALANCE_HEALTH &&
                mapABInfo->worldMultiplierTargetLevel >= 75 &&
                avgMapCreatureLevelRounded < 75
            )
            {
                newBaseValue *= (float)(mapABInfo->worldMultiplierTargetLevel - 70) * 0.3f;
                LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) base {} value is boosted by LevelScalingEndGameBoost. New value: {}",
                    map->GetMapName(),
                    mapABInfo->worldMultiplierTargetLevel,
                    baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
                    newBaseValue
                );
            }
            else if (
                baseValueType == BaseValueType::AUTOBALANCE_DAMAGE_HEALING &&
                instanceMap->GetMaxPlayers() <= 5 &&
                mapABInfo->worldMultiplierTargetLevel >= 75 &&
                avgMapCreatureLevelRounded < 75
            )
            {
                newBaseValue *= (float)(mapABInfo->worldMultiplierTargetLevel - 70) * 0.3f;
                LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) base {} value is boosted by LevelScalingEndGameBoost. New value: {}",
                    map->GetMapName(),
                    mapABInfo->worldMultiplierTargetLevel,
                    baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
                    newBaseValue
                );
            }
            else {
                LOG_DEBUG("module.AutoBalance",
                    "getWorldMultiplier: LevelScalingEndGameBoost is enabled, but the instance is not eligible for the boost.");
            }
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) new {} base is {}.",
            map->GetMapName(),
            mapABInfo->highestPlayerLevel,
            baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
            newBaseValue
        );

        // update the world multiplier accordingly
        worldMultiplier *= newBaseValue / originalBaseValue;
    }
    else
    {
        mapABInfo->worldMultiplierTargetLevel = avgMapCreatureLevelRounded;
        LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}) not level scaled due to level scaling being disabled or the instance's average creature level being inside the skip range.", map->GetMapName(), avgMapCreatureLevelRounded);
    }

    LOG_DEBUG("module.AutoBalance", "AutoBalance::getWorldMultiplier: Map {} ({}->{}) final {} multiplier is {}.",
              map->GetMapName(), avgMapCreatureLevelRounded,
              mapABInfo->highestPlayerLevel,
              baseValueType == BaseValueType::AUTOBALANCE_HEALTH ? "health" : "damage",
              worldMultiplier
    );

    return worldMultiplier;
}

void LoadMapSettings(Map* map)
{
    // Load (or create) the map's info
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // create an InstanceMap object
    InstanceMap* instanceMap = map->ToInstanceMap();

    LOG_DEBUG("module.AutoBalance", "AutoBalance::LoadMapSettings: Loading settings for map {} ({}{}, {}-player {}).", 
        map->GetMapName(),
        map->GetId(),
        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
        instanceMap->GetMaxPlayers(),
        instanceMap->IsHeroic() ? "Heroic" : "Normal"
    );

    // determine the minumum player count
    if (isDungeonInMinPlayerMap(map->GetId(), instanceMap->IsHeroic()))
    {
        mapABInfo->minPlayers = instanceMap->IsHeroic() ? minPlayersPerHeroicDungeonIdMap[map->GetId()] : minPlayersPerDungeonIdMap[map->GetId()];
    }
    else if (instanceMap->IsHeroic())
    {
        mapABInfo->minPlayers = minPlayersHeroic;
    }
    else
    {
        mapABInfo->minPlayers = minPlayersNormal;
    }

    // if the minPlayers value we determined is less than the max number of players in this map, adjust down
    if (mapABInfo->minPlayers > instanceMap->GetMaxPlayers())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::LoadMapSettings: Your settings tried to set a minimum player count of {} which is greater than {}'s max player count of {}. Adjusting down.",
            mapABInfo->minPlayers,
            map->GetMapName(),
            instanceMap->GetMaxPlayers()
        );

        mapABInfo->minPlayers = instanceMap->GetMaxPlayers();
    }

    LOG_DEBUG("module.AutoBalance", "AutoBalance::LoadMapSettings: Map {} ({}{}, {}-player {}) has a minimum player count of {}.", 
        map->GetMapName(),
        map->GetId(),
        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
        instanceMap->GetMaxPlayers(),
        instanceMap->IsHeroic() ? "Heroic" : "Normal",
        mapABInfo->minPlayers
    );

    //
    // Dynamic Level Scaling Floor and Ceiling
    //

    // 5-player normal dungeons
    if (instanceMap->GetMaxPlayers() <= 5 && !instanceMap->IsHeroic())
    {
        mapABInfo->levelScalingDynamicCeiling = LevelScalingDynamicLevelCeilingDungeons;
        mapABInfo->levelScalingDynamicFloor = LevelScalingDynamicLevelFloorDungeons;

    }
    // 5-player heroic dungeons
    else if (instanceMap->GetMaxPlayers() <= 5 && instanceMap->IsHeroic())
    {
        mapABInfo->levelScalingDynamicCeiling = LevelScalingDynamicLevelCeilingHeroicDungeons;
        mapABInfo->levelScalingDynamicFloor = LevelScalingDynamicLevelFloorHeroicDungeons;
    }
    // Normal raids
    else if (instanceMap->GetMaxPlayers() > 5 && !instanceMap->IsHeroic())
    {
        mapABInfo->levelScalingDynamicCeiling = LevelScalingDynamicLevelCeilingRaids;
        mapABInfo->levelScalingDynamicFloor = LevelScalingDynamicLevelFloorRaids;
    }
    // Heroic raids
    else if (instanceMap->GetMaxPlayers() > 5 && instanceMap->IsHeroic())
    {
        mapABInfo->levelScalingDynamicCeiling = LevelScalingDynamicLevelCeilingHeroicRaids;
        mapABInfo->levelScalingDynamicFloor = LevelScalingDynamicLevelFloorHeroicRaids;
    }
    // something went wrong
    else
    {
        LOG_ERROR("module.AutoBalance", "AutoBalance::LoadMapSettings: Unable to determine dynamic scaling floor and ceiling for instance {} ({}{}, {}-player {}).", 
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
            instanceMap->GetMaxPlayers(),
            instanceMap->IsHeroic() ? "Heroic" : "Normal"
        );

        mapABInfo->levelScalingDynamicCeiling = 3;
        mapABInfo->levelScalingDynamicFloor = 5;

    }

    //
    // Level Scaling Skip Levels
    //

    // Load the global settings into the map
    mapABInfo->levelScalingSkipHigherLevels = LevelScalingSkipHigherLevels;
    mapABInfo->levelScalingSkipLowerLevels = LevelScalingSkipLowerLevels;

    //
    // Per-instance overrides, if applicable
    //

    if (hasDynamicLevelOverride(map->GetId()))
    {
        AutoBalanceLevelScalingDynamicLevelSettings* myDynamicLevelSettings = &levelScalingDynamicLevelOverrides[map->GetId()];

        // LevelScaling.SkipHigherLevels
        if (myDynamicLevelSettings->skipHigher != -1)
            mapABInfo->levelScalingSkipHigherLevels = myDynamicLevelSettings->skipHigher;

        // LevelScaling.SkipLowerLevels
        if (myDynamicLevelSettings->skipLower != -1)
            mapABInfo->levelScalingSkipLowerLevels = myDynamicLevelSettings->skipLower;

        // LevelScaling.DynamicLevelCeiling
        if (myDynamicLevelSettings->ceiling != -1)
            mapABInfo->levelScalingDynamicCeiling = myDynamicLevelSettings->ceiling;

        // LevelScaling.DynamicLevelFloor
        if (myDynamicLevelSettings->floor != -1)
            mapABInfo->levelScalingDynamicFloor = myDynamicLevelSettings->floor;
    }
}

void AddCreatureToMapData(Creature* creature, bool addToCreatureList = true, Player* playerToExcludeFromChecks = nullptr, bool forceRecalculation = false)
{
    // make sure we have a creature and that it's assigned to a map
    if (!creature || !creature->GetMap())
        return;

    // if this isn't a dungeon, skip
    if (!(creature->GetMap()->IsDungeon()))
        return;

    // get AutoBalance data
    InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));
    AutoBalanceMapInfo *mapABInfo=instanceMap->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
    AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

    // store the creature's original level if this is the first time seeing it
    if (creatureABInfo->UnmodifiedLevel == 0)
    {
        // handle summoned creatures
        if (creature->IsSummon())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Creature {} ({}->\?\?) is a summon.", creature->GetName(), creature->GetLevel());
            if (creature->ToTempSummon() &&
                creature->ToTempSummon()->GetSummoner() &&
                creature->ToTempSummon()->GetSummoner()->ToCreature())
            {
                Creature* summoner = creature->ToTempSummon()->GetSummoner()->ToCreature();
                if (!summoner)
                {
                    creatureABInfo->UnmodifiedLevel = mapABInfo->avgCreatureLevel;
                    LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) is not owned by a summoner. Original level is {}.", 
                                creature->GetName(), 
                                creature->GetLevel()
                    );
                }
                else
                {
                    Creature* summonerCreature = summoner->ToCreature();
                    AutoBalanceCreatureInfo *summonerABInfo=summonerCreature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

                    // if the creature level is within the expected range of NPC levels (as defined by the LFG min and max) for this map, use the creature's spawned level
                    if (creature->GetLevel() >= ((float)mapABInfo->lfgMinLevel * .85f) && creature->GetLevel() <= ((float)mapABInfo->lfgMaxLevel * 1.15f))
                    {
                        creatureABInfo->UnmodifiedLevel = creature->GetLevel();
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) is owned by {} ({}). Creature level is within the expected range of NPC levels for this map. Summon original level set to {}.", 
                                    creature->GetName(), 
                                    creature->GetLevel(), 
                                    summonerCreature->GetName(), 
                                    creatureABInfo->UnmodifiedLevel,
                                    creature->GetLevel()
                        );
                    }
                    // if the summoner is outside the expected range of NPC levels (as defined by the LFG min and max) for this map, use the creature's spawned level
                    else if (summonerABInfo->UnmodifiedLevel < ((float)mapABInfo->lfgMinLevel * .85f) || summonerABInfo->UnmodifiedLevel > ((float)mapABInfo->lfgMaxLevel * 1.15f))
                    {
                        creatureABInfo->UnmodifiedLevel = creature->GetLevel();
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) is owned by {} ({}). Summoner is outside the expected range of NPC levels for this map. Summon original level set to {}.", 
                                    creature->GetName(), 
                                    creature->GetLevel(), 
                                    summonerCreature->GetName(), 
                                    creatureABInfo->UnmodifiedLevel,
                                    creature->GetLevel()
                        );
                    }

                    // if the summoner level is reasonable, the summon inherits the summoner's pre-scaled level as its pre-scaled level
                    else
                    {
                        creatureABInfo->UnmodifiedLevel = summonerABInfo->UnmodifiedLevel;
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) is owned by {} ({}{}). It will inherit it's owner's original level value of {}.", 
                                    creature->GetName(), 
                                    creature->GetLevel(), 
                                    summonerCreature->GetName(), 
                                    summonerABInfo->UnmodifiedLevel, 
                                    summonerCreature->GetLevel() != summonerABInfo->UnmodifiedLevel ? "->" + std::to_string(summonerCreature->GetLevel()) : "", 
                                    summonerABInfo->UnmodifiedLevel
                        );
                    }
                }
            }
            else
            {
                creatureABInfo->UnmodifiedLevel = mapABInfo->avgCreatureLevel;
                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) does not have a summoner.", creature->GetName(), creature->GetLevel());
            }

            // if this is a summon, we shouldn't track it in any list and it does not contribute to the average level
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): Summoned creature {} ({}) will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }
        // handle "special" creatures
        else if (creature->IsCritter() || creature->IsTotem() || creature->IsTrigger())
        {
            // if this is an intentionally-low-level creature (below 85% of the minimum LFG level), leave it where it is
            if (creature->GetLevel() < ((float)mapABInfo->lfgMinLevel * .85f))
            {
                creatureABInfo->UnmodifiedLevel = creature->GetLevel();
            }
            // otherwise, set it to the target level of the instance so it will get scaled properly
            else
            {
                creatureABInfo->UnmodifiedLevel = mapABInfo->lfgTargetLevel;
            }

        }
        // creature isn't a summon, just store their unmodified level
        else
        {
            creatureABInfo->UnmodifiedLevel = creature->GetLevel();
            //LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is not a summon.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        }
    }

    // if this is a creature controlled by the player, skip for stats
    if (((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer()))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is controlled by the player and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        return;
    }

    // if this is a non-relevant creature, skip for stats
    if (creature->IsCritter() || creature->IsTotem() || creature->IsTrigger())
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is a critter, totem, or trigger and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        return;
    }

    // if the creature level is below 85% of the minimum LFG level, assume it's a flavor creature and shouldn't be tracked 
    if (creatureABInfo->UnmodifiedLevel < ((float)mapABInfo->lfgMinLevel * .85f))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is below 85% of the LFG min level of {} and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->lfgMinLevel);
        return;
    }

    // if the creature level is above 125% of the maximum LFG level, assume it's a flavor creature or holiday boss and shouldn't be tracked
    if (creatureABInfo->UnmodifiedLevel > ((float)mapABInfo->lfgMaxLevel * 1.15f))
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is above 115% of the LFG max level of {} and will not affect the map's stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->lfgMaxLevel);
        return;
    }

    // is this creature already in the map's creature list?
    bool isCreatureAlreadyInCreatureList = creatureABInfo->isInCreatureList;

    // add the creature to the map's creature list if configured to do so
    if (addToCreatureList && !isCreatureAlreadyInCreatureList)
    {
        mapABInfo->allMapCreatures.push_back(creature);
        creatureABInfo->isInCreatureList = true;
        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is creature #{} in the creature list.", creature->GetName(), creatureABInfo->UnmodifiedLevel, mapABInfo->allMapCreatures.size());
    }

    // alter stats for the map if needed
    bool isIncludedInMapStats = true;

    // if this creature was already in the creature list, don't consider it for map stats (again)
    // exception for if forceRecalculation is true (used on player enter/exit to recalculate map stats)
    if (isCreatureAlreadyInCreatureList && !forceRecalculation)
    {
        isIncludedInMapStats = false;
    }

    Map::PlayerList const &playerList = creature->GetMap()->GetPlayers();
    if (!playerList.IsEmpty())
    {
        // only do these additional checks if we still think they need to be applied to the map stats
        if (isIncludedInMapStats)
        {
            // if the creature is vendor, trainer, or has gossip, don't use it to update map stats
            if  ((creature->IsVendor() ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER_PROFESSION) ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) ||
                    creature->HasUnitFlag(UNIT_FLAG_IMMUNE_TO_PC) ||
                    creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE)) &&
                    (!creature->IsDungeonBoss())
                )
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is a a vendor, trainer, or is otherwise not attackable - do not include in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
                isIncludedInMapStats = false;
            }
            else
            {
                // if the creature is friendly to a player, don't use it to update map stats
                for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                {
                    Player* playerHandle = playerIteration->GetSource();

                    // if this player matches the player we're supposed to skip or is a Game Master, skip
                    if (playerHandle == playerToExcludeFromChecks || playerHandle->IsGameMaster())
                    {
                        continue;
                    }

                    // if the creature is friendly and not a boss
                    if (creature->IsFriendlyTo(playerHandle) && !creature->IsDungeonBoss())
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is friendly to {} - do not include in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel, playerHandle->GetName());
                        isIncludedInMapStats = false;
                        break;
                    }
                }

                // perform the distance check if an override is configured for this map
                if (hasLevelScalingDistanceCheckOverride(instanceMap->GetId()))
                {
                    uint32 distance = levelScalingDistanceCheckOverrides[instanceMap->GetId()];
                    bool isPlayerWithinDistance = false;

                    for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                    {
                        Player* playerHandle = playerIteration->GetSource();

                        // if this player matches the player we're supposed to skip, skip
                        if (playerHandle == playerToExcludeFromChecks || playerHandle->IsGameMaster())
                        {
                            continue;
                        }

                        if (playerHandle->IsWithinDist(creature, 500))
                        {
                            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is in range ({} world units) of player {} and is considered active.", creature->GetName(), creatureABInfo->UnmodifiedLevel, distance, playerHandle->GetName());
                            isPlayerWithinDistance = true;
                            break;
                        }
                        else
                        {
                            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is NOT in range ({} world units) of any player and is NOT considered active.", creature->GetName(), creature->GetLevel(), distance);
                        }
                    }

                    // if no players were within the distance, don't include this creature in the map stats
                    if (!isPlayerWithinDistance)
                        isIncludedInMapStats = false;
                }
            }
        }

        if (isIncludedInMapStats)
        {
            // mark this creature as being considered in the map stats
            creatureABInfo->isActive = true;

            // update the highest and lowest creature levels
            if (creatureABInfo->UnmodifiedLevel > mapABInfo->highestCreatureLevel || mapABInfo->highestCreatureLevel == 0)
                mapABInfo->highestCreatureLevel = creatureABInfo->UnmodifiedLevel;
            if (creatureABInfo->UnmodifiedLevel < mapABInfo->lowestCreatureLevel || mapABInfo->lowestCreatureLevel == 0)
                mapABInfo->lowestCreatureLevel = creatureABInfo->UnmodifiedLevel;

            // calculate the new average creature level
            float creatureCount = mapABInfo->activeCreatureCount;
            float newAvgCreatureLevel = (((float)mapABInfo->avgCreatureLevel * creatureCount) + (float)creatureABInfo->UnmodifiedLevel) / (creatureCount + 1.0f);
            mapABInfo->avgCreatureLevel = newAvgCreatureLevel;

            // increment the active creature counter
            mapABInfo->activeCreatureCount++;

            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is included in map stats, adjusting avgCreatureLevel to {}", creature->GetName(), creatureABInfo->UnmodifiedLevel, newAvgCreatureLevel);

            // reset the last config time so that the map data will get updated
            lastConfigTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): lastConfigTime reset to {}", lastConfigTime);
        }
        else if (isCreatureAlreadyInCreatureList)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is already included in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): {} ({}) is NOT included in map stats.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::AddCreatureToMapData(): There are {} active creatures.", mapABInfo->activeCreatureCount);
    }
}

void RemoveCreatureFromMapData(Creature* creature)
{
    // get map data
    AutoBalanceMapInfo *mapABInfo=creature->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if the creature is in the all creature list, remove it
    if (mapABInfo->allMapCreatures.size() > 0)
    {
        for (std::vector<Creature*>::iterator creatureIteration = mapABInfo->allMapCreatures.begin(); creatureIteration != mapABInfo->allMapCreatures.end(); ++creatureIteration)
        {
            if (*creatureIteration == creature)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance::RemoveCreatureFromMapData(): {} ({}) is in the creature list and will be removed. There are {} creatures left.", creature->GetName(), creature->GetLevel(), mapABInfo->allMapCreatures.size() - 1);
                mapABInfo->allMapCreatures.erase(creatureIteration);

                // mark this creature as removed
                AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
                creatureABInfo->isInCreatureList = false;
                break;
            }
        }
    }
}

void UpdateMapPlayerStats(Map* map, bool adjustPlayerCount = true)
{
    // get the map's info
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    InstanceMap* instanceMap = map->ToInstanceMap();

    // get the map's player list
    Map::PlayerList const &playerList = map->GetPlayers();

    // if there are players on the map
    if (!playerList.IsEmpty() && map->IsDungeon())
    {
        uint8 highestPlayerLevel = 0;
        uint8 lowestPlayerLevel = 80;

        // iterate through the players and update the highest and lowest player levels
        for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
        {
            Player* playerHandle = playerIteration->GetSource();
            if (playerHandle && !playerHandle->IsGameMaster())
            {
                if (playerHandle->getLevel() > highestPlayerLevel || highestPlayerLevel == 0)
                    highestPlayerLevel = playerHandle->getLevel();

                if (playerHandle->getLevel() < lowestPlayerLevel || lowestPlayerLevel == 0)
                    lowestPlayerLevel = playerHandle->getLevel();
            }
        }

        mapABInfo->highestPlayerLevel = highestPlayerLevel;
        mapABInfo->lowestPlayerLevel = lowestPlayerLevel;

        if (!highestPlayerLevel)
        {
            mapABInfo->highestPlayerLevel = mapABInfo->lfgTargetLevel;
            mapABInfo->lowestPlayerLevel = mapABInfo->lfgTargetLevel;
            
            // no non-GM players on the map, disable it
            mapABInfo->enabled = false;
        }

        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapPlayerStats(): Map {} ({}{}, {}-player {}) player level range: {} - {}.", 
            map->GetMapName(),
            map->GetId(),
            map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
            instanceMap->GetMaxPlayers(),
            instanceMap->IsHeroic() ? "Heroic" : "Normal",
            mapABInfo->lowestPlayerLevel, 
            mapABInfo->highestPlayerLevel
        );
    }

    // update the player count (unless we should specifically skip this step)
    if (adjustPlayerCount)
    {
        mapABInfo->playerCount = map->GetPlayersCountExceptGMs();
    }

    // start with the real player count
    uint32 adjustedPlayerCount = mapABInfo->playerCount;

    // if the adjusted player count is below the min players setting, adjust it
    if (adjustedPlayerCount < mapABInfo->minPlayers)
        adjustedPlayerCount = mapABInfo->minPlayers;

    // adjust by the PlayerDifficultyOffset
    adjustedPlayerCount += PlayerCountDifficultyOffset;

    // store the adjusted player count in the map's info
    mapABInfo->adjustedPlayerCount = adjustedPlayerCount;
}

void UpdateMapDataIfNeeded(Map* map)
{
    // get map data
    AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

    // if map needs update
    if (mapABInfo->configTime != lastConfigTime)
    {
        LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded(): Map {} ({}{}) config is out of date ({} != {}) and will be updated.",
                  map->GetMapName(),
                  map->GetId(),
                  map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                  mapABInfo->configTime,
                  lastConfigTime
        );

        // should the map be enabled?
        mapABInfo->enabled = ShouldMapBeEnabled(map);

        if (!mapABInfo->enabled)
        {            
            // mark the config updated to prevent checking the disabled map repeatedly
            mapABInfo->configTime = lastConfigTime;

            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded(): Map {} ({}{}) config time updated to {}.", 
                        map->GetMapName(),
                        map->GetId(),
                        map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                        mapABInfo->configTime
            );

            return;
        }
        
        // load the map's settings
        LoadMapSettings(map);

        // update the map's player stats
        UpdateMapPlayerStats(map);

        // if LevelScaling is disabled OR if the average creature level is inside the skip range,
        // set the map level to the average creature level, rounded to the nearest integer
        if (!LevelScaling ||
            ((mapABInfo->avgCreatureLevel <= mapABInfo->highestPlayerLevel + mapABInfo->levelScalingSkipHigherLevels && mapABInfo->levelScalingSkipHigherLevels != 0) &&
            (mapABInfo->avgCreatureLevel >= mapABInfo->highestPlayerLevel - mapABInfo->levelScalingSkipLowerLevels && mapABInfo->levelScalingSkipLowerLevels != 0))
        )
        {
            mapABInfo->mapLevel = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);
            mapABInfo->isLevelScalingEnabled = false;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded(): Map {} ({}{}, {}-player {}) scaling is disabled. Map level is now {} (original).", 
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                map->ToInstanceMap()->GetMaxPlayers(),
                map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal", 
                mapABInfo->mapLevel
            );
        }
        // If the average creature level is lower than the highest player level,
        // set the map level to the average creature level, rounded to the nearest integer
        else if (mapABInfo->avgCreatureLevel <= mapABInfo->highestPlayerLevel)
        {
            mapABInfo->mapLevel = (uint8)(mapABInfo->avgCreatureLevel + 0.5f);
            mapABInfo->isLevelScalingEnabled = true;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded(): Map {} ({}{}, {}-player {}) scaling is enabled. Map level is now {} (average creature level).", 
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                map->ToInstanceMap()->GetMaxPlayers(),
                map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal", 
                mapABInfo->mapLevel
            );
        }
        // caps at the highest player level
        else
        {
            mapABInfo->mapLevel = mapABInfo->highestPlayerLevel;
            mapABInfo->isLevelScalingEnabled = true;
            LOG_DEBUG("module.AutoBalance", "AutoBalance::UpdateMapDataIfNeeded(): Map {} ({}{}, {}-player {}) scaling is enabled. Map level is now {} (highest player level).",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : "",
                map->ToInstanceMap()->GetMaxPlayers(),
                map->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal", 
                mapABInfo->mapLevel
            );
        }

        // Update World Damage or Healing multiplier
        // Used for scaling damage and healing between players and/or units
        mapABInfo->worldDamageHealingMultiplier = getWorldMultiplier(map, BaseValueType::AUTOBALANCE_DAMAGE_HEALING);

        // Update World Health multiplier
        // Used for scaling damage against destructible game objects
        mapABInfo->worldHealthMultiplier = getWorldMultiplier(map, BaseValueType::AUTOBALANCE_HEALTH);

        // mark the config updated
        mapABInfo->configTime = lastConfigTime;
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
        lastConfigTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void OnStartup() override
    {
    }

    void SetInitialWorldSettings()
    {
        forcedCreatureIds.clear();
        disabledDungeonIds.clear();
        dungeonOverrides.clear();
        bossOverrides.clear();
        statModifierOverrides.clear();
        statModifierBossOverrides.clear();
        statModifierCreatureOverrides.clear();
        levelScalingDynamicLevelOverrides.clear();
        levelScalingDistanceCheckOverrides.clear();

        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID40", ""), 40);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID25", ""), 25);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID10", ""), 10);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID5", ""), 5);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.ForcedID2", ""), 2);
        LoadForcedCreatureIdsFromString(sConfigMgr->GetOption<std::string>("AutoBalance.DisabledID", ""), 0);

        // Disabled Dungeon IDs
        disabledDungeonIds = LoadDisabledDungeons(sConfigMgr->GetOption<std::string>("AutoBalance.Disable.PerInstance", ""));

        // Min Players
        minPlayersNormal = sConfigMgr->GetOption<int>("AutoBalance.MinPlayers", 1);
        minPlayersHeroic = sConfigMgr->GetOption<int>("AutoBalance.MinPlayers.Heroic", 1);

        if (sConfigMgr->GetOption<float>("AutoBalance.PerDungeonPlayerCounts", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.PerDungeonPlayerCounts` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        minPlayersPerDungeonIdMap = LoadMinPlayersPerDungeonId(
            sConfigMgr->GetOption<std::string>("AutoBalance.MinPlayers.PerInstance", sConfigMgr->GetOption<std::string>("AutoBalance.PerDungeonPlayerCounts", "", false), false)
        ); // `AutoBalance.PerDungeonPlayerCounts` for backwards compatibility
        minPlayersPerHeroicDungeonIdMap = LoadMinPlayersPerDungeonId(
            sConfigMgr->GetOption<std::string>("AutoBalance.MinPlayers.Heroic.PerInstance", sConfigMgr->GetOption<std::string>("AutoBalance.PerDungeonPlayerCounts", "", false), false)
        ); // `AutoBalance.PerDungeonPlayerCounts` for backwards compatibility

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

        levelScalingDynamicLevelOverrides = LoadDynamicLevelOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.LevelScaling.DynamicLevel.PerInstance", "", false)
        );

        levelScalingDistanceCheckOverrides = LoadDistanceCheckOverrides(
            sConfigMgr->GetOption<std::string>("AutoBalance.LevelScaling.DynamicLevel.DistanceCheck.PerInstance", "", false)
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

        if (sConfigMgr->GetOption<int>("AutoBalance.levelUseDbValuesWhenExists", -1, false) != -1)
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.levelUseDbValuesWhenExists` defined in `AutoBalance.conf`. This variable has been removed and has no effect. Please see `AutoBalance.conf.dist` for more details.");

        // Misc Settings
        // TODO: Organize and standardize variable names

        PlayerChangeNotify = sConfigMgr->GetOption<bool>("AutoBalance.PlayerChangeNotify", 1);

        rewardEnabled = sConfigMgr->GetOption<bool>("AutoBalance.reward.enable", 1);
        PlayerCountDifficultyOffset = sConfigMgr->GetOption<uint32>("AutoBalance.playerCountDifficultyOffset", 0);
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

        // LevelScaling.*
        LevelScaling = sConfigMgr->GetOption<bool>("AutoBalance.LevelScaling", true);

        std::string LevelScalingMethodString = sConfigMgr->GetOption<std::string>("AutoBalance.LevelScaling.Method", "dynamic", false);
        if (LevelScalingMethodString == "fixed")
        {
            LevelScalingMethod = AUTOBALANCE_SCALING_FIXED;
        }
        else if (LevelScalingMethodString == "dynamic")
        {
            LevelScalingMethod = AUTOBALANCE_SCALING_DYNAMIC;
        }
        else
        {
            LOG_ERROR("server.loading", "mod-autobalance: invalid value `{}` for `AutoBalance.LevelScaling.Method` defined in `AutoBalance.conf`. Defaulting to a value of `dynamic`.", LevelScalingMethodString);
            LevelScalingMethod = AUTOBALANCE_SCALING_DYNAMIC;
        }

        if (sConfigMgr->GetOption<float>("AutoBalance.LevelHigherOffset", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.LevelHigherOffset` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        LevelScalingSkipHigherLevels = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.SkipHigherLevels", sConfigMgr->GetOption<uint32>("AutoBalance.LevelHigherOffset", 3, false), true);
        if (sConfigMgr->GetOption<float>("AutoBalance.LevelLowerOffset", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.LevelLowerOffset` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        LevelScalingSkipLowerLevels = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.SkipLowerLevels", sConfigMgr->GetOption<uint32>("AutoBalance.LevelLowerOffset", 5, false), true);

        LevelScalingDynamicLevelCeilingDungeons = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Ceiling.Dungeons", 1);
        LevelScalingDynamicLevelFloorDungeons = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Floor.Dungeons", 5);
        LevelScalingDynamicLevelCeilingHeroicDungeons = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Ceiling.HeroicDungeons", 2);
        LevelScalingDynamicLevelFloorHeroicDungeons = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Floor.HeroicDungeons", 5);
        LevelScalingDynamicLevelCeilingRaids = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Ceiling.Raids", 3);
        LevelScalingDynamicLevelFloorRaids = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Floor.Raids", 5);
        LevelScalingDynamicLevelCeilingHeroicRaids = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Ceiling.HeroicRaids", 3);
        LevelScalingDynamicLevelFloorHeroicRaids = sConfigMgr->GetOption<uint8>("AutoBalance.LevelScaling.DynamicLevel.Floor.HeroicRaids", 5);

        if (sConfigMgr->GetOption<float>("AutoBalance.LevelEndGameBoost", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.LevelEndGameBoost` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        LevelScalingEndGameBoost = sConfigMgr->GetOption<bool>("AutoBalance.LevelScaling.EndGameBoost", sConfigMgr->GetOption<bool>("AutoBalance.LevelEndGameBoost", 1, false), true);

        // RewardScaling.*
        // warn the console if deprecated values are detected
        if (sConfigMgr->GetOption<float>("AutoBalance.DungeonScaleDownXP", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.DungeonScaleDownXP` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");
        if (sConfigMgr->GetOption<float>("AutoBalance.DungeonScaleDownMoney", false, false))
            LOG_WARN("server.loading", "mod-autobalance: deprecated value `AutoBalance.DungeonScaleDownMoney` defined in `AutoBalance.conf`. This variable will be removed in a future release. Please see `AutoBalance.conf.dist` for more details.");

        std::string RewardScalingMethodString = sConfigMgr->GetOption<std::string>("AutoBalance.RewardScaling.Method", "dynamic", false);
        if (RewardScalingMethodString == "fixed")
        {
            RewardScalingMethod = AUTOBALANCE_SCALING_FIXED;
        }
        else if (RewardScalingMethodString == "dynamic")
        {
            RewardScalingMethod = AUTOBALANCE_SCALING_DYNAMIC;
        }
        else
        {
            LOG_ERROR("server.loading", "mod-autobalance: invalid value `{}` for `AutoBalance.RewardScaling.Method` defined in `AutoBalance.conf`. Defaulting to a value of `dynamic`.", RewardScalingMethodString);
            RewardScalingMethod = AUTOBALANCE_SCALING_DYNAMIC;
        }

        RewardScalingXP = sConfigMgr->GetOption<bool>("AutoBalance.RewardScaling.XP", sConfigMgr->GetOption<bool>("AutoBalance.DungeonScaleDownXP", true, false));
        RewardScalingXPModifier = sConfigMgr->GetOption<float>("AutoBalance.RewardScaling.XP.Modifier", 1.0f, false);

        RewardScalingMoney = sConfigMgr->GetOption<bool>("AutoBalance.RewardScaling.Money", sConfigMgr->GetOption<bool>("AutoBalance.DungeonScaleDownMoney", true, false));
        RewardScalingMoneyModifier = sConfigMgr->GetOption<float>("AutoBalance.RewardScaling.Money.Modifier", 1.0f, false);

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

        virtual void OnLevelChanged(Player* player, uint8 oldlevel) override
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            
            LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnLevelChanged(): {} has leveled from {} to {}", player->GetName(), oldlevel, player->getLevel());
            if (!player || player->IsGameMaster())
                return;

            Map* map = player->GetMap();

            if (!map || !map->IsDungeon())
                return;

            // first update the map's player stats
            UpdateMapPlayerStats(map);

            // schedule all creatures for an update
            lastConfigTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        void OnGiveXP(Player* player, uint32& amount, Unit* victim, uint8 /*xpSource*/) override
        {
            Map* map = player->GetMap();

            // If this isn't a dungeon, make no changes
            if (!map->IsDungeon() || !victim)
                return;

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            if (victim && RewardScalingXP && mapABInfo->enabled)
            {
                Map* map = player->GetMap();

                AutoBalanceCreatureInfo *creatureABInfo=victim->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

                if (map->IsDungeon())
                {
                    if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnGiveXP(): Distributing XP from '{}' to '{}' in dynamic mode - {}->{}",
                                 victim->GetName(), player->GetName(), amount, uint32(amount * creatureABInfo->XPModifier));
                        amount = uint32(amount * creatureABInfo->XPModifier);
                    }
                    else if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
                    {
                        // Ensure that the players always get the same XP, even when entering the dungeon alone
                        auto maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                        auto currentPlayerCount = map->GetPlayersCountExceptGMs();
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnGiveXP(): Distributing XP from '{}' to '{}' in fixed mode - {}->{}",
                                 victim->GetName(), player->GetName(), amount, uint32(amount * creatureABInfo->XPModifier * ((float)currentPlayerCount / maxPlayerCount)));
                        amount = uint32(amount * creatureABInfo->XPModifier * ((float)currentPlayerCount / maxPlayerCount));
                    }
                }
            }
        }

        void OnBeforeLootMoney(Player* player, Loot* loot) override
        {
            Map* map = player->GetMap();

            // If this isn't a dungeon, make no changes
            if (!map->IsDungeon())
                return;

            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            ObjectGuid sourceGuid = loot->sourceWorldObjectGUID;

            if (mapABInfo->enabled && RewardScalingMoney)
            {
                // if the loot source is a creature, honor the modifiers for that creature
                if (sourceGuid.IsCreature())
                {
                    Creature* sourceCreature = ObjectAccessor::GetCreature(*player, sourceGuid);
                    AutoBalanceCreatureInfo *creatureABInfo=sourceCreature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

                    // Dynamic Mode
                    if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
                    {
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney(): Distributing money from '{}' in dynamic mode - {}->{}",
                                 sourceCreature->GetName(), loot->gold, uint32(loot->gold * creatureABInfo->MoneyModifier));
                        loot->gold = uint32(loot->gold * creatureABInfo->MoneyModifier);
                    }
                    // Fixed Mode
                    else if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
                    {
                        // Ensure that the players always get the same money, even when entering the dungeon alone
                        auto maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                        auto currentPlayerCount = map->GetPlayersCountExceptGMs();
                        LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney(): Distributing money from '{}' in fixed mode - {}->{}",
                                 sourceCreature->GetName(), loot->gold, uint32(loot->gold * creatureABInfo->MoneyModifier * ((float)currentPlayerCount / maxPlayerCount)));
                        loot->gold = uint32(loot->gold * creatureABInfo->MoneyModifier * ((float)currentPlayerCount / maxPlayerCount));
                    }
                }
                // for all other loot sources, just distribute in Fixed mode as though the instance was full
                else
                {
                    auto maxPlayerCount = ((InstanceMap*)sMapMgr->FindMap(map->GetId(), map->GetInstanceId()))->GetMaxPlayers();
                    auto currentPlayerCount = map->GetPlayersCountExceptGMs();
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_PlayerScript::OnBeforeLootMoney(): Distributing money from a non-creature in fixed mode - {}->{}",
                             loot->gold, uint32(loot->gold * ((float)currentPlayerCount / maxPlayerCount)));
                    loot->gold = uint32(loot->gold * ((float)currentPlayerCount / maxPlayerCount));
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

        void ModifyPeriodicDamageAurasTick(Unit* target, Unit* source, uint32& amount, SpellInfo const* spellInfo) override
        {
            // if the spell is negative (damage), we need to flip the sign
            // if the spell is positive (healing or other) we keep it the same
            int32 adjustedAmount = !spellInfo->IsPositive() ? amount * -1 : amount;
            
            // uncomment to debug this hook
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            
            if (_debug_damage_and_healing) _Debug_Output("ModifyPeriodicDamageAurasTick", target, source, adjustedAmount, "BEFORE:", spellInfo->SpellName[0], spellInfo->Id);

            // set amount to the absolute value of the function call
            // the provided amount doesn't indicate whether it's a positive or negative value
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount);
            amount = abs(adjustedAmount);

            if (_debug_damage_and_healing) _Debug_Output("ModifyPeriodicDamageAurasTick", target, source, adjustedAmount, "AFTER:", spellInfo->SpellName[0], spellInfo->Id);
        }

        void ModifySpellDamageTaken(Unit* target, Unit* source, int32& amount, SpellInfo const* spellInfo) override
        {
            // if the spell is negative (damage), we need to flip the sign
            // if the spell is positive (healing or other) we keep it the same
            int32 adjustedAmount = !spellInfo->IsPositive() ? amount * -1 : amount;
            
            // uncomment to debug this hook
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            
            if (_debug_damage_and_healing) _Debug_Output("ModifySpellDamageTaken", target, source, adjustedAmount, "BEFORE:", spellInfo->SpellName[0], spellInfo->Id);

            // set amount to the absolute value of the function call
            // the provided amount doesn't indicate whether it's a positive or negative value
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount);
            amount = abs(adjustedAmount);

            if (_debug_damage_and_healing) _Debug_Output("ModifySpellDamageTaken", target, source, adjustedAmount, "AFTER:", spellInfo->SpellName[0], spellInfo->Id);
        }

        void ModifyMeleeDamage(Unit* target, Unit* source, uint32& amount) override
        {
            // melee damage is always negative, so we need to flip the sign
            int32 adjustedAmount = amount * -1;
            
            // uncomment to debug this hook
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            
            if (_debug_damage_and_healing) _Debug_Output("ModifyMeleeDamage", target, source, adjustedAmount, "BEFORE:", "Melee");

            // set amount to the absolute value of the function call
            adjustedAmount = _Modify_Damage_Healing(target, source, adjustedAmount);
            amount = abs(adjustedAmount);
            
            if (_debug_damage_and_healing) _Debug_Output("ModifyMeleeDamage", target, source, adjustedAmount, "AFTER:", "Melee");
        }

        void ModifyHealReceived(Unit* target, Unit* source, uint32& amount, SpellInfo const* spellInfo) override
        {         
            // healing is always positive, no need for any sign flip
            
            // uncomment to debug this hook
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));
            
            if (_debug_damage_and_healing) _Debug_Output("ModifyHealReceived", target, source, amount, "BEFORE:", spellInfo->SpellName[0], spellInfo->Id);

            amount = _Modify_Damage_Healing(target, source, amount);

            if (_debug_damage_and_healing) _Debug_Output("ModifyHealReceived", target, source, amount, "AFTER:", spellInfo->SpellName[0], spellInfo->Id);
        }

        void OnAuraApply(Unit* unit, Aura* aura) override {
            // uncomment to debug this hook
            bool _debug_damage_and_healing = (unit && unit->GetTypeId() == TYPEID_PLAYER);
            
            // Only if this aura has a duration
            if (aura->GetDuration() > 0 || aura->GetMaxDuration() > 0)
            {
                uint32 auraDuration = _Modifier_CCDuration(unit, aura->GetCaster(), aura);

                // only update if we decided to change it
                if (auraDuration != (float)aura->GetDuration())
                {
                    if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::OnAuraApply(): Spell '{}' had it's duration adjusted ({}->{}).", aura->GetSpellInfo()->SpellName[0], aura->GetMaxDuration()/1000, auraDuration/1000);
                    
                    aura->SetMaxDuration(auraDuration);
                    aura->SetDuration(auraDuration);
                }
            }
        }

    private:
        [[maybe_unused]] bool _debug_damage_and_healing = false; // defaults to false, overwritten in each function

        void _Debug_Output(std::string function_name, Unit* target, Unit* source, int32 amount, std::string prefix = "", std::string spell_name = "Unknown Spell", uint32 spell_id = 0)
        {
            if (target && source && amount)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::{}(): {} {} {} {} ({} - {})", function_name, prefix, source->GetName(), amount, target->GetName(), spell_name, spell_id);
            }
            else if (target && source)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::{}(): {} {} 0 {} ({} - {})", function_name, prefix, source->GetName(), target->GetName(), spell_name, spell_id);
            }
            else if (target && amount)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::{}(): {} ?? {} {} ({} - {})", function_name, prefix, amount, target->GetName(), spell_name, spell_id);
            } 
            else if (target)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::{}(): {} ?? ?? {} ({} - {})", function_name, prefix, target->GetName(), spell_name, spell_id);
            } 
            else
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::{}(): {} W? T? F? ({} - {})", function_name, prefix, spell_name, spell_id);
            }
        }

        int32 _Modify_Damage_Healing(Unit* target, Unit* source, int32 amount)
        {
            //
            // Pre-flight Checks
            //

            // uncomment to debug this function
            bool _debug_damage_and_healing = ((source && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer())) || (target && target->GetTypeId() == TYPEID_PLAYER));

            // check that we're enabled globally, else return the original value
            if (!EnableGlobal)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): EnableGlobal is false, returning original value of {}.", amount);

                return amount;
            }

            // if the source is gone (logged off? despawned?), use the same target and source
            // hacky, but better than crashing or having the damage go to 1.0x
            if (!source)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is null, using target as source.");

                source = target;
            }

            // make sure the source and target are in an instance, else return the original damage
            if (!(source->GetMap()->IsDungeon() && target->GetMap()->IsDungeon()))
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Not in an instance, returning original value of {}.", amount);

                return amount;
            }

            // make sure that the source is in the world, else return the original value
            if (!source->IsInWorld())
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source does not exist in the world, returning original value of {}.", amount);

                return amount;
            }

            // get the maps' info
            AutoBalanceMapInfo *sourceMapABInfo = source->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            AutoBalanceMapInfo *targetMapABInfo = target->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if either the target or the source's maps are not enabled, return the original damage
            if (!sourceMapABInfo->enabled || !targetMapABInfo->enabled)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source or Target's map is not enabled, returning original value of {}.", amount);

                return amount;
            }

            //
            // Source and Target Checking
            //

            // if the source is a player and they are healing themselves, return the original value
            if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount >= 0)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a player that is self-healing, returning original value of {}.", amount);

                return amount;
            }
            // if the source is a player and they are damaging themselves, log to debug but continue
            else if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount < 0)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a player that is self-damaging, continuing.");
            }
            // if the source is a player and they are damaging unit that is friendly, log to debug but continue
            else if (source->GetTypeId() == TYPEID_PLAYER && target->IsFriendlyTo(source) && amount < 0)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a player that is damaging a friendly unit, continuing.");
            }
            // if the source is a player under any other condition, return the original value
            else if (source->GetTypeId() == TYPEID_PLAYER)
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a player, returning original value of {}.", amount);

                return amount;
            }

            // if the source is under the control of the player, return the original damage
            // noteably, this should NOT include mind control targets
            if ((source->IsHunterPet() || source->IsPet() || source->IsSummon()) && source->IsControlledByPlayer())
            {
                if (_debug_damage_and_healing)
                    LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a pet or summon, returning original value of {}.", amount);

                return amount;
            }

            //
            // Multiplier calculation
            //
            float damageMultiplier = 1.0f;

            // if the source is a player AND the target is that same player AND the value is damage (negative), use the map's multiplier
            if (source->GetTypeId() == TYPEID_PLAYER && source->GetGUID() == target->GetGUID() && amount < 0)
            {
                damageMultiplier = sourceMapABInfo->worldDamageHealingMultiplier;
                if (_debug_damage_and_healing)
                {
                    LOG_DEBUG("module.AutoBalance.Damage",
                              "AutoBalance_UnitScript::_Modify_Damage_Healing(): Source is a player and the target is that same player, using the map's multiplier: {}",
                              damageMultiplier
                    );
                }
            }
            // if the target is a player AND the value is healing (positive), use the map's damage multiplier
            // (player to player healing was already eliminated in the Source and Target Checking section)
            else if (target->GetTypeId() == TYPEID_PLAYER && amount >= 0)
            {
                damageMultiplier = targetMapABInfo->worldDamageHealingMultiplier;
                if (_debug_damage_and_healing)
                {
                    LOG_DEBUG("module.AutoBalance.Damage",
                              "AutoBalance_UnitScript::_Modify_Damage_Healing(): Target for healing is a player, using the map's multiplier: {}",
                              damageMultiplier
                    );
                }
            }
            // if the target is a player AND the source is not a creature, use the map's multiplier
            else if (target->GetTypeId() == TYPEID_PLAYER && source->GetTypeId() != TYPEID_UNIT && amount < 0)
            {
                damageMultiplier = targetMapABInfo->worldDamageHealingMultiplier;
                if (_debug_damage_and_healing)
                {
                    LOG_DEBUG("module.AutoBalance.Damage",
                              "AutoBalance_UnitScript::_Modify_Damage_Healing(): Target is a player and the source is not a creature, using the map's damage multiplier: {}",
                              damageMultiplier
                    );
                }
            }
            // otherwise, use the source creature's damage multiplier
            else
            {
                damageMultiplier = source->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo")->DamageMultiplier;
                if (_debug_damage_and_healing)
                {
                    LOG_DEBUG("module.AutoBalance.Damage",
                              "AutoBalance_UnitScript::_Modify_Damage_Healing(): Using the source creature's damage multiplier: {}",
                              damageMultiplier
                    );
                }
            }

            // we are good to go, return the original damage times the multiplier
            if (_debug_damage_and_healing)
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_UnitScript::_Modify_Damage_Healing(): Returning modified damage: {} * {} = {}", amount, damageMultiplier, amount * damageMultiplier);

            return amount * damageMultiplier;
        }

        uint32 _Modifier_CCDuration(Unit* target, Unit* caster, Aura* aura)
        {
            // store the original duration of the aura
            float originalDuration = (float)aura->GetDuration();

            // check that we're enabled globally, else return the original duration
            if (!EnableGlobal)
                return originalDuration;

            // ensure that both the target and the caster are defined
            if (!target || !caster)
                return originalDuration;

            // if the aura wasn't cast just now, don't change it
            if (aura->GetDuration() != aura->GetMaxDuration())
                return originalDuration;

            // if the target isn't a player or the caster is a player, return the original duration
            if (!target->IsPlayer() || caster->IsPlayer())
                return originalDuration;

            // make sure we're in an instance, else return the original duration
            if (!(target->GetMap()->IsDungeon() && caster->GetMap()->IsDungeon()))
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

class AutoBalance_GameObjectScript : public AllGameObjectScript
{
    public:
    AutoBalance_GameObjectScript()
        : AllGameObjectScript("AutoBalance_GameObjectScript")
        {}

        void OnGameObjectModifyHealth(GameObject* target, Unit* source, int32& amount, SpellInfo const* spellInfo) override
        {
            // uncomment to debug this hook
            bool _debug_damage_and_healing = (source && target && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer()));
            
            if (_debug_damage_and_healing) _Debug_Output("OnGameObjectModifyHealth", target, source, amount, "BEFORE:", spellInfo->SpellName[0], spellInfo->Id);

            // modify the amount
            amount = _Modify_GameObject_Damage_Healing(target, source, amount);

            if (_debug_damage_and_healing) _Debug_Output("OnGameObjectModifyHealth", target, source, amount, "AFTER:", spellInfo->SpellName[0], spellInfo->Id);
        }

    private:

        [[maybe_unused]] bool _debug_damage_and_healing = false; // defaults to false, overwritten in each function

        void _Debug_Output(std::string function_name, GameObject* target, Unit* source, int32 amount, std::string prefix = "", std::string spell_name = "Unknown Spell", uint32 spell_id = 0)
        {
            if (target && source && amount)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::{}(): {} {} {} {} ({} - {})", function_name, prefix, source->GetName(), amount, target->GetName(), spell_name, spell_id);
            }
            else if (target && source)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::{}(): {} {} 0 {} ({} - {})", function_name, prefix, source->GetName(), target->GetName(), spell_name, spell_id);
            }
            else if (target && amount)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::{}(): {} ?? {} {} ({} - {})", function_name, prefix, amount, target->GetName(), spell_name, spell_id);
            } 
            else if (target)
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::{}(): {} ?? ?? {} ({} - {})", function_name, prefix, target->GetName(), spell_name, spell_id);
            } 
            else
            {
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::{}(): {} W? T? F? ({} - {})", function_name, prefix, spell_name, spell_id);
            }
        }
        
        int32 _Modify_GameObject_Damage_Healing(GameObject* target, Unit* source, int32 amount)
        {
            //
            // Pre-flight Checks
            //

            // uncomment to debug this function
            bool _debug_damage_and_healing = (source && target && (source->GetTypeId() == TYPEID_PLAYER || source->IsControlledByPlayer()));

            // check that we're enabled globally, else return the original value
            if (!EnableGlobal)
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing(): EnableGlobal is false, returning original value of {}.", amount);

                return amount;
            }

            // make sure the target is in an instance, else return the original damage
            if (!(target->GetMap()->IsDungeon()))
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing(): Target is not in an instance, returning original value of {}.", amount);

                return amount;
            }

            // make sure the target is in the world, else return the original value
            if (!target->IsInWorld())
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing(): Target does not exist in the world, returning original value of {}.", amount);

                return amount;
            }

            // get the map's info
            AutoBalanceMapInfo *targetMapABInfo = target->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // if the target's map is not enabled, return the original damage
            if (!targetMapABInfo->enabled)
            {
                if (_debug_damage_and_healing) LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing(): Target's map is not enabled, returning original value of {}.", amount);

                return amount;
            }

            //
            // Multiplier calculation
            //
            
            // calculate the new damage amount using the map's World Health Multiplier
            int32 newAmount = _Calculate_Amount_For_GameObject(target, amount, targetMapABInfo->worldHealthMultiplier);

            if (_debug_damage_and_healing)
                LOG_DEBUG("module.AutoBalance.Damage", "AutoBalance_GameObjectScript::_Modify_GameObject_Damage_Healing(): Returning modified damage: {} -> {}", amount, newAmount);

            return newAmount;
        }

        int32 _Calculate_Amount_For_GameObject (GameObject* target, int32 amount, float multiplier)
        {
            // since it would be very complicated to reduce the real health of destructible game objects, instead we will
            // adjust the damage to them as though their health were scaled. Damage will usually be dealt by vehicles and
            // other non-player sources, so this effect shouldn't be as noticable as if we applied it to the player.
            uint32 realMaxHealth = target->GetGOValue()->Building.MaxHealth;

            uint32 scaledMaxHealth = realMaxHealth * multiplier;
            float percentDamageOfScaledMaxHealth = (float)amount / (float)scaledMaxHealth;

            uint32 scaledAmount = realMaxHealth * percentDamageOfScaledMaxHealth;

            return scaledAmount;
        }
};


class AutoBalance_AllMapScript : public AllMapScript
{
    public:
    AutoBalance_AllMapScript()
        : AllMapScript("AutoBalance_AllMapScript")
        {
        }

        void OnCreateMap(Map* map)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnCreateMap(): {} ({}{})",
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            // clear out any previously-recorded data
            map->CustomData.Erase("AutoBalanceMapInfo");

            // get the map's LFG stats even if not enabled
            LFGDungeonEntry const* dungeon = GetLFGDungeon(map->GetId(), map->GetDifficulty());
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            if (dungeon) {
                mapABInfo->lfgMinLevel = dungeon->MinLevel;
                mapABInfo->lfgMaxLevel = dungeon->MaxLevel;
                mapABInfo->lfgTargetLevel = dungeon->TargetLevel;
            }
        }

        void OnPlayerEnterAll(Map* map, Player* player)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerEnterAll(): Player {} enters {} ({}{})",
                player->GetName(),
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            if (!map->IsDungeon())
                return;

            if (player->IsGameMaster())
                return;

            // get the map's info
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");
            
            // Update the map's level if it is out of date
            UpdateMapDataIfNeeded(map);

            // recalculate the zone's level stats
            mapABInfo->highestCreatureLevel = 0;
            mapABInfo->lowestCreatureLevel = 0;
            mapABInfo->avgCreatureLevel = 0;
            mapABInfo->activeCreatureCount = 0;

            // see which existing creatures are active
            for (std::vector<Creature*>::iterator creatureIterator = mapABInfo->allMapCreatures.begin(); creatureIterator != mapABInfo->allMapCreatures.end(); ++creatureIterator)
            {
                AddCreatureToMapData(*creatureIterator, false, nullptr, true);
            }

            // updates the player count, player levels for the map
            UpdateMapPlayerStats(map);

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

                                Player* mapPlayer = playerIteration->GetSource();

                                if (mapPlayer && mapPlayer == player) // This is the player that entered
                                {
                                    chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 Welcome to %s (%u-player %s). There are %u player(s) in this instance. Difficulty set to %u player(s).|r",
                                        map->GetMapName(),
                                        instanceMap->GetMaxPlayers(),
                                        instanceDifficulty,
                                        mapABInfo->playerCount,
                                        mapABInfo->adjustedPlayerCount
                                    );
                                }
                                else
                                {
                                    chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s enters the instance. There are %u player(s) in this instance. Difficulty set to %u player(s).|r",
                                        player->GetName().c_str(),
                                        mapABInfo->playerCount,
                                        mapABInfo->adjustedPlayerCount
                                    );
                                }


                            }
                        }
                    }
                }
            }
        }

        void OnPlayerLeaveAll(Map* map, Player* player)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll(): Player {} exits {} ({}{})",
                player->GetName(),
                map->GetMapName(),
                map->GetId(),
                map->GetInstanceId() ? "-" + std::to_string(map->GetInstanceId()) : ""
            );

            if (!EnableGlobal)
                return;

            // get the map's info
            AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

            // Update the map's level if it is out of date
            UpdateMapDataIfNeeded(map);

            // recalculate the zone's level stats
            mapABInfo->highestCreatureLevel = 0;
            mapABInfo->lowestCreatureLevel = 0;
            mapABInfo->avgCreatureLevel = 0;
            mapABInfo->activeCreatureCount = 0;

            // see which existing creatures are active
            for (std::vector<Creature*>::iterator creatureIterator = mapABInfo->allMapCreatures.begin(); creatureIterator != mapABInfo->allMapCreatures.end(); ++creatureIterator)
            {
                AddCreatureToMapData(*creatureIterator, false, player, true);
            }

            bool areAnyPlayersInCombat = false;

            // updates the player count and levels for the map
            if (map->GetEntry() && map->GetEntry()->IsDungeon())
            {
                // determine if any players in the map are in combat
                // if so, do not adjust the player count
                Map::PlayerList const& mapPlayerList = map->GetPlayers();
                for (Map::PlayerList::const_iterator itr = mapPlayerList.begin(); itr != mapPlayerList.end(); ++itr)
                {
                    if (Player* mapPlayer = itr->GetSource())
                    {
                        if (mapPlayer->IsInCombat() && mapPlayer->GetMap() == map)
                        {
                            areAnyPlayersInCombat = true;

                            // notify the player that they left the instance while combat was in progress
                            ChatHandler chatHandle = ChatHandler(player->GetSession());
                            chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 You left the instance while combat was in progress. The instance player count is still %u.", mapABInfo->playerCount);

                            break;
                        }
                    }
                }
                if (areAnyPlayersInCombat)
                {
                    for (Map::PlayerList::const_iterator itr = mapPlayerList.begin(); itr != mapPlayerList.end(); ++itr)
                    {
                        if (Player* mapPlayer = itr->GetSource())
                        {
                            // only for the players who are in the instance and did not leave
                            if (mapPlayer != player)
                            {
                                ChatHandler chatHandle = ChatHandler(mapPlayer->GetSession());
                                chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s left the instance while combat was in progress. The instance player count is still %u.", player->GetName().c_str(), mapABInfo->playerCount);
                            }
                        }
                    }
                }
                else
                {
                    mapABInfo->playerCount = map->GetPlayersCountExceptGMs() - 1;
                    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllMapScript::OnPlayerLeaveAll(): Player left, new player count is {}", mapABInfo->playerCount);
                }
            }

            // updates the player count, player levels for the map (but don't re-count players)
            UpdateMapPlayerStats(map, false);

            if (PlayerChangeNotify && !player->IsGameMaster() && !areAnyPlayersInCombat && mapABInfo->enabled)
            {
                if (map->GetEntry()->IsDungeon() && player)
                {
                    Map::PlayerList const &playerList = map->GetPlayers();
                    if (!playerList.IsEmpty())
                    {
                        for (Map::PlayerList::const_iterator playerIteration = playerList.begin(); playerIteration != playerList.end(); ++playerIteration)
                        {
                            Player* mapPlayer = playerIteration->GetSource();
                            if (mapPlayer && mapPlayer != player)
                            {
                                ChatHandler chatHandle = ChatHandler(mapPlayer->GetSession());
                                chatHandle.PSendSysMessage("|cffFF0000 [AutoBalance]|r|cffFF8000 %s left the instance. There are %u player(s) in this instance. Difficulty set to %u player(s).|r",
                                    player->GetName().c_str(),
                                    mapABInfo->playerCount,
                                    mapABInfo->adjustedPlayerCount);
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
        if (creature->GetMap()->IsDungeon())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel(): {} ({}) | {}", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        creature->GetGUID().ToString()
            );

            // Update the map's data if it is out of date
            UpdateMapDataIfNeeded(creature->GetMap());

            Map* creatureMap = creature->GetMap();
            InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));
            
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::Creature_SelectLevel(): {} ({}) is in map {} ({}{}{}{})", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        creatureMap->GetMapName(),
                        creatureMap->GetId(),
                        instanceMap ? "-" + std::to_string(instanceMap->GetId()) : "",
                        instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                        instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );

            // add the creature to the map's tracking list
            AddCreatureToMapData(creature);

            // do an initial modification of the creature
            ModifyCreatureAttributes(creature);
        }
    }

    void OnCreatureAddWorld(Creature* creature) override
    {   
        if (creature->GetMap()->IsDungeon())
        {
            Map* creatureMap = creature->GetMap();
            InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureAddWorld(): {} ({}) added to map {} ({}{}{}{})", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        creatureMap->GetMapName(),
                        creatureMap->GetId(),
                        instanceMap ? "-" + std::to_string(instanceMap->GetId()) : "",
                        instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                        instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );
        }
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        if (creature->GetMap()->IsDungeon())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureRemoveWorld(): {} ({}) | {}", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        creature->GetGUID().ToString()
            );
                        
            InstanceMap* instanceMap = creature->GetMap()->ToInstanceMap();
            Map* baseMap = sMapMgr->FindBaseMap(creature->GetMapId());

            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnCreatureRemoveWorld(): {} ({}) removed from map {} ({}{}{}{})", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        baseMap->GetMapName(),
                        baseMap->GetId(),
                        instanceMap ? "-" + std::to_string(instanceMap->GetInstanceId()) : "",
                        instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                        instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );
        
            // remove the creature from the map's tracking list, if present
            RemoveCreatureFromMapData(creature);
        }
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        // If the config is out of date and the creature was reset, run modify against it
        if (ResetCreatureIfNeeded(creature))
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance:: {}", SPACER);
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnAllCreatureUpdate(): Creature {} ({}) | {}", 
                        creature->GetName(), 
                        creature->GetLevel(),
                        creature->GetGUID().ToString()
            );
            
            // Update the map's data if it is out of date
            UpdateMapDataIfNeeded(creature->GetMap());
            
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::OnAllCreatureUpdate(): Creature {} ({}) is reset to its original stats.", creature->GetName(), creature->GetLevel());

            ModifyCreatureAttributes(creature);
        }
    }

    // Reset the passed creature to stock if the config has changed
    bool ResetCreatureIfNeeded(Creature* creature)
    {
        // make sure we have a creature and that it's assigned to a map
        if (!creature || !creature->GetMap())
            return false;

        // if this isn't a dungeon, make no changes
        if (!(creature->GetMap()->IsDungeon()))
            return false;

        // if this is a pet or summon controlled by the player, make no changes
        if ((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer())
            return false;

        // if this is a non-relevant creature, skip
        // level and health checks for some nasty level 1 critters in some encounters
        if ((creature->IsCritter() && creature->GetLevel() <= 5 && creature->GetMaxHealth() < 100) || creature->IsTotem())
            return false;

        // get (or create) the creature's info
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

        // if creature is dead and configTime is 0, skip
        if (creature->isDead() && creatureABInfo->configTime == 0)
        {
            return false;
        }
        // if the creature is dead but configTime is NOT 0, we set it to 0 so that it will be recalculated if revived
        // also remember that this creature was once alive but is now dead
        else if (creature->isDead())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ResetCreatureIfNeeded(): {} ({}) is dead and configTime is not 0 - prime for reset if revived.", creature->GetName(), creature->GetLevel());
            creatureABInfo->configTime = 0;
            creatureABInfo->wasAliveNowDead = true;
            return false;
        }

        // if the config is outdated, reset the creature
        if (creatureABInfo->configTime != lastConfigTime)
        {
            // retain some values
            uint8 unmodifiedLevel = creatureABInfo->UnmodifiedLevel;
            bool isActive = creatureABInfo->isActive;
            bool wasAliveNowDead = creatureABInfo->wasAliveNowDead;
            bool isInCreatureList = creatureABInfo->isInCreatureList;

            // reset AutoBalance modifiers
            creature->CustomData.Erase("AutoBalanceCreatureInfo");
            AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");

            // grab the creature's template and the original creature's stats
            CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();

            // set the creature's level
            creature->SetLevel(unmodifiedLevel);
            creatureABInfo->UnmodifiedLevel = unmodifiedLevel;

            // get the creature's base stats
            CreatureBaseStats const* origCreatureStats = sObjectMgr->GetCreatureBaseStats(unmodifiedLevel, creatureTemplate->unit_class);

            // health
            float currentHealthPercent = (float)creature->GetHealth() / (float)creature->GetMaxHealth();
            creature->SetMaxHealth(origCreatureStats->GenerateHealth(creatureTemplate));
            creature->SetHealth((float)origCreatureStats->GenerateHealth(creatureTemplate) * currentHealthPercent);

            // mana
            if (creature->getPowerType() == POWER_MANA && creature->GetPower(POWER_MANA) >= 0 && creature->GetMaxPower(POWER_MANA) > 0)
            {
                float currentManaPercent = creature->GetPower(POWER_MANA) / creature->GetMaxPower(POWER_MANA);
                creature->SetMaxPower(POWER_MANA, origCreatureStats->GenerateMana(creatureTemplate));
                creature->SetPower(POWER_MANA, creature->GetMaxPower(POWER_MANA) * currentManaPercent);
            }

            // armor
            creature->SetArmor(origCreatureStats->GenerateArmor(creatureTemplate));

            // restore the saved data
            creatureABInfo->isActive = isActive;
            creatureABInfo->wasAliveNowDead = wasAliveNowDead;
            creatureABInfo->isInCreatureList = isInCreatureList;

            // damage and ccduration are handled using AutoBalanceCreatureInfo data only

            // return true to indicate that the creature was reset
            return true;
        }

        // fixes an issue with summons showing as the incorrect level
        // ramps levels up or down over time to not overwhelm the client (?)
        if (creature->IsSummon() && creatureABInfo->selectedLevel && (creature->GetLevel() != creatureABInfo->selectedLevel))
        {
            if (creatureABInfo->selectedLevel - creature->getLevel() > 0)
            {
                creature->SetLevel(std::min(creatureABInfo->selectedLevel, (uint8)(creature->GetLevel() + 2)));
            }
            else
            {
                creature->SetLevel(std::max(creatureABInfo->selectedLevel, (uint8)(creature->GetLevel() - 2)));
            }
        }

        // creature was not reset, return false
        return false;

    }

    void ModifyCreatureAttributes(Creature* creature)
    {
        // make sure we have a creature and that it's assigned to a map
        if (!creature || !creature->GetMap())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): creature or creature's map is null, not changed.");
            return;
        }

        // if this isn't a dungeon, make no changes
        if (!(creature->GetMap()->IsDungeon()))
        {
            // LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is not in a dungeon, not changed.", creature->GetName(), creature->GetLevel());
            return;
        }

        // if this creature is in the dungeon's base map, make no changes
        if (!(creature->GetMap()->ToInstanceMap()))
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is in the dungeon's base map, not changed.", creature->GetName(), creature->GetLevel());
            return;
        }

        // if this is a pet or summon controlled by the player, make no changes
        if ((creature->IsHunterPet() || creature->IsPet() || creature->IsSummon()) && creature->IsControlledByPlayer())
        {    LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is a pet or summon controlled by the player, not changed.", creature->GetName(), creature->GetLevel());
            return;
        }

        // TODO: Is this still needed?
        // // if this is a non-relevant creature, make no changes
        // // ensure that critters are just flavor and not real enemies (critters are used very inconsistently in WoW)
        // if ((creature->IsCritter() && creature->GetLevel() <= 5 && creature->GetMaxHealth() < 100))
        // {
        //     LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is a critter <= Lvl 5 or totem, not changed.", creature->GetName(), creature->GetLevel());
        //     return;
        // }

        // grab creature and map data
        AutoBalanceCreatureInfo *creatureABInfo=creature->CustomData.GetDefault<AutoBalanceCreatureInfo>("AutoBalanceCreatureInfo");
        Map* baseMap = sMapMgr->FindBaseMap(creature->GetMapId());
        InstanceMap* instanceMap = ((InstanceMap*)sMapMgr->FindMap(creature->GetMapId(), creature->GetInstanceId()));
        AutoBalanceMapInfo *mapABInfo=instanceMap->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        // mark the creature as updated using the current settings if needed
        if (creatureABInfo->configTime != lastConfigTime)
            creatureABInfo->configTime = lastConfigTime;

        // check to make sure that the creature's map is enabled for scaling
        if (!mapABInfo->enabled)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is in map {} ({}{}{}{}) that is not enabled, not changed.", 
                      creature->GetName(), 
                      creature->GetLevel(),
                      baseMap ? baseMap->GetMapName() : "Unknown",
                      baseMap ? std::to_string(baseMap->GetId()) : "Unknown",
                      instanceMap ? "-" + std::to_string(instanceMap->GetId()) : "",
                      instanceMap ? ", " + std::to_string(instanceMap->GetMaxPlayers()) + "-player" : "",
                      instanceMap ? instanceMap->IsHeroic() ? " Heroic" : " Normal" : ""
            );
            return;
        }

        // if this creature is below 85% of the minimum LFG level for the map, make no changes
        // if this is a critter that is substantial enough to be considered a real enemy, still modify it
        // if this is a trigger, still modify it
        if ((creatureABInfo->UnmodifiedLevel < (float)mapABInfo->lfgMinLevel * .85f) && 
             !(creature->IsCritter() && creatureABInfo->UnmodifiedLevel >= 5 && creature->GetMaxHealth() > 100) && 
             !creature->IsTrigger()
           )
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is below 85% of the minimum LFG level for the map, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        // if this creature is above 115% of the maximum LFG level for the map, make no changes
        // if this is a critter that is substantial enough to be considered a real enemy, still modify it
        // if this is a trigger, still modify it
        if ((creatureABInfo->UnmodifiedLevel > (float)mapABInfo->lfgMaxLevel * 1.15f) && 
             !(creature->IsCritter() && creatureABInfo->UnmodifiedLevel >= 5 && creature->GetMaxHealth() > 100) && 
             !creature->IsTrigger()
           )
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is above 115% of the maximum LFG level for the map, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        // if the creature was dead (but this function is being called because they are being revived), reset it and allow modifications
        if (creatureABInfo->wasAliveNowDead)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) was dead but appears to be alive now, reset wasAliveNowDead flag.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            // if the creature was dead, reset it
            creatureABInfo->wasAliveNowDead = false;
        }
        // if the creature is dead and wasn't marked as dead by this script, simply skip
        else if (creature->isDead())
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is dead, do not modify.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        CreatureTemplate const *creatureTemplate = creature->GetCreatureTemplate();

        // check to see if the creature is in the forced num players list
        uint32 forcedNumPlayers = GetForcedNumPlayers(creatureTemplate->Entry);

        if (forcedNumPlayers == 0)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is in the forced num players list with a value of 0, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return; // forcedNumPlayers 0 means that the creature is contained in DisabledID -> no scaling
        }

        // start with the map's adjusted player count
        uint32 adjustedPlayerCount = mapABInfo->adjustedPlayerCount;

        // if the forced value is set and the adjusted player count is above the forced value, change it to match
        if (forcedNumPlayers > 0 && adjustedPlayerCount > forcedNumPlayers)
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is in the forced num players list with a value of {}, adjusting adjustedPlayerCount to match.", creature->GetName(), creatureABInfo->UnmodifiedLevel, forcedNumPlayers);
            adjustedPlayerCount = instanceMap->GetMaxPlayers();
        }

        // store the current player count in the creature and map's data
        creatureABInfo->instancePlayerCount = adjustedPlayerCount;

        if (!creatureABInfo->instancePlayerCount) // no players in map, do not modify attributes
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): {} ({}) is on a map with no players, not changed.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            return;
        }

        if (!sABScriptMgr->OnBeforeModifyAttributes(creature, creatureABInfo->instancePlayerCount))
            return;

        // only scale levels if level scaling is enabled and the instance's average creature level is not within the skip range
        if (LevelScaling &&
             ((mapABInfo->avgCreatureLevel > mapABInfo->highestPlayerLevel + mapABInfo->levelScalingSkipHigherLevels || mapABInfo->levelScalingSkipHigherLevels == 0) ||
              (mapABInfo->avgCreatureLevel < mapABInfo->highestPlayerLevel - mapABInfo->levelScalingSkipLowerLevels || mapABInfo->levelScalingSkipLowerLevels == 0))
           )
        {
            uint8 selectedLevel;

            // handle "special" creatures
            if ((creature->IsTrigger() || creature->IsTotem() || creature->IsCritter()) && creature->GetLevel() <= 5)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes(): Creature {} ({}) is a trigger, totem, or critter, not level scaled.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
                selectedLevel = creatureABInfo->UnmodifiedLevel;
            }
            // if we're using dynamic scaling, calculate the creature's level based relative to the highest player level in the map
            else if (LevelScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) dynamic scaling floor: {}, ceiling: {}.", 
                          creature->GetName(), 
                          creatureABInfo->UnmodifiedLevel, 
                          mapABInfo->levelScalingDynamicFloor, 
                          mapABInfo->levelScalingDynamicCeiling
                );

                // calculate the creature's new level
                selectedLevel = (mapABInfo->highestPlayerLevel + mapABInfo->levelScalingDynamicCeiling) - (mapABInfo->highestCreatureLevel - creatureABInfo->UnmodifiedLevel);

                // check to be sure that the creature's new level is at least the dynamic scaling floor
                if (selectedLevel < (mapABInfo->highestPlayerLevel - mapABInfo->levelScalingDynamicFloor))
                {
                    selectedLevel = mapABInfo->highestPlayerLevel - mapABInfo->levelScalingDynamicFloor;
                }

                // check to be sure that the creature's new level is no higher than the dynamic scaling ceiling
                if (selectedLevel > (mapABInfo->highestPlayerLevel + mapABInfo->levelScalingDynamicCeiling))
                {
                    selectedLevel = mapABInfo->highestPlayerLevel + mapABInfo->levelScalingDynamicCeiling;
                }

                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) scaled to level {} via dynamic scaling.", creature->GetName(), creatureABInfo->UnmodifiedLevel, selectedLevel);
            }
            // otherwise we're using "fixed" scaling and should use the highest player level in the map
            else
            {
                selectedLevel = mapABInfo->highestPlayerLevel;
                LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) scaled to level {} via fixed scaling.", creature->GetName(), creatureABInfo->UnmodifiedLevel, selectedLevel);
            }

            creatureABInfo->selectedLevel = selectedLevel;
            
            // summons have their level change done in the ResetCreatureIfNeeded function
            // this is to workaround an issue where summons show as the wrong level in the client
            if (!creature->IsSummon())
            {
                // set the creature's level
                creature->SetLevel(selectedLevel);
            }
            
        }
        else
        {
            LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) not level scaled due to level scaling being disabled or the instance's average creature level being inside the skip range.", creature->GetName(), creatureABInfo->UnmodifiedLevel);
            creatureABInfo->selectedLevel = creatureABInfo->UnmodifiedLevel;
        }

        creatureABInfo->entry = creature->GetEntry();

        CreatureBaseStats const* origCreatureStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->UnmodifiedLevel, creatureTemplate->unit_class);
        CreatureBaseStats const* creatureStats = sObjectMgr->GetCreatureBaseStats(creatureABInfo->selectedLevel, creatureTemplate->unit_class);

        uint32 baseMana = origCreatureStats->GenerateMana(creatureTemplate);
        uint32 scaledHealth = 0;
        uint32 scaledMana = 0;

        // Inflection Point
        AutoBalanceInflectionPointSettings inflectionPointSettings = getInflectionPointSettings(instanceMap, creature->IsDungeonBoss());

        // Generate the default multiplier
        float defaultMultiplier = getDefaultMultiplier(instanceMap, inflectionPointSettings);

        if (!sABScriptMgr->OnAfterDefaultMultiplier(creature, defaultMultiplier))
            return;

        // Stat Modifiers
        AutoBalanceStatModifiers statModifiers = getStatModifiers(instanceMap, creature);
        float statMod_global        = statModifiers.global;
        float statMod_health        = statModifiers.health;
        float statMod_mana          = statModifiers.mana;
        float statMod_armor         = statModifiers.armor;
        float statMod_damage        = statModifiers.damage;
        float statMod_ccDuration    = statModifiers.ccduration;

        //
        //  Health Scaling
        //

        float healthMultiplier = defaultMultiplier * statMod_global * statMod_health;

        if (healthMultiplier <= MinHPModifier)
            healthMultiplier = MinHPModifier;

        float hpStatsRate  = 1.0f;
        float originalHealth = origCreatureStats->GenerateHealth(creatureTemplate);

        float newBaseHealth;

        // The database holds multiple values for base health, one for each expansion
        // This code will smooth transition between the different expansions based on the highest player level in the instance
        // Only do this if level scaling is enabled

        if (LevelScaling)
        {
            float vanillaHealth = creatureStats->BaseHealth[0];
            float bcHealth = creatureStats->BaseHealth[1];
            float wotlkHealth = creatureStats->BaseHealth[2];

            // vanilla health
            if (mapABInfo->highestPlayerLevel <= 60)
            {
                newBaseHealth = vanillaHealth;
            }
            // transition from vanilla to BC health
            else if (mapABInfo->highestPlayerLevel < 63)
            {
                float vanillaMultiplier = (63 - mapABInfo->highestPlayerLevel) / 3.0f;
                float bcMultiplier = 1.0f - vanillaMultiplier;

                newBaseHealth = (vanillaHealth * vanillaMultiplier) + (bcHealth * bcMultiplier);
            }
            // BC health
            else if (mapABInfo->highestPlayerLevel <= 70)
            {
                newBaseHealth = bcHealth;
            }
            // transition from BC to WotLK health
            else if (mapABInfo->highestPlayerLevel < 73)
            {
                float bcMultiplier = (73 - mapABInfo->highestPlayerLevel) / 3.0f;
                float wotlkMultiplier = 1.0f - bcMultiplier;

                newBaseHealth = (bcHealth * bcMultiplier) + (wotlkHealth * wotlkMultiplier);
            }
            // WotLK health
            else
            {
                newBaseHealth = wotlkHealth;

                // special increase for end-game content
                if (LevelScalingEndGameBoost)
                    if (mapABInfo->highestPlayerLevel >= 75 && creatureABInfo->UnmodifiedLevel < 75)
                    {
                        newBaseHealth *= (float)(mapABInfo->highestPlayerLevel-70) * 0.3f;
                    }
            }

            float newHealth = newBaseHealth * creatureTemplate->ModHealth;
            hpStatsRate = newHealth / originalHealth;

            healthMultiplier *= hpStatsRate;
        }

        creatureABInfo->HealthMultiplier = healthMultiplier;
        scaledHealth = round(originalHealth * creatureABInfo->HealthMultiplier);

        //
        //  Mana Scaling
        //
        float manaStatsRate  = 1.0f;
        float newMana = creatureStats->GenerateMana(creatureTemplate);
            manaStatsRate = newMana/float(baseMana);

        // check to be sure that manaStatsRate is not nan
        if (manaStatsRate != manaStatsRate)
        {
            creatureABInfo->ManaMultiplier = 0.0f;
        }
        else
        {
            creatureABInfo->ManaMultiplier =  defaultMultiplier * manaStatsRate * statMod_global * statMod_mana;

            if (creatureABInfo->ManaMultiplier <= MinManaModifier)
            {
                creatureABInfo->ManaMultiplier = MinManaModifier;
            }
        }

        scaledMana = round(baseMana * creatureABInfo->ManaMultiplier);

        //
        //  Armor Scaling
        //
        creatureABInfo->ArmorMultiplier = defaultMultiplier * statMod_global * statMod_armor;
        uint32 newBaseArmor = round(creatureABInfo->ArmorMultiplier * (LevelScaling ? creatureStats->GenerateArmor(creatureTemplate) : origCreatureStats->GenerateArmor(creatureTemplate)));

        //
        //  Damage Scaling
        //
        float damageMul = defaultMultiplier * statMod_global * statMod_damage;

        // Can not be less than MinDamageModifier
        if (damageMul <= MinDamageModifier)
        {
            damageMul = MinDamageModifier;
        }

        // Calculate the new base damage
        float origDmgBase = origCreatureStats->GenerateBaseDamage(creatureTemplate);
        float newDmgBase = 0;

        float vanillaDamage = creatureStats->BaseDamage[0];
        float bcDamage = creatureStats->BaseDamage[1];
        float wotlkDamage = creatureStats->BaseDamage[2];

        // The database holds multiple values for base damage, one for each expansion
        // This code will smooth transition between the different expansions based on the highest player level in the instance
        // Only do this if level scaling is enabled

        if (LevelScaling)
            {
            // vanilla damage
            if (mapABInfo->highestPlayerLevel <= 60)
            {
                newDmgBase=vanillaDamage;
            }
            // transition from vanilla to BC damage
            else if (mapABInfo->highestPlayerLevel < 63)
            {
                float vanillaMultiplier = (63 - mapABInfo->highestPlayerLevel) / 3.0;
                float bcMultiplier = 1.0f - vanillaMultiplier;

                newDmgBase=(vanillaDamage * vanillaMultiplier) + (bcDamage * bcMultiplier);
            }
            // BC damage
            else if (mapABInfo->highestPlayerLevel <= 70)
            {
                newDmgBase=bcDamage;
            }
            // transition from BC to WotLK damage
            else if (mapABInfo->highestPlayerLevel < 73)
            {
                float bcMultiplier = (73 - mapABInfo->highestPlayerLevel) / 3.0;
                float wotlkMultiplier = 1.0f - bcMultiplier;

                newDmgBase=(bcDamage * bcMultiplier) + (wotlkDamage * wotlkMultiplier);
            }
            // WotLK damage
            else
            {
                newDmgBase=wotlkDamage;

                // special increase for end-game content
                if (LevelScalingEndGameBoost && instanceMap->GetMaxPlayers() <= 5) {
                    if (mapABInfo->highestPlayerLevel >= 75 && creatureABInfo->UnmodifiedLevel < 75)
                        newDmgBase *= float(mapABInfo->highestPlayerLevel-70) * 0.3f;
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

        uint32 prevPlayerDamageRequired = creature->GetPlayerDamageReq();
        uint32 prevCreateHealth = creature->GetCreateHealth();

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

        uint32 playerDamageRequired = creature->GetPlayerDamageReq();
        if(prevPlayerDamageRequired == 0)
        {
            // If already reached damage threshold for loot, drop to zero again
            creature->LowerPlayerDamageReq(playerDamageRequired, true);
        }
        else
        {
            // Scale the damage requirements similar to creature HP scaling
            uint32 scaledPlayerDmgReq = float(prevPlayerDamageRequired) * float(scaledHealth) / float(prevCreateHealth);
            // Do some math
            creature->LowerPlayerDamageReq(playerDamageRequired - scaledPlayerDmgReq, true);
        }

        //
        // Reward Scaling
        //

        // calculate the average multiplier after level scaling is applied
        float averageMultiplierAfterLevelScaling;
        // use health and damage to calculate the average multiplier
        averageMultiplierAfterLevelScaling = (creatureABInfo->HealthMultiplier + creatureABInfo->DamageMultiplier) / 2.0f;

        // XP Scaling
        if (RewardScalingXP)
        {
            if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
            {
                creatureABInfo->XPModifier = RewardScalingXPModifier;
            }
            else if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
                creatureABInfo->XPModifier = averageMultiplierAfterLevelScaling * RewardScalingXPModifier;
            }
        }

        // Money Scaling
        if (RewardScalingMoney)
        {
            //LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) has an average post-level-scaling modifier of {}.", creature->GetName(), creature->GetLevel(), averageMultiplierAfterLevelScaling);

            if (RewardScalingMethod == AUTOBALANCE_SCALING_FIXED)
            {
                creatureABInfo->MoneyModifier = RewardScalingMoneyModifier;
            }
            else if (RewardScalingMethod == AUTOBALANCE_SCALING_DYNAMIC)
            {
                creatureABInfo->MoneyModifier = averageMultiplierAfterLevelScaling * RewardScalingMoneyModifier;
            }
        }

        // debug log the new stat multipliers stored in CreatureABInfo in a compact, single-line format
        LOG_DEBUG("module.AutoBalance", "AutoBalance_AllCreatureScript::ModifyCreatureAttributes: Creature {} ({}) has multipliers: H:{} M:{} A:{} D:{} CC:{} XP:{} $:{}.",
                  creature->GetName(),
                  creature->GetLevel(),
                  creatureABInfo->HealthMultiplier,
                  creatureABInfo->ManaMultiplier,
                  creatureABInfo->ArmorMultiplier,
                  creatureABInfo->DamageMultiplier,
                  creatureABInfo->CCDurationMultiplier,
                  creatureABInfo->XPModifier,
                  creatureABInfo->MoneyModifier
                  );


        creature->UpdateAllStats();
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
            { "getoffset",        SEC_PLAYER,                            true, &HandleABGetOffsetCommand,                 "Shows current global player offset value." },
            { "mapstat",          SEC_PLAYER,                            true, &HandleABMapStatsCommand,                  "Shows current autobalance information for this map" },
            { "creaturestat",     SEC_PLAYER,                            true, &HandleABCreatureStatsCommand,             "Shows current autobalance information for selected creature." },
        };

        static std::vector<ChatCommand> commandTable =
        {
            { "autobalance",     SEC_PLAYER,                             false, NULL,                      "", ABCommandTable },
            { "ab",              SEC_PLAYER,                             false, NULL,                      "", ABCommandTable },
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
            lastConfigTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
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

    static bool HandleABMapStatsCommand(ChatHandler* handler, const char* /*args*/)
    {
        Player *player;
        player = handler->getSelectedPlayer() ? handler->getSelectedPlayer() : handler->GetPlayer();

        AutoBalanceMapInfo *mapABInfo=player->GetMap()->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        if (player->GetMap()->IsDungeon())
        {
            handler->PSendSysMessage("---");
            handler->PSendSysMessage("Map: ID %u-%u | %s (%u-player %s)%s",
                                    player->GetMapId(),
                                    player->GetInstanceId(),
                                    player->GetMap()->GetMapName(),
                                    player->GetMap()->ToInstanceMap()->GetMaxPlayers(),
                                    player->GetMap()->ToInstanceMap()->IsHeroic() ? "Heroic" : "Normal",
                                    mapABInfo->enabled ? "" : " | AutoBalance DISABLED");
            handler->PSendSysMessage("Players on map: %u (Lvl %u - %u)",
                                    mapABInfo->playerCount,
                                    mapABInfo->lowestPlayerLevel,
                                    mapABInfo->highestPlayerLevel
                                    );
            if (mapABInfo->playerCount < mapABInfo->minPlayers && !PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage("Adjusted Player Count: %u (Map Minimum)", mapABInfo->adjustedPlayerCount);
            }
            else if (mapABInfo->playerCount < mapABInfo->minPlayers && PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage("Adjusted Player Count: %u (Map Minimum + Difficulty Offset of %u)", mapABInfo->adjustedPlayerCount, PlayerCountDifficultyOffset);
            }
            else if (PlayerCountDifficultyOffset)
            {
                handler->PSendSysMessage("Adjusted Player Count: %u (Difficulty Offset of %u)", mapABInfo->adjustedPlayerCount, PlayerCountDifficultyOffset);
            }
            else
            {
                handler->PSendSysMessage("Adjusted Player Count: %u", mapABInfo->adjustedPlayerCount);
            }
            handler->PSendSysMessage("LFG Range: Lvl %u - %u (Target: Lvl %u)", mapABInfo->lfgMinLevel, mapABInfo->lfgMaxLevel, mapABInfo->lfgTargetLevel);
            handler->PSendSysMessage("Map Level: %u%s",
                                    (uint8)(mapABInfo->avgCreatureLevel+0.5f),
                                    mapABInfo->isLevelScalingEnabled && mapABInfo->enabled ? "->" + std::to_string(mapABInfo->highestPlayerLevel) + " (Level Scaling Enabled)" : " (Level Scaling Disabled)"
                                    );
            handler->PSendSysMessage("World health|damage multiplier: %.3f | %.3f",
                                    mapABInfo->worldHealthMultiplier,
                                    mapABInfo->worldDamageHealingMultiplier
                                    );
            handler->PSendSysMessage("Original Creature Level Range: %u - %u (Avg: %.2f)",
                                    mapABInfo->lowestCreatureLevel,
                                    mapABInfo->highestCreatureLevel,
                                    mapABInfo->avgCreatureLevel
                                    );
            handler->PSendSysMessage("Active | Total Creatures in map: %u | %u",
                                    mapABInfo->activeCreatureCount,
                                    mapABInfo->allMapCreatures.size()
                                    );

            return true;
        }
        else
        {
            handler->PSendSysMessage("The target is not in a dungeon.");
            return true;
        }
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

        handler->PSendSysMessage("---");
        handler->PSendSysMessage("%s (%u%s%s), %s",
                                  target->GetName(),
                                  creatureABInfo->UnmodifiedLevel,
                                  creatureABInfo->UnmodifiedLevel != target->GetLevel() ? "->" + std::to_string(creatureABInfo->selectedLevel) : "",
                                  target->IsDungeonBoss() ? " | Boss" : "",
                                  creatureABInfo->isActive ? "Active for Map Stats" : "Ignored for Map Stats");
        handler->PSendSysMessage("Health multiplier: %.3f", creatureABInfo->HealthMultiplier);
        handler->PSendSysMessage("Mana multiplier: %.3f", creatureABInfo->ManaMultiplier);
        handler->PSendSysMessage("Armor multiplier: %.3f", creatureABInfo->ArmorMultiplier);
        handler->PSendSysMessage("Damage multiplier: %.3f", creatureABInfo->DamageMultiplier);
        handler->PSendSysMessage("CC Duration multiplier: %.3f", creatureABInfo->CCDurationMultiplier);
        handler->PSendSysMessage("XP multiplier: %.3f  Money multiplier: %.3f", creatureABInfo->XPModifier, creatureABInfo->MoneyModifier);

        return true;

    }
};

class AutoBalance_GlobalScript : public GlobalScript {
public:
    AutoBalance_GlobalScript() : GlobalScript("AutoBalance_GlobalScript") { }

    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type,  uint32 /*creditEntry*/, Unit* /*source*/, Difficulty /*difficulty_fixed*/, DungeonEncounterList const* /*encounters*/, uint32 /*dungeonCompleted*/, bool updated) override {
        //if (!dungeonCompleted)
        //    return;

        if (!rewardEnabled || !updated)
            return;

        if (map->GetPlayersCountExceptGMs() < MinPlayerReward)
            return;

        AutoBalanceMapInfo *mapABInfo=map->CustomData.GetDefault<AutoBalanceMapInfo>("AutoBalanceMapInfo");

        // skip if it's not a pre-wotlk dungeon/raid and if it's not scaled
        if (!LevelScaling || mapABInfo->mapLevel <= 70 || mapABInfo->lfgMinLevel <= 70
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
    new AutoBalance_GameObjectScript();
    new AutoBalance_AllCreatureScript();
    new AutoBalance_AllMapScript();
    new AutoBalance_CommandScript();
    new AutoBalance_GlobalScript();
}
